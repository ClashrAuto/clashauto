// DNS 形状的 UDP 洪水发生器。
//
// 存在的理由：lwIP 已知会在「单设备 DNS 洪水 + 并发 TCP」下撞 assert（纯 DNS 或纯 TCP
// 都不崩，必须混合）。换栈的原始动机之一就是这个，所以候选栈必须在同一个配方下被打一遍。
//
// ★ 它自己数发出去的包并在结束时打印实际 pps —— 「跑了洪水」不等于「打到了那个量级」，
//   没有这个计数器，一次因为 EAGAIN 全丢的空跑会被误读成「扛住了」。
//
// 用法: udpflood <dst-ip> <dport> <pps> <seconds> [payload-bytes]
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: udpflood <dst-ip> <dport> <pps> <seconds> [size]\n");
        return 2;
    }
    const char *dst = argv[1];
    int dport = atoi(argv[2]);
    long pps = atol(argv[3]);
    double secs = atof(argv[4]);
    int size = argc > 5 ? atoi(argv[5]) : 40;
    if (size < 12) size = 12;
    if (size > 1400) size = 1400;

    // DNS 查询形状的载荷：前 12 字节是 DNS 头（ID 随机、标准查询、QDCOUNT=1），
    // 后面填成一个 A 记录问题段。栈那边没有 UDP 监听者，所以走的是
    // ip4_input → udp_input → 无匹配 PCB → ICMP 端口不可达 这条最费的路。
    unsigned char buf[1500];
    memset(buf, 0, sizeof(buf));
    buf[2] = 0x01;                    // RD=1
    buf[5] = 0x01;                    // QDCOUNT=1
    // 显式 17 字节（7 'example' 3 'com' 0  + QTYPE=A + QCLASS=IN），避免把字符串字面量
    // 末尾隐含的 '\0' 当成第 18 字节读——那会越界读字面量存储，编译器会警告。
    static const unsigned char q[17] = {
        7,'e','x','a','m','p','l','e', 3,'c','o','m', 0, 0,1, 0,1};
    memcpy(buf + 12, q, sizeof(q));

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(dport);
    if (inet_pton(AF_INET, dst, &a.sin_addr) != 1) { fprintf(stderr, "bad dst\n"); return 2; }

    // 每个包换一个源端口 —— 单设备打出成千上万条伪流，正是把栈的 PCB/池子撑爆的形状
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    int sndbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);

    const double t0 = now_s(), tend = t0 + secs;
    long sent = 0, eagain = 0, err = 0;
    const double per_pkt = 1.0 / (double)pps;

    while (1) {
        double t = now_s();
        if (t >= tend) break;
        // 按目标速率节流：落后多少就补多少，不做忙等自旋
        long due = (long)((t - t0) / per_pkt) - sent - eagain;
        if (due <= 0) { struct timespec ts = {0, 200000}; nanosleep(&ts, NULL); continue; }
        if (due > 4096) due = 4096;
        for (long i = 0; i < due; i++) {
            *(unsigned short *)buf = (unsigned short)(sent + i);  // DNS ID 变化
            ssize_t n = sendto(s, buf, size, 0, (struct sockaddr *)&a, sizeof(a));
            if (n > 0) sent++;
            else if (errno == EAGAIN || errno == ENOBUFS) { eagain++; }
            else { err++; break; }
        }
    }
    double dt = now_s() - t0;
    printf("{\"sent\":%ld,\"dropped_local\":%ld,\"err\":%ld,\"sec\":%.2f,"
           "\"actual_pps\":%.0f,\"target_pps\":%ld,\"bytes\":%ld}\n",
           sent, eagain, err, dt, sent / dt, pps, sent * (long)size);
    return 0;
}
