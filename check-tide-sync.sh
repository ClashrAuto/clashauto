#!/bin/bash
# 检查 tide 的**两个版本指针**有没有走散。
#
# ★ tide 在这个仓库里被指了两次，而两处互不校验：
#
#   1. 父仓库把 tide/ 当 git 子模块跟踪，记的是一个 commit；
#   2. clash/go.mod 里另有一条 `require github.com/ClashrAuto/tide vX.Y.Z`，
#      而 Coast 客户端（clash/adapter/outbound/tide.go）**实际链接的是这一条**。
#
# 2026-08-07 发现两者已经差了 40 个提交：子模块指到 main，go.mod 还写着 v0.2.0。
# 也就是说三十轮审计的修复——读协程死锁、UDP 关联寿命、path_id 唯一性、
# 会话上界、h3 入口路径、空用户表失败关闭——一条都没进 Coast 客户端。
#
# 它之所以能瞒这么久，是因为**两边各自都是绿的**：tide 的 verify.sh 全过，
# clash 也编得过；没有任何一处会去比对这两个指针。而症状也不像版本问题——
# 当时表现为"适配层里没有 h3 字段"，看上去只是漏写了一个选项。
#
# 用法：
#   bash check-tide-sync.sh          # 只检查
#   bash check-tide-sync.sh --fix    # 打印该执行的 go get 命令
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -d tide/.git ] && [ ! -f tide/.git ]; then
  echo "tide/ 子模块没有检出（git submodule update --init）"; exit 2
fi

head=$(git -C tide rev-parse HEAD)
pin=$(grep -oE 'github\.com/ClashrAuto/tide v[^ ]+' clash/go.mod | awk '{print $2}' | head -1)
if [ -z "$pin" ]; then
  echo "clash/go.mod 里找不到 tide 的 require 行"; exit 2
fi

# 伪版本（v0.2.1-0.20260807012446-153bfaaec870）末段就是 12 位 commit 前缀；
# 纯 tag（v0.2.0）则要去 tide 里解析成 commit。
if [[ "$pin" =~ -([0-9a-f]{12})$ ]]; then
  pinned="${BASH_REMATCH[1]}"
else
  pinned=$(git -C tide rev-parse --short=12 "${pin}^{commit}" 2>/dev/null || echo "")
  if [ -z "$pinned" ]; then
    echo "go.mod 钉的是 $pin，但 tide 里没有这个 tag —— 无法比对"; exit 1
  fi
fi

if [ "${head:0:12}" = "$pinned" ]; then
  echo "同步 ✓  tide=${head:0:12}  clash/go.mod=$pin"
  exit 0
fi

behind=$(git -C tide rev-list --count "${pinned}..${head}" 2>/dev/null || echo "?")
echo "★ 两个版本指针走散了："
echo "    子模块 tide/     = ${head:0:12}"
echo "    clash/go.mod 钉  = $pin  (${pinned})"
echo "    Coast 客户端实际链接的是后者，落后 ${behind} 个提交。"
echo
echo "修：在构建机上（本机拉不动 goproxy）执行"
echo "    cd clash && GOPROXY=https://goproxy.cn,direct GOSUMDB=off GOFLAGS=-mod=mod \\"
echo "        go get github.com/ClashrAuto/tide@${head:0:7} && go build ./..."
echo "然后把 go.mod / go.sum 带回来提交。"
exit 1
