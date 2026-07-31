# 「完全替换 mihomo 内核」还差什么

按**证据**列出的现状与剩余缺口。每条都标了是**真机验过**、**只编译过**，还是**估计**。
（2026-07-31 大幅更新：数据面已经替换完成，缺口整体移到了控制面与 TUN。）

## 一句话现状

**数据面已经不需要 mihomo 了**（真机验证：杀掉核心后网关照常服务）。
**控制面还完全依赖它**（节点列表 / 选择 / 延迟 / 连接 / 流量 / 日志）。

---

## 一、数据面：谁在搬运字节

| 路径 | 状态 | 证据 |
|---|---|---|
| 局域网被代理设备 | ✅ **进程内** | 真机：单请求 **0.70 ms**（同路内核转发 0.5 ms）、饱和 2883 conn/s、失败率 0% |
| 本机应用流量（系统代理） | 🟡 **已接线，端到端未验** | 入站真机验过（7891，strict 下全部 200）；**系统代理改指它这一步需要真实桌面会话，未验** |
| 本机 TUN | 🟡 **数据面已验通，尚未接「增强」按钮** | 真机隔离台：10/10 全 200、`cc=10/0/0/0/0/0`（见下节）。挡在接线前的是**环路**，不是数据面 |

### 「杀掉 mihomo 还能不能活」——能（真机）

```
pkill core → core=NO、7899 无人监听
劫持状态: ['192.168.20.34', '192.168.20.239']   ← 保住（旧行为是全部撤销）
设备连打 10 次 http://223.5.5.5/ → 全部 404 / ~27ms
cc=10/5/0/0/0/0   socksFail=0                   ← 10 条全走进程内
```

为达成这一点，今天拆掉了三层依赖：

1. **出站**（早已具备）：8 协议（Hy2 / Reality / SS / Trojan / TUIC / VLESS / VMess / Direct）
   + 4 传输（QUIC / TLS / uTLS / WebSocket）+ 规则引擎 + DNS。
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

### 挡在「增强」按钮前面的是环路，不是数据面

TUN 一旦接管默认路由，coast 自己的出站也会被路由进 TUN → 死循环。三个平台的做法不同，但都归结为
**同一件事：本进程发起的连接必须绕开自己的 TUN**，因此都需要先有一个**统一的拨号收口**——
现在 `QTcpSocket`/`QUdpSocket` 散落在约 10 处（`DirectOutbound`、`ShadowsocksOutbound`、
`VlessOutbound`、`VmessOutbound`、`TlsClient`、`WsClient`、`QuicTransport`、`Socks5Client`），
逐个改必然漏一个，而漏一个的表现就是「偶发死循环」。

| 平台 | 机制 | 要点 |
|---|---|---|
| Linux | `SO_MARK` + 策略路由 | 出站 socket 打 `SO_MARK=0x…`；`ip rule add fwmark 0x… lookup main pref 100`，TUN 的默认路由放进更低优先级的独立表。**必须在 `connect()` 之前 setsockopt** → Qt 里要么走 `setSocketDescriptor(自建 fd)`，要么退而求其次用 cgroup + `nft mangle` 打标（不改一行出站代码，但要求进程在自己的 cgroup 里） |
| Windows | `IP_UNICAST_IF` / `IPV6_UNICAST_IF` | 无 `SO_MARK`。把出站 socket 钉在**物理默认路由网卡**的 ifindex 上，路由查表就不会命中 wintun（WireGuard-windows / mihomo 同法）。配套：TUN 用 `0.0.0.0/1`+`128.0.0.0/1` 而非改默认路由 |
| macOS | `IP_BOUND_IF` / `IPV6_BOUND_IF` | 同上，钉在 en0 之类的物理口。网络切换（WiFi↔有线、换网）时要重新探测并对**新建**连接生效 |

三者共同还欠一个「当前物理默认路由网卡」的探测器 + 变更通知（Linux `RTNETLINK`、Windows
`NotifyRouteChange2`、macOS route socket / SCNetworkReachability）。

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
4. **TUN 接「增强」按钮** —— 数据面已验通（上一节），**卡点变成了「统一拨号收口 + 环路排除」**。
   这一步做完之前不要动 UI：环路的表现是整机断网，比不接更糟。各平台权限（macOS 要 root helper）
   仍在，且与 ① 的收益重叠。

---

## 四、明确不打算做的

- **不追求 mihomo 的全部配置兼容面**。目标是「Coast 自己生成的 full.yaml 里用到的那些」，
  不是做一个 mihomo 的完整替代实现。范围失控的风险比缺功能的风险大。
- **TUIC 暂时不动**：等 msquic 2.6 的 keying-material exporter（上游阻塞）。
- **SOCKS5 UDP ASSOCIATE**（本机入站）：本机 UDP 留给 TUN 那条路。
