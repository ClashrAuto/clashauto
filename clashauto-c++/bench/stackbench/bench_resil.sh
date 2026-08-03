#!/bin/bash
# 崩溃韧性：混合负载（DNS 形状的 UDP 洪水 + 并发 TCP）。
#
# 为什么是混合：lwIP 已知纯 DNS 不崩、纯 TCP 不崩，**两者叠加**才撞 assert。
# 所以这里必须让 iperf3 一直在跑，洪水盖在上面打，而不是分开跑两遍。
#
# 每一档都记三件事，缺一不可：
#   · 洪水**实际**打出去多少 pps（发生器自报计数器——"跑了"不等于"打到了量级"）
#   · 洪水期间 TCP 还剩多少吞吐（活着但吞吐塌成 0 也是一种失败）
#   · 栈进程是否还活着 + 洪水停了以后能否恢复
#
# 用法: bench_resil.sh <stack-name> <可执行文件>
set -u
BENCH_DIR=${BENCH_DIR:-/root/stackbench}
. "$BENCH_DIR/lib.sh"
RATES=${RATES:-"20000 100000 400000"}
FLOOD_SEC=${FLOOD_SEC:-8}
PAR=${PAR:-8}
NAME=$1; shift
BIN=$1; shift

TAP=sb0; STACK_IP=10.99.0.2; TARGET=203.0.113.5
rss_mb() { awk '/^VmRSS:/{printf "%.1f", $2/1024}' "/proc/$1/status" 2>/dev/null || echo 0; }
alive()  { kill -0 "$SPID" 2>/dev/null && echo 1 || echo 0; }

teardown() { kill -- -"$SPID" 2>/dev/null; kill "$SPID" 2>/dev/null; sleep 0.3
             kill -9 -- -"$SPID" 2>/dev/null; "$BENCH_DIR/clean.sh" >/dev/null 2>&1; }

tcp_gbps() {  # $1=秒数 —— 跑一段 TCP，回报吞吐（塌了就是 0）
    local out
    out=$(timeout $(( $1 + 15 )) iperf3 -c "$TARGET" -p 5201 -t "$1" -P "$PAR" -J 2>/dev/null)
    echo "$out" | jq -r '.end.sum_received.bits_per_second // 0' \
        | awk '{printf "%.3f", $1/1e9}'
}

"$BENCH_DIR/clean.sh" || exit 1
STAMP=$(date -Is)

iperf3 -s -B 127.0.0.1 -p 5201 -D --logfile /tmp/iperf_s.log
sleep 0.5
ip tuntap add dev "$TAP" mode tap || exit 1
ip addr add 10.99.0.1/24 dev "$TAP" || exit 1
ip link set "$TAP" up || exit 1
setsid "$BIN" "$TAP" "$STACK_IP" 255.255.255.0 "$@" </dev/null >/tmp/stack.log 2>&1 &
SPID=$!
sleep 1
kill -0 "$SPID" 2>/dev/null || { echo "[FAIL] $NAME 起不来"; cat /tmp/stack.log; teardown; exit 1; }
ip route add 203.0.113.0/24 via "$STACK_IP" dev "$TAP" 2>/dev/null
timeout 5 python3 -c "import socket;socket.create_connection(('$TARGET',5201),timeout=4).close()" 2>/dev/null \
    || { echo "[FAIL] $NAME 连通性自证失败"; cat /tmp/stack.log; teardown; exit 1; }
echo "[ok] $NAME 连通性自证通过"

BASE=$(tcp_gbps 6)
record "{\"ts\":\"$STAMP\",\"case\":\"resil-base\",\"stack\":\"$NAME\",\"gbps\":$BASE,\"rss_mb\":$(rss_mb "$SPID"),\"alive\":$(alive)}"
echo "[base] $NAME 洪水前 TCP = $BASE Gbps"

DIED=0
for R in $RATES; do
    [ "$(alive)" = "0" ] && { DIED=1; break; }
    T0=$(cpu_ticks "$SPID")
    # TCP 先跑起来，洪水盖在上面 —— 混合负载是这个测试的全部意义
    ( tcp_gbps $(( FLOOD_SEC + 3 )) >/tmp/tcp_under.txt ) &
    TJ=$!
    sleep 1
    FL=$("$BENCH_DIR/bin/udpflood" "$TARGET" 53 "$R" "$FLOOD_SEC" 40 2>/dev/null)
    wait $TJ
    UNDER=$(cat /tmp/tcp_under.txt 2>/dev/null); [ -z "$UNDER" ] && UNDER=0
    CS=$(cpu_delta_cores "$SPID" "$T0")
    A=$(alive)
    [ -z "$FL" ] && FL='{"sent":0,"actual_pps":0}'
    record "{\"ts\":\"$STAMP\",\"case\":\"resil\",\"stack\":\"$NAME\",\"target_pps\":$R,\"flood\":$FL,\"tcp_gbps_under_flood\":$UNDER,\"tcp_gbps_base\":$BASE,\"proc_core_sec\":$CS,\"rss_mb\":$(rss_mb "$SPID"),\"alive\":$A}"
    if [ "$A" = "0" ]; then
        echo "[CRASH] $NAME 在 ${R} pps 洪水下死了"
        echo "--- 栈最后的输出 ---"; tail -12 /tmp/stack.log
        DIED=1; break
    fi
    sleep 1
done

# 恢复检查：活着不代表还能用
if [ "$DIED" = "0" ]; then
    sleep 2
    AFTER=$(tcp_gbps 6)
    record "{\"ts\":\"$STAMP\",\"case\":\"resil-after\",\"stack\":\"$NAME\",\"gbps\":$AFTER,\"gbps_base\":$BASE,\"rss_mb\":$(rss_mb "$SPID"),\"alive\":$(alive)}"
    echo "[after] $NAME 洪水后恢复 = $AFTER Gbps（洪水前 $BASE）"
fi

teardown
echo "[done] $NAME 韧性测试结束 died=$DIED"
