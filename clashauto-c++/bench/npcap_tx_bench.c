// Npcap 发送路径微基准 —— 回答 Phase 0 的唯一未知数：
// 「Windows 上把帧交给 Npcap，每帧要烧多少 **CPU**？」
//
// 为什么必须单独量、且必须分开 CPU 与墙钟：
//   L2Endpoint_win.cpp 记的 "PacketSendPackets(256) 12.3 µs/帧 115.9 MB/s" 是**墙钟**。
//   115.9 MB/s = 927 Mb/s ≈ 千兆网卡线速，所以那 12.3 µs 很可能大部分是「等空口/等线速」，
//   而不是 CPU。若真是等线速，则 Npcap 几乎不吃 CPU → 换掉 lwIP 的收益能拿满；
//   若 12.3 µs 真是 CPU，则 Npcap 自己就要 ~1 核/Gbps，与 lwIP 同量级 → 换栈收益被它吃掉一半。
//   **这两种情况给出完全相反的工程结论，所以必须量。**
//
// 手法：GetProcessTimes 取用户态+内核态 CPU 时间，QueryPerformanceCounter 取墙钟，同一批
// 帧分别报 µs-CPU/帧 与 µs-墙钟/帧。批量大小可变（1 = 逐帧 pcap_sendpacket 路径的等价物）。
//
// 发的是什么：目的 MAC = 指定靶机（已知单播，交换机只转发到那一个口，不会泛洪全网段），
// ethertype 0x88B5（IEEE 实验用，靶机内核收到直接丢弃）。靶机侧可用 tcpdump 计数验证真上线。
//
// 编译: gcc -O2 -o npcap_tx_bench.exe npcap_tx_bench.c
// 用法: npcap_tx_bench.exe <NPF名或\Device\NPF_{GUID}> <dst-mac> <src-mac> <batch> <frames> [payload]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// Packet.dll 的三个符号（手动声明，避免 Packet32.h 与 pcap.h 的 bpf_program 冲突）
typedef void *(*FnOpenAdapter)(const char *);
typedef void (*FnCloseAdapter)(void *);
typedef int (*FnSendPackets)(void *, void *, unsigned long, int);

// NPF BufferedWrite 的逐条头：sizeof=16, caplen@8, len@12（MinGW 实测，与 MSVC 版布局相同）
#pragma pack(push, 1)
struct DumpHdr {
    unsigned int ts_sec;
    unsigned int ts_usec;
    unsigned int caplen;
    unsigned int len;
};
#pragma pack(pop)

static int parse_mac(const char *s, unsigned char out[6])
{
    unsigned int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return 0;
    for (int i = 0; i < 6; i++) out[i] = (unsigned char)v[i];
    return 1;
}

static double cpu_seconds(void)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    ULONGLONG kt = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
    ULONGLONG ut = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (double)(kt + ut) / 1e7; // 100ns 单位
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "usage: %s <npf-name> <dst-mac> <src-mac> <batch> <frames> [payload=1500]\n",
                argv[0]);
        return 2;
    }
    const char *npf = argv[1];
    unsigned char dst[6], src[6];
    if (!parse_mac(argv[2], dst) || !parse_mac(argv[3], src)) {
        fprintf(stderr, "bad mac\n"); return 2;
    }
    const int batch = atoi(argv[4]);
    const long frames = atol(argv[5]);
    int payload = argc > 6 ? atoi(argv[6]) : 1500;
    const int syncFlag = argc > 7 ? atoi(argv[7]) : 0;   // 生产 L2Endpoint_win.cpp:279 用 0
    if (payload < 46) payload = 46;
    if (payload > 1500) payload = 1500;
    const int framelen = 14 + payload;

    // —— 加载 Packet.dll（不在默认搜索路径，必须走 System32\Npcap\）——
    HMODULE h = GetModuleHandleA("Packet.dll");
    if (!h) {
        char sysdir[MAX_PATH] = {0};
        char full[MAX_PATH + 32];
        if (GetSystemDirectoryA(sysdir, MAX_PATH)) {
            snprintf(full, sizeof(full), "%s\\Npcap\\Packet.dll", sysdir);
            h = LoadLibraryA(full);
        }
        if (!h) h = LoadLibraryA("Packet.dll");
    }
    if (!h) { fprintf(stderr, "load Packet.dll failed (%lu)\n", GetLastError()); return 1; }
    FnOpenAdapter openAdapter = (FnOpenAdapter)GetProcAddress(h, "PacketOpenAdapter");
    FnCloseAdapter closeAdapter = (FnCloseAdapter)GetProcAddress(h, "PacketCloseAdapter");
    FnSendPackets sendPackets = (FnSendPackets)GetProcAddress(h, "PacketSendPackets");
    if (!openAdapter || !sendPackets) { fprintf(stderr, "GetProcAddress failed\n"); return 1; }

    void *ad = openAdapter(npf);
    if (!ad) { fprintf(stderr, "PacketOpenAdapter(%s) failed (%lu)\n", npf, GetLastError()); return 1; }

    // —— 造一批帧：连续的 (DumpHdr + 帧)，无对齐填充 ——
    const size_t slot = sizeof(struct DumpHdr) + framelen;
    unsigned char *buf = (unsigned char *)malloc(slot * batch);
    if (!buf) { fprintf(stderr, "oom\n"); return 1; }
    for (int i = 0; i < batch; i++) {
        unsigned char *p = buf + slot * i;
        struct DumpHdr *hd = (struct DumpHdr *)p;
        hd->ts_sec = 0; hd->ts_usec = 0;
        hd->caplen = framelen; hd->len = framelen;
        unsigned char *f = p + sizeof(struct DumpHdr);
        memcpy(f, dst, 6);
        memcpy(f + 6, src, 6);
        f[12] = 0x88; f[13] = 0xB5;              // IEEE 实验用 ethertype，靶机内核丢弃
        memset(f + 14, 0x5B, payload);
        // 前 4 字节放序号，便于靶机侧核对（每批第一帧带批号）
    }

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    const long batches = frames / batch;
    const long sent_frames = batches * batch;

    double c0 = cpu_seconds();
    QueryPerformanceCounter(&t0);
    long okBatches = 0;
    long long okBytes = 0;
    for (long b = 0; b < batches; b++) {
        // 每批第一帧写批号（同时防止编译器把 buf 当常量优化掉）
        *(unsigned int *)(buf + sizeof(struct DumpHdr) + 14) = (unsigned int)b;
        int r = sendPackets(ad, buf, (unsigned long)(slot * batch), syncFlag);
        if (r > 0) { okBatches++; okBytes += r; }
    }
    QueryPerformanceCounter(&t1);
    double c1 = cpu_seconds();

    const double wall = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    const double cpu = c1 - c0;
    const double mb = (double)sent_frames * framelen / 1e6;

    printf("{\"sync\":%d,\"batch\":%d,\"frames\":%ld,\"framelen\":%d,\"okBatches\":%ld,\"okBytes\":%lld,"
           "\"wall_s\":%.3f,\"cpu_s\":%.3f,"
           "\"wall_us_per_frame\":%.2f,\"cpu_us_per_frame\":%.2f,"
           "\"MBps\":%.1f,\"Mbps\":%.0f,\"cpu_cores_per_Gbps\":%.3f}\n",
           syncFlag, batch, sent_frames, framelen, okBatches, okBytes,
           wall, cpu,
           wall * 1e6 / sent_frames, cpu * 1e6 / sent_frames,
           mb / wall, mb * 8 / wall,
           // 核/Gbps = (cpu/wall 即占用核数) / (Gbps) = cpu*1000/(MB*8)
           mb > 0 ? cpu * 1000.0 / (mb * 8.0) : 0.0);

    if (closeAdapter) closeAdapter(ad);
    free(buf);
    return 0;
}
