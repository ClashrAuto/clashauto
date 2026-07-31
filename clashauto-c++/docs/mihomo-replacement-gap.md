# 「完全替换 mihomo 内核」还差什么

按**证据**列出的现状与剩余缺口。每条都标了是**真机验过**、**只编译过**，还是**估计**。
（2026-07-31 大幅更新：数据面已经替换完成，缺口整体移到了控制面与 TUN。）

## 一句话现状

**数据面已经不需要 mihomo 了**（真机验证：杀掉核心后网关照常服务）。局域网设备、本机入站、
本机 TUN 三条路都已在进程内跑通，TUN 那条连环路规避都验过了 —— 只差把「增强」按钮接上去。
**控制面还完全依赖它**（节点列表 / 选择 / 延迟 / 连接 / 流量 / 日志）—— 这是现在真正的大头。

---

## 一、数据面：谁在搬运字节

| 路径 | 状态 | 证据 |
|---|---|---|
| 局域网被代理设备 | ✅ **进程内** | 真机：单请求 **0.70 ms**（同路内核转发 0.5 ms）、饱和 2883 conn/s、失败率 0% |
| 本机应用流量（系统代理） | 🟡 **已接线，端到端未验** | 入站真机验过（7891，strict 下全部 200）；**系统代理改指它这一步需要真实桌面会话，未验** |
| 本机 TUN | ✅ **整条链路验通，只差 UI 接线** | 隔离台 10/10 全 200 `cc=10/0/0/0/0/0`；**真接管默认路由**的整体自检在 x86_64 容器 + aarch64 真机两处 PASS。环路已解决（见下节） |

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

### 环路问题：**已解决并三平台验过**（2026-07-31 晚）

TUN 一旦接管默认路由，coast 自己的出站也会被路由进 TUN → 死循环（表现是整机断网，比不开 TUN 更糟）。
现已落地 `SelfRouteGuard`（`src/net/core/SelfRouteGuard.h`）：探物理出口 → 把出站 socket 钉在它上面。

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

⚠️ **唯一未覆盖：QUIC / Hysteria2**。它的 socket 由 msquic 内部创建，Qt 这条路够不着。
`LocalTunService::start()` 因此会**主动拒绝**在有 QUIC 节点时启动并说明原因 —— 不让用户点下去
才发现网没了。解法：msquic 的 `QUIC_PARAM_CONN_LOCAL_ADDRESS`（须在 `ConnectionStart` 之前设）。

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

## 四、明确不打算做的

- **不追求 mihomo 的全部配置兼容面**。目标是「Coast 自己生成的 full.yaml 里用到的那些」，
  不是做一个 mihomo 的完整替代实现。范围失控的风险比缺功能的风险大。
- **TUIC 暂时不动**：等 msquic 2.6 的 keying-material exporter（上游阻塞）。
- **SOCKS5 UDP ASSOCIATE**（本机入站）：本机 UDP 留给 TUN 那条路。
