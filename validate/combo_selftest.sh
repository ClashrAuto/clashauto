#!/usr/bin/env bash
# 组合自测：「透明网关已在跑 + 再开进程内 TUN(增强)」——本次线上故障的验收标准。
#
# 故障回顾：lwIP 全进程只能有一份协议栈；而 DevicesController 每轮局域网扫描都会
# ensureGatewayConfigured() → 网关工作线程把栈建起来。进程内 TUN 当时自己 new 一个，
# init() 恒被判重拒掉 → 用户点「增强」永远报「已有一个网关协议栈实例在运行」。
# 修法：TUN 挂到同一份栈上当第二张网卡（NetStack::addNic 本来就支持多 netif）。
#
# 本脚本只负责**造环境**（TAP + 路由 + 静态邻居），判定全在进程内（见 GatewaySelfTest.h
# runComboSelfTest 的四条断言）。两条路各有独立判据，一条通不能替另一条作数：
#   · 网关路：curl → TAP → lwIP → 假 SOCKS，判据 = 假 SOCKS 的 CONNECT 计数 + 每设备用户名
#   · TUN 路：curl → TUN → lwIP → 进程内 DIRECT → 真目标，判据 = 真实 HTTP 返回码
#
# 用法:  sudo bash validate/combo_selftest.sh /path/to/coast [1|2|both]
#          1 = 先网关后 TUN（复现线上顺序，默认）  2 = 先 TUN 后网关（反向顺序）
#          both = 依次跑两种顺序，全过才算过
# 依赖:  root、/dev/net/tun、iproute2、curl、能上外网（TUN 路要真出网）。返回 0=通过。
set -u

BIN="${1:-./coast}"
MODE="${2:-1}"
TAP="cst0"
LOCALMAC="02:00:00:00:00:01"     # Coast 用户态栈 netif 的 MAC（= 静态邻居目标）
VICTIM_IP="10.9.9.1"             # TAP 侧内核 IP（= 被劫持设备 IP）
SERVER_IP="192.0.2.10"           # 网关路的靶（TEST-NET-1，不可路由；经 /32 路由进 TAP）
SOCKS_PORT="7899"
TUN_TARGET="${COAST_TUNSERVICE_TARGET:-http://223.5.5.5/}"

fail() { echo "COMBO-HARNESS: FAIL — $*" >&2; exit 1; }
[ "$(id -u)" = "0" ] || fail "需要 root"
[ -e /dev/net/tun ] || fail "无 /dev/net/tun"
[ -x "$BIN" ] || fail "找不到可执行文件: $BIN"

cleanup() {
  ip rule del pref 50 2>/dev/null
  ip link del "$TAP" 2>/dev/null
  # TUN 侧的路由/规则由进程自己还原（TunSession::stop）；这里只兜底删残留的网卡。
  ip link del coast0 2>/dev/null
}
trap cleanup EXIT

setup_tap() {
  ip link del "$TAP" 2>/dev/null
  ip tuntap add dev "$TAP" mode tap || fail "建 TAP 失败"
  ip addr add "$VICTIM_IP/24" dev "$TAP"
  ip link set "$TAP" up
  sysctl -q -w "net.ipv4.conf.$TAP.rp_filter=0" 2>/dev/null || true
  # 关掉 TAP 的校验和/分段卸载：否则内核写入 TAP 的帧校验和不完整，会被下游误判。
  ethtool -K "$TAP" tx off rx off tso off gso off gro off 2>/dev/null || true
  VICTIM_MAC="$(cat /sys/class/net/$TAP/address)"
  ip route add "$SERVER_IP/32" dev "$TAP"
  ip neigh replace "$SERVER_IP" lladdr "$LOCALMAC" dev "$TAP" nud permanent
  # ★★ 这条规则是**测试脚手架**，不是被测行为，别删（第一版漏了它，第一次真机跑就栽了）：
  #   TunSession 装的是「ip rule pref 200 → table 989（0.0.0.0/1 + 128.0.0.0/1 dev coast0）」。
  #   pref 200 比 main 的 32766 优先，所以 TUN 一开，**连 $SERVER_IP/32 dev cst0 这条更具体的路由
  #   都被绕过去了** —— 本机发往网关靶的 curl 会拐进 TUN。
  #   现象很有迷惑性：假 SOCKS 照样收到 CONNECT（因为 TUN 那条路的出站工厂是同一份、也会回退到它），
  #   只是用户名成了 'local' 而不是被劫持设备的 dev-<mac>；只看「有没有 CONNECT」的话会误判成通过。
  #   真实场景里网关的帧是**别的机器**经 ARP 投毒送到物理网卡上的，根本不查本机路由表；
  #   本机自己 curl 打 TAP 是脚手架特有的走法，所以这里用 pref 50 把它钉回 main 表。
  ip rule del pref 50 2>/dev/null
  ip rule add to "$SERVER_IP/32" lookup main pref 50
}

run_one() {
  local m="$1"
  echo "=============================================================="
  echo "COMBO-HARNESS: 顺序 $m（1=先网关后 TUN，2=先 TUN 后网关）"
  setup_tap
  echo "COMBO-HARNESS: tap=$TAP victimMac=$VICTIM_MAC server=$SERVER_IP tunTarget=$TUN_TARGET"
  QT_QPA_PLATFORM=offscreen \
  COAST_COMBO_SELFTEST="$m" \
  COAST_SELFTEST_TAP="$TAP" \
  COAST_SELFTEST_LOCALMAC="$LOCALMAC" \
  COAST_SELFTEST_VICTIM_IP="$VICTIM_IP" \
  COAST_SELFTEST_VICTIM_MAC="$VICTIM_MAC" \
  COAST_SELFTEST_SOCKSPORT="$SOCKS_PORT" \
  COAST_SELFTEST_SERVER_IP="$SERVER_IP" \
  COAST_TUNSERVICE_TARGET="$TUN_TARGET" \
    timeout 180 "$BIN"
  local rc=$?
  ip link del "$TAP" 2>/dev/null
  echo "COMBO-HARNESS: 顺序 $m 返回码 = $rc"
  return $rc
}

RC=0
case "$MODE" in
  both)
    run_one 1 || RC=$?
    if [ "$RC" = "0" ]; then run_one 2 || RC=$?; fi
    ;;
  *)
    run_one "$MODE" || RC=$?
    ;;
esac

if [ "$RC" = "0" ]; then
  echo "COMBO-HARNESS: PASS ✅ 网关与进程内 TUN 共用一份协议栈、两条路同时可用"
  exit 0
fi
# 2 = 前提不成立（环境本来就不通），3 = 环境错误，1 = 断言失败
fail "组合自测返回 $RC（2=前提不成立/3=环境错误/1=断言失败，详见上面的 COMBO: 行）"
