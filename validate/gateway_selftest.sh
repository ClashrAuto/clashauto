#!/usr/bin/env bash
# 透明网关 headless 自测（Linux）——验证「用户态栈 + SOCKS 桥接」的转发逻辑，**不需要真实 ARP、不需要真机**。
#
# 原理：建一个 TAP 设备当「被劫持设备」的链路，用**真实内核 TCP**（curl）发起连接：
#   curl → 内核经 TAP 发帧 → Coast(COAST_GATEWAY_SELFTEST) 的 NetStack 终结 → 拨内置假 SOCKS5。
#   静态邻居(ip neigh)代替 ARP，故「不验证真实 ARP，只验证栈/转发逻辑」。
#   因为 lwIP 的 accept 只在三次握手完成后触发，假 SOCKS 收到带正确用户名的 CONNECT 即证明整条链路通。
#
# 用法:  sudo bash validate/gateway_selftest.sh /path/to/coast
# 依赖:  root、/dev/net/tun、iproute2、curl。返回 0=通过。
set -u

BIN="${1:-./coast}"
TAP="cst0"
LOCALMAC="02:00:00:00:00:01"     # Coast 用户态栈 netif 的 MAC（= 静态邻居目标）
VICTIM_IP="10.9.9.1"             # TAP 侧内核 IP（= 被劫持设备 IP）
SERVER_IP="192.0.2.10"           # 目标服务器（TEST-NET-1，不可路由；经 /32 路由进 TAP）

fail() { echo "SELFTEST-HARNESS: FAIL — $*" >&2; exit 1; }
[ "$(id -u)" = "0" ] || fail "需要 root"
[ -e /dev/net/tun ] || fail "无 /dev/net/tun"
[ -x "$BIN" ] || fail "找不到可执行文件: $BIN"

# ── 假 SOCKS 的监听口：动态挑空闲的，**绝不能写死 7899** ────────────────────────
# 7899 正是 Coast 网关自己的 SOCKS 监听口。在**真网关机**上跑这个门禁（恰恰是最该跑它的
# 地方——那里有真核心、真设备台账）必然撞车：假 SOCKS 绑不上 → NetStack 转而拨到**真核心** →
# 15s 超时 → FAIL。基线恒红的门禁等于没有门禁，只会训练人无视它。实测（Pi，真核心占着
# 7899）就是这个下场：NETSTACK ACCEPT 一路正常，纯粹卡在假 SOCKS 起不来。
pick_free_port() {
  local used p
  used="$(ss -lnt 2>/dev/null | tail -n +2 | awk '{print $4}' | sed 's/.*://')"
  for p in $(seq 17899 17999); do
    printf '%s\n' "$used" | grep -qx "$p" || { printf '%s\n' "$p"; return 0; }
  done
  return 1
}
SOCKS_PORT="$(pick_free_port)" || fail "17899-17999 无空闲端口可给假 SOCKS"
echo "SELFTEST-HARNESS: 假 SOCKS 端口 = $SOCKS_PORT（动态挑选，避开真核心的 7899）"

# ── NDP RA 解析自测（纯逻辑，不碰网络，毫秒级）─────────────────────────────
# runNdpRaSelfTest 早就写好、也有 COAST_NDP_RA_SELFTEST 钩子，但**从来没被任何发布验证
# 触发**（此前 validate 只跑下面的数据面自测）。而 parseRouterAdvert 是「从 RA 学 v6 路由器」
# 的核心、也是「双栈设备 v6 漏代理」这类静默故障的唯一防线 —— 回归它，门禁却毫无察觉。
# 它无需 TAP/网络，在这里顺手跑掉，把它纳入发布门禁。返回非 0 即整体失败。
echo "SELFTEST-HARNESS: 先跑 NDP RA 解析自测（纯逻辑）"
if ! COAST_NDP_RA_SELFTEST=1 QT_QPA_PLATFORM=offscreen "$BIN" >/dev/null 2>&1; then
  fail "NDP RA 解析自测失败（parseRouterAdvert 回归）"
fi
echo "SELFTEST-HARNESS: NDP RA 解析自测 PASS"

cleanup() {
  [ -n "${COAST_PID:-}" ] && kill "$COAST_PID" 2>/dev/null
  ip link del "$TAP" 2>/dev/null
}
trap cleanup EXIT

# 建 TAP + 配 IP/路由/静态邻居（无 ARP）
ip tuntap add dev "$TAP" mode tap || fail "建 TAP 失败"
ip addr add "$VICTIM_IP/24" dev "$TAP"
ip link set "$TAP" up
sysctl -q -w "net.ipv4.conf.$TAP.rp_filter=0" 2>/dev/null || true
# 关掉 TAP 的校验和/分段卸载：否则内核写入 TAP 的帧校验和不完整，会被下游误判（双保险，
# lwipopts 已把入站校验和校验关掉）。
ethtool -K "$TAP" tx off rx off tso off gso off gro off 2>/dev/null || true
VICTIM_MAC="$(cat /sys/class/net/$TAP/address)"
ip route add "$SERVER_IP/32" dev "$TAP"
ip neigh replace "$SERVER_IP" lladdr "$LOCALMAC" dev "$TAP" nud permanent

echo "SELFTEST-HARNESS: tap=$TAP victimMac=$VICTIM_MAC server=$SERVER_IP"

# 起 Coast 自测（headless / offscreen）
COAST_LOG="$(mktemp)"
QT_QPA_PLATFORM=offscreen \
COAST_GATEWAY_SELFTEST=1 \
COAST_GATEWAY_DEBUG=1 \
COAST_SELFTEST_TAP="$TAP" \
COAST_SELFTEST_LOCALMAC="$LOCALMAC" \
COAST_SELFTEST_VICTIM_IP="$VICTIM_IP" \
COAST_SELFTEST_VICTIM_MAC="$VICTIM_MAC" \
COAST_SELFTEST_SOCKSPORT="$SOCKS_PORT" \
  "$BIN" >"$COAST_LOG" 2>&1 &
COAST_PID=$!

# 等假 SOCKS 就绪
for i in $(seq 1 20); do
  grep -q "假 SOCKS 就绪" "$COAST_LOG" && break
  kill -0 "$COAST_PID" 2>/dev/null || { cat "$COAST_LOG"; fail "Coast 提前退出"; }
  sleep 0.3
done

# —— 诊断：路由/链路/邻居/是否有帧真的到 TAP ——
echo "----- diag: link -----";      ip -br link show "$TAP" || true
echo "----- diag: addr -----";      ip -br addr show "$TAP" || true
echo "----- diag: route get -----"; ip route get "$SERVER_IP" || true
echo "----- diag: neigh -----";     ip neigh show dev "$TAP" || true
# 抓 TAP 上前几帧（看内核有没有把 SYN 发到 tap）
TCPD_OUT="$(mktemp)"
( timeout 10 tcpdump -i "$TAP" -c 5 -nne 2>/dev/null >"$TCPD_OUT" ) &
TCPD_PID=$!
sleep 0.5

# 真实内核 curl → SERVER_IP（经 TAP → NetStack → 假 SOCKS），-v 存 stderr
CURL_ERR="$(mktemp)"
CURL_OUT="$(curl -sv -m 8 "http://$SERVER_IP/" 2>"$CURL_ERR" || true)"
echo "SELFTEST-HARNESS: curl 返回 = '$CURL_OUT'"
wait "$TCPD_PID" 2>/dev/null || true
echo "----- diag: tcpdump on $TAP -----"; cat "$TCPD_OUT"; echo "(end)"
echo "----- diag: curl -v -----";        tail -15 "$CURL_ERR"
rm -f "$TCPD_OUT" "$CURL_ERR"

# 等 Coast 自测退出并取其返回码（0=收到带正确用户名的 CONNECT）
wait "$COAST_PID"; COAST_RC=$?
COAST_PID=""
echo "----- coast selftest log -----"; cat "$COAST_LOG"; echo "------------------------------"
rm -f "$COAST_LOG"

# 判定：Coast 侧证明「帧→握手→catch-all→SOCKS+身份」通；curl 侧证明回程通。
if [ "$COAST_RC" = "0" ] && echo "$CURL_OUT" | grep -q "COAST_SELFTEST_OK"; then
  echo "SELFTEST-HARNESS: PASS ✅ 用户态栈转发 + 每设备身份 + 双向通"
  exit 0
fi
fail "coast_rc=$COAST_RC curl='$CURL_OUT'（未同时满足：CONNECT 带正确用户名 + 回程 HTTP 标记）"
