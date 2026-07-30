# 进程内 DNS 真机验证（以及它暴露出来的两件事）

测试台：树莓派 5 作网关，`192.168.20.239`（Ubuntu 虚机）作被劫持设备，`coastcore: true`。

## 结论：进程内 DNS 跑通了 ✅

设备侧 `dig www.baidu.com` → **`198.18.0.4`**（我们分配的 fake-ip，不是真实 IP），随后
`curl http://www.baidu.com/` → **200**、`http://www.qq.com/` → **302**。

网关侧计数（`logs/gateway-diag.log`）：

```
dns=0  dnsNoReply=0  dnsLocal=10/0        ← 我们自己答了 10 条、转发 0 条；dns=0 = mihomo 劫持路径一次没跑
dnsLocal=6/0   dnsLocal=2/0               ← 后续采样同样全是本地应答
NETSTACK fakeip 198.18.0.4 -> www.baidu.com   ← 我们自己发的假 IP，accept 时反查回域名成功
```

`dns=0` 是关键：它证明**这些 DNS 一条都没经过 mihomo**。整条链路（设备查询 → 我们合成 fake-ip →
设备连假 IP → 我们反查回域名 → 交给出站）在真机上闭环了。

## 暴露出来的两件事（都不是这次改动引入的）

### 1. 设备优先用 IPv6 DNS 时，网关**完全看不到** DNS

`.239` 的 systemd-resolved 选的是路由器的 **v6** 地址（`240e:...::1`），DNS 走 v6 直达路由器。
此时 `dns=0` **且** `dnsLocal=0/0` —— 我们的进程内 DNS 和旧的 mihomo 劫持**都没触发**。
把设备钉到 v4 DNS（`resolvectl dns ens18 192.168.20.1`）后才有了上面那些数字。

这不是本次改动引入的问题（`dns=0` 证明旧路径同样从未生效），但它是「完全替换 mihomo」路上一个
独立的真缺口：**双栈环境里 fake-ip 分流对这类设备直接失效**。

### 根因（已定位到代码，未修）

`NdpSpoofer` **只投毒路由器的链路本地地址**：

- `sendSpoof()` / `heal()` 发的 NA，Target Address 恒为 `m_routerLL`；
- `answerNeighborSolicit()` 更是明确拒绝：`if (memcmp(f + kOffNsTarget, m_routerLL, 16) != 0) return;`
  —— 只抢答针对 LL 的 NS。

而设备的 DNS 服务器是路由器的**全局**地址（本例 `240e:3a1:7ed1:bbc3::1`，来自 RA 的 RDNSS/DHCPv6）。
那是个**链上（on-link）地址**，设备**不会**经默认路由器转发，而是直接 NS 解析它 →
我们不应答 → 真路由器应答 → **这条流量整段绕过网关**。

注意影响面**不止 DNS**：任何发往路由器全局地址的流量（路由器 Web 管理页、它自身跑的服务）
都同样绕过。一般上网流量不受影响——那些是 off-link，走默认路由器，而默认路由器的地址来自 RA
的**源地址（链路本地）**，正好在我们已投毒的范围内。

### 修法（下次直接开工）

拓扑结构里目前**没有** `routerGlobal6` 字段（`NicSpec` 只有 `routerLinkLocal6` / `routerMac6` /
`localGlobal6` / `prefix6`），所以要**学**：

1. 旁听线上的 NA/NS，**源 MAC == `m_routerMac`** 的帧所声称的 Target Address，记为"也是路由器的地址"；
2. 把这些地址加入投毒集合：`answerNeighborSolicit` 对它们同样抢答，周期 NA 也为它们各发一份；
3. `heal()` 时同样要把它们还原回真路由器 MAC ——**否则撤劫持后设备会连不上路由器本身**，
   这一条务必和 LL 走同一套还原路径（v4 侧的教训见 memory 里"重启后代理恢复四修"那条）。

风险点：投毒集合会随网络里出现的地址增长，需要设上限并只接受**本网卡前缀内**的地址，
避免被伪造 NA 喂进无关地址。

### 端到端实测（commit 32de877 之后）：投毒生效，但 v6 回封还没通

把设备 DNS 切回 IPv6（`resolvectl dns ens18 240e:3a1:7ed1:bbc3::1`）后：

```
240e:3a1:7ed1:bbc3::1  lladdr 2c:cf:67:95:32:80  router   ← ★ 指向网关了（此前是真路由器 70:a7:41:a4:19:7b）
dnsLocal=40/1                                              ← v6 的 DNS 查询**真的进了**进程内 DNS
dig www.baidu.com → communications error to 127.0.0.53#53: timed out   ← 但设备没收到应答
```

**结论分两半**：
- ✅ **投毒 + 收包这一半通了**：路由器全局地址被成功抢答，v6 DNS 查询到达我们、并被本地应答逻辑处理了 40 次；
- ❌ **回封这一半没通**：设备等不到应答。下一步要查 `NetStack::sendUdpResponse6` 这条 v6 回封路径
  （`answerDnsLocally` 里 v6 分支走的就是它），以及源地址伪装是否正确（要伪装成设备原本查询的那个
  **v6** DNS 地址）。注意 v4 那条路早已验证可用，所以问题**局限在 v6 回封**。

**收尾验证（安全相关，务必照做）**：停网关后设备侧三条表项全部还原成真路由器
`70:a7:41:a4:19:7b`（v4 网关 / v6 全局 / v6 链路本地），外网 `200` —— 说明 `healOne` 对
**新学到的全局地址**同样生效，这正是本次改动里风险最高的一条。

### 2. 策略组解析不出叶子 → 绝大多数连接仍回退核心

新加的分原因记账立刻把它指了出来：

```
cc=4/7/0/0/0/0        # 进程内 4 / **无路由 7** / 节点缺 0 / 协议缺 0 / UDP 不支持 0 / 严格拒绝 0
[CCROUTE] host=www.baidu.com needsResolve=false target=🎯 全球直连 node=   ← node 是空的
```

规则命中了策略组「🎯 全球直连」，但 `ProxyConfig::resolveTarget()` 解析不出叶子（`node=` 空）
→ 按「宁可少走进程内也绝不误路由」的原则回退核心。本测试台 `proxies: []`（没挂订阅），
组里应当只剩 DIRECT，那就**应该**解析成 DIRECT 走进程内直连才对。
下一步要查的是 `ClashService::groupLeafMap()` 为什么没给出这一条（`groupMapSize=6` 说明map 非空）。

### 后续：根因是 YAML 转义没解开，已修（commit 733be40）

`full.yaml` 的规则是这样写的（注意那是 **YAML 转义序列**，不是 emoji 本身）：

```yaml
- "GEOIP,CN,\U0001F3AF 全球直连"
```

我们只剥了引号、没解转义，于是 target 拿到的是**字面量**：反斜杠 + `U0001F3AF` 这 10 个 ASCII
字符再加中文；而 `groupToNode` 的键来自核心 REST API，是**真正的 emoji 字符**。两者永远匹配不上。
真实订阅几乎清一色用 emoji 组名 → Rule 模式绝大部分流量都在回退。

修复后同一测试台复测：

| | `cc=` 进程内/无路由/节点缺/协议缺/UDP/严格 | CCROUTE |
|---|---|---|
| 修复前 | `4/7/0/0/0/0`（64% 回退） | `target=🎯 全球直连 node=<空>` |
| 修复后 | `22/0/0/0/0/0`、`19/3/0/0/0/0` | `target=🐟 漏网之鱼 node=DIRECT` ✅ |

**这正是 `cc=` 这行存在的意义**：在它之前，「回退了」是个看不见的事实；现在差距是一个具体的数字
和一个具体的组名，照着修就行。

## 复现

```bash
# 网关侧
HOME=/root QT_QPA_PLATFORM=offscreen COAST_GATEWAY_TESTDEV=<设备IP> COAST_GATEWAY_DEBUG=1 ./coast
# 设备侧（先确保用的是 v4 DNS，否则网关根本看不到查询）
resolvectl dns <iface> <路由器v4>; resolvectl flush-caches
dig +short www.baidu.com          # 期望 198.18.x.x = 我们答的
curl -o /dev/null -w '%{http_code}' http://www.baidu.com/
# 网关侧看计数
grep -oE 'dns=[0-9]+ .*dnsLocal=[0-9]+/[0-9]+' logs/gateway-diag.log | tail
grep -oE 'cc=[0-9/]+' logs/gateway-diag.log | tail
```
