// Npcap 发送路径 v3 —— 测两个此前没试过的零成本旋钮：
//   ① PacketSetLoopbackBehavior(NPF_DISABLE_LOOPBACK)
//      默认（WinPcap 兼容）会给每个注入帧加 NDIS_SEND_FLAGS_CHECK_FOR_LOOPBACK，
//      并且不豁免发起者自己的抓包实例。关掉能同时省 NDIS 的 loopback 扫描和自回环捕获。
//   ② 并发抓包句柄的影响
//      Npcap 的 NPF_DoTap() 在 pFiltMod->BpfCount == 0 时直接 return；只要同一张网卡上还有
//      任何抓包句柄打开（ARP 网关必然有），每个注入帧都要走 NPF_GetMetadata + 逐实例 bpf_filter。
//      SkipSentPackets 只豁免"发起该帧的那个句柄"。→ 量一下这部分占比。
//
// 用法: npcap_tx3.exe <npf> <dst> <src> <batch> <frames> <payload> <loopback:0=默认,1=disable> <cap:0/1>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef void *(*FnOpenAdapter)(const char *);
typedef void (*FnCloseAdapter)(void *);
typedef int (*FnSendPackets)(void *, void *, unsigned long, int);
typedef int (*FnSetLoopback)(void *, unsigned int);   // PacketSetLoopbackBehavior

typedef struct pcap pcap_t;
typedef pcap_t *(*FnOpenLive)(const char *, int, int, int, char *);
typedef void (*FnPClose)(pcap_t *);

#pragma pack(push, 1)
struct DumpHdr { unsigned int ts_sec, ts_usec, caplen, len; };
#pragma pack(pop)

#define NPF_DISABLE_LOOPBACK 1
#define NPF_ENABLE_LOOPBACK  2

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
    if (argc < 9) { fprintf(stderr,"usage: %s <npf> <dst> <src> <batch> <frames> <payload> <loopbackDisable:0/1> <concurrentCapture:0/1>\n",argv[0]); return 2; }
    unsigned char dst[6], src[6];
    if(!pmac(argv[2],dst)||!pmac(argv[3],src)) return 2;
    const int batch=atoi(argv[4]); const long frames=atol(argv[5]);
    int payload=atoi(argv[6]); if(payload<46)payload=46; if(payload>1500)payload=1500;
    const int lbDisable=atoi(argv[7]); const int wantCap=atoi(argv[8]);
    const int flen=14+payload;

    char sysdir[MAX_PATH]={0}, full[MAX_PATH+32];
    GetSystemDirectoryA(sysdir,MAX_PATH);
    HMODULE hp = GetModuleHandleA("Packet.dll");
    if(!hp){ snprintf(full,sizeof(full),"%s\\Npcap\\Packet.dll",sysdir); hp=LoadLibraryA(full); }
    if(!hp){ fprintf(stderr,"load Packet.dll failed\n"); return 1; }
    FnOpenAdapter openAd=(FnOpenAdapter)GetProcAddress(hp,"PacketOpenAdapter");
    FnCloseAdapter closeAd=(FnCloseAdapter)GetProcAddress(hp,"PacketCloseAdapter");
    FnSendPackets sendPk=(FnSendPackets)GetProcAddress(hp,"PacketSendPackets");
    FnSetLoopback setLb=(FnSetLoopback)GetProcAddress(hp,"PacketSetLoopbackBehavior");
    if(!openAd||!sendPk){ fprintf(stderr,"GetProcAddress failed\n"); return 1; }

    // 可选：并发抓包句柄（模拟 ARP 网关的真实状态——收帧句柄始终开着）
    pcap_t *cap = NULL; HMODULE hw = NULL;
    if (wantCap) {
        snprintf(full,sizeof(full),"%s\\Npcap\\wpcap.dll",sysdir);
        hw = LoadLibraryA(full); if(!hw) hw = LoadLibraryA("wpcap.dll");
        if (hw) {
            FnOpenLive ol=(FnOpenLive)GetProcAddress(hw,"pcap_open_live");
            char err[256]={0};
            if (ol) cap = ol(argv[1], 65535, 1 /*混杂*/, 1, err);
            if (!cap) fprintf(stderr,"warn: 抓包句柄没开成: %s\n", err);
        }
    }

    void *ad = openAd(argv[1]);
    if(!ad){ fprintf(stderr,"OpenAdapter failed\n"); return 1; }
    int lbOk = -1;
    if (lbDisable) {
        if (setLb) lbOk = setLb(ad, NPF_DISABLE_LOOPBACK);
        else fprintf(stderr,"warn: PacketSetLoopbackBehavior 符号不存在\n");
    }

    const size_t slot=sizeof(struct DumpHdr)+flen;
    unsigned char *buf=(unsigned char*)malloc(slot*batch);
    for(int i=0;i<batch;i++){
        unsigned char *p=buf+slot*i; struct DumpHdr *hd=(struct DumpHdr*)p;
        hd->ts_sec=0; hd->ts_usec=0; hd->caplen=flen; hd->len=flen;
        unsigned char *f=p+sizeof(struct DumpHdr);
        memcpy(f,dst,6); memcpy(f+6,src,6); f[12]=0x88; f[13]=0xB5;
        memset(f+14,0x5B,payload);
    }

    const long rounds=frames/batch;
    LARGE_INTEGER fq,t0,t1; QueryPerformanceFrequency(&fq);
    double c0=cpu_s(); QueryPerformanceCounter(&t0);
    long ok=0;
    for(long r=0;r<rounds;r++){ if(sendPk(ad,buf,(unsigned long)(slot*batch),0)>0) ok++; }
    QueryPerformanceCounter(&t1); double c1=cpu_s();

    double wall=(double)(t1.QuadPart-t0.QuadPart)/(double)fq.QuadPart, cpu=c1-c0;
    long long tot=(long long)ok*batch; double mb=(double)tot*flen/1e6;
    printf("{\"loopbackDisable\":%d,\"setLbRet\":%d,\"concurrentCapture\":%d,\"capOpened\":%d,"
           "\"batch\":%d,\"framelen\":%d,\"frames\":%lld,\"wall_s\":%.3f,\"cpu_s\":%.3f,"
           "\"cpu_us_per_frame\":%.2f,\"Mbps\":%.0f,\"Mfps\":%.3f,\"cores_per_Gbps\":%.3f}\n",
           lbDisable,lbOk,wantCap,cap?1:0,batch,flen,tot,wall,cpu,
           tot?cpu*1e6/tot:0, mb*8/wall, tot/wall/1e6, mb>0?cpu*1000.0/(mb*8.0):0.0);

    if(closeAd) closeAd(ad);
    return 0;
}
