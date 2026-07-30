// 压测客户端：单线程 epoll，维持 N 条并发「connect → GET → 读到 EOF → 关闭」。
//
// 为什么不用 python：上一轮的负载端是多线程 python，GIL + 线程调度本身就是变量，
// 而我们要量的恰恰是**建连延迟的毫秒级分布**。这份是纯 C、非阻塞、零线程。
//
// 用法: ./load <host> <port> <concurrency> <seconds>
// 输出: ok/fail 计数 + 分原因失败 + 建连耗时直方图（与 coast 的 connMs 分桶对齐）
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXC 4096
#define DEADLINE_MS 5000

enum { ST_CONN = 0, ST_REQ, ST_RESP };

typedef struct {
    int fd;
    int st;
    double t0;    // 发起 connect 的时刻
    double tconn; // 连上的时刻
    int got;      // 已读字节
} Conn;

static Conn g_c[MAXC];
static int g_nc;
static struct sockaddr_in g_dst;
static int g_ep;
static char g_req[256];
static int g_reqlen;

static long ok = 0, fail_conn = 0, fail_read = 0, fail_timeout = 0, fail_sock = 0;
// 分桶：<1ms / <10ms / <25ms / <100ms / >=100ms —— 与 GatewayDiag 的 connMs 对齐
static long hconn[5], hreq[5];

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void bucket(long *h, double ms)
{
    if (ms < 1) h[0]++;
    else if (ms < 10) h[1]++;
    else if (ms < 25) h[2]++;
    else if (ms < 100) h[3]++;
    else h[4]++;
}

static void closec(int i)
{
    if (g_c[i].fd >= 0) {
        epoll_ctl(g_ep, EPOLL_CTL_DEL, g_c[i].fd, NULL);
        close(g_c[i].fd);
        g_c[i].fd = -1;
    }
}

// 起一条新连接占住槽位 i
static void startc(int i)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { fail_sock++; g_c[i].fd = -1; return; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    g_c[i].fd = fd;
    g_c[i].st = ST_CONN;
    g_c[i].got = 0;
    g_c[i].t0 = now_ms();
    int r = connect(fd, (struct sockaddr *)&g_dst, sizeof(g_dst));
    if (r != 0 && errno != EINPROGRESS) { fail_conn++; closec(i); return; }
    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data.u32 = (unsigned)i;
    epoll_ctl(g_ep, EPOLL_CTL_ADD, fd, &ev);
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s <host> <port> <conc> <secs>\n", argv[0]); return 2; }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    g_nc = atoi(argv[3]);
    double secs = atof(argv[4]);
    if (g_nc > MAXC) g_nc = MAXC;

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl); // 客户端自己也别顶死在 1024
    }
    signal(SIGPIPE, SIG_IGN);

    memset(&g_dst, 0, sizeof(g_dst));
    g_dst.sin_family = AF_INET;
    g_dst.sin_port = htons(port);
    inet_pton(AF_INET, host, &g_dst.sin_addr);
    g_reqlen = snprintf(g_req, sizeof(g_req),
                        "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);

    g_ep = epoll_create1(0);
    for (int i = 0; i < g_nc; i++) g_c[i].fd = -1;

    double start = now_ms(), deadline = start + secs * 1000.0;
    for (int i = 0; i < g_nc; i++) startc(i);

    struct epoll_event evs[MAXC];
    char buf[8192];
    double last_scan = start;
    for (;;) {
        double t = now_ms();
        if (t >= deadline) break;
        int n = epoll_wait(g_ep, evs, MAXC, 20);
        t = now_ms();
        for (int k = 0; k < n; k++) {
            int i = (int)evs[k].data.u32;
            Conn *c = &g_c[i];
            if (c->fd < 0) continue;
            if (c->st == ST_CONN) {
                int err = 0;
                socklen_t el = sizeof(err);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err != 0) { fail_conn++; closec(i); continue; }
                c->tconn = t;
                bucket(hconn, t - c->t0);
                ssize_t w = write(c->fd, g_req, g_reqlen);
                if (w != g_reqlen) { fail_read++; closec(i); continue; }
                c->st = ST_RESP;
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.u32 = (unsigned)i;
                epoll_ctl(g_ep, EPOLL_CTL_MOD, c->fd, &ev);
                continue;
            }
            // ST_RESP：读到 EOF 算一次成功
            for (;;) {
                ssize_t r = read(c->fd, buf, sizeof(buf));
                if (r > 0) { c->got += r; continue; }
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (c->got > 0) { ok++; bucket(hreq, t - c->t0); }
                else fail_read++;
                closec(i);
                break;
            }
        }
        // 超时扫描 + 补槽（每 50ms 一次，成本可忽略）
        if (t - last_scan >= 50.0) {
            last_scan = t;
            for (int i = 0; i < g_nc; i++)
                if (g_c[i].fd >= 0 && t - g_c[i].t0 > DEADLINE_MS) { fail_timeout++; closec(i); }
        }
        for (int i = 0; i < g_nc; i++)
            if (g_c[i].fd < 0 && now_ms() < deadline) startc(i);
    }
    for (int i = 0; i < g_nc; i++) closec(i);

    double el = (now_ms() - start) / 1000.0;
    long tot = ok + fail_conn + fail_read + fail_timeout + fail_sock;
    printf("elapsed=%.2fs ok=%ld fail=%ld (conn=%ld read=%ld timeout=%ld sock=%ld) rate=%.0f/s failpct=%.1f%%\n",
           el, ok, tot - ok, fail_conn, fail_read, fail_timeout, fail_sock,
           ok / el, tot ? 100.0 * (tot - ok) / tot : 0.0);
    printf("connMs=%ld/%ld/%ld/%ld/%ld  reqMs=%ld/%ld/%ld/%ld/%ld   (<1/<10/<25/<100/>=100)\n",
           hconn[0], hconn[1], hconn[2], hconn[3], hconn[4],
           hreq[0], hreq[1], hreq[2], hreq[3], hreq[4]);
    return 0;
}
