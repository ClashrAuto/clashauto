// 极简 HTTP 靶服务器：SO_REUSEPORT × N 线程 + epoll，只为「绝不成为瓶颈」而写。
//
// 为什么不用 python 的 ThreadingHTTPServer：上一轮压测里它自己就是个变量（GIL、每连接一线程、
// backlog 默认 5），"网关比直连慢 N 倍"里有多少是它的锅根本分不清。这份是纯 C、非阻塞、
// backlog 4096、每核一个独立 accept 队列，Pi5 上能轻松跑到万级 conn/s。
//
// 用法: ./httpsrv <port> [threads]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

static const char kResp[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
static const int kRespLen = sizeof(kResp) - 1;

static int g_port = 8000;
static volatile long g_served = 0;

static void *worker(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ls < 0) { perror("socket"); return NULL; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(ls, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)); // 每线程独立 accept 队列
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(g_port);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0) { perror("bind"); return NULL; }
    if (listen(ls, 4096) != 0) { perror("listen"); return NULL; }

    int ep = epoll_create1(0);
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = ls;
    epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);

    struct epoll_event evs[256];
    char buf[8192];
    for (;;) {
        int n = epoll_wait(ep, evs, 256, -1);
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == ls) {
                for (;;) {
                    int c = accept4(ls, NULL, NULL, SOCK_NONBLOCK);
                    if (c < 0) break;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    struct epoll_event ce;
                    ce.events = EPOLLIN;
                    ce.data.fd = c;
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, c, &ce) != 0) close(c);
                }
                continue;
            }
            // 读到请求就应答并关闭（Connection: close，本测试量的是**新建连接**速率）
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r > 0) {
                ssize_t off = 0;
                while (off < kRespLen) {
                    ssize_t w = write(fd, kResp + off, kRespLen - off);
                    if (w <= 0) break;
                    off += w;
                }
                __sync_fetch_and_add(&g_served, 1);
            }
            epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1) g_port = atoi(argv[1]);
    int nthreads = (argc > 2) ? atoi(argv[2]) : 4;

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "httpsrv on :%d, %d threads, fd=%llu\n", g_port, nthreads,
            (unsigned long long)rl.rlim_cur);
    fflush(stderr);
    for (int i = 1; i < nthreads; i++) {
        pthread_t t;
        pthread_create(&t, NULL, worker, NULL);
    }
    worker(NULL);
    return 0;
}
