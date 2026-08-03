#!/bin/bash
# 每次测试前必须先跑：把上一轮的残留全部清掉，保证各栈起跑线相同。
# 只动本 harness 自己创建的东西（tun 名一律 sb*，端口 5201/5202/7777），
# 绝不碰机器上已有的 vdev*/docker0 等别的测试留下的设备。
set -u
BENCH_DIR=${BENCH_DIR:-/root/stackbench}

# 1) 杀掉被测栈与打流工具
for p in lwip2socks gvnet2socks smoltcp2socks relay iperf3 pingpong; do
    pkill -x "$p" >/dev/null 2>&1
done
pkill -f "$BENCH_DIR/bin/" >/dev/null 2>&1
# 回显靶机是 python3 起的，进程名不叫 pingpong —— 必须按命令行杀，否则它会一直挂着
# 占住 5202 端口，还会把 ssh 会话的 stdout 攥在手里让远程命令永不返回。
pkill -f "pingpong.py" >/dev/null 2>&1
sleep 0.3
for p in lwip2socks gvnet2socks smoltcp2socks relay iperf3 pingpong; do
    pkill -9 -x "$p" >/dev/null 2>&1
done

# 2) 删掉本 harness 的 tun 设备（名字前缀 sb）
for t in $(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' | cut -d@ -f1 | grep -E '^sb[0-9]+$'); do
    ip link del "$t" >/dev/null 2>&1
done

# 3) 删掉本 harness 的路由/邻居残留
ip route del 10.99.0.0/24 >/dev/null 2>&1
ip route del 203.0.113.0/24 >/dev/null 2>&1

# 4) 清页缓存 + 让 TIME_WAIT 的端口先放掉
sync
echo 3 >/proc/sys/vm/drop_caches 2>/dev/null
sleep 1

# 5) 自证清理干净——把断言打出来，别只说"清了"
# 注意：pgrep -c 在计数为 0 时既打印 0 又返回退出码 1，`|| echo 0` 会再补一个 0
# 变成 "0\n0" 把断言打飞——所以这里用 wc -l 计数。
leftover_proc=$(( $(pgrep -x 'iperf3|relay|lwip2socks|gvnet2socks|smoltcp2socks' 2>/dev/null | wc -l) \
                 + $(pgrep -f 'pingpong.py' 2>/dev/null | wc -l) ))
leftover_tun=$(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' | cut -d@ -f1 | grep -cE '^sb[0-9]+$')
busy=$(ss -Hltn 2>/dev/null | awk '{print $4}' | grep -cE ':(5201|5202|7777)$')
idle=$(awk '/^cpu /{t=0;for(i=2;i<=NF;i++)t+=$i;print 100*$5/t}' /proc/stat)
echo "[clean] procs=$leftover_proc tun=$leftover_tun listen=$busy idle%=$idle at $(date -Is)"
[ "$leftover_proc" = "0" ] && [ "$leftover_tun" = "0" ] && [ "$busy" = "0" ] || { echo "[clean] FAILED — 残留未清干净"; exit 1; }
exit 0
