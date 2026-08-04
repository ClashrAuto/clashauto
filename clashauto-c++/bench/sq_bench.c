// 对照组：pcap_sendqueue_transmit —— Npcap 的第三条发送路径（走 NPF 的 BufferedWrite）。
// 生产因「WiFi 上丢帧」弃用了它改走 PacketSendPackets，但**以太网上的 CPU 成本从没量过**。
// 若它明显更便宜，就值得在有线场景下按介质分派（生产已有 adapter.wifi 这个判据）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct pcap pcap_t;
struct pcap_send_queue { unsigned int maxlen; unsigned int len; char *buffer; };
// Windows 上 long=4B，故 timeval=8B，pcap_pkthdr 共 16B（与 NPF dump_bpf_hdr 一致）
struct pcap_pkthdr_w { unsigned int ts_sec, ts_usec, caplen, len; };

typedef pcap_t *(*FnOpenLive)(const char *, int, int, int, char *);
typedef void (*FnClose)(pcap_t *);
typedef struct pcap_send_queue *(*FnSqAlloc)(unsigned int);
typedef void (*FnSqDestroy)(struct pcap_send_queue *);
typedef int (*FnSqQueue)(struct pcap_send_queue *, const struct pcap_pkthdr_w *, const unsigned char *);
typedef unsigned int (*FnSqTransmit)(pcap_t *, struct pcap_send_queue *, int);

static double cpu_s(void)
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULONGLONG kt = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
    ULONGLONG ut = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (double)(kt + ut) / 1e7;
}
static int pmac(const char *s, unsigned char o[6])
{ unsigned int v[6]; if (sscanf(s,"%x:%x:%x:%x:%x:%x",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5])!=6) return 0;
  for(int i=0;i<6;i++) o[i]=(unsigned char)v[i]; return 1; }

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr,"usage: %s <npf> <dst> <src> <batch> <frames> [payload]\n",argv[0]); return 2; }
    unsigned char dst[6], src[6];
    if(!pmac(argv[2],dst)||!pmac(argv[3],src)) return 2;
    const int batch = atoi(argv[4]); const long frames = atol(argv[5]);
    int payload = argc>6?atoi(argv[6]):1500; if(payload<46)payload=46; if(payload>1500)payload=1500;
    const int flen = 14 + payload;

    char sysdir[MAX_PATH]={0}, full[MAX_PATH+32];
    GetSystemDirectoryA(sysdir,MAX_PATH);
    snprintf(full,sizeof(full),"%s\\Npcap\\wpcap.dll",sysdir);
    HMODULE h = LoadLibraryA(full);
    if(!h) h = LoadLibraryA("wpcap.dll");
    if(!h){ fprintf(stderr,"load wpcap failed\n"); return 1; }
    FnOpenLive openLive=(FnOpenLive)GetProcAddress(h,"pcap_open_live");
    FnClose closeP=(FnClose)GetProcAddress(h,"pcap_close");
    FnSqAlloc sqAlloc=(FnSqAlloc)GetProcAddress(h,"pcap_sendqueue_alloc");
    FnSqDestroy sqDestroy=(FnSqDestroy)GetProcAddress(h,"pcap_sendqueue_destroy");
    FnSqQueue sqQueue=(FnSqQueue)GetProcAddress(h,"pcap_sendqueue_queue");
    FnSqTransmit sqTransmit=(FnSqTransmit)GetProcAddress(h,"pcap_sendqueue_transmit");
    if(!openLive||!sqAlloc||!sqQueue||!sqTransmit){ fprintf(stderr,"missing symbols\n"); return 1; }

    char err[256]={0};
    pcap_t *p = openLive(argv[1],128,0,1,err);
    if(!p){ fprintf(stderr,"open_live: %s\n",err); return 1; }

    // 预填一个队列，反复 transmit（transmit 不消耗队列内容）
    const unsigned int qsize = (unsigned int)((sizeof(struct pcap_pkthdr_w)+flen)*batch + 4096);
    struct pcap_send_queue *q = sqAlloc(qsize);
    if(!q){ fprintf(stderr,"sendqueue_alloc failed\n"); return 1; }
    unsigned char *f = (unsigned char*)malloc(flen);
    memcpy(f,dst,6); memcpy(f+6,src,6); f[12]=0x88; f[13]=0xB5; memset(f+14,0x5B,payload);
    struct pcap_pkthdr_w hd = {0,0,(unsigned)flen,(unsigned)flen};
    for(int i=0;i<batch;i++){
        if(sqQueue(q,&hd,f)<0){ fprintf(stderr,"sendqueue_queue failed at %d\n",i); return 1; }
    }

    const long rounds = frames/batch;
    LARGE_INTEGER fq,t0,t1; QueryPerformanceFrequency(&fq);
    double c0=cpu_s(); QueryPerformanceCounter(&t0);
    long long okBytes=0;
    for(long r=0;r<rounds;r++){
        unsigned int n = sqTransmit(p,q,0 /*sync=0，与生产 PacketSendPackets 口径一致*/);
        okBytes += n;
    }
    QueryPerformanceCounter(&t1); double c1=cpu_s();
    double wall=(double)(t1.QuadPart-t0.QuadPart)/(double)fq.QuadPart, cpu=c1-c0;
    long long tot=(long long)rounds*batch; double mb=(double)tot*flen/1e6;
    printf("{\"path\":\"sendqueue\",\"batch\":%d,\"framelen\":%d,\"frames\":%lld,\"okBytes\":%lld,"
           "\"wall_s\":%.3f,\"cpu_s\":%.3f,\"cpu_us_per_frame\":%.2f,\"Mbps\":%.0f,"
           "\"Mfps\":%.3f,\"cores_per_Gbps\":%.3f}\n",
           batch,flen,tot,okBytes,wall,cpu,tot?cpu*1e6/tot:0,mb*8/wall,tot/wall/1e6,
           mb>0?cpu*1000.0/(mb*8.0):0.0);
    if(sqDestroy) sqDestroy(q);
    if(closeP) closeP(p);
    return 0;
}
