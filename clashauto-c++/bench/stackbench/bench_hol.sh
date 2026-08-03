#!/bin/bash
# 队头阻塞 / 公平性：bulk 流打满栈的同时，交互式小请求的延迟被拖到多少。
# 真实网关体验的核心——一个大下载正在跑，网页点击/游戏包还能不能秒回。
# 单线程 poll 的栈（lwIP / smoltcp）尤其可能在这里翻车：长流的收发把 poll 循环占满，
# 小请求排在后面。这是纯 bulk 吞吐永远测不到的一面。
#
# 做法：后台 iperf3 -P4 灌满，前台同时跑 500 次串行乒乓（1 字节往返），量 idle 与 under-bulk
# 两种情形下的往返延迟，对比膨胀倍数。
# 用法: bench_hol.sh <stack-name> <可执行文件>
set -u
BENCH_DIR=${BENCH_DIR:-/root/stackbench}
. "$BENCH_DIR/lib.sh"
BULK_SEC=${BULK_SEC:-12}
PP_N=${PP_N:-500}
NAME=$1; shift
BIN=$1; shift
TAP=sb0; STACK_IP=10.99.0.2; TARGET=203.0.113.5

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
    || { echo "[FAIL] $NAME 连通性自证失败"; teardown; exit 1; }
echo "[ok] $NAME 连通性自证通过"

# 1) idle 延迟基线
IDLE=$(python3 "$BENCH_DIR/pingpong.py" "$TARGET" 5202 "$PP_N" 2>/dev/null)
record "{\"ts\":\"$STAMP\",\"case\":\"hol-idle\",\"stack\":\"$NAME\",\"pingpong\":${IDLE:-null}}"

# 2) bulk 打满时的延迟
setsid iperf3 -c "$TARGET" -p 5201 -t "$BULK_SEC" -P 4 -J </dev/null >/tmp/bulk.json 2>&1 &
BJ=$!
sleep 2   # 等 bulk 真正拉满再打延迟
UNDER=$(python3 "$BENCH_DIR/pingpong.py" "$TARGET" 5202 "$PP_N" 2>/dev/null)
BULK_GBPS=$(cat /tmp/bulk.json 2>/dev/null | jq -r '.end.sum_received.bits_per_second // 0' | awk '{printf "%.3f",$1/1e9}')
wait $BJ 2>/dev/null
record "{\"ts\":\"$STAMP\",\"case\":\"hol-underbulk\",\"stack\":\"$NAME\",\"pingpong\":${UNDER:-null},\"bulk_gbps\":${BULK_GBPS:-0}}"

# 膨胀倍数
echo "$IDLE $UNDER" | python3 -c "
import sys,json
a=sys.stdin.read().split()
try:
  i=json.loads(' '.join(a[:len(a)//2])); u=json.loads(' '.join(a[len(a)//2:]))
  print('[hol] $NAME idle p50=%.3f p99=%.3f  under-bulk p50=%.3f p99=%.3f  膨胀 p50=%.1fx p99=%.1fx  bulk=$BULK_GBPS Gbps'
        %(i['p50_ms'],i['p99_ms'],u['p50_ms'],u['p99_ms'],u['p50_ms']/max(i['p50_ms'],1e-6),u['p99_ms']/max(i['p99_ms'],1e-6)))
except Exception as e: print('[hol] $NAME parse err',e)
" 2>/dev/null || echo "[hol] $NAME idle=$IDLE under=$UNDER"

teardown
echo "[done] $NAME HOL 测试结束"
