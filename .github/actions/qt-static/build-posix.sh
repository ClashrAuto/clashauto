#!/usr/bin/env bash
# 从源码编一套**静态** Qt（Linux / macOS）。产物 = $QT_PREFIX。
#
# 为什么必须自己编：aqtinstall 发的 Qt 全是共享构建，官方从不提供静态二进制。
# 「打包全用静态链接库」这件事的全部成本就在这个脚本里，其余（CRT / libstdc++ /
# OpenSSL）都是顺手的事。
#
# 需要的环境变量：
#   QT_VERSION   如 6.8.3
#   QT_PREFIX    安装前缀
#   QT_SRC       源码解压目录
#   QT_JOBS      并行度
#   QT_EXTRA_CMAKE  （可选）追加给 configure 的 -- 之后的 CMake 参数，空格分隔
set -euo pipefail

QTMM="${QT_VERSION%.*}"          # 6.8.3 → 6.8
: "${QT_JOBS:=$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu )}"
: "${QT_EXTRA_CMAKE:=}"
OS="$(uname -s)"

# ★ 模块集就这三个，是数出来的不是猜的：
#     qtbase          —— Core/Gui/Widgets/Network/Sql(+QSQLITE 驱动)/平台插件
#     qtshadertools   —— qtdeclarative 的构建期依赖（Qt Quick 的着色器）
#     qtdeclarative   —— Qml/Quick/QuickControls2
#   **没有 qtsvg**：qrc 里那两个 chevron-*.svg 只被 src/MainWindow.cpp 引用，而那是
#   没参与编译的 Widgets 遗留界面；QML 侧的图标全是字体（iconfont/remixicon.ttf）。
#   哪天 QML 真用上 .svg，这里和 src/StaticDepsSelfTest.cpp 的断言要一起改。
#   也没有 qttools：静态构建根本不需要 windeployqt/macdeployqt。
MODULES=(qtbase qtshadertools qtdeclarative)

echo "::group::[qt-static] 依赖"
if [ "$OS" = "Linux" ]; then
  export DEBIAN_FRONTEND=noninteractive
  sudo apt-get update -qq
  # 静态 Qt 依然要链系统的 xcb/GL/fontconfig —— 它们跨不过 glibc/驱动这条边界，
  # 不可能静态进来。这就是 Linux 这条腿「静态只能到 Qt 为止」的原因，.deb 的
  # Depends 也因此缩不了多少（见 release.yml 里 Depends 那段注释）。
  # ⚠️ 名字必须逐个是对的：apt 只要遇到**一个**未知包名就整条命令失败，一个都装不上。
  #   （这里原先写过 libxcb-xkb0-dev —— 不存在的包，正确名是 libxcb-xkb-dev。当时还配了个
  #    `|| 装那一个` 的兜底，于是失败路径变成"只装上那一个、其余三十个全缺"，
  #    然后 configure 报 xcb 不可用 —— 错因离现场隔了两层。所以宁可让它硬失败在 apt 这一行。）
  #   下面这批在 ubuntu-22.04 / 24.04 上都存在（已在 24.04 上用 apt-cache 逐个核过）。
  sudo apt-get install -y -qq --no-install-recommends \
    build-essential ninja-build cmake python3 perl \
    libgl1-mesa-dev libegl1-mesa-dev libglu1-mesa-dev \
    libfontconfig1-dev libfreetype-dev \
    libx11-dev libx11-xcb-dev libxext-dev libxfixes-dev libxi-dev libxrender-dev \
    libxcb1-dev libxcb-glx0-dev libxcb-keysyms1-dev libxcb-image0-dev \
    libxcb-shm0-dev libxcb-icccm4-dev libxcb-sync-dev libxcb-xfixes0-dev \
    libxcb-shape0-dev libxcb-randr0-dev libxcb-render-util0-dev libxcb-util-dev \
    libxcb-xinerama0-dev libxcb-xkb-dev libxcb-cursor-dev \
    libxkbcommon-dev libxkbcommon-x11-dev libsm-dev libice-dev \
    libssl-dev zlib1g-dev
else
  command -v ninja >/dev/null || brew install ninja
fi
echo "::endgroup::"

echo "::group::[qt-static] 取源码"
mkdir -p "$QT_SRC" && cd "$QT_SRC"
# download.qt.io 在部分区域很慢，镜像优先；三个都失败才算失败（而不是静默拿到半个包）。
MIRRORS=(
  "https://download.qt.io/archive/qt/$QTMM/$QT_VERSION/submodules"
  "https://mirrors.ustc.edu.cn/qtproject/archive/qt/$QTMM/$QT_VERSION/submodules"
  "https://qt-mirror.dannhauer.de/archive/qt/$QTMM/$QT_VERSION/submodules"
)
for m in "${MODULES[@]}"; do
  tb="$m-everywhere-src-$QT_VERSION.tar.xz"
  if [ ! -s "$tb" ]; then
    ok=0
    for base in "${MIRRORS[@]}"; do
      echo "  $tb  ←  $base"
      if curl -fsSL --retry 3 --connect-timeout 25 -o "$tb.part" "$base/$tb"; then
        mv "$tb.part" "$tb"; ok=1; break
      fi
    done
    [ "$ok" = 1 ] || { echo "::error::$tb 所有镜像均下载失败"; exit 1; }
  fi
  [ -d "$m" ] || { mkdir -p "$m"; tar xf "$tb" -C "$m" --strip-components=1; }
done
echo "::endgroup::"

echo "::group::[qt-static] configure + build qtbase"
# 每一条 flag 的取舍：
#   -static             库出 .a；插件也变静态库，必须由 CMake 侧 qt_import_qml_plugins 引入
#   -no-icu             省掉随包发 libicu*.so（~30MB）；Qt6 无 ICU 只损失少量本地化排序
#   -no-glib            去掉 libglib2.0 依赖；GTK 平台主题本来就没装 dev 包、编不出来
#   -qt-*               pcre/harfbuzz/png/jpeg/zlib 用 Qt 自带，不外链系统库
#   -system-freetype    必须与系统 fontconfig 配对；混用 Qt 自带 freetype 会冲突
#   TLS 后端            Linux=openssl-runtime（运行期 dlopen libssl.so.3，.deb 已 Depends）
#                       macOS=securetransport（系统自带，因此 mac 包完全不需要 OpenSSL）
COMMON=(
  -static -release -prefix "$QT_PREFIX"
  -opensource -confirm-license
  -nomake examples -nomake tests
  -no-icu
  -qt-pcre -qt-harfbuzz -qt-libpng -qt-libjpeg -qt-zlib -qt-doubleconversion
  -sql-sqlite -no-sql-mysql -no-sql-psql -no-sql-odbc
)
if [ "$OS" = "Linux" ]; then
  COMMON+=(-no-glib -system-freetype -fontconfig -xcb -bundled-xcb-xinput -openssl-runtime)
else
  COMMON+=(-securetransport -no-openssl)
fi

rm -rf "$QT_SRC/b-qtbase"; mkdir -p "$QT_SRC/b-qtbase"; cd "$QT_SRC/b-qtbase"
# shellcheck disable=SC2086
"$QT_SRC/qtbase/configure" "${COMMON[@]}" -- -DCMAKE_BUILD_TYPE=Release $QT_EXTRA_CMAKE
cmake --build . --parallel "$QT_JOBS"
cmake --install .
echo "::endgroup::"

for m in qtshadertools qtdeclarative; do
  echo "::group::[qt-static] $m"
  rm -rf "$QT_SRC/b-$m"; mkdir -p "$QT_SRC/b-$m"; cd "$QT_SRC/b-$m"
  # shellcheck disable=SC2086
  "$QT_PREFIX/bin/qt-configure-module" "$QT_SRC/$m" -- $QT_EXTRA_CMAKE
  cmake --build . --parallel "$QT_JOBS"
  cmake --install .
  echo "::endgroup::"
done

echo "[qt-static] 产物概览"
du -sh "$QT_PREFIX"
echo "静态库: $(find "$QT_PREFIX/lib" -maxdepth 1 -name '*.a' | wc -l) 个"
# ★ 这条断言是整个脚本的门禁：只要 lib/ 下还有 .so/.dylib，说明 -static 没生效，
#   后面 app 照样能编能链，直到打包完、用户装上才发现少 DLL。必须当场炸。
shared_count="$(find "$QT_PREFIX/lib" -maxdepth 1 \( -name '*.so*' -o -name '*.dylib' \) | wc -l)"
if [ "$shared_count" -ne 0 ]; then
  echo "::error::$QT_PREFIX/lib 里有 $shared_count 个共享库 —— 这不是静态 Qt"
  find "$QT_PREFIX/lib" -maxdepth 1 \( -name '*.so*' -o -name '*.dylib' \) | head -20
  exit 1
fi
# 三个最容易漏的插件，缺任何一个都不会让 app 编不过，只会让某块功能静默失效。
for p in platforms/libqxcb.a platforms/libqoffscreen.a sqldrivers/libqsqlite.a; do
  case "$OS:$p" in Darwin:platforms/libqxcb.a) continue;; esac
  [ -f "$QT_PREFIX/plugins/$p" ] || echo "::warning::缺插件 $p"
done
echo "QT_STATIC_BUILD_OK"
