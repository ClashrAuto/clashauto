#!/bin/sh
# 验证 TunSession 的 Linux 策略路由：本进程打了 fwmark 的包必须绕开 TUN，其它包必须进 TUN。
#
# 跑法（Docker 容器自带独立 netns，不碰宿主网络；不需要 Linux 开发机）：
#   docker run --rm -i --privileged alpine:3.20 sh -s < tools/tunroute/rulecheck.sh
#
# 实测结果（2026-07-31，Docker 29.6.2 / alpine 3.20）：
#   A 组 不带mark→coast0  带mark→coast0   ← 错误配置下 SO_MARK 完全失效（反向对照成立）
#   B 组 不带mark→coast0  带mark→eth0     ← 修好后普通流量进 TUN、自身出站走物理口
#   === 策略路由验证 PASS ===
#
# 用 `ip route get <ip> [mark <m>]` 直接问内核"这样的包会走哪"——不产生任何真实流量，
# 也不需要真的把包发出去，是这条规则最干净的验法。
#
# 关键在于**两组都要跑**：
#   A 组 = 我原先写错的配置（路由进 main + rule 查 main）→ 必须**验出问题**，否则这个测试是空的
#   B 组 = 修好的配置（路由进独立表 989）→ 必须两条都对
# 只跑 B 组的话，"测试通过"可能只是因为这个测试压根测不出区别。
set -e
apk add --no-cache iproute2 >/dev/null 2>&1
MARK=0x43535431
TABLE=989
PHYS=$(ip -o route show default | head -1 | sed 's/.*dev \([^ ]*\).*/\1/')
echo "物理出口网卡：$PHYS"

ip tuntap add dev coast0 mode tun
ip addr add 198.18.0.1/24 dev coast0
ip link set coast0 up

# 问内核：去 1.1.1.1 的包（带/不带 mark）分别从哪张网卡出去
dev_for() { ip route get 1.1.1.1 $1 2>/dev/null | sed -n 's/.*dev \([^ ]*\).*/\1/p' | head -1; }

echo ""
echo "===== A 组：我原先写错的配置（/1 路由进 main 表）====="
ip rule add fwmark $MARK lookup main pref 100
ip route add 0.0.0.0/1 dev coast0
ip route add 128.0.0.0/1 dev coast0
A_PLAIN=$(dev_for "")
A_MARK=$(dev_for "mark $MARK")
echo "  不带 mark → $A_PLAIN   (期望 coast0)"
echo "  带 mark   → $A_MARK   (期望 $PHYS，但错误配置下应当仍是 coast0)"
if [ "$A_MARK" = "coast0" ]; then
    echo "  ✓ 反向对照成立：错误配置确实让 SO_MARK 完全失效（这正是被修掉的 bug）"
    NEG_OK=1
else
    echo "  ✗ 反向对照不成立：错误配置竟然也能绕开 TUN —— 那说明这个测试测不出区别，B 组的结论不作数"
    NEG_OK=0
fi
ip route del 0.0.0.0/1 dev coast0
ip route del 128.0.0.0/1 dev coast0
ip rule del fwmark $MARK lookup main pref 100

echo ""
echo "===== B 组：修好的配置（/1 路由进独立表 $TABLE）====="
ip rule add fwmark $MARK lookup main pref 100
ip rule add lookup $TABLE pref 200
ip route add 0.0.0.0/1 dev coast0 table $TABLE
ip route add 128.0.0.0/1 dev coast0 table $TABLE
B_PLAIN=$(dev_for "")
B_MARK=$(dev_for "mark $MARK")
echo "  不带 mark → $B_PLAIN   (期望 coast0：普通流量进 TUN)"
echo "  带 mark   → $B_MARK   (期望 $PHYS：我们自己的出站走物理口)"
echo ""
ip rule show

OK=1
[ "$B_PLAIN" = "coast0" ] || { echo "✗ 普通流量没进 TUN"; OK=0; }
[ "$B_MARK" = "$PHYS" ] || { echo "✗ 打了 mark 的流量没走物理口 —— 环路仍在"; OK=0; }
[ "$NEG_OK" = "1" ] || OK=0

echo ""
if [ "$OK" = "1" ]; then echo "=== 策略路由验证 PASS ==="; else echo "=== 策略路由验证 FAIL ==="; fi
[ "$OK" = "1" ]
