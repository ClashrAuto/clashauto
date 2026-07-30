#!/usr/bin/env bash
# 为 macOS 构建 **universal(x86_64 + arm64)** 的 libmsquic，并摆成 CMake 认得的 SDK 布局：
#   <sdk>/include/{msquic.h,msquic_posix.h,quic_platform_posix.h,quic_sal_stub.h}
#   <sdk>/lib/libmsquic.dylib        (install name = @rpath/libmsquic.dylib)
#
# ★ 为什么 macOS 非得自己编：微软对 macOS **既没有预编译包、也没有包管理器渠道**
#   （msquic 的 GitHub Release 一个资产都没有；Homebrew 无 formula；NuGet 那套只有 Windows）。
#   Windows/Linux 都有现成二进制，唯独 macOS 只能从源码来。
#
# ★ 为什么要编两遍再 lipo：msquic 在 Darwin 上**不支持一次编出 universal**。它给 quictls 传的
#   Configure 目标是按 CMAKE_OSX_ARCHITECTURES 精确匹配的（见 msquic/submodules/CMakeLists.txt：
#   只认 `arm64` 或 `x86_64`，多架构会走到 `message(ERROR "WTF ...")` 那条分支）。
#   所以：单架构各编一次 → lipo 合并。
#
# 用法：  bash tools/build_msquic_macos.sh <sdk-dir> [msquic-version]
# 例：    bash tools/build_msquic_macos.sh "$HOME/msquic-sdk" 2.5.9
#
# 幂等：<sdk>/lib/libmsquic.dylib 已存在且是 universal 就直接返回（配合 CI 缓存）。
# 依赖：git、cmake、ninja（brew install cmake ninja）、Xcode Command Line Tools。
#
# ★ 写法坑：双引号串里 `$var` 后面若紧跟中文/全角标点（如 `）` `，`），必须写 `${var}`。
#   某些 libc（macOS 正是）把 ≥0x80 的字节判成字母，bash 就把全角字符吃进变量名——
#   `"$arch）"` 被解析成变量 `arch）`，配上下面的 set -u 当场 unbound variable 退出。
set -euo pipefail

SDK="${1:?用法: build_msquic_macos.sh <sdk-dir> [version]}"
VER="${2:-${MSQUIC_VERSION:-2.5.9}}"
DEPLOY_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}"   # 与 app 的 CMAKE_OSX_DEPLOYMENT_TARGET 保持一致

if [ -f "$SDK/lib/libmsquic.dylib" ] && [ -f "$SDK/include/msquic.h" ]; then
  archs="$(lipo -archs "$SDK/lib/libmsquic.dylib" 2>/dev/null || echo '')"
  case "$archs" in
    *x86_64*arm64*|*arm64*x86_64*)
      echo "msquic SDK 已就绪（${archs}）：$SDK"; exit 0 ;;
  esac
  echo "已有 SDK 但不是 universal（${archs}），重建"
fi

SRC="${MSQUIC_SRC_DIR:-${RUNNER_TEMP:-/tmp}/msquic-src}"
rm -rf "$SRC"
echo "== 取 msquic v$VER 源码（浅克隆）=="
git clone --depth 1 --branch "v$VER" https://github.com/microsoft/msquic.git "$SRC"
cd "$SRC"
# 只要 TLS 后端(quictls)与日志代码生成(clog)；googletest / xdp-for-windows 用不上，别拉。
git submodule update --init --depth 1 submodules/quictls submodules/clog

for arch in x86_64 arm64; do
  echo "== 编 msquic（${arch}）=="
  cmake -B "build-$arch" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DQUIC_TLS_LIB=quictls \
    -DQUIC_BUILD_TOOLS=OFF \
    -DQUIC_BUILD_TEST=OFF \
    -DQUIC_BUILD_PERF=OFF \
    -DQUIC_ENABLE_LOGGING=OFF \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY_TARGET"
  cmake --build "build-$arch" --parallel
done

# 产物路径随版本/生成器会变，别写死 —— 找真文件（跳过符号链接）。
# 注意不用 `find | head -1`：pipefail 下 head 提前关管道会让 find 吃 SIGPIPE，
# 整条流水线非零 → set -e 让脚本**无声退出**，一行报错都不留。sed 读完全部输入，没这毛病。
dylib_of() {
  find "$1" -type f -name 'libmsquic*.dylib' | sed -n 1p
}
X="$(dylib_of "build-x86_64")"
A="$(dylib_of "build-arm64")"
[ -n "$X" ] && [ -n "$A" ] || { echo "没找到编出来的 libmsquic.dylib（x86_64='$X' arm64='$A'）" >&2; exit 1; }

mkdir -p "$SDK/include" "$SDK/lib"
lipo -create "$X" "$A" -output "$SDK/lib/libmsquic.dylib"
# install name 用 @rpath：app 侧把它放进 Coast.app/Contents/Frameworks/，
# 配合 macdeployqt 写的 @executable_path/../Frameworks 即可被加载器找到。
install_name_tool -id "@rpath/libmsquic.dylib" "$SDK/lib/libmsquic.dylib"
cp src/inc/msquic.h src/inc/msquic_posix.h src/inc/quic_platform_posix.h src/inc/quic_sal_stub.h \
   "$SDK/include/"

echo "== 完成 =="
echo "  $SDK/lib/libmsquic.dylib  →  $(lipo -archs "$SDK/lib/libmsquic.dylib")"
otool -D "$SDK/lib/libmsquic.dylib" | tail -1
