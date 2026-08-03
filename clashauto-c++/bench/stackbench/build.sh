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

# —— 候选 F：同一份 lwIP，只把窗口从 128 KiB 提到 1 MiB ——
# 存在的意义：gVisor 是自动调窗（可到 MB 级），lwIP 被 MEM_SIZE 钉在 128 KiB，
# 两者吞吐差里混着「窗口大小」这个因子。这个变体把它单独拆出来量。
# lwipopts 由生产那份 **sed 派生**，不是手抄——避免两份配置悄悄走散。
# 改动量的硬约束（init.c 会 #error 的那几条）：
#   TCP_WND(1M) >> TCP_RCV_SCALE(5) = 32768 != 0            ✓
#   TCP_WND(1M) <= 0xFFFF << 5 = 2,097,120                   ✓
#   TCP_SND_QUEUELEN = 4*1M/1460 = 2872 <= MEMP_NUM_TCP_SEG(16384) ✓
#   TCP_WND(1M) <= PBUF_POOL_SIZE(2048) * (1600-54) = 3.1 MB ✓
#   MEM_SIZE 同步 16M→64M：tcp_write 是 COPY 进 PBUF_RAM，发送缓冲直接压这个堆
rm -rf lwip_port_big && cp -r lwip_port lwip_port_big
sed -i -e 's/^#define TCP_WND  *(128 \* 1024).*/#define TCP_WND (1024 * 1024)/' \
       -e 's/^#define TCP_SND_BUF  *(128 \* 1024).*/#define TCP_SND_BUF (1024 * 1024)/' \
       -e 's/^#define TCP_RCV_SCALE  *2\b.*/#define TCP_RCV_SCALE 5/' \
       -e 's/^#define MEM_SIZE  *(16 \* 1024 \* 1024).*/#define MEM_SIZE (64 * 1024 * 1024)/' \
       lwip_port_big/lwipopts.h
grep -E '^#define (TCP_WND|TCP_SND_BUF|TCP_RCV_SCALE|MEM_SIZE) ' lwip_port_big/lwipopts.h
gcc -O2 -w -I"$LW/include" -I"$BENCH_DIR/lwip_port_big" \
    -o bin/lwip2socks_big lwip2socks.c lwip_port_big/coast_lwip_diag.c $LWSRC
echo "[build] lwip2socks_big ok"

if [ -d gvnet ]; then
    # 不要 -mod=mod：它会让 go 重新解析依赖，把 go.mod 里钉住的 gvisor 版本换成另一个
    # 快照（gvisor 的 pkg/tcpip/stack 在某些快照里 package 名冲突，直接编不过）。
    # 版本必须钉死，否则「同一个候选 B」在两轮之间偷偷变了。
    ( cd gvnet && PATH=/usr/local/go/bin:$PATH GOFLAGS=-mod=readonly \
        GOPROXY=https://goproxy.cn,direct GOSUMDB=sum.golang.google.cn \
        go build -o "$BENCH_DIR/bin/gvnet2socks" . ) && echo "[build] gvnet2socks ok"
fi

if [ -d smol ]; then
    ( cd smol && PATH=$HOME/.cargo/bin:$PATH cargo build --release 2>&1 | tail -5 \
        && cp target/release/smoltcp2socks "$BENCH_DIR/bin/" ) && echo "[build] smoltcp2socks ok"
fi

ls -l bin/
