// UDP echo 靶 + 负载生成器（合一）。测的是被测栈的 **UDP 转发** 能力——12 轮全是 TCP，
// 而真实网关约三成流量是 UDP/QUIC(HTTP3)/DNS/视频。不用 iperf3 是因为 iperf3 -u 仍要一条
// TCP 控制连接同端口，会把 TCP 路径混进 UDP 测量。
//
// server: recvfrom → 原样 sendto 回去（在 127.0.0.1:<port>，被测栈把 UDP 转发到这里）。
// load:   向 <dst>:<port> 以固定大小尽力发，收回显，统计 发/收/丢 与 往返吞吐。
//         每个包带 8 字节序号，用于算真实到达率（丢包率是 UDP 栈的关键指标）。
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

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

static int run_server(int port){
    int s=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(port);
    inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    if(bind(s,(struct sockaddr*)&a,sizeof(a))<0){perror("bind");return 1;}
    char buf[2048]; struct sockaddr_in c; socklen_t cl;
    for(;;){ cl=sizeof(c);
        ssize_t n=recvfrom(s,buf,sizeof(buf),0,(struct sockaddr*)&c,&cl);
        if(n>0) sendto(s,buf,n,0,(struct sockaddr*)&c,cl);
    }
}

// load: <dst> <port> <size> <seconds> [inflight]
static int run_load(const char*dst,int port,int size,double secs,int inflight){
    if(size<8)size=8; if(size>1400)size=1400; if(inflight<1)inflight=64;
    int s=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(port);
    if(inet_pton(AF_INET,dst,&a.sin_addr)!=1){fprintf(stderr,"bad dst\n");return 2;}
    if(connect(s,(struct sockaddr*)&a,sizeof(a))<0){perror("connect");return 1;}
    fcntl(s,F_SETFL,fcntl(s,F_GETFL,0)|O_NONBLOCK);
    int rb=1<<21; setsockopt(s,SOL_SOCKET,SO_RCVBUF,&rb,sizeof(rb)); setsockopt(s,SOL_SOCKET,SO_SNDBUF,&rb,sizeof(rb));

    char buf[2048]; memset(buf,'u',size);
    unsigned long long sent=0,rcv=0,seq=0; long eagain=0;
    double t0=now_s(),tend=t0+secs;
    // 保持 inflight 个在途：每收到一个就补发一个，收不到就先灌满窗口
    for(int i=0;i<inflight;i++){ *(unsigned long long*)buf=seq++; if(send(s,buf,size,0)>0)sent++; else eagain++; }
    while(now_s()<tend){
        ssize_t n; int got=0;
        while((n=recv(s,buf,sizeof(buf),MSG_DONTWAIT))>0){ rcv++; got++; if(got>256)break; }
        int refill=got>0?got:1;
        for(int i=0;i<refill;i++){ *(unsigned long long*)buf=seq++; if(send(s,buf,size,0)>0)sent++; else {eagain++;break;} }
        if(got==0){ struct timespec ts={0,50000}; nanosleep(&ts,NULL); }
    }
    // 排空在途回包
    double drain=now_s()+0.3;
    while(now_s()<drain){ ssize_t n=recv(s,buf,sizeof(buf),MSG_DONTWAIT); if(n>0)rcv++; else {struct timespec ts={0,100000}; nanosleep(&ts,NULL);} }
    double dt=now_s()-t0;
    double loss = sent>0 ? 100.0*(sent-rcv)/(double)sent : 0;
    printf("{\"sent\":%llu,\"recv\":%llu,\"loss_pct\":%.2f,\"sec\":%.2f,\"tx_pps\":%.0f,\"rx_pps\":%.0f,"
           "\"rx_gbps\":%.3f,\"size\":%d,\"eagain\":%ld}\n",
           sent,rcv,loss,dt,sent/dt,rcv/dt,rcv*(double)size*8/dt/1e9,size,eagain);
    return 0;
}

int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage:\n  udpecho server <port>\n  udpecho load <dst> <port> <size> <sec> [inflight]\n");return 2;}
    if(!strcmp(argv[1],"server")) return run_server(atoi(argv[2]));
    if(!strcmp(argv[1],"load")){
        if(argc<6){fprintf(stderr,"load needs <dst> <port> <size> <sec>\n");return 2;}
        return run_load(argv[2],atoi(argv[3]),atoi(argv[4]),atof(argv[5]),argc>6?atoi(argv[6]):64);
    }
    fprintf(stderr,"unknown mode\n"); return 2;
}
