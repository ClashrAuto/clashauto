// 多流 UDP 生成器：模拟「很多设备各自跑 QUIC」——大量**不同源端口**的 UDP 流。
// 专打 R14 那个 NAT 表：natdev.rs 的 flow map 每见一个新四元组就建一个内核 socket，
// 而且（当前实现）**从不淘汰**。这正是 R10 那个 TCP fd 泄漏的 UDP 翻版，必须验证。
//
// 每个「流」= 一个独立源端口的 socket，发几个包就换下一个，持续 T 秒，累计制造 N 个不同流。
// 用法: udpmany <dst> <port> <flows> <seconds>
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

int main(int argc,char**argv){
    if(argc<5){ fprintf(stderr,"usage: udpmany <dst> <port> <flows> <seconds>\n"); return 2; }
    const char*dst=argv[1]; int port=atoi(argv[2]); long flows=atol(argv[3]); double secs=atof(argv[4]);
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(port);
    if(inet_pton(AF_INET,dst,&a.sin_addr)!=1){ fprintf(stderr,"bad dst\n"); return 2; }
    char buf[64]="quic-ish"; unsigned long long made=0,sent=0,rcv=0; double t0=now_s(),tend=t0+secs;

    // 同时保持一小批活跃流（每批 200 个不同源端口），发完就关、再开新的，累计造出很多流。
    #define BATCH 200
    while(now_s()<tend && made<flows){
        int socks[BATCH]; int nb=0;
        for(int i=0;i<BATCH && made<flows;i++){
            int s=socket(AF_INET,SOCK_DGRAM,0);
            if(s<0)break;
            // 不 bind → 内核自动分配一个新源端口，天然是一条新流
            if(connect(s,(struct sockaddr*)&a,sizeof(a))<0){ close(s); continue; }
            fcntl(s,F_SETFL,fcntl(s,F_GETFL,0)|O_NONBLOCK);
            socks[nb++]=s; made++;
            // 每流发 3 个包
            for(int k=0;k<3;k++){ if(send(s,buf,sizeof(buf),0)>0)sent++; }
        }
        // 收一会儿回包
        double until=now_s()+0.02;
        while(now_s()<until){
            for(int i=0;i<nb;i++){ char r[64]; if(recv(socks[i],r,sizeof(r),MSG_DONTWAIT)>0)rcv++; }
        }
        for(int i=0;i<nb;i++) close(socks[i]);   // 客户端关掉；服务端/网关侧是否也回收才是重点
    }
    double dt=now_s()-t0;
    printf("{\"flows_made\":%llu,\"sent\":%llu,\"recv\":%llu,\"sec\":%.2f,\"flows_per_s\":%.0f}\n",
           made,sent,rcv,dt,made/dt);
    return 0;
}
