#!/usr/bin/env bash
# 一键：构建验证镜像 + 下载最新 Coast release + 全平台验证。
# 用法（git-bash / WSL）：  bash run.sh
#   PROXY=  bash run.sh          # 能直连 GitHub 时禁用代理
#   REPO=owner/repo bash run.sh  # 验别的仓库
set -euo pipefail
cd "$(dirname "$0")"
IMG=coast-validate

# 宿主机本地代理（mihomo 混合端口）。Docker Desktop 里 host.docker.internal = 宿主机。
# 容器下载 GitHub release 走它；设 PROXY= 空则直连。（apt 装依赖走直连，不用代理。）
PROXY="${PROXY-http://host.docker.internal:7890}"

echo ">>> docker build $IMG"
docker build -t "$IMG" .

ARGS=(--rm)
# 命名卷缓存下载（/work），重跑不再重下 ~150MB。清缓存：docker volume rm coast-dl
ARGS+=(-v coast-dl:/work)
[ -n "$PROXY" ] && ARGS+=(-e "https_proxy=$PROXY" -e "http_proxy=$PROXY")
[ -n "${REPO:-}" ] && ARGS+=(-e "VALIDATE_REPO=$REPO")

echo ">>> docker run（下载 + 验证；Linux 版会 Xvfb 真跑一遍）"
docker run "${ARGS[@]}" "$IMG"
