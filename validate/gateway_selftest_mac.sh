#!/usr/bin/env bash
# 透明网关 headless 自测（macOS）——等价于 Linux 版，但用 macOS 的 **feth**（成对虚拟以太网，
# 无需 kext）代替 TAP，且**真跑一遍 L2Endpoint_mac(BPF)**：
#   feth0 <-> feth1 对等；Coast 的 BPF 绑 feth0，内核侧 IP/路由挂 feth1。curl 发出的帧经 feth1→feth0
#   被 BPF 读到 → NetStack 终结 → 假 SOCKS；回包 BPF 写 feth0→feth1→内核→curl。静态 arp 代替真实 ARP。
#
# 需以 **root** 运行（BPF + ifconfig 都要）：CI 里 `sudo bash validate/gateway_selftest_mac.sh <Coast二进制>`。
set -u

BIN="${1:?用法: sudo bash gateway_selftest_mac.sh /path/to/Coast}"
F0=feth0
F1=feth1
LOCALMAC="02:00:00:00:00:01"   # Coast 用户态栈 netif MAC（= 静态 arp 目标）
VICTIM_IP="10.9.9.1"           # feth1(内核侧) IP = 被劫持设备 IP
SERVER_IP="192.0.2.10"         # 目标服务器（TEST-NET-1，经 host 路由挂 feth1）
SOCKS_PORT="7899"

fail() { echo "SELFTEST-HARNESS(mac): FAIL — $*" >&2; exit 1; }
[ "$(id -u)" = "0" ] || fail "需要 root"
[ -x "$BIN" ] || fail "找不到可执行文件: $BIN"

cleanup() {
  [ -n "${COAST_PID:-}" ] && kill "$COAST_PID" 2>/dev/null
  ifconfig "$F0" destroy 2>/dev/null
  ifconfig "$F1" destroy 2>/dev/null
}
trap cleanup EXIT

# 建 feth 对 + 配 IP/路由/静态 arp（无真实 ARP）
ifconfig "$F0" create || fail "建 $F0 失败"
ifconfig "$F1" create || fail "建 $F1 失败"
ifconfig "$F0" peer "$F1" || fail "peer 失败"
ifconfig "$F1" inet "$VICTIM_IP" netmask 255.255.255.0
ifconfig "$F0" up
ifconfig "$F1" up
VICTIM_MAC="$(ifconfig "$F1" | awk '/ether/{print $2; exit}')"
[ -n "$VICTIM_MAC" ] || fail "取 $F1 MAC 失败"
route -n add -host "$SERVER_IP" -interface "$F1" >/dev/null 2>&1
arp -s "$SERVER_IP" "$LOCALMAC" >/dev/null 2>&1 || fail "arp -s 失败"

echo "SELFTEST-HARNESS(mac): $F0<->$F1 victimMac=$VICTIM_MAC server=$SERVER_IP"

COAST_LOG="$(mktemp)"
QT_QPA_PLATFORM=offscreen \
COAST_GATEWAY_SELFTEST=1 \
COAST_GATEWAY_DEBUG=1 \
COAST_SELFTEST_TAP="$F0" \
COAST_SELFTEST_LOCALMAC="$LOCALMAC" \
COAST_SELFTEST_VICTIM_IP="$VICTIM_IP" \
COAST_SELFTEST_VICTIM_MAC="$VICTIM_MAC" \
COAST_SELFTEST_SOCKSPORT="$SOCKS_PORT" \
  "$BIN" >"$COAST_LOG" 2>&1 &
COAST_PID=$!

for i in $(seq 1 20); do
  grep -q "假 SOCKS 就绪" "$COAST_LOG" && break
  kill -0 "$COAST_PID" 2>/dev/null || { cat "$COAST_LOG"; fail "Coast 提前退出"; }
  sleep 0.3
done

CURL_OUT="$(curl -s -m 8 "http://$SERVER_IP/" 2>/dev/null || true)"
echo "SELFTEST-HARNESS(mac): curl 返回 = '$CURL_OUT'"
wait "$COAST_PID"; COAST_RC=$?
COAST_PID=""
echo "----- coast selftest log -----"; cat "$COAST_LOG"; echo "------------------------------"
rm -f "$COAST_LOG"

if [ "$COAST_RC" = "0" ] && echo "$CURL_OUT" | grep -q "COAST_SELFTEST_OK"; then
  echo "SELFTEST-HARNESS(mac): PASS ✅ BPF 端点 + 用户态栈转发 + 每设备身份 + 双向通"
  exit 0
fi
fail "coast_rc=$COAST_RC curl='$CURL_OUT'"
