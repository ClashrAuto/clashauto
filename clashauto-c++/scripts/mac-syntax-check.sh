#!/bin/bash
# macOS 侧的**编译自查**：不需要 cmake/ninja，只用 clang++ -fsyntax-only 过一遍指定的
# 翻译单元，抓类型/声明/包含错误。
#
# 为什么需要它：这台开发/测试 Mac 上没有 cmake、ninja、brew（只有 Xcode 与 Qt），
# 装工具链超出测试范围，于是「改了 macOS 相关代码却从没在 macOS 上编译过」成了常态 ——
# 而 LanGateway_linux.cpp 实为 POSIX 通用、在 macOS 上**也参与编译**，Linux 编过不代表
# macOS 编得过。真机上就靠这个脚本发现过：Linux 全绿、macOS 侧却因缺 moc/框架路径报错。
#
# 用法（在 clashauto-c++ 目录下，或传 -C <dir>）：
#     bash scripts/mac-syntax-check.sh
#     bash scripts/mac-syntax-check.sh src/net/PfRules.cpp src/foo.cpp   # 只查指定文件
#
# ★ 它**不是**完整构建：不链接、不生成 .app、不跑 qrc/uic。CI 的 macOS job 仍是最终判据。
#   但它能在几秒内挡掉绝大多数「只在 macOS 上出现」的编译错误。
set -uo pipefail

QT=$(ls -d "$HOME"/Qt/*/macos 2>/dev/null | head -1)
if [ -z "$QT" ]; then
    echo "找不到 Qt（期望 ~/Qt/<ver>/macos）" >&2
    exit 2
fi
MOC=$(find "$HOME/Qt" -name moc -type f -perm +111 2>/dev/null | head -1)

# Qt 的 framework 布局：头文件在 <Qt>/lib/QtXxx.framework/Headers，且要 -F 指向 lib。
INC=""
for m in Core Network Gui Sql Widgets Qml Quick; do
    INC="$INC -I$QT/lib/Qt$m.framework/Headers"
done
INC="$INC -F$QT/lib"
# 曾经这里要加 lwIP 的移植头（-Isrc/net/lwip_port -Ithird_party/lwip/src/include）。
# lwIP 已整体删除；macOS 走 pf rdr，NetStack.cpp 只在 Windows 编，本脚本查不到也不该查它。

# 默认查「macOS 上参与编译、且我们最常改」的那几个。传参可覆盖。
FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then
    FILES=(
        src/net/PfRules.cpp
        src/net/TproxyRules.cpp
        src/net/LanGateway_linux.cpp
        src/net/NetStack_stub.cpp
        src/ConfigBuilder.cpp
        src/AppConfig.cpp
        src/CoreController.cpp
        src/qml/DevicesController.cpp
    )
fi

fail=0
for f in "${FILES[@]}"; do
    [ -f "$f" ] || { printf "  %-38s 跳过（不存在）\n" "$(basename "$f")"; continue; }
    # 文件末尾 #include "xxx.moc" 的，先用 moc 生成，否则报 file not found（假错误）
    if grep -q '#include "'"$(basename "${f%.cpp}")"'\.moc"' "$f" 2>/dev/null && [ -n "$MOC" ]; then
        "$MOC" $INC -Isrc -Isrc/net "$f" -o "${f%.cpp}.moc" 2>/dev/null
    fi
    printf "  %-38s " "$(basename "$f")"
    out=$(clang++ -std=c++17 -fsyntax-only $INC -Isrc -Isrc/net -Isrc/qml "$f" 2>&1)
    n=$(printf '%s' "$out" | grep -cE " error:")
    if [ "$n" = "0" ]; then
        echo "OK"
    else
        echo "$n 个错误："
        printf '%s' "$out" | grep -E " error:" | head -5 | sed 's/^/      /'
        fail=1
    fi
done
echo "=== $([ $fail -eq 0 ] && echo "macOS 侧无编译错误" || echo "存在编译错误") ==="
exit $fail
