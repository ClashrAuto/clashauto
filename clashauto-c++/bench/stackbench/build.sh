#!/bin/bash
# 编译所有被测栈。lwIP 用的是**仓库里那一份** vendored 源码 + 同一份 lwipopts.h，
# 文件清单与 clashauto-c++/CMakeLists.txt 保持一致，保证量的就是生产那个栈。
set -eu
BENCH_DIR=${BENCH_DIR:-/root/stackbench}
cd "$BENCH_DIR"
mkdir -p bin

gcc -O2 -o bin/relay relay.c
echo "[build] relay ok"

LW=$BENCH_DIR/lwip
LWSRC="
$LW/core/init.c $LW/core/def.c $LW/core/dns.c $LW/core/inet_chksum.c
$LW/core/ip.c $LW/core/mem.c $LW/core/memp.c $LW/core/netif.c
$LW/core/pbuf.c $LW/core/raw.c $LW/core/stats.c $LW/core/sys.c
$LW/core/tcp.c $LW/core/tcp_in.c $LW/core/tcp_out.c $LW/core/timeouts.c
$LW/core/udp.c
$LW/core/ipv4/etharp.c $LW/core/ipv4/icmp.c $LW/core/ipv4/ip4.c
$LW/core/ipv4/ip4_addr.c $LW/core/ipv4/ip4_frag.c $LW/core/ipv4/acd.c
$LW/core/ipv6/ip6.c $LW/core/ipv6/ip6_addr.c $LW/core/ipv6/ip6_frag.c
$LW/core/ipv6/nd6.c $LW/core/ipv6/icmp6.c $LW/core/ipv6/ethip6.c
$LW/core/ipv6/mld6.c $LW/core/ipv6/inet6.c
$LW/netif/ethernet.c
"
gcc -O2 -w -I"$LW/include" -I"$BENCH_DIR/lwip_port" \
    -o bin/lwip2socks lwip2socks.c lwip_port/coast_lwip_diag.c $LWSRC
echo "[build] lwip2socks ok"

if [ -d gvnet ]; then
    ( cd gvnet && PATH=/usr/local/go/bin:$PATH GOFLAGS=-mod=mod \
        GOPROXY=https://goproxy.cn,direct GOSUMDB=sum.golang.google.cn \
        go build -o "$BENCH_DIR/bin/gvnet2socks" . ) && echo "[build] gvnet2socks ok"
fi

ls -l bin/
