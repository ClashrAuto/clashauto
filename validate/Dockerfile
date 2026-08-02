# Coast release 全平台下载+验证容器（Debian trixie，匹配 Linux 构建环境）。
# 只装：下载/解包/校验工具 + Xvfb（无头跑 Linux 版）。
#
# ★ **故意不预装任何 Qt6 运行时库。** 这里曾经列着 libgl1 / libxcb-cursor0 / libfontconfig1
#   等一长串——正好是 .deb 在 Depends 里声明的那批。后果是 validate.sh 里
#   `apt-get install ./coast.deb` 根本不需要解析依赖，那条 "「.deb 声明的运行时依赖解析 OK」"
#   的断言是空的：**Depends 漏写一项也一路绿灯**，只有干净的用户机器会炸。
#   （真机核对确认过：libx11-xcb1 / libsm6 / libice6 / libzstd1 都曾漏写，且没有任何已声明
#     的依赖会间接带入它们；前三个是 libQt6XcbQpa 的 NEEDED，缺一个平台插件就加载不了。）
#
#   现在这些库必须全部由 .deb 自己的 Depends 拉进来，harness 才真的在测那件事。
#   xvfb/xauth/dbus-x11 是 harness 自己要用的，它们会顺带带进 libx11-6/libxcb1 这类最基础的
#   X 库 —— 无法避免，但风险最高的那几个（libxcb-cursor0、libgl1、libopengl0、libx11-xcb1、
#   libzstd1）都不在它们的依赖里，仍然是真测。
#   便携 tar.gz 那节只做 `tar tzf` 结构检查、不执行二进制，所以不受影响。
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl jq unzip xz-utils p7zip-full file coreutils \
      xvfb xauth dbus-x11 \
    && rm -rf /var/lib/apt/lists/*

COPY validate.sh /usr/local/bin/validate.sh
RUN chmod +x /usr/local/bin/validate.sh

# 可覆盖：只验某平台或改仓库。
ENV VALIDATE_REPO=ClashrAuto/clashauto
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/validate.sh"]
