#!/bin/bash
# 通用被测栈跑分：吞吐(3 轮取中位) + 延迟(串行乒乓) + CPU(进程 utime+stime)
#
# 拓扑（各栈完全相同）：
#   iperf3/pingpong ──> sb0(TAP, 10.99.0.1/24) ──帧──> 被测栈(10.99.0.2, catch-all)
#                        路由 203.0.113.0/24 via 10.99.0.2
#                                                    └─> connect 127.0.0.1:<原目的端口>
#                                                              └─> iperf3 -s :5201 / echo :5202
# 目的地 203.0.113.5 既不是本机地址也不是 sb0 网段 → 必须真穿被测栈，不会被内核抄近路。
#
# 用法: bench_stack.sh <stack-name> <可执行文件> [额外参数...]
#   被测程序统一按 `<bin> sb0 10.99.0.2 255.255.255.0` 调用。
set -u
BENCH_DIR=${BENCH_DIR:-/root/stackbench}
. "$BENCH_DIR/lib.sh"
DUR=${DUR:-10}
ROUNDS=${ROUNDS:-3}
PP_N=${PP_N:-500}
NAME=$1; shift
BIN=$1; shift

TAP=sb0
STACK_IP=10.99.0.2
TARGET=203.0.113.5

teardown() {
    # setsid 起的栈自成一个进程组，杀组不杀单进程——Go 那种多线程/多子进程的栈
    # 只 kill 主 pid 会留下孤儿。留下来的孤儿会空转满一个核，污染**后面每一轮**。
    kill -- -"$SPID" 2>/dev/null
    kill "$SPID" 2>/dev/null
    sleep 0.3
    kill -9 -- -"$SPID" 2>/dev/null
    "$BENCH_DIR/clean.sh" >/dev/null 2>&1
}

"$BENCH_DIR/clean.sh" || exit 1
STAMP=$(date -Is)

# —— 起靶：iperf3(吞吐) + echo(延迟) ——
# 后台进程一律 setsid + </dev/null + 重定向：否则它们攥着 ssh 会话的 stdout/stderr，
# 远程命令跑完也不返回（排查这个坑花了两轮）。
iperf3 -s -B 127.0.0.1 -p 5201 -D --logfile /tmp/iperf_s.log
setsid python3 "$BENCH_DIR/pingpong.py" server 5202 </dev/null >/tmp/echo.log 2>&1 &
sleep 0.5

# —— 建 TAP ——
ip tuntap add dev "$TAP" mode tap || exit 1
ip addr add 10.99.0.1/24 dev "$TAP" || exit 1
ip link set "$TAP" up || exit 1

# —— 起被测栈 ——
setsid "$BIN" "$TAP" "$STACK_IP" 255.255.255.0 "$@" </dev/null >/tmp/stack.log 2>&1 &
SPID=$!
sleep 1
if ! kill -0 "$SPID" 2>/dev/null; then
    echo "[FAIL] $NAME 起不来："; cat /tmp/stack.log; teardown; exit 1
fi
ip route add 203.0.113.0/24 via "$STACK_IP" dev "$TAP" 2>/dev/null

# —— 连通性自证：栈没真接管就别开量（本仓库栽过——只看日志 armed 不算数）——
if ! timeout 5 python3 -c "import socket,sys;s=socket.create_connection(('$TARGET',5201),timeout=4);s.close()" 2>/dev/null; then
    echo "[FAIL] $NAME 连通性自证失败（$TARGET:5201 连不上），不开量"
    echo "--- stack.log ---"; cat /tmp/stack.log
    echo "--- neigh ---"; ip neigh show dev "$TAP"
    teardown; exit 1
fi
echo "[ok] $NAME 连通性自证通过（$TARGET:5201 可连）"

# —— 吞吐 × ROUNDS ——（单次不可信，本仓库已经在这上面栽过两次）
GLIST=""
for r in $(seq 1 "$ROUNDS"); do
    T0=$(cpu_ticks "$SPID")
    MB=$(machine_busy_start)
    OUT=$(iperf3 -c "$TARGET" -p 5201 -t "$DUR" -J 2>/dev/null)
    BUSY=$(machine_busy_cores "$MB")
    CS=$(cpu_delta_cores "$SPID" "$T0")
    BPS=$(echo "$OUT" | jq -r '.end.sum_received.bits_per_second // 0')
    RETR=$(echo "$OUT" | jq -r '.end.sum_sent.retransmits // 0')
    GBPS=$(awk -v b="$BPS" 'BEGIN{printf "%.3f", b/1e9}')
    CPG=$(awk -v c="$CS" -v g="$GBPS" -v d="$DUR" 'BEGIN{ if(g>0) printf "%.3f", c/(g*d); else print "0"}')
    GLIST="$GLIST $GBPS"
    record "{\"ts\":\"$STAMP\",\"case\":\"tput\",\"stack\":\"$NAME\",\"round\":$r,\"gbps\":$GBPS,\"retr\":$RETR,\"proc_core_sec\":$CS,\"machine_core_sec\":$BUSY,\"cores_per_gbps\":$CPG,\"dur\":$DUR}"
    sleep 1
done
MED=$(echo $GLIST | tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')

# —— 延迟 ——
T0=$(cpu_ticks "$SPID")
PP=$(timeout 60 python3 "$BENCH_DIR/pingpong.py" "$TARGET" 5202 "$PP_N" 2>/dev/null)
CS=$(cpu_delta_cores "$SPID" "$T0")
[ -z "$PP" ] && PP='{"error":"pingpong failed"}'
record "{\"ts\":\"$STAMP\",\"case\":\"lat\",\"stack\":\"$NAME\",\"pingpong\":$PP,\"proc_core_sec\":$CS,\"n\":$PP_N}"

record "{\"ts\":\"$STAMP\",\"case\":\"summary\",\"stack\":\"$NAME\",\"gbps_rounds\":[$(echo $GLIST | tr ' ' ',')],\"gbps_median\":$MED}"

teardown
echo "[done] $NAME median=$MED Gbps"
