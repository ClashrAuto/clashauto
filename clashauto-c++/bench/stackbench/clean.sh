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
# ★ 按 /proc/<pid>/exe 的**真实可执行文件路径**杀，不要按命令行（pkill -f）。
#   两个坑一次踩全：
#   · pkill -x lwip2socks 漏掉 lwip2socks_big（-x 要求完全相等），于是一个 100% CPU
#     空转的进程活过了清理，污染了后面两轮测量；
#   · 改成 pkill -f 'bin/(lwip2socks|…)' 之后更糟：**调用方自己的命令行里也有这串**
#     （`./bench_stack.sh lwip-128k ./bin/lwip2socks`），clean.sh 把自己的父 shell 连同
#     整条 ssh 会话一起杀了，症状是「远程命令毫无输出直接返回」。
#   readlink /proc/<pid>/exe 拿到的是解析后的绝对路径，`./bin/x` 起的也能匹配上，
#   而 shell/sshd 的 exe 是 /usr/bin/bash，天然不会误伤。
kill_bench_bins() {
    local sig=$1 pid exe
    for pid in /proc/[0-9]*; do
        pid=${pid#/proc/}
        [ "$pid" = "$$" ] && continue
        exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || continue
        case "$exe" in "$BENCH_DIR/bin/"*) kill "-$sig" "$pid" 2>/dev/null ;; esac
    done
}
count_bench_bins() {
    local pid exe n=0
    for pid in /proc/[0-9]*; do
        exe=$(readlink "${pid}/exe" 2>/dev/null) || continue
        case "$exe" in "$BENCH_DIR/bin/"*) n=$((n+1)) ;; esac
    done
    echo "$n"
}
kill_bench_bins TERM
# 回显靶机是 python3 起的，进程名不叫 pingpong —— 必须按命令行杀，否则它会一直挂着
# 占住 5202 端口，还会把 ssh 会话的 stdout 攥在手里让远程命令永不返回。
pkill -f "pingpong\.py server" >/dev/null 2>&1
sleep 0.3
kill_bench_bins KILL
pkill -9 -x iperf3 >/dev/null 2>&1

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
leftover_proc=$(( $(count_bench_bins) \
                 + $(pgrep -x 'iperf3' 2>/dev/null | wc -l) \
                 + $(pgrep -f 'pingpong\.py server' 2>/dev/null | wc -l) ))
leftover_tun=$(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' | cut -d@ -f1 | grep -cE '^sb[0-9]+$')
busy=$(ss -Hltn 2>/dev/null | awk '{print $4}' | grep -cE ':(5201|5202|7777)$')

# ★ 机器必须**当下**是闲的，不能只看「开机以来的累计 idle%」——那个数被开机至今的历史
#   稀释掉了，一个正在 100% 空转的进程只会让它从 99.9 掉到 97.8，看着完全正常。
#   吃过一次亏：漏杀的 lwip2socks_big 空转满一个核，累计 idle% 毫无反应，两轮测量被污染。
#   这里改成采样 1 秒的**瞬时**占用，超过 0.30 核就直接判失败。
read -r _ a1 a2 a3 a4 a5 a6 a7 a8 _ < /proc/stat
t1=$((a1+a2+a3+a4+a5+a6+a7+a8)); i1=$a4
sleep 1
read -r _ b1 b2 b3 b4 b5 b6 b7 b8 _ < /proc/stat
t2=$((b1+b2+b3+b4+b5+b6+b7+b8)); i2=$b4
busy_cores=$(awk -v dt=$((t2-t1)) -v di=$((i2-i1)) -v n="$(nproc)" \
    'BEGIN{ if(dt<=0){print "0"} else printf "%.3f", (1-di/dt)*n }')
top1=$(ps -eo pcpu,comm --sort=-pcpu --no-headers | head -1 | tr -s ' ')

echo "[clean] procs=$leftover_proc tun=$leftover_tun listen=$busy busy_cores=$busy_cores top=[$top1] at $(date -Is)"
[ "$leftover_proc" = "0" ] && [ "$leftover_tun" = "0" ] && [ "$busy" = "0" ] \
    || { echo "[clean] FAILED — 残留未清干净"; exit 1; }
awk -v b="$busy_cores" 'BEGIN{exit !(b<0.30)}' \
    || { echo "[clean] FAILED — 机器不空闲（$busy_cores 核在跑，top=[$top1]），测了也不算数"; exit 1; }
exit 0
