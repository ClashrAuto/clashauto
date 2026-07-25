# Coast release 全平台下载+验证容器（Debian trixie，匹配 Linux 构建环境）。
# 装：下载/解包/校验工具 + Xvfb（无头跑 Linux 版）+ Qt6 运行时依赖的系统库。
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl jq unzip xz-utils p7zip-full file coreutils \
      xvfb xauth dbus-x11 \
      libgl1 libegl1 libglib2.0-0 libfontconfig1 libfreetype6 libdbus-1-3 \
      libxkbcommon0 libxkbcommon-x11-0 \
      libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
      libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xkb1 libxcb-glx0 \
    && rm -rf /var/lib/apt/lists/*

COPY validate.sh /usr/local/bin/validate.sh
RUN chmod +x /usr/local/bin/validate.sh

# 可覆盖：只验某平台或改仓库。
ENV VALIDATE_REPO=ClashrAuto/clashauto
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/validate.sh"]
