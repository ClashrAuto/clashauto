#!/usr/bin/env bash
# 透明网关的 Linux 发布门禁。
#
# ── 2026-08-04：这个脚本**瘦了一半**，原因写在这里 ────────────────────────────────
# 它原本做两件事：
#   ① NDP RA 解析自测（纯字节逻辑，不碰网络）
#   ② 用户态栈数据面自测（建 TAP 当"被劫持设备"的链路，真实内核 curl 打进去，
#      验证 NetStack 终结 TCP → 拨假 SOCKS 时带着正确的每设备用户名）
#
# ② 已经删掉：lwIP 被整体移除后，**Linux 的网关数据面是 TPROXY（内核转发），没有用户态栈**
# （macOS 同理，走 pf rdr）。NetStack 只在 Windows 编，非 Windows 是个 init() 直接报错的桩。
# 继续跑 ② 只会得到一条恒红的门禁 —— 而恒红的门禁等于没有门禁，只会训练人无视它。
#
# ② 的等价物没有消失，只是换了平台：`runSmolGatewaySelfTest()`（src/net/RustStackSelfTest.cpp），
# 假二层端点 + 假 SOCKS + 合成帧，跑同一套断言（SYN → SYN-ACK 源端口还原 → SOCKS CONNECT
# 带对设备身份），在 CI 的 windows job 里每次构建都跑，不需要 root、不碰真网卡。
#
# TPROXY 那条路目前**没有**等价的端到端自测 —— 这是一个已知缺口，不是已解决的问题。
# 要补的话，GatewaySelfTest.cpp 里那套 TAP + 静态邻居 + 真实 curl 的脚手架仍然可用。
#
# 用法:  sudo bash validate/gateway_selftest.sh /path/to/coast
# 依赖:  可执行的 coast 二进制。返回 0=通过。
set -u

BIN="${1:-./coast}"

fail() { echo "SELFTEST-HARNESS: FAIL — $*" >&2; exit 1; }
[ -x "$BIN" ] || fail "找不到可执行文件: $BIN"

# ── NDP RA 解析自测（纯逻辑，不碰网络，毫秒级）─────────────────────────────
# parseRouterAdvert 是「从 RA 学 v6 路由器」的核心，也是「双栈设备 v6 漏代理」这类**静默**
# 故障的唯一防线 —— 回归了它，用户的表现只是 v6 流量绕过代理直接出去，不报任何错。
# 这个钩子（COAST_NDP_RA_SELFTEST）曾经写好了却从没被任何发布验证触发过，纳入门禁才算数。
# 它与数据面无关，所以三端通吃，也不受 lwIP 移除的影响。
echo "SELFTEST-HARNESS: NDP RA 解析自测（纯逻辑）"
if ! COAST_NDP_RA_SELFTEST=1 QT_QPA_PLATFORM=offscreen "$BIN" >/dev/null 2>&1; then
  fail "NDP RA 解析自测失败（parseRouterAdvert 回归）"
fi

echo "SELFTEST-HARNESS: PASS ✅ NDP RA 解析"
echo "SELFTEST-HARNESS: 注 —— 数据面自测已迁至 Windows job（coaststack gateway self-test）；"
echo "SELFTEST-HARNESS:      Linux 的 TPROXY 路径目前没有端到端自测，属已知缺口。"
exit 0
