# 「完全替换 mihomo 内核」还差什么

按**证据**列出的现状与剩余缺口。每条都标了是**真机验过**、**只编译过**，还是**估计**。
（2026-07-31 大幅更新：数据面已经替换完成，缺口整体移到了控制面与 TUN。）

## 一句话现状

**数据面对「进程内实现了的那部分协议/传输」已能脱离 mihomo，但真实机场订阅里有相当比例的节点
仍必须回退核心** —— 这是 2026-07-31 晚用真实订阅（Pi .91，对照同机 mihomo）复测后修正的结论。
早先「数据面已经不需要 mihomo 了」的证据只有 **DIRECT 直连** 和 **拿 mihomo 当参照服务端的协议握手**，
**真实机场节点从没验过**；一验就暴露两类问题（都已修，见下）：
- **不支持的传输/插件被静默拨错**：`vless/reality network:grpc`、`hysteria2 obfs:salamander`、`ss plugin`
  等，进程内没实现却不报错，拿错的传输去硬拨 → 失败且伪装成「网络不通」。现已在**创建阶段返回
  nullptr 干净回退核心**（传输守卫，见 `proto/OutboundRegistry.h`）。
- **Hysteria2 的真机 bug**：见下节。修完 HK2-HY2 真机可用（对照 mihomo）。

**控制面仍完全依赖 mihomo**（节点列表 / 选择 / 延迟 / 连接 / 流量 / 日志）—— 这是现在真正的大头。

---

## 一、数据面：谁在搬运字节

| 路径 | 状态 | 证据 |
|---|---|---|
| 局域网被代理设备 | ✅ **进程内** | 真机：单请求 **0.70 ms**（同路内核转发 0.5 ms）、饱和 2883 conn/s、失败率 0% |
| 本机应用流量（系统代理） | 🟡 **已接线，端到端未验** | 入站真机验过（7891，strict 下全部 200）；**系统代理改指它这一步需要真实桌面会话，未验** |
| 本机 TUN | 🟡 **链路验通；QUIC 环路 Linux 侧刚补上，端到端绿灯未拿到** | 隔离台 10/10 全 200；**真接管默认路由**的整体自检（DIRECT 出站）两处 PASS。**但真实 QUIC 节点下的环路此前从未真验**——一验就发现 Linux 仍环路，已补 `ip rule from <物理IP> lookup main`（见下节） |

### 真实机场节点：谁能进程内、谁必须回退核心（2026-07-31 真机复测）

用用户真实订阅（Pi `/root/sub-full.yaml`，27 vless / 19 hysteria2 / 10 tuic）+ **不碰 TUN 的出站探针**
（本机混合入站 → strict CoreDialerFactory → 该节点 → `curl -x`）逐节点验，并**同机 mihomo 做对照**：

| 节点类型 | 进程内 | 证据 / 处置 |
|---|---|---|
| `hysteria2`（无 obfs，如 HK2-HY2） | ✅ **可用** | 修完 3 个 bug 后 HK2-HY2 探针 **8/10 → paced 15/15**（curl=404）；对照 mihomo 15/15。剩余偶发失败是该服务端**丢我们最初 1~2 个 Initial**（约 +3s）导致的握手长尾，非协议 bug |
| `vless`+`reality-opts`+`network:grpc`（如 HK-1） | ⛔ **回退核心** | grpc 传输未实现。守卫日志 `[reality] network=grpc 未实现 → 回退核心`。**这正是「进程内拨不通真实节点」最初被发现的那个** |
| `hysteria2`+`obfs:salamander`（如 JP1） | ⛔ **回退核心** | Salamander 包混淆未实现（msquic 自持 UDP socket，不像 quic-go 能注入 PacketConn）。真机：明文拨 **0/12**，经 mihomo **8/8**。守卫已挡 |
| `ss`+`plugin`、`vless/vmess/trojan`+非 tcp/ws 传输 | ⛔ **回退核心** | 同类未实现，守卫一并挡（防静默拨错） |

**三个 Hysteria2 真机 bug（都在「握手成功之后」，最隐蔽）**：
1. **把对端 `STOP_SENDING` 当致命错**：源站响应完就 `Connection: close`，机场服务端据此对我们的**上行**发
   `STOP_SENDING`（=它不再读我们，正常收尾）。旧代码把它和 `RESET_STREAM` 一起当 `failed` → 拆连接 →
   **已到的响应体被丢弃**，表现为 curl=000。已拆成两个信号：`peerReceiveAborted`（STOP_SENDING，忽略）
   vs `failed`（RESET_STREAM）。见 `QuicTransport.cpp` / `Hysteria2Outbound.cpp`。
2. **关连接前不冲刷客户端写缓冲**：`dataReceived` 刚把响应体 `write()` 进 curl 的 socket，紧接着
   `peerSendShutdown` 就 `closeSession` → `QAbstractSocket` 析构不等排空 → 响应体连同对象被丢 → curl=000。
   `MixedInbound::closeSession` 现在**先 `flush()` 再 close**。修此点：curl=000 → 404。
3. **QUIC 握手/空闲太紧**：未设 `HandshakeIdleTimeout`/`IdleTimeout`/`KeepAlive` → 偶发空闲超时。已显式给足
   （对齐 hysteria2 客户端惯例）。（曾试压低 `InitialRttMs` 加速重传，但更激进的 Initial 疑似触发服务端
   反刷限速 → **已回退**，与 mihomo 默认对齐更稳。）

### 「杀掉 mihomo 还能不能活」——能（真机）

```
pkill core → core=NO、7899 无人监听
劫持状态: ['192.168.20.34', '192.168.20.239']   ← 保住（旧行为是全部撤销）
设备连打 10 次 http://223.5.5.5/ → 全部 404 / ~27ms
cc=10/5/0/0/0/0   socksFail=0                   ← 10 条全走进程内
```

为达成这一点，今天拆掉了三层依赖：

1. **出站**：8 协议（Hy2 / Reality / SS / Trojan / TUIC / VLESS / VMess / Direct）+ 传输
   （QUIC / TLS / uTLS / WebSocket）+ 规则引擎 + DNS。
   ⚠️ **传输/插件覆盖是有限的**（真机复测才发现）：vless/vmess 只有 tcp+ws，reality/trojan 只有裸
   TCP+TLS，**grpc/h2/httpupgrade 未实现**；ss **无插件**；hysteria2 **无 obfs(salamander)**；tuic 待
   msquic 2.6。这些**在创建阶段返回 nullptr 干净回退核心**（传输守卫），绝不静默拨错。所以「进程内
   出站」= 「订阅里恰好落在已实现协议+传输里的那些节点」，不是全部。
2. **内核门禁**（`a1f5d1d`）：`onCoreRunningChanged` / `resumeProxies` 原本不看 `coastcore`，
   核心一停就把劫持**全撤**——哪怕进程内引擎完全有能力接管。这是最隐蔽的一层。
3. **策略组映射**（`3cb1cfc`）：「组 → 叶子」原本只来自核心 REST，核心一停 Rule 模式就
   解析不出目标、整类回退到一个已经不在的核心。现在能从自己生成的 `full.yaml` 解析兜底。

### 延迟：已经和内核转发持平

真凶是 **TPACKET_v3 的块超时**（`aeb9d79`），不在 lwIP 也不在出站。详见
[gateway-load-test.md](gateway-load-test.md)。关掉环之后 **11.00 ms → 0.70 ms（15.7×）**，
吞吐还涨 14%。

**多进程被数据否掉**：数据面 `late=0`、零丢弃、CPU 只占单核 15%。瓶颈从来不在并行度上。

### 本机 TUN：数据面已验通（Linux，真机）

`TunEndpoint_linux` 伪装成 `IL2Endpoint`（给裸 IP 包前置 14 字节以太头），于是网关那条路验过的
一整套（netif / 设备表 / accept-all 补丁 / 按目的地选出站）**一行不改**直接复用。验证台见
[`tools/gwbench/tunstack.cpp`](../tools/gwbench/tunstack.cpp)：coast0 移进独立 netns 让"应用"在里面跑，
出站留在主 netns —— 天然无环路，从而把「数据面通不通」和「环路怎么解」两件事拆开。

严格模式 + 只装 DIRECT + fallback 传 `nullptr`，所以下面这组数就等于「一次都没经过 mihomo」：

```
靶机 10.99.0.2:8000（netns srv）× 10   → 200 ×10
  ep.rx=61(4282 B) ep.tx=36  tcpAcc=10 close=10 abort=0 socksFail=0
  cc=10/0/0/0/0/0
公网 223.5.5.5:80 × 5                  → 404 ×5（阿里 DNS 对 / 就是 404，说明真出了公网）
  cc=5/0/0/0/0/0
```

**第一版驱动恒 000 的真因与 TUN 无关**：它用 `QProcess::waitForFinished` 等 curl，那只泵子进程
自己的 IO、**不跑主事件循环**，而 TUN fd 的 `QSocketNotifier` 与 lwIP 的 25 ms 泵都挂在那上面 ——
curl 的整整 8 秒里一帧都没被读走（对照实测：阻塞版 `ep.rx=1 ep.tx=0 tcpAcc=0`）。该分支作为反面
教材留在驱动里（`COAST_TUNTEST_BLOCK=1`）。**生产代码未作任何改动。**

### 环路问题：已换主方案 —— /32 主机路由（协议无关，Linux/macOS 真机对照证；Windows 只编过）

> **2026-07-31 深夜重构**：环路排除从「三平台三套 socket 机制」换成「**/32 主机路由为主层 +
> 原 socket 机制为保险层**」。动机见 [upstream-comparison.md](upstream-comparison.md)：msquic
> 自持 UDP socket、拿不到 fd，Windows/macOS 又没有 Linux 那种按源地址的策略路由，所以那两个
> 平台的 QUIC 环路一直无解也无证据。/32 方案与 socket 归属无关：**TUN 起来时给每个代理服务器
> 地址 + 系统 DNS 加一条经物理网关的 /32(v6 /128) 主机路由**，查表时 /32 永远比 TUN 的 /1 更具体。
>
> **落地形状**（`TunSession.cpp` 安装、`LocalTunService::start()` ①.5 收集材料）：
> - 域名在**接管路由之前**阻塞解析（接管后解析请求自己会被 TUN 抓走）；解析失败的节点跳过并
>   记日志，不挡 TUN 启动。系统 DNS 地址（`SelfRouteGuard::systemDnsServers()`）一并排除 ——
>   运行中的 QHostInfo 都靠它。真机（Pi，真实订阅）：**56 个域名全解析成功、失败 0，排除集合
>   v4 ×65**。
> - 物理网关来自 `SelfRouteGuard::physicalGateway()`（Linux /proc/net/route；macOS `route -n get
>   default`；Windows `GetIpForwardTable2` 的 NextHop）。**探不到网关就拒绝接管**（没有网关装不出
>   /32，宁可不开也不开出断网）。地址=网关本身（网关兼任 DNS）时用接口路由，不写「经它自己」
>  （macOS 上那样写会 Can't assign requested address）。
> - **Linux 的 /32 必须进 TUN 专用表 989，不能进 main**：未打标的包在 pref 200 就被引去查 989，
>   main 里的 /32 永远轮不到。macOS/Windows 单表，直接更具体即胜出。
> - undo 沿用 TunSession 的「先入栈再执行、stop() 逆序全试」约定；看门狗保留。
>
> **真机证据（2026-07-31，全部带反向对照）**：
> - **Linux（Pi .91，真实 Hy2 节点 hk2.dexlos.com:20302，tcpdump 记 QUIC 握手包）**：
>   - A 组（`COAST_TUN_NO_HOSTROUTES=1` + `COAST_TUN_NO_QUICRULE=1`，无任何 QUIC 防护）：
>     **coast0=3 / eth0=0**，`ip route get` → `dev coast0` —— 反向对照成立（确实环路）。
>   - B 组（只 `COAST_TUN_NO_QUICRULE=1`，即 **/32 单独作用**）：**coast0=0 / eth0=5**，
>     `ip route get` → `via 192.168.20.1 dev eth0 table 989` —— /32 自己就够。
>   - C 组（生产默认，/32 + pref90 并存）：**coast0=0 / eth0=3~4**，规则表 90/100/200 共存无冲突。
>   - D 组（TUN + 65 条 /32 全装 + DIRECT 出站）：**接管中 curl=404，端到端 PASS**，停止后
>     规则/表/网卡零残留、网络恢复。
> - **macOS（.34，13.7.8，`/private/tmp/tunmac` 台子真接管默认路由）**：A 组无 /32 → 探针地址
>   `route -n get` 落 utun7（反向对照成立）；B 组 /32 生效 → `en0 via 192.168.20.1`，普通目标
>   9.9.9.9 仍进 TUN，还原后 curl=404。⚠ 这是**路由查表级**证据；macOS 上**真 QUIC 流量的
>   环路验证仍未做**（本机没有 msquic 构建）。
> - **Windows：只编过（MinGW 全量）**，netsh 路径没真跑（建 wintun + 改路由要管理员）。
>   **Windows 的 QUIC 环路仍无真机证据。**
>
> **端到端「TUN + 真实 Hy2」那一格仍是空的**：机场服务端对本客户端的握手整段静默（同机 mihomo
> 对照 curl=404 正常，我们的 Initial 重传 6 次无一回包 —— 是针对客户端指纹/行为的反刷，不是 IP 封锁，
> 也不是路由问题：B/C 组已证包都从物理口出去了）。订阅里其余节点全是 reality+grpc / salamander /
> tuic，进程内暂不支持，换不了别的真节点。
>
> **已知局限（有意为之）**：排除集合是 TUN 启动那一刻的快照，**运行中订阅/节点变化不热更新**，
> TUN 重启时按新配置重建；v6 主机路由只在探到 v6 网关时装（当前 TUN 本就只接管 v4 默认路由，
> v6 流量不进 TUN）。
>
> **保险层的去留**：socket 选项（SO_MARK / IP_UNICAST_IF / IP_BOUND_IF）与 msquic 的
> LOCAL_ADDRESS 钉源**保留**——已被验证有效，且在「/32 集合过期」时仍兜底。Linux 那条**专为
> QUIC 的 pref 90 源地址规则**已标注「/32 拿到真机证据后可撤」（B/C 组就是证据），下一轮可删。
> 对照开关 `COAST_TUN_NO_HOSTROUTES` / `COAST_TUN_NO_QUICRULE` 只供测试。

#### （下为旧记录：socket 机制时代的证据，保险层仍在用）

TCP 侧三平台验过；**QUIC 侧此前是个未真验的洞，后补上（Linux 真机证）**

TUN 一旦接管默认路由，coast 自己的出站也会被路由进 TUN → 死循环（表现是整机断网，比不开 TUN 更糟）。
现已落地 `SelfRouteGuard`（`src/net/core/SelfRouteGuard.h`）：探物理出口 → 把出站 socket 钉在它上面。

> ⚠️ **2026-07-31 晚更正**：本节标题原写「已解决并三平台验过」，那**只对 TCP 出站成立**。QUIC/Hysteria2
> 的 socket 由 msquic 内部建，`SO_MARK` 打不上——而 Linux 的环路规避**恰恰**靠 `SO_MARK`。真机一验
> （Pi .91，tcpdump）：TUN 接管时 Hy2 的握手 UDP 包 **5 个全走 coast0、0 个走物理口** → 铁证环路，
> 与「三平台验过」的说法直接冲突。**根因**：`QUIC_PARAM_CONN_LOCAL_ADDRESS` 只钉了**源地址**、没打
> `fwmark`，于是不命中 pref 100 的 fwmark 规则，落到 pref 200 的 TUN 表。**补法**（`TunSession.cpp`）：
> 既然 msquic 已把源钉在物理 IP 上，就按**源地址**兜一条 `ip rule add from <物理IP> lookup main pref 90`。
> 修后同一 tcpdump：**coast0 0 个、物理口 6 个** → 环路消除（反向对照成立）。
> **仍欠**：修后没拿到「TUN 下真实 Hy2 端到端绿灯」——复测把 Pi 的 IP 打进了机场服务端反刷限速
> （连打上百次冷握手），服务端对本机新握手整段静默；路由层已证不环路，但端到端那一格仍是空的。

**Qt 上的死结与解法**：选项必须在 `connect()` **之前**作用于 fd，而 Qt 在 `connectToHost()` 之前
根本没建 socket。出路是 `prepareSocket()`：**先 `bind()`**（Qt 的 bind 会把 fd 建出来），拿到 fd
打完选项再 connect；绑 `QHostAddress::Any` 成双栈并把 v4/v6 两个选项**都**打上，于是不必为了确定
地址族先解析域名。

| 平台 | 机制 | 验证（**都带反向对照**：钉到回环口必须连不通，否则"通过"可能只是选项压根没生效） |
|---|---|---|
| Windows | `IP_UNICAST_IF` / `IPV6_UNICAST_IF` | 真机（以太网 2 / ifIndex=22）：读回一致、连得通 223.5.5.5、钉回环口 `Network unreachable` → PASS。**set 要网络序、get 返回主机序**（实测，非文档推断） |
| macOS | `IP_BOUND_IF` / `IPV6_BOUND_IF` | 真机 13.7.8（en0 / ifIndex=4）：同上 → PASS |
| Linux | `SO_MARK` + 策略路由 | 容器实测（`tools/tunroute/rulecheck.sh`，`ip route get … mark …` 直接问内核）：不带 mark→coast0、带 mark→eth0 → PASS |

**Linux 那两条 rule 缺一不可，且 TUN 路由必须进独立表**：
```
ip rule add fwmark 0x43535431 lookup main pref 100   # 我们的包 → main（只有物理默认路由）
ip rule add                   lookup 989  pref 200   # 其它包 → TUN 专用表
```
曾经写成「/1 路由进 main + rule 查 main」，那是自相矛盾的 —— 打了标的包查 main，而 main 里正躺着
指向 TUN 的路由，`SO_MARK` 形同虚设。`rulecheck.sh` 里**两组配置都跑**：错误配置下带 mark 仍解析到
`coast0`（反向对照成立，证明这个测试测得出问题），修好后才解析到物理口。

**拨号点已全部收口**（审计要按 socket **建点**查，不能按 `connectToHost` 查 —— 协议出站大多经我们
自己的 `TlsClient`/`UtlsClient`，保护加在类内部，调用处看不见）：
```
grep -rn "new QTcpSocket\|new QUdpSocket\|new QSslSocket" src/
```
覆盖 `DirectOutbound`(TCP+UDP)、`ShadowsocksOutbound`(TCP+UDP)、`VlessOutbound`、`VmessOutbound`、
`TlsClient`、`UtlsClient`、`LatencyProbe`。审计顺带挖出 `LatencyProbe`（它拨的是代理服务器，同样
会环路，且更隐蔽 —— 后台定时跑，环路了只表现为"延迟测不出来"）。

**QUIC / Hysteria2 的环路**：它的 socket 由 msquic 内部创建，`SO_MARK` 那条路够不着。两级防护：
① msquic 侧 `QUIC_PARAM_CONN_LOCAL_ADDRESS` 把源地址钉在物理出口（须在 `ConnectionStart` 之前设）；
② **Linux 侧还必须**配一条 `ip rule from <物理IP> lookup main`——只钉源地址**不改变路由决策**，Linux 靠
`fwmark`/源地址匹配的 rule 才真正把包引到物理表（见上文更正框，真机 tcpdump 证）。`LocalTunService::start()`
现在**不再硬拒**有 QUIC 节点，改为**告警**（机制已上、Linux 环路真机证消除，但端到端绿灯未拿到——若开启后
整机断网，关掉「增强」自救）。Windows/macOS 的 `IP_UNICAST_IF`/`IP_BOUND_IF` 是否也够（它们改的是**出口
选择**而非仅源地址，理论上够），**尚无真机 QUIC 验证**。

三者共同还欠一个「物理出口变更」的通知（Linux `RTNETLINK`、Windows `NotifyRouteChange2`、
macOS route socket）：换网（WiFi↔有线）后新建连接会钉在一张已经没了的网卡上。现在只在
`start()` 时探一次。

### TUN 三层的现状

| 层 | Linux | Windows | macOS |
|---|---|---|---|
| 设备层 `TunEndpoint_*` | ✅ 端到端（`cc=10/0/0/0/0/0`） | 🟡 编过 + 11 个导出符号对着真 DLL 核过；**建网卡要管理员，未真跑** | ✅ 系统调用序列真机验（utun7、6 个包、4 字节前缀为网络序 AF） |
| 激活层 `TunSession` | ✅ C++ 分支真跑（自检里六条 ip 命令 rc 全 0）+ `ip rule` 设计双组对照 | 🟡 只 syntax-check | ✅ 真机**真接管默认路由**再还原，零残留 |
| 会话层 `LocalTunService` | ✅ **整体自检 PASS**（x86_64 容器 + aarch64 真机 Pi 两处独立跑通） | 🟡 编过 | 🟡 编过 |

`LocalTunService` 把四层串起来，启动顺序**不可换**：先开 `SelfRouteGuard`（必须早于任何出站）→
建设备拿真名 → 起 NetStack → **最后**才接管路由。停止严格逆序。

**整体自检结果**（`COAST_TUNSERVICE_SELFTEST=1`，TUN **真的接管默认路由**，非绕开环路的隔离台）：

```
起服务前： curl=404
  | 物理出口：eth0 (ifIndex=2)
  | TUN 网卡：coast0
  | [rc=0] ip addr add 198.18.0.2/24 dev coast0
    [rc=0] ip rule add fwmark 0x43535431 lookup main pref 100
    [rc=0] ip rule add lookup 989 pref 200
    [rc=0] ip route add 0.0.0.0/1 / 128.0.0.0/1 dev coast0 table 989
接管中： curl=404 ✓ 经 TUN→NetStack→进程内出站 绕回来了
停服务后： curl=404 ✓ 路由已还原
=== PASS ===   x86_64 容器 与 aarch64 真机(Pi .91) 结果一致
```
事后**从外部**独立复核（不只信自检自述）：规则表只剩默认三条、`coast0` 已清理、网络恢复。
Pi 上还多验到一点容器测不出的：它有**两条默认路由**（eth0 metric 100 / wlan0 metric 600），
`SelfRouteGuard` 正确挑了 eth0。

**这次自检的价值不在 PASS，在于它第一次真跑就抓出两个 bug**，且两个都不在任何单个组件里、
全在「串起来」这一层：① TUN 两端地址方向反了（路由全对但 curl 恒 000）；② 出站工厂被删两次
（`NetStack` 的契约是取得所有权）→ 停服务 SIGSEGV。四个部件各自都有真机证据，合起来仍错了
两处 —— 这就是「整体自检 PASS 之前不动 UI」的实证理由。

---

## 二、控制面：谁在提供状态（**这是现在的主要缺口**）

`ClashService` 是唯一的 mihomo REST 客户端。全仓库对它的调用：

| 调用 | 端点 | 进程内等价物 |
|---|---|---|
| `refreshNodes` | `/proxies` | **部分**：`ProxyConfig` 已从 full.yaml 解析节点，`parseProxyGroupsLeaf` 已能解析组结构 |
| `selectNode` / `setSelectedGroup` | `/proxies/<group>` | ❌ 选择状态由核心持有（`cache.db` 的 `store-selected`） |
| `testDelays` | `/proxies/<n>/delay` | **部分**：`LatencyProbe` 自己 `connectToHost` 探测，但节点来源仍是核心 |
| `setMode` | `/configs` | **部分**：`RuleEngine` 有完整规则匹配，缺 global/direct 模式切换的落地 |
| `fetchConnections` / `closeConnection` | `/connections` | **部分**：网关侧 `NetStack` 自己持有连接表；**本机入站的连接目前谁都看不见** |
| 流量 | `/traffic` | **部分**：`GatewayDiag` 有 rx/tx；同样不含本机入站 |
| 日志 | 核心 stdout | ❌ |

**"部分"的含义**：这些等价物只覆盖**网关那条路**的数据。本机入站的连接/流量/日志目前
在 UI 上完全不可见——这是接了本机入站之后新出现的缺口。

---

## 三、优先级（附理由）

1. **本机系统代理端到端验证** —— 代码已就位（`6d60277`），但设系统代理需要真实桌面会话，
   CI 和 headless 派都验不了。**需要在真实机器上试一次**：开关打开后系统代理是否指向 7891、
   浏览器是否照常上网。这是唯一挡在「本机流量也替换掉」前面的事。
2. **本机入站的连接/流量可见性** —— 现在开了它，UI 反而看不到这部分流量，是个体验倒退。
3. **选择状态搬进程内** —— 控制面脱离 REST 的第一步（组结构已经能自己解析了，缺的是"用户选了哪个"）。
4. **TUN 接「增强」按钮** —— 前置条件**已全部满足**：数据面验通、环路三平台验过、
   `LocalTunService` 整体自检在 x86_64 容器与 aarch64 真机两处 PASS（见上节）。
   剩下的是 UI 接线本身，以及两个**平台层面**尚未真跑的格子：
   Windows 建 wintun 网卡要管理员、macOS 的会话层只编过。
   接线时务必保留 `start()` 里那两道拒绝（有 QUIC 节点 / 探不到物理出口）——
   它们是「点下去才发现断网」的唯一防线。
5. **msquic 本地地址钉定** —— 关掉最后一个环路缺口（Hy2）。在那之前 `LocalTunService`
   会拒绝在有 QUIC 节点时启动，用户不会误踩，所以它不阻塞 ④。

---

## 四、上游怎么做的（mihomo / sing-box）

读过两边源码后的对比见 **[upstream-comparison.md](upstream-comparison.md)**。一句话：
它们靠 Go 的 `net.Dialer.Control` / `ListenConfig.Control` 在 connect 前拿到裸 fd，
**而且 quic-go 接受外部 `net.PacketConn`，所以 QUIC 的 UDP socket 也走同一条钩子** ——
一套机制覆盖全部协议，obfs/grpc 也因此只是包装层。

我们用 msquic 拿不到这个前提（它自持 socket、只给 `QUIC_PARAM_CONN_LOCAL_ADDRESS`），
于是被迫两套机制。⚠️ 其中 Linux 那条靠「按源地址做策略路由」，**Windows/macOS 无等价能力且未验证**
—— 那两个平台的 Hy2+TUN 很可能仍然环路。建议方向（协议无关、三平台同形）见该文档第四节。

## 五、明确不打算做的

- **不追求 mihomo 的全部配置兼容面**。目标是「Coast 自己生成的 full.yaml 里用到的那些」，
  不是做一个 mihomo 的完整替代实现。范围失控的风险比缺功能的风险大。
- **TUIC 暂时不动**：等 msquic 2.6 的 keying-material exporter（上游阻塞）。
- **SOCKS5 UDP ASSOCIATE**（本机入站）：本机 UDP 留给 TUN 那条路。
