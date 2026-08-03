// 候选 A —— lwIP（现状基线）。
//
// 用**生产同一份** vendored lwIP + 同一份 lwipopts.h + 同样的 accept-all 补丁，
// 搭一个最小 tun2socks：TAP 收以太帧 → ethernet_input → catch-all 监听终结 TCP
// → 用内核 socket 连 127.0.0.1:<原始目的端口> → 双向搬字节。
// 与 src/net/NetStack.cpp 的差别只有「出站不走 SOCKS5 而是直连」——为的是不把 mihomo
// 的开销混进测量里。协议处理路径（etharp / ip4 / tcp_in / tcp_out）完全一致。
//
// 用法: lwip2socks <tap-if> <stack-ip> <netmask>
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

// ★ 必须跟着 lwipopts 走，不能写死：只有落地到 socket 后才 tcp_recved，所以「已收下但
//   还没还窗口」的字节数上界就是 TCP_WND。写死 512K 的话，候选 F 把 TCP_WND 提到 1 MiB
//   之后每条连接都会在 on_recv 里撞上界直接被 abort —— 而且症状是「吞吐变 0」而不是编译错。
#define BUFSZ (2 * (TCP_WND > TCP_SND_BUF ? TCP_WND : TCP_SND_BUF))
#define MAXFRAME 2048

static int g_tap = -1;
static int g_ep = -1;
static struct netif g_nif;
static u8_t g_mac[6] = {0x02, 0x00, 0x5b, 0x00, 0x00, 0x02};

u32_t sys_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

// epoll data.ptr 的第一个字段一律是 kind：0=TCP 连接、1=UDP 流。NULL=TAP。
// 这样一个 epoll 循环同时喂 TCP 上游 socket 和 UDP 上游 socket，不必开两套。
#define KIND_TCP 0
#define KIND_UDP 1

struct conn {
    int kind;                 // = KIND_TCP（calloc 清零即是）
    struct tcp_pcb *pcb;
    int sock;
    char up[BUFSZ];   int up_len, up_off;   // 设备 → 上游
    char dn[BUFSZ];   int dn_len, dn_off;   // 上游 → 设备
    int dev_fin;      // 设备侧已发 FIN
    int up_eof;       // 上游侧已 EOF
    int want_out;     // sock 上是否已注册 EPOLLOUT
    int closing;
};

static void conn_free(struct conn *c)
{
    if (!c) return;
    if (c->sock >= 0) { epoll_ctl(g_ep, EPOLL_CTL_DEL, c->sock, NULL); close(c->sock); c->sock = -1; }
    if (c->pcb) {
        tcp_arg(c->pcb, NULL); tcp_recv(c->pcb, NULL); tcp_sent(c->pcb, NULL); tcp_err(c->pcb, NULL);
        tcp_abort(c->pcb); c->pcb = NULL;
    }
    free(c);
}

static void mod_sock(struct conn *c, int out)
{
    if (c->sock < 0 || c->want_out == out) return;
    c->want_out = out;
    struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP | (out ? EPOLLOUT : 0), .data.ptr = c};
    epoll_ctl(g_ep, EPOLL_CTL_MOD, c->sock, &ev);
}

// 设备 → 上游：把 up 缓冲写进 socket，写出去多少就 tcp_recved 多少（背压靠接收窗口，
// 与 NetStack.cpp 的做法一致：**先落地再还窗口**，不无条件全开）
static int flush_up(struct conn *c)
{
    while (c->up_off < c->up_len) {
        ssize_t n = write(c->sock, c->up + c->up_off, c->up_len - c->up_off);
        if (n > 0) {
            c->up_off += (int)n;
            if (c->pcb) tcp_recved(c->pcb, (u16_t)(n > 0xFFFF ? 0xFFFF : n));
        } else {
            if (errno == EAGAIN) { mod_sock(c, 1); return 0; }
            return -1;
        }
    }
    c->up_len = c->up_off = 0;
    mod_sock(c, 0);
    if (c->dev_fin) shutdown(c->sock, SHUT_WR);
    return 0;
}

// 上游 → 设备：把 dn 缓冲经 tcp_write 交给 lwIP，能塞多少塞多少（受 tcp_sndbuf 限制）
static int flush_dn(struct conn *c)
{
    if (!c->pcb) return -1;
    int wrote = 0;
    while (c->dn_off < c->dn_len) {
        u16_t space = tcp_sndbuf(c->pcb);
        if (space == 0) break;
        int n = c->dn_len - c->dn_off;
        if (n > space) n = space;
        err_t e = tcp_write(c->pcb, c->dn + c->dn_off, (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM) break;
        if (e != ERR_OK) return -1;
        c->dn_off += n;
        wrote += n;
    }
    if (wrote) tcp_output(c->pcb);
    if (c->dn_off == c->dn_len) {
        c->dn_len = c->dn_off = 0;
        if (c->up_eof && !c->closing) { c->closing = 1; tcp_shutdown(c->pcb, 0, 1); }
    }
    return 0;
}

static void on_err(void *arg, err_t err)
{
    (void)err;
    struct conn *c = arg;
    if (!c) return;
    c->pcb = NULL;              // lwIP 已经释放了 pcb，不能再碰
    conn_free(c);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct conn *c = arg;
    if (!c) { if (p) pbuf_free(p); tcp_abort(pcb); return ERR_ABRT; }
    if (err != ERR_OK) { if (p) pbuf_free(p); conn_free(c); return ERR_ABRT; }
    if (!p) {                                  // 设备侧 FIN
        c->dev_fin = 1;
        if (c->up_off == c->up_len) shutdown(c->sock, SHUT_WR);
        return ERR_OK;
    }
    // up 缓冲一定装得下：只有落地后才 tcp_recved，未还的窗口 ≤ TCP_WND(128K) < BUFSZ(512K)
    if (c->up_len - c->up_off + (int)p->tot_len > BUFSZ) {
        pbuf_free(p); conn_free(c); return ERR_ABRT;
    }
    if (c->up_off > 0 && c->up_len > c->up_off) {
        memmove(c->up, c->up + c->up_off, c->up_len - c->up_off);
        c->up_len -= c->up_off; c->up_off = 0;
    } else if (c->up_off == c->up_len) {
        c->up_len = c->up_off = 0;
    }
    pbuf_copy_partial(p, c->up + c->up_len, p->tot_len, 0);
    c->up_len += p->tot_len;
    pbuf_free(p);
    if (flush_up(c) < 0) { conn_free(c); return ERR_ABRT; }
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb; (void)len;
    struct conn *c = arg;
    if (!c) return ERR_OK;
    if (flush_dn(c) < 0) { conn_free(c); return ERR_ABRT; }
    mod_sock(c, c->want_out);   // 缓冲腾出后重新收上游
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;

    // 原始目的端口来自 catch-all 监听接下来的新 pcb（accept-all 补丁保留了它）
    u16_t dport = pcb->local_port;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ta = {0};
    ta.sin_family = AF_INET;
    ta.sin_port = htons(dport);
    inet_pton(AF_INET, "127.0.0.1", &ta.sin_addr);
    if (s < 0 || connect(s, (struct sockaddr *)&ta, sizeof(ta)) < 0) {
        if (s >= 0) close(s);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);

    struct conn *c = calloc(1, sizeof(*c));
    if (!c) { close(s); tcp_abort(pcb); return ERR_ABRT; }
    c->pcb = pcb; c->sock = s;

    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_nagle_disable(pcb);

    struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.ptr = c};
    epoll_ctl(g_ep, EPOLL_CTL_ADD, s, &ev);
    return ERR_OK;
}

// netif 出口：lwIP 要发一帧 → 直接写回 TAP
static err_t tap_linkoutput(struct netif *nif, struct pbuf *p)
{
    (void)nif;
    static char buf[MAXFRAME];
    if (p->tot_len > sizeof(buf)) return ERR_BUF;
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    ssize_t n = write(g_tap, buf, p->tot_len);
    (void)n;
    return ERR_OK;
}

static err_t tap_netif_init(struct netif *nif)
{
    nif->name[0] = 's'; nif->name[1] = 'b';
    nif->output = etharp_output;
    nif->linkoutput = tap_linkoutput;
    nif->mtu = 1500;
    nif->hwaddr_len = 6;
    memcpy(nif->hwaddr, g_mac, 6);
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    return ERR_OK;
}

// ===== UDP 转发（固定端口 UDP_PORT，单流/少流，够测吞吐+丢包）=====
// lwIP 的 UDP 绑到 IP_ANY:UDP_PORT 就能收任意目的 IP 的 UDP（udp_input_local_match 里
// local_ip isany → 匹配任何 dst ip），所以 **不用** 像 TCP 那样打 catch-all 补丁。
#define UDP_PORT 5203
#define UDP_MAXFLOW 256

struct uflow {
    int kind;              // = KIND_UDP
    int sock;              // 上游内核 UDP socket
    ip_addr_t cip;         // 设备侧客户端 IP
    u16_t cport;           // 设备侧客户端端口
    ip_addr_t dip;         // 原始目的 IP（回包要以它为源）
};
static struct uflow *g_uflows[UDP_MAXFLOW];
static int g_nuflow = 0;
static struct udp_pcb *g_udp_tx;   // 回包用：每次发前把 local 设成原始目的 (IP,port)

static struct uflow *uflow_find(const ip_addr_t *cip, u16_t cport) {
    for (int i = 0; i < g_nuflow; i++)
        if (g_uflows[i]->cport == cport && ip_addr_cmp(&g_uflows[i]->cip, cip))
            return g_uflows[i];
    return NULL;
}

// 设备 → 上游：lwIP 收到 UDP → 找/建流 → sendto 内核 127.0.0.1:UDP_PORT
static void udp_dev_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb;
    struct uflow *f = uflow_find(addr, port);
    if (!f && g_nuflow < UDP_MAXFLOW) {
        f = calloc(1, sizeof(*f));
        f->kind = KIND_UDP;
        f->sock = socket(AF_INET, SOCK_DGRAM, 0);
        f->cip = *addr; f->cport = port;
        f->dip = *ip_current_dest_addr();   // 原始目的 IP（回包源）
        struct sockaddr_in up = {0};
        up.sin_family = AF_INET; up.sin_port = htons(UDP_PORT);
        inet_pton(AF_INET, "127.0.0.1", &up.sin_addr);
        connect(f->sock, (struct sockaddr *)&up, sizeof(up));
        fcntl(f->sock, F_SETFL, fcntl(f->sock, F_GETFL, 0) | O_NONBLOCK);
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = f};
        epoll_ctl(g_ep, EPOLL_CTL_ADD, f->sock, &ev);
        g_uflows[g_nuflow++] = f;
    }
    if (f) {
        static char b[2048];
        int n = p->tot_len > sizeof(b) ? sizeof(b) : p->tot_len;
        pbuf_copy_partial(p, b, n, 0);
        (void)!write(f->sock, b, n);
    }
    pbuf_free(p);
}

// 上游 → 设备：内核 UDP 回包 → 以「原始目的 (IP,UDP_PORT)」为源 udp_sendto 回客户端
static void udp_reply(struct uflow *f) {
    static char b[2048];
    for (;;) {
        ssize_t n = read(f->sock, b, sizeof(b));
        if (n <= 0) break;
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (!p) break;
        pbuf_take(p, b, (u16_t)n);
        // 回包源必须是原始目的 (IP, UDP_PORT)——但 udp_sendto_if 有道检查：源 IP 不等于
        // netif 的 IP 就 ERR_RTE 丢包（udp.c：local_ip doesn't match）。伪造网关的整个前提就是
        // 源 IP 不是本机地址，所以必须用 udp_sendto_if_src 显式传源、绕过那道检查。
        g_udp_tx->local_port = UDP_PORT;
        udp_sendto_if_src(g_udp_tx, p, &f->cip, f->cport, &g_nif, &f->dip);
        pbuf_free(p);
    }
}

static int tap_open(const char *name)
{
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) return -1;
    struct ifreq ifr = {0};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) { close(fd); return -1; }
    return fd;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: lwip2socks <tap> <ip> <mask>\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);

    g_tap = tap_open(argv[1]);
    if (g_tap < 0) { perror("tap"); return 1; }
    fcntl(g_tap, F_SETFL, fcntl(g_tap, F_GETFL, 0) | O_NONBLOCK);

    lwip_init();

    ip4_addr_t ip, mask, gw;
    ip4addr_aton(argv[2], &ip);
    ip4addr_aton(argv[3], &mask);
    ip4_addr_set_zero(&gw);
    if (!netif_add(&g_nif, &ip, &mask, &gw, NULL, tap_netif_init, ethernet_input)) {
        fprintf(stderr, "netif_add failed\n"); return 1;
    }
    netif_set_default(&g_nif);
    netif_set_up(&g_nif);
    netif_set_link_up(&g_nif);

    struct tcp_pcb *l = tcp_new_ip_type(IPADDR_TYPE_ANY);
    tcp_bind(l, IP_ANY_TYPE, 0);                       // 端口 0 = catch-all（accept-all 补丁）
    struct tcp_pcb *lp = tcp_listen_with_backlog(l, TCP_DEFAULT_LISTEN_BACKLOG);
    if (!lp) { fprintf(stderr, "tcp_listen failed\n"); return 1; }
    // ★ 必须在 listen **之后**再强制置 0：tcp_bind(pcb, IP_ANY_TYPE, 0) 里的 0 是
    //   「自动分配一个端口」，不是「通配」——listen 后 local_port 是个真端口，
    //   tcp_in.c 的 `lpcb->local_port == 0` 通配分支就永远命中不了，SYN 全被 RST。
    //   NetStack.cpp 里有同一行（d->listener->local_port = 0），漏了它就是这个症状。
    lp->local_port = 0;
    tcp_accept(lp, on_accept);

    // UDP 转发：一个收 pcb（IP_ANY:UDP_PORT，收任意目的 IP）、一个发 pcb（回包前改 local）
    struct udp_pcb *urx = udp_new();
    udp_bind(urx, IP_ANY_TYPE, UDP_PORT);
    udp_recv(urx, udp_dev_recv, NULL);
    g_udp_tx = udp_new();

    g_ep = epoll_create1(0);
    struct epoll_event tev = {.events = EPOLLIN, .data.ptr = NULL};
    epoll_ctl(g_ep, EPOLL_CTL_ADD, g_tap, &tev);

    // ★ 把窗口配置打进 banner：候选 A 和 F 是同一份 .c 编两次，产物大小都一样，
    //   光看文件名分不出跑的是哪个配置。让程序自己报，比对着 Makefile 猜可靠。
    fprintf(stderr, "lwip2socks up on %s ip=%s TCP_WND=%d TCP_SND_BUF=%d MEM_SIZE=%d\n",
            argv[1], argv[2], (int)TCP_WND, (int)TCP_SND_BUF, (int)MEM_SIZE);

    struct epoll_event evs[256];
    char frame[MAXFRAME];
    for (;;) {
        int n = epoll_wait(g_ep, evs, 256, 10);
        for (int i = 0; i < n; i++) {
            struct conn *c = evs[i].data.ptr;
            if (c && c->kind == KIND_UDP) {            // UDP 上游回包
                udp_reply((struct uflow *)c);
                continue;
            }
            if (!c) {                                  // TAP 可读：把已到的帧全喂进去
                for (;;) {
                    ssize_t r = read(g_tap, frame, sizeof(frame));
                    if (r <= 0) break;
                    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)r, PBUF_POOL);
                    if (!p) break;                     // 池子见底，丢帧（lwIP 会靠重传补回来）
                    pbuf_take(p, frame, (u16_t)r);
                    if (g_nif.input(p, &g_nif) != ERR_OK) pbuf_free(p);
                }
                continue;
            }
            if (c->sock < 0) continue;
            int dead = 0;
            if (evs[i].events & EPOLLOUT) { if (flush_up(c) < 0) dead = 1; }
            if (!dead && (evs[i].events & (EPOLLIN | EPOLLRDHUP))) {
                for (;;) {
                    int space = BUFSZ - c->dn_len;
                    if (space <= 0) break;
                    ssize_t r = read(c->sock, c->dn + c->dn_len, space);
                    if (r > 0) { c->dn_len += (int)r; continue; }
                    if (r == 0) { c->up_eof = 1; break; }
                    if (errno == EAGAIN) break;
                    dead = 1; break;
                }
                if (!dead && flush_dn(c) < 0) dead = 1;
                if (!dead && c->up_eof && c->dn_off == c->dn_len && !c->closing) {
                    c->closing = 1; tcp_shutdown(c->pcb, 0, 1);
                }
            }
            if (!dead && (evs[i].events & (EPOLLERR | EPOLLHUP))) dead = 1;
            if (dead) conn_free(c);
        }
        sys_check_timeouts();
    }
    return 0;
}
