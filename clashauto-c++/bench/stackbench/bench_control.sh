#!/bin/bash
# 对照组测量：
#   ctl-loopback —— iperf3 直打回环，量这台机器的天花板（不含任何中继）
#   ctl-relay    —— 经无 IP 栈的单线程 splice 中继，量「只搬字节」的地板成本
# 两个数一起给出「被测栈的开销 = 实测 - ctl-relay」的分母与基准。
set -u
BENCH_DIR=${BENCH_DIR:-/root/stackbench}
. "$BENCH_DIR/lib.sh"
DUR=${DUR:-10}
STAMP=$(date -Is)
GIT_NOTE=${GIT_NOTE:-}

"$BENCH_DIR/clean.sh" || exit 1

# ── ctl-loopback ────────────────────────────────────────────────────────────
iperf3 -s -B 127.0.0.1 -p 5201 -D --logfile /tmp/iperf_s.log
sleep 0.5
MB=$(machine_busy_start)
OUT=$(iperf3 -c 127.0.0.1 -p 5201 -t "$DUR" -J 2>/dev/null)
BUSY=$(machine_busy_cores "$MB")
BPS=$(echo "$OUT" | jq -r '.end.sum_received.bits_per_second // 0')
RETR=$(echo "$OUT" | jq -r '.end.sum_sent.retransmits // 0')
GBPS=$(awk -v b="$BPS" 'BEGIN{printf "%.3f", b/1e9}')
CPG=$(awk -v c="$BUSY" -v g="$GBPS" -v d="$DUR" 'BEGIN{ if(g>0) printf "%.3f", c/(g*d); else print "0"}')
record "{\"ts\":\"$STAMP\",\"case\":\"ctl-loopback\",\"stack\":\"none\",\"gbps\":$GBPS,\"retr\":$RETR,\"machine_core_sec\":$BUSY,\"cores_per_gbps\":$CPG,\"dur\":$DUR,\"note\":\"iperf3 直打回环，机器天花板\"}"
pkill -x iperf3; sleep 0.5

# ── ctl-relay ───────────────────────────────────────────────────────────────
"$BENCH_DIR/clean.sh" || exit 1
iperf3 -s -B 127.0.0.1 -p 5201 -D --logfile /tmp/iperf_s.log
sleep 0.5
"$BENCH_DIR/bin/relay" 7777 127.0.0.1 5201 &
RPID=$!
sleep 0.5
T0=$(cpu_ticks "$RPID")
MB=$(machine_busy_start)
OUT=$(iperf3 -c 127.0.0.1 -p 7777 -t "$DUR" -J 2>/dev/null)
BUSY=$(machine_busy_cores "$MB")
CS=$(cpu_delta_cores "$RPID" "$T0")
BPS=$(echo "$OUT" | jq -r '.end.sum_received.bits_per_second // 0')
RETR=$(echo "$OUT" | jq -r '.end.sum_sent.retransmits // 0')
GBPS=$(awk -v b="$BPS" 'BEGIN{printf "%.3f", b/1e9}')
CPG=$(awk -v c="$CS" -v g="$GBPS" -v d="$DUR" 'BEGIN{ if(g>0) printf "%.3f", c/(g*d); else print "0"}')
record "{\"ts\":\"$STAMP\",\"case\":\"ctl-relay\",\"stack\":\"none(splice)\",\"gbps\":$GBPS,\"retr\":$RETR,\"proc_core_sec\":$CS,\"machine_core_sec\":$BUSY,\"cores_per_gbps\":$CPG,\"dur\":$DUR,\"note\":\"单线程 epoll+splice 中继，无 IP 栈——字节搬运的地板成本\"}"
kill "$RPID" 2>/dev/null
"$BENCH_DIR/clean.sh" >/dev/null
echo "[done] $STAMP"
