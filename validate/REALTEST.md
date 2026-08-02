# 透明网关 · 真机联调清单（Linux）

CI 的 `gateway-selftest` 只用 TAP + 静态邻居验证了**用户态栈的转发逻辑**（不含真实 ARP）。
真正的「一键代理局域网设备」还得在**有 root 的真实 Linux + 真实局域网 + 一台你自己的测试设备**上验。
本清单帮你一步步确认整条链路，并在出问题时定位。

> ⚠ 只在**你自己的局域网**、劫持**你自己的设备**上测。ARP 投毒会改目标设备的 ARP 缓存，
> 退出/关开关必须能可靠还原，否则设备会短暂断网。

---

## 0. 前提

- 一台真实 Linux（**不是** QEMU NAT 虚机；要能看到真实局域网广播/ARP），x64。
- root 权限或给二进制 `CAP_NET_RAW`（见下）。透明网关走 `AF_PACKET`，无权限时
  `gatewayReady=false`、设备页代理开关不生效。
- 一台**你自己的**测试设备（手机/另一台电脑），和这台 Linux 在**同一网段、同一交换机/AP**下。
- mihomo 核心能正常跑、有可用订阅、这台 Linux 自己能科学上网（先确认基础功能 OK 再测网关）。

## 1. 拿包 & 起程序

从最新 Release 下 `Coast-*-linux-x64-portable.tar.gz`，解开：

```bash
tar xzf Coast-*-linux-x64-portable.tar.gz
cd Coast
# 方式 A：直接 root 跑
sudo ./coast
# 方式 B：给权限后普通用户跑（更符合日常）
sudo setcap cap_net_raw,cap_net_admin+ep ./coast
./coast
```

先在设置页确认：核心在跑、订阅有节点、这台机器自己能上网。

## 2. 设备发现（不需要劫持，先验这步）

进「设备」页 → 应能扫描到局域网设备：本机、网关（都带「保护」徽章、不可劫持）、
以及你的测试设备（显示 IP / MAC / 厂商，能识别到名字/型号更好）。

- 扫不到设备：确认不是 QEMU NAT；`ip -br addr` 看本机网段；防火墙有没有拦 mDNS(5353)/SSDP(1900)/NetBIOS(137)。
- 概览条应显示 在线数 / 网关 IP。

## 3. 开一台设备的「代理网络」

点你测试设备那行的**代理开关**（或进详情页点大开关）。此时后台会：

1. 落 `devices.json` → `ConfigBuilder` 重生成 `full.yaml`：加一个 **`coast-gateway` socks inbound**
   （`127.0.0.1:7899`，带 `users: dev-<去冒号mac>/coast`）+ 该设备的 `IN-USER` 规则（若设了策略）→ 热重载 mihomo。
2. `LanGateway`：对该设备启动 **ARP 双向投毒** + 用户态栈接管其流量 → 每条连接拨 `127.0.0.1:7899`
   （用户名 `dev-<mac>`）进 mihomo。

**要观察的：**

- ✅ 测试设备**还能上网**（能刷网页/视频），且出口 IP 变成你节点的 IP（在设备上开
  `myip.ipip.net` / `ip.sb` 看）。
- ✅ mihomo `/connections` 里，该设备发起的连接 `metadata.inboundUser == dev-<它的mac>`
  （证明每设备身份链路通）。查法：
  ```bash
  # secret 见 config.yaml 的 external-controller secret（若有）
  curl -s http://127.0.0.1:9191/connections | \
    python3 -c "import sys,json;[print(c['metadata'].get('sourceIP'),c['metadata'].get('inboundUser'),c['metadata'].get('host')) for c in json.load(sys.stdin)['connections']]"
  ```
- ✅ 设备详情页出现**实时流量**（速率/会话/累计）和该设备的**连接列表**（热更新）。

## 4. 每设备策略（可选）

详情页「策略」选：规则分流 / 指定节点 / 强制直连 / **禁止上网(REJECT)**。改完会重生成
`full.yaml` 的 `IN-USER` 规则并热重载。验证：

- 选「禁止上网」→ 该设备立刻断网（其它设备不受影响）。
- 选「指定节点」+ 某节点 → 该设备出口 IP 变成那个节点。

## 5. 关开关 / 退出 —— 还原（**安全关键**）

- 关掉该设备的代理开关 → `LanGateway.disableDevice` 会给设备与网关**重发正确 ARP(heal)**，
  设备的 ARP 缓存恢复指向真网关 → **不该断网**。观察设备能立刻继续上网。
- 退出 Coast（`aboutToQuit → disableAll`）→ 同样还原所有被劫持设备。
- **崩溃/被杀**：下次启动 `recoverFromCrash()` 会读 `gateway_active.json`（在
  `~/.local/share/Coast/`）先补发还原 ARP。可手动模拟：开着代理时 `kill -9` coast，
  在设备上确认最多几秒后恢复（或重启 coast 触发 panic-restore）。

## 6. 出问题时定位

开调试日志（把 NetStack 的 IN/OUT/ACCEPT 打到 stderr）：

```bash
COAST_GATEWAY_DEBUG=1 sudo -E ./coast 2> coast.log
# 看：NETSTACK IN（设备帧进栈）/ OUT（回包）/ ACCEPT（握手完成+拨 mihomo，带 user）
```

想让 lwIP 自己打丢包原因：改 `src/net/lwip_port/lwipopts.h` 里 `LWIP_DEBUG 1` +
`IP_DEBUG/TCP_INPUT_DEBUG/ETHARP_DEBUG = LWIP_DBG_ON` 重编（联调套路见 git log）。

**常见现象 → 方向：**

| 现象 | 可能原因 |
|---|---|
| 开关点了但 `gatewayReady=false` / 开关无效 | 无 root/CAP_NET_RAW；或没扫到网关 MAC（先扫一轮） |
| 设备直接断网、上不了 | ARP 投毒生效但回程/栈没通；看 `NETSTACK IN` 有没有、`OUT` 有没有 |
| 有 IN 无 OUT | lwIP 丢了 SYN（开 lwIP 调试看原因） |
| 有 OUT 但设备 RST/连不上 | 源端口/源 IP/校验和问题（对照自测里修过的几类） |
| 能上网但 mihomo 没有该设备连接 | SOCKS 没进 mihomo；确认 `full.yaml` 有 `coast-gateway` listener、端口 7899 |
| `inboundUser` 为空 | mihomo 版本不支持 per-user listener / IN-USER（换较新 mihomo） |
| 路由器有 ARP 绑定/防攻击 | 投毒被拦，属预期限制；换环境或后续做 ARP 之外的方案 |

## 7. 已知边界

- 只在同一广播域（同交换机/AP）内有效；跨 VLAN/隔离 AP（AP 客户端隔离）无效。
- IPv6 目前不接管（只劫持 IPv4）；设备若走 IPv6 会绕过 → 如需彻底代理，设备侧关 IPv6 或后续补。
- Windows(Npcap)/macOS(BPF) 尚未实现（当前为桩，`gatewayReady=false`）。
