// Npcap 发送路径 v2 —— 找优化空间。R19 量到 ~14 µs CPU/帧、~1.2 核/Gbps，且 batch 从 16 到
// 256 完全不再改善，说明剩下的成本是**内核里的每帧工作**（NDIS/miniport），批量摊不掉。
//
// 本程序回答三个决定优化方向的问题：
//   ① **多线程能不能放大？** 每线程一个独立 PacketOpenAdapter 句柄。
//      能线性放大 → 那 14 µs 是每帧 CPU，生产的单条 TxWorker 线程就是人为瓶颈，
//                    多开几条即可把天花板抬到 2.5~3.5 Gbps（届时 lwIP 重新成为瓶颈）。
//      不放大   → 驱动/miniport 内部串行，换栈也好、多线程也好都救不了，只能换注入机制。
//   ② **成本是每帧还是每字节？** 扫帧长 64/512/1514。
//      每帧恒定 → 是每包描述符开销，大包/聚合才有意义。
//      随字节涨 → 是拷贝带宽，另一套优化。
//   ③ **sendqueue 是不是另一条更便宜的内核路径？**（生产因 WiFi 丢帧弃用了它，但以太网上没测过）
//
// 用法: npcap_tx2.exe <npf> <dst-mac> <src-mac> <threads> <batch> <frames-per-thread> <payload>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef void *(*FnOpenAdapter)(const char *);
typedef void (*FnCloseAdapter)(void *);
typedef int (*FnSendPackets)(void *, void *, unsigned long, int);

#pragma pack(push, 1)
struct DumpHdr { unsigned int ts_sec, ts_usec, caplen, len; };
#pragma pack(pop)

static FnOpenAdapter g_open;
static FnCloseAdapter g_close;
static FnSendPackets g_send;

static unsigned char g_dst[6], g_src[6];
static int g_batch, g_framelen;
static long g_frames;

typedef struct {
    int idx;
    long okBatches;
    long long okBytes;
    double cpu;          // 本线程的 CPU 时间（GetThreadTimes）
} ThreadRes;

static double thread_cpu_seconds(HANDLE th)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(th, &c, &e, &k, &u)) return 0;
    ULONGLONG kt = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
    ULONGLONG ut = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (double)(kt + ut) / 1e7;
}

static DWORD WINAPI worker(LPVOID arg)
{
    ThreadRes *r = (ThreadRes *)arg;
    const char *npf = (const char *)NULL;
    (void)npf;
    return 0;
}

// 真正的工作体：每线程自己开句柄、自己造 buffer，互不共享（避免伪共享与锁）
static const char *g_npf;

static DWORD WINAPI worker2(LPVOID arg)
{
    ThreadRes *r = (ThreadRes *)arg;
    void *ad = g_open(g_npf);
    if (!ad) { fprintf(stderr, "thread %d: OpenAdapter failed\n", r->idx); return 1; }

    const size_t slot = sizeof(struct DumpHdr) + g_framelen;
    unsigned char *buf = (unsigned char *)malloc(slot * g_batch);
    for (int i = 0; i < g_batch; i++) {
        unsigned char *p = buf + slot * i;
        struct DumpHdr *hd = (struct DumpHdr *)p;
        hd->ts_sec = 0; hd->ts_usec = 0;
        hd->caplen = g_framelen; hd->len = g_framelen;
        unsigned char *f = p + sizeof(struct DumpHdr);
        memcpy(f, g_dst, 6); memcpy(f + 6, g_src, 6);
        f[12] = 0x88; f[13] = 0xB5;
        memset(f + 14, 0x5B, g_framelen - 14);
        f[14] = (unsigned char)r->idx;   // 线程号，靶机侧可分辨
    }

    const long batches = g_frames / g_batch;
    HANDLE self = GetCurrentThread();
    double c0 = thread_cpu_seconds(self);
    for (long b = 0; b < batches; b++) {
        int n = g_send(ad, buf, (unsigned long)(slot * g_batch), 0);
        if (n > 0) { r->okBatches++; r->okBytes += n; }
    }
    r->cpu = thread_cpu_seconds(self) - c0;

    if (g_close) g_close(ad);
    free(buf);
    return 0;
}

static int parse_mac(const char *s, unsigned char out[6])
{
    unsigned int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 0;
    for (int i = 0; i < 6; i++) out[i] = (unsigned char)v[i];
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: %s <npf> <dst-mac> <src-mac> <threads> <batch> <frames-per-thread> <payload>\n", argv[0]);
        return 2;
    }
    g_npf = argv[1];
    if (!parse_mac(argv[2], g_dst) || !parse_mac(argv[3], g_src)) { fprintf(stderr, "bad mac\n"); return 2; }
    const int threads = atoi(argv[4]);
    g_batch = atoi(argv[5]);
    g_frames = atol(argv[6]);
    int payload = atoi(argv[7]);
    if (payload < 46) payload = 46;
    if (payload > 1500) payload = 1500;
    g_framelen = 14 + payload;

    HMODULE h = GetModuleHandleA("Packet.dll");
    if (!h) {
        char sysdir[MAX_PATH] = {0}, full[MAX_PATH + 32];
        if (GetSystemDirectoryA(sysdir, MAX_PATH)) {
            snprintf(full, sizeof(full), "%s\\Npcap\\Packet.dll", sysdir);
            h = LoadLibraryA(full);
        }
        if (!h) h = LoadLibraryA("Packet.dll");
    }
    if (!h) { fprintf(stderr, "load Packet.dll failed\n"); return 1; }
    g_open = (FnOpenAdapter)GetProcAddress(h, "PacketOpenAdapter");
    g_close = (FnCloseAdapter)GetProcAddress(h, "PacketCloseAdapter");
    g_send = (FnSendPackets)GetProcAddress(h, "PacketSendPackets");
    if (!g_open || !g_send) { fprintf(stderr, "GetProcAddress failed\n"); return 1; }

    ThreadRes *res = (ThreadRes *)calloc(threads, sizeof(ThreadRes));
    HANDLE *hs = (HANDLE *)calloc(threads, sizeof(HANDLE));
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < threads; i++) {
        res[i].idx = i;
        hs[i] = CreateThread(NULL, 0, worker2, &res[i], 0, NULL);
    }
    WaitForMultipleObjects(threads, hs, TRUE, INFINITE);
    QueryPerformanceCounter(&t1);

    double wall = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    long long totFrames = 0; double totCpu = 0;
    for (int i = 0; i < threads; i++) {
        totFrames += (long long)res[i].okBatches * g_batch;
        totCpu += res[i].cpu;
    }
    double mb = (double)totFrames * g_framelen / 1e6;

    printf("{\"threads\":%d,\"batch\":%d,\"framelen\":%d,\"frames\":%lld,"
           "\"wall_s\":%.3f,\"cpu_s\":%.3f,\"cpu_us_per_frame\":%.2f,"
           "\"Mbps\":%.0f,\"Mfps\":%.3f,\"cores_per_Gbps\":%.3f,\"cores_used\":%.2f}\n",
           threads, g_batch, g_framelen, totFrames,
           wall, totCpu, totFrames ? totCpu * 1e6 / totFrames : 0,
           mb * 8 / wall, totFrames / wall / 1e6,
           mb > 0 ? totCpu * 1000.0 / (mb * 8.0) : 0.0,
           totCpu / wall);
    (void)worker;
    return 0;
}
