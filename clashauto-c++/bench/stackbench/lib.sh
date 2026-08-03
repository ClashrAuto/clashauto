#!/bin/bash
# 公共测量函数：CPU 采样（核·秒）+ 结果落盘（每次测试一行 JSON，永不覆盖）
BENCH_DIR=${BENCH_DIR:-/root/stackbench}
RESULTS=${RESULTS:-$BENCH_DIR/results.jsonl}
HZ=$(getconf CLK_TCK)

# cpu_ticks <pid> -> utime+stime（含所有线程）
# comm 字段可能含空格/括号，必须从最后一个 ')' 之后开始数，否则字段偏移全错
cpu_ticks() {
    local pid=$1
    [ -r "/proc/$pid/stat" ] || { echo 0; return; }
    sed 's/.*) //' "/proc/$pid/stat" | awk '{print $12+$13}'
}

# cpu_cores <pid> <t0_ticks> <wall_seconds> -> 核·秒 与 核数
cpu_delta_cores() {
    local pid=$1 t0=$2
    local t1
    t1=$(cpu_ticks "$pid")
    awk -v a="$t0" -v b="$t1" -v hz="$HZ" 'BEGIN{printf "%.3f", (b-a)/hz}'
}

# 整机忙碌核数（用 /proc/stat 差分），配合 machine_busy_start
machine_busy_start() { awk '/^cpu /{i=$5;t=0;for(j=2;j<=NF;j++)t+=$j;print i" "t}' /proc/stat; }
machine_busy_cores() {
    local s=$1
    local i0=${s%% *} t0=${s##* }
    awk -v i0="$i0" -v t0="$t0" -v hz="$(getconf CLK_TCK)" '/^cpu /{i=$5;t=0;for(j=2;j<=NF;j++)t+=$j;printf "%.3f", (t-t0-(i-i0))/hz}' /proc/stat
}

# record <json>  —— 追加一行，绝不覆盖历史
record() {
    mkdir -p "$(dirname "$RESULTS")"
    echo "$1" >>"$RESULTS"
    echo "[record] $1"
}
