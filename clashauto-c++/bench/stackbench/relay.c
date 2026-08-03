// relay —— 对照组：**没有任何 IP 栈**的纯 TCP 中继（单线程 epoll + splice 零拷贝）。
//
// 存在的意义：被测的每个用户态栈（lwIP / gVisor netstack / smoltcp）干的事都是
// 「终结一条 TCP + 把字节搬到另一条 TCP」。如果不先量出「只搬字节」要多少 CPU，
// 就分不清某个栈的开销里哪部分是它自己的协议处理、哪部分是所有方案都躲不掉的搬运成本。
// 这个 relay 就是那条地板线：同样单线程、同样一进一出两条 TCP，只是中间没有 IP 栈。
//
// 用法: relay <listen_port> <target_host> <target_port>
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAXCONN 4096

struct half {
    int fd;         // 本端 socket
    int peer;       // 对端 socket
    int pr, pw;     // splice 用的管道读/写端
    int inpipe;     // 管道里还剩多少字节
    int eof;        // 本端已读完
    int dead;
};

static struct half H[MAXCONN * 2 + 64];

static int set_nb(int fd)
{
    int f = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static void close_pair(int ep, int a)
{
    int b = H[a].peer;
    if (H[a].fd >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, H[a].fd, NULL); close(H[a].fd); }
    if (H[a].pr >= 0) { close(H[a].pr); close(H[a].pw); }
    H[a].fd = H[a].pr = H[a].pw = -1;
    H[a].dead = 1;
    if (b >= 0 && !H[b].dead) {
        if (H[b].fd >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, H[b].fd, NULL); close(H[b].fd); }
        if (H[b].pr >= 0) { close(H[b].pr); close(H[b].pw); }
        H[b].fd = H[b].pr = H[b].pw = -1;
        H[b].dead = 1;
    }
}

// 把 a 端读到的字节经管道 splice 给它的对端。返回 0 正常，-1 需要关闭。
static int pump(int ep, int a)
{
    struct half *h = &H[a];
    for (;;) {
        if (h->inpipe == 0 && !h->eof) {
            ssize_t n = splice(h->fd, NULL, h->pw, NULL, 1 << 20,
                               SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
            if (n > 0) {
                h->inpipe += (int)n;
            } else if (n == 0) {
                h->eof = 1;
            } else {
                if (errno == EAGAIN) break;
                return -1;
            }
        }
        while (h->inpipe > 0) {
            ssize_t n = splice(h->pr, NULL, H[h->peer].fd, NULL, h->inpipe,
                               SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
            if (n > 0) {
                h->inpipe -= (int)n;
            } else {
                if (errno == EAGAIN) return 0;   // 对端写满，等它可写
                return -1;
            }
        }
        if (h->eof && h->inpipe == 0) return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: relay <lport> <thost> <tport>\n"); return 2; }
    int lport = atoi(argv[1]), tport = atoi(argv[3]);

    signal(SIGPIPE, SIG_IGN);
    for (size_t i = 0; i < sizeof(H) / sizeof(H[0]); i++) { H[i].fd = H[i].pr = H[i].pw = -1; H[i].dead = 1; }

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(lport);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    listen(ls, 128);
    set_nb(ls);

    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = -1};
    epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);

    struct epoll_event evs[256];
    for (;;) {
        int n = epoll_wait(ep, evs, 256, -1);
        for (int i = 0; i < n; i++) {
            int slot = evs[i].data.fd;
            if (slot < 0) {                       // 监听口
                for (;;) {
                    int c = accept4(ls, NULL, NULL, SOCK_NONBLOCK);
                    if (c < 0) break;
                    int u = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
                    struct sockaddr_in ta = {0};
                    ta.sin_family = AF_INET;
                    ta.sin_port = htons(tport);
                    inet_pton(AF_INET, argv[2], &ta.sin_addr);
                    connect(u, (struct sockaddr *)&ta, sizeof(ta));   // 非阻塞，EINPROGRESS 正常
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    setsockopt(u, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                    int a = -1, b = -1;
                    for (size_t k = 1; k < sizeof(H) / sizeof(H[0]); k++)
                        if (H[k].dead) { if (a < 0) a = (int)k; else { b = (int)k; break; } }
                    if (b < 0) { close(c); close(u); continue; }

                    int pa[2], pb[2];
                    if (pipe2(pa, O_NONBLOCK) < 0 || pipe2(pb, O_NONBLOCK) < 0) { close(c); close(u); continue; }
                    fcntl(pa[0], F_SETPIPE_SZ, 1 << 20);
                    fcntl(pb[0], F_SETPIPE_SZ, 1 << 20);

                    H[a] = (struct half){.fd = c, .peer = b, .pr = pa[0], .pw = pa[1], .inpipe = 0, .eof = 0, .dead = 0};
                    H[b] = (struct half){.fd = u, .peer = a, .pr = pb[0], .pw = pb[1], .inpipe = 0, .eof = 0, .dead = 0};

                    struct epoll_event e1 = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP, .data.fd = a};
                    struct epoll_event e2 = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP, .data.fd = b};
                    epoll_ctl(ep, EPOLL_CTL_ADD, c, &e1);
                    epoll_ctl(ep, EPOLL_CTL_ADD, u, &e2);
                }
                continue;
            }
            if (H[slot].dead) continue;
            int rc = 0;
            if (evs[i].events & (EPOLLIN | EPOLLRDHUP)) rc |= pump(ep, slot);
            if (!H[slot].dead && !H[H[slot].peer].dead && (evs[i].events & EPOLLOUT))
                rc |= pump(ep, H[slot].peer);
            if (rc < 0 || (evs[i].events & (EPOLLERR | EPOLLHUP))) close_pair(ep, slot);
        }
    }
    return 0;
}
