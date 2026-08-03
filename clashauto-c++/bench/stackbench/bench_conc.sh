#!/bin/bash
# 并发与内存韧性：单流性能已经分出胜负，这里量的是**并发下还成不成立**。
#
# 三个维度，每个都可能单独把一个候选枪毙掉：
#   · 并发吞吐   iperf3 -P {1,8,64}，看曲线是涨、平、还是塌
#   · 内存       栈进程的 RSS（空载 / 64 并发下）——smoltcp 的缓冲是**预分配**的，
#                 这一项它天然吃亏，必须量出来而不是嘴上说说
#   · 存活       每档跑完检查进程还在不在（lwIP 已知会在混合负载下撞 assert）
#
# 用法: bench_conc.sh <stack-name> <可执行文件>
set -u
BENCH_DIR=${BENCH_DIR:-/root/stackbench}
. "$BENCH_DIR/lib.sh"
DUR=${DUR:-10}
PARS=${PARS:-"1 8 64"}
NAME=$1; shift
BIN=$1; shift

TAP=sb0; STACK_IP=10.99.0.2; TARGET=203.0.113.5
rss_mb() { awk '/^VmRSS:/{printf "%.1f", $2/1024}' "/proc/$1/status" 2>/dev/null || echo 0; }

teardown() { kill -- -"$SPID" 2>/dev/null; kill "$SPID" 2>/dev/null; sleep 0.3
             kill -9 -- -"$SPID" 2>/dev/null; "$BENCH_DIR/clean.sh" >/dev/null 2>&1; }

"$BENCH_DIR/clean.sh" || exit 1
STAMP=$(date -Is)

iperf3 -s -B 127.0.0.1 -p 5201 -D --logfile /tmp/iperf_s.log
setsid python3 "$BENCH_DIR/pingpong.py" server 5202 </dev/null >/tmp/echo.log 2>&1 &
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

RSS_IDLE=$(rss_mb "$SPID")

for P in $PARS; do
    T0=$(cpu_ticks "$SPID")
    OUT=$(iperf3 -c "$TARGET" -p 5201 -t "$DUR" -P "$P" -J 2>/dev/null)
    CS=$(cpu_delta_cores "$SPID" "$T0")
    RSS=$(rss_mb "$SPID")
    ALIVE=$(kill -0 "$SPID" 2>/dev/null && echo 1 || echo 0)
    BPS=$(echo "$OUT" | jq -r '.end.sum_received.bits_per_second // 0')
    RETR=$(echo "$OUT" | jq -r '.end.sum_sent.retransmits // 0')
    GBPS=$(awk -v b="$BPS" 'BEGIN{printf "%.3f", b/1e9}')
    CPG=$(awk -v c="$CS" -v g="$GBPS" -v d="$DUR" 'BEGIN{if(g>0)printf "%.3f",c/(g*d);else print "0"}')
    record "{\"ts\":\"$STAMP\",\"case\":\"conc\",\"stack\":\"$NAME\",\"par\":$P,\"gbps\":$GBPS,\"retr\":$RETR,\"proc_core_sec\":$CS,\"cores_per_gbps\":$CPG,\"rss_mb\":$RSS,\"rss_idle_mb\":$RSS_IDLE,\"alive\":$ALIVE,\"dur\":$DUR}"
    [ "$ALIVE" = "0" ] && { echo "[FAIL] $NAME 在 P=$P 时死了"; tail -5 /tmp/stack.log; break; }
    sleep 1
done

# 连接风暴：600 条极短连接，压 PCB 池 / TIME_WAIT / 监听补位。
# 单流吞吐测不出这一项，而透明网关面对的真实流量恰恰是它。
T0=$(cpu_ticks "$SPID")
CHURN=$(timeout 120 python3 - "$TARGET" <<'PY'
import socket, sys, time
host = sys.argv[1]
ok = fail = 0
t0 = time.time()
for _ in range(600):
    try:
        s = socket.create_connection((host, 5202), timeout=2)
        s.sendall(b"x"); s.recv(16); s.close(); ok += 1
    except Exception:
        fail += 1
print('{"ok":%d,"fail":%d,"sec":%.2f}' % (ok, fail, time.time() - t0))
PY
)
CS=$(cpu_delta_cores "$SPID" "$T0")
ALIVE=$(kill -0 "$SPID" 2>/dev/null && echo 1 || echo 0)
[ -z "$CHURN" ] && CHURN='{"ok":0,"fail":600,"sec":0}'
record "{\"ts\":\"$STAMP\",\"case\":\"churn\",\"stack\":\"$NAME\",\"churn\":$CHURN,\"proc_core_sec\":$CS,\"rss_mb\":$(rss_mb "$SPID"),\"alive\":$ALIVE}"

teardown
echo "[done] $NAME 并发测试结束"
