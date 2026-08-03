#pragma once

// Windows 透明网关的**数据面**：WFP 的 ALE_CONNECT_REDIRECT 连接重定向。
// 与 Linux 的 TproxyRules（nftables TPROXY）、macOS 的 PfRules（pf rdr + DIOCNATLOOK）三足对应。
//
// ★★ 本文件目前**只有设计与实测前提，没有实现** —— 因为落地被硬性条件挡住（见下方「阻塞」）。
//    放在这里是为了把已经查证/实测清楚的东西固定下来，避免下次从零再猜一遍。
//
// ── ★ 现状实测：**当前的 lwIP + Npcap 网关在 Windows 上没能把设备带通**（2026-08-03）──
//   在动驱动之前，先把"今天这条路到底行不行"量清楚了。测法：在 239 上新建一张 macvlan
//   （192.168.20.210 / 02:00:00:00:00:10，Pi 没在代理它，与生产网关零冲突），让 Windows 上的
//   Coast 用 COAST_GATEWAY_TESTDEV 接管它，全程用高权限计划任务跑（Npcap 抓包要管理员）。
//
//   App 侧一切正常：日志 `[TESTDEV] enabling proxy for 192.168.20.210 (…) inLan=1`，
//   核心在跑并监听 127.0.0.1:7899，full.yaml 里 coast-gateway 的 users 有该设备的用户名。
//   **但设备立刻断网**（接管前 curl=200，接管后=000，停掉网关后又回到 200）。
//
//   gateway-diag.log 给出了确切位置（10s 一行）：
//       rx=96(10/s) fed=12 bypLan=6 nonVictim=72 tx=46(5/s)
//       tcpAcc=0 tcpXmit=0 tcpRecv=0 socksFail=0 pcb=0/1/0 rxdrop=0 txdrop=0
//   · rx>0 → **Npcap 抓包是工作的**；fed>0 → 帧确实喂进了 lwIP；tx>0 → 我们也在发；
//   · 但 **tcpAcc=0 / tcpXmit=0 / pcb=0** → lwIP **一条 TCP 都没接受过**；
//   · socksFail=0 说明连"拨 SOCKS 失败"都没发生 —— 根本没走到那一步；
//   · 且被接管设备连网关的 ARP 都解析不出来（`ip neigh` 为空）。
//   综合看，卡点在 **ARP/NDP 投毒这一层**（设备根本没把包发给我们），而不是 lwIP 或核心。
//
//   ★★ 追查到底了：**Windows 发出的二层帧根本没到局域网**（在设备侧抓包判定）。
//      在设备所在的 239 上对 ens18 抓 200 个 ARP/ICMPv6 帧，同时跑 Windows 网关：
//        · 来自 Windows MAC（bc:24:11:8f:4d:f1）的帧 —— **一个都没有**；
//        · 同一份抓包里，**Pi（物理机）的投毒帧清清楚楚**：
//          `2c:cf:67:95:32:80 > 02:00:00:00:00:0X … Reply 192.168.20.1 is-at 2c:cf:67:95:32:80`
//      抓包点没问题、同样的手法在物理机上可见 → 问题在 **Windows 的发送路径**。
//      而 Windows 侧诊断是 `tx=46(5/s) txdrop=0` —— 帧已经交给 Npcap 且没被它丢，
//      所以卡在 **Npcap→模拟网卡→hypervisor** 这一段，**不在 lwIP，也不在我们的网关逻辑**。
//      再加一组**对照**把 hypervisor 洗清了：同一次抓包里同时看
//        · Windows **内核**发的 ICMP：`bc:24:11:8f:4d:f1 > … ICMP echo request` —— **到了**
//        · Windows **内核**发的 ARP Request：`Request who-has 192.168.20.239 tell …51` —— **到了**
//        · **Npcap 注入**的投毒帧（Reply 192.168.20.1 is-at bc:24:…）—— **一个都没有**
//        · 同期 Pi 的同类投毒帧 —— 到了（证明抓得到这类帧）
//      内核帧照常通过，说明虚拟交换机没有拦我们；而且投毒帧用的**本来就是自己的源 MAC**，
//      压根不涉及 MAC 伪造。**所以不是 hypervisor 过滤，是 Npcap 的发送路径在这台机器上不生效。**
//      我们这侧 `pcap_sendpacket` 的返回值是查了的（sendOneRaw 判 ==0），诊断也是 txdrop=0 ——
//      即 Npcap **报告成功但帧没上线**。
//      → 待办：在**物理 Windows 机 / 换非模拟网卡**上复跑一次即可定性；或换用 Packet.dll 的
//        批量发送路径与逐帧路径互相对照（COAST_GW_TXBATCH=0 可强制逐帧）看是否只有其中一条坏。
//      ★ 顺带暴露出一个**产品问题**（尚未修）：发送不生效时，被接管设备**直接断网**，而界面
//        依旧显示"已代理"，用户拿不到任何信号 —— 诊断计数里其实全有（tcpAcc=0/fed≈0），
//        只是没往 UI 报。做成自动检测要小心误报（真正空闲的设备本来就没流量），
//        比较稳的做法是开网关时先做一次**发送自检**（注入一帧、看自己的抓包收不收得到），
//        但那需要一台"发送正常"的 Windows 做正对照才敢上线，本机验不了。
//
//   ★ 对"上万兆"的意义：无论上面那条是不是虚拟机造成的，lwIP 这条路本身已被实测判死
//     （单线程、0.95 核/Gbps、单核封顶 ~1.05 Gbps，见 NetStack.h），所以 Windows 要上万兆
//     只能走本文件描述的内核重定向；这次实测只是又说明"现状连千兆都还没验通"。
//
// ── 为什么是 WFP callout，而不是 WinDivert 之类的用户态方案 ──────────────────
// Windows 上想做「透明地把别人的连接引到本机代理」，只有 WFP 的 ALE_CONNECT_REDIRECT 这一条
// 是原生且被官方支持的路：
//   ① 内核 callout 注册在 FWPM_LAYER_ALE_CONNECT_REDIRECT_V4/V6 层，在连接建立**之前**把目的
//      地址改写成本机代理端口，并用 FwpsRedirectHandleCreate0 拿到的句柄填进
//      FWPS_CONNECT_REQUEST0::localRedirectHandle；
//   ② 用户态代理服务收到这条被重定向的连接后，对**它自己那个 socket** 发
//      WSAIoctl(SIO_QUERY_WFP_CONNECTION_REDIRECT_RECORDS) 与
//      WSAIoctl(SIO_QUERY_WFP_CONNECTION_REDIRECT_CONTEXT)，取回**重定向前的原始目的地**。
//      （Windows 8 起提供；只有在 ALE_CONNECT_REDIRECT 层被重定向的连接才查得到。）
//   这套的语义与另外两个平台一一对应：
//      Linux   TPROXY 不改地址 + fwmark 策略路由        → 核心 tproxy 入站
//      macOS   pf rdr 改地址 + ioctl(DIOCNATLOOK) 还原  → 核心 redir 入站
//      Windows WFP 改地址 + WSAIoctl(...REDIRECT_RECORDS) 还原 → 需要核心支持这套取回方式
//
// ★ **用户态方案（WinDivert 等）在架构上做不到**，别再往那个方向试：
//   Windows 在 WinDivert 所处层之下**强制 TCP 反欺骗** —— 用户态代码无法注入「源地址是外部
//   IP」的包并让 Windows TCP 栈接受它。也就是说「在用户态伪造 TCP 对端」在 Windows 上是
//   架构性不可能，这正是必须写内核 callout 的根本原因（而不是"用户态更麻烦"）。
//   另：多个 callout 驱动都做「拦下→原样重注入」时会形成无限循环，这也是 WinDivert 文档
//   自己点名的坑。
//
// ── 阻塞：本机（192.168.20.51）四个前提**全部不满足**，实测确认 ────────────────
//   磁盘         C: 仅 4.7 GB 可用 / 共 98.9 GB   —— WDK + VS 需 10 GB 以上，装不下
//   Windows Kits **未安装**                        —— 没有 km/fwpsk.h、km/wdm.h，驱动无从编译
//   Visual Studio **未安装**                       —— 没有 MSVC 驱动工具链
//   Secure Boot  **已开启**                        —— 未签名驱动无法加载；要么关 Secure Boot
//                                                    并开 testsigning，要么用 EV 证书做正式签名
//   （已装的 npcap / wintun 服务在跑，但它们是二层抓包与虚拟网卡，**替代不了**连接重定向。）
//
//   这四条里，磁盘与工具链属于"环境可以准备"，而 **Secure Boot + 驱动签名**是产品级问题：
//   要发给真实用户，驱动必须有 EV 代码签名证书并通过 Microsoft 硬件门户的 attestation 签名，
//   否则用户机器上加载不了。这一步不是写代码能解决的，需要先有证书。
//
// ── 因此当前 Windows 仍走 lwIP ─────────────────────────────────────────────
//   AppConfig 里 gatewayTproxy / gatewayPf 在 Windows 上恒为 false，数据面是 lwIP + Npcap。
//   代价与 Linux 侧实测一致：lwIP 单线程，跑满千兆约 0.9 核、单核封顶 ≈1 Gbps，**到不了万兆**。
//   在拿到驱动签名条件之前，Windows 的万兆目标无法达成 —— 这是事实，不是没做完。
//
// ── 真要做时的落地顺序（每一步都可单独验证，别一口气写完再调）──────────────────
//   1. 备好环境：腾出 ≥15 GB 磁盘、装 VS + WDK；开发机关 Secure Boot 并
//      `bcdedit /set testsigning on`（仅开发机；发布必须走 EV 签名 + attestation）。
//   2. 先写**最小 callout**：只注册 ALE_CONNECT_REDIRECT_V4、对固定某个目的 IP 做重定向，
//      验证 inf 安装、驱动加载、连接确实被改道。
//   3. 用户态侧验证 WSAIoctl 两个 IOCTL 能取回原始目的地（对应 macOS 那边 DIOCNATLOOK 的角色）。
//   4. 接核心：确认核心能以这种方式拿到原始目的地（Linux 用 tproxy 入站、macOS 用 redir 入站，
//      Windows 需要确认核心是否有对应入站类型；没有就得在我们自己的 fork 里加）。
//   5. 最后才是 WfpRedirect 这一层的 install/remove/syncDevices，与 TproxyRules/PfRules 同构。
//
// 参考（都已读过并据此写成上面的结论）：
//   Using Bind or Connect Redirection (Microsoft Learn)
//   SIO_QUERY_WFP_CONNECTION_REDIRECT_RECORDS / _CONTEXT (Microsoft Learn)
//   WinDivert 2.2 文档中关于 TCP 反欺骗与重注入循环的说明

// 有意不声明任何类：没有实现，声明了只会诱使调用方去接一个空壳。
// 真正动工时在这里加 class WfpRedirect，接口照 TproxyRules / PfRules 的形状来。
