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
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#define BUFSZ (512 * 1024)          // ≥ TCP_WND / TCP_SND_BUF(各 128 KiB)，留足余量
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

struct conn {
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

    g_ep = epoll_create1(0);
    struct epoll_event tev = {.events = EPOLLIN, .data.ptr = NULL};
    epoll_ctl(g_ep, EPOLL_CTL_ADD, g_tap, &tev);

    fprintf(stderr, "lwip2socks up on %s ip=%s\n", argv[1], argv[2]);

    struct epoll_event evs[256];
    char frame[MAXFRAME];
    for (;;) {
        int n = epoll_wait(g_ep, evs, 256, 10);
        for (int i = 0; i < n; i++) {
            struct conn *c = evs[i].data.ptr;
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
