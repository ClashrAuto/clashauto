#!/bin/bash
# 小对象负载跑分：并发短连接 + 固定小响应，量 完成率 / 吞吐 / 每对象 CPU。
# 对每个对象大小跑一遍，看单流 4× 优势在小对象上还剩多少。
# 用法: bench_obj.sh <stack-name> <可执行文件>
set -u
BENCH_DIR=${BENCH_DIR:-/root/stackbench}
. "$BENCH_DIR/lib.sh"
DUR=${DUR:-8}
WORKERS=${WORKERS:-32}
SIZES=${SIZES:-"4096 65536 1048576"}   # 4KiB(小页) / 64KiB(典型网页对象) / 1MiB(大对象)
NAME=$1; shift
BIN=$1; shift
TAP=sb0; STACK_IP=10.99.0.2; TARGET=203.0.113.5

teardown() { kill -- -"$SPID" 2>/dev/null; kill "$SPID" 2>/dev/null; sleep 0.3
             kill -9 -- -"$SPID" 2>/dev/null; "$BENCH_DIR/clean.sh" >/dev/null 2>&1; }

"$BENCH_DIR/clean.sh" || exit 1
STAMP=$(date -Is)
ip tuntap add dev "$TAP" mode tap || exit 1
ip addr add 10.99.0.1/24 dev "$TAP" || exit 1
ip link set "$TAP" up || exit 1
setsid "$BIN" "$TAP" "$STACK_IP" 255.255.255.0 "$@" </dev/null >/tmp/stack.log 2>&1 &
SPID=$!
sleep 1
kill -0 "$SPID" 2>/dev/null || { echo "[FAIL] $NAME 起不来"; cat /tmp/stack.log; teardown; exit 1; }
ip route add 203.0.113.0/24 via "$STACK_IP" dev "$TAP" 2>/dev/null

for SZ in $SIZES; do
    # 每个大小换个端口起 sink server，避免 TIME_WAIT 串味
    PORT=$(( 5300 + (SZ % 100) ))
    OBJ=$SZ setsid python3 "$BENCH_DIR/objload.py" server "$PORT" </dev/null >/tmp/sink.log 2>&1 &
    SINK=$!
    sleep 0.5
    # 连通性自证：这个端口真能穿栈
    timeout 4 python3 -c "import socket;socket.create_connection(('$TARGET',$PORT),timeout=3).close()" 2>/dev/null \
        || { echo "[FAIL] $NAME size=$SZ 连通性自证失败"; kill $SINK 2>/dev/null; continue; }
    T0=$(cpu_ticks "$SPID")
    OUT=$(OBJ=$SZ timeout $(( DUR + 20 )) python3 "$BENCH_DIR/objload.py" "$TARGET" "$PORT" "$WORKERS" "$DUR" 2>/dev/null)
    CS=$(cpu_delta_cores "$SPID" "$T0")
    kill "$SINK" 2>/dev/null
    [ -z "$OUT" ] && OUT='{"objs":0,"obj_per_s":0,"gbps":0}'
    OPS=$(echo "$OUT" | sed 's/.*"obj_per_s":\([0-9]*\).*/\1/')
    # 每对象 CPU（微秒）：把栈的处理成本摊到每个完成的对象上
    US=$(echo "$OUT" | awk -v c="$CS" -F'"objs":' '{split($2,a,",");o=a[1]+0; if(o>0)printf "%.1f",1e6*c/o; else print 0}')
    record "{\"ts\":\"$STAMP\",\"case\":\"obj\",\"stack\":\"$NAME\",\"size\":$SZ,\"workers\":$WORKERS,\"load\":$OUT,\"proc_core_sec\":$CS,\"us_per_obj\":$US,\"alive\":$(kill -0 $SPID 2>/dev/null && echo 1 || echo 0)}"
    sleep 1
done

teardown
echo "[done] $NAME 小对象测试结束"
