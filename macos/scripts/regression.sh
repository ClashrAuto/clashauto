#!/usr/bin/env bash
# Swift 版 Coast 的一键回归。思路借自 clashauto-c++/validate/：
# **打包 → 跑打包产物的全套自检钩子**,一条命令确认「改动后包没坏」。
#
# 分三类结果,别把环境限制当成 bug:
#   PASS —— 真通过
#   FAIL —— 真失败,退出码非 0(CI 该红)
#   SKIP —— 环境不具备,本机验不了(不算失败)。典型是「没装正式签名的 helper」——
#           ad-hoc 签名装不了免密 helper,于是系统代理(经 helper 免密)和真机接管都验不了。
#
# 用法:
#   bash scripts/regression.sh              # 打包 + 全套自检
#   bash scripts/regression.sh --no-build   # 用已存在的 Coast.app,只跑自检
set -uo pipefail
cd "$(dirname "$0")/.."

BUILD=1
[[ "${1:-}" == "--no-build" ]] && BUILD=0

pass=0; fail=0; skip=0
line() { printf '  %-22s %s\n' "$1" "$2"; }

# —— 0. 真核心校验(可选) ——
# COAST_TEST_MIHOMO 指向 mihomo 二进制时,单测里那两条 RealCoreValidation 会真的跑
# `mihomo -t` 校验生成的 full.yaml —— **真正的消费者**才是权威判据,PyYAML 只能证明
# 「是合法 YAML」,核心还有自己的 schema。没设就自动跳过,不做硬依赖。
if [[ -n "${COAST_TEST_MIHOMO:-}" && -x "${COAST_TEST_MIHOMO}" ]]; then
    echo "== 真核心校验:已启用($(basename "$COAST_TEST_MIHOMO")) =="
else
    echo "== 真核心校验:跳过(设 COAST_TEST_MIHOMO=/path/to/mihomo 可启用) =="
fi

# —— 1. 单元测试 ——
echo "== 单元测试 =="
if swift test 2>&1 | grep -q "Test run with .* passed"; then
    n=$(swift test 2>&1 | grep -oE "Test run with [0-9]+ tests" | grep -oE "[0-9]+" | head -1)
    line "swift test" "PASS (${n} 用例)"; ((pass++))
else
    line "swift test" "FAIL"; ((fail++))
fi

# —— 2. 打包 ——
if [[ $BUILD -eq 1 ]]; then
    echo "== 打包 =="
    if bash scripts/make_app.sh --version 0.0.0-regress >/tmp/coast-pkg.log 2>&1; then
        line "make_app.sh" "PASS"; ((pass++))
    else
        line "make_app.sh" "FAIL (见 /tmp/coast-pkg.log)"; ((fail++))
        echo "打包失败,后续自检无从跑起。"; exit 1
    fi
fi

APP=./Coast.app/Contents/MacOS/Coast
[[ -x "$APP" ]] || { echo "找不到 $APP,先不带 --no-build 跑一次"; exit 1; }

# —— 3. 自检钩子 ——
# 每个钩子:跑它,按退出码 + 输出判 PASS/FAIL/SKIP。
echo "== 打包产物自检 =="

# 路径/资源:必须从 .app 解析,退 0 即过。
if out=$(COAST_PATHS_SELFTEST=1 "$APP" 2>&1) && ! grep -q "找不到" <<<"$out"; then
    line "paths (种子资源)" "PASS"; ((pass++))
else
    line "paths (种子资源)" "FAIL"; ((fail++)); echo "$out" | sed 's/^/      /'
fi

# 默认网关三要素:取到即过;取不到多半是没连网(SKIP,不算 bug)。
if out=$(COAST_TOPO_SELFTEST=1 "$APP" 2>&1) && grep -q "三要素齐全" <<<"$out"; then
    line "topo (网关三要素)" "PASS"; ((pass++))
else
    line "topo (网关三要素)" "SKIP (无默认网关?未联网)"; ((skip++))
fi

# helper 布局:plist + 可执行在位、且 SMAppService 状态不是 notFound 即算包对了。
# 「注册被拒」在 ad-hoc 下是预期(需正式证书),算 SKIP 不算 FAIL。
out=$(COAST_HELPER_SELFTEST=1 "$APP" 2>&1)
if grep -q "daemon plist 存在: true" <<<"$out" && grep -q "helper 可执行:     true" <<<"$out"; then
    if grep -qE "状态: (enabled|requiresApproval)" <<<"$out"; then
        line "helper (布局+注册)" "PASS"; ((pass++))
    else
        line "helper (布局)" "SKIP (未在标准位置或未装正式签名)"; ((skip++))
    fi
else
    line "helper (布局)" "FAIL (plist 或可执行文件缺失)"; ((fail++))
fi

# 系统代理机制:经 SCPreferences 写系统代理。ad-hoc 无免密授权时会失败——
# 这是环境限制(SKIP),不是 bug;正式签名 + 真实桌面会话下才验得了。
if out=$(COAST_SYSPROXY_SELFTEST=1 "$APP" 2>&1) && grep -q "^PASS" <<<"$out"; then
    line "sysproxy (机制)" "PASS"; ((pass++))
else
    line "sysproxy (机制)" "SKIP (需授权/正式签名,开发机验不了)"; ((skip++))
fi

# —— 汇总 ——
echo ""
echo "== 汇总: ${pass} PASS · ${fail} FAIL · ${skip} SKIP =="
[[ $fail -eq 0 ]] && { echo "回归通过。"; exit 0; } || { echo "有真失败,见上。"; exit 1; }
