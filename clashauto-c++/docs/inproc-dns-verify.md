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
独立的真缺口：**双栈环境里 fake-ip 分流对这类设备直接失效**。要么让 v6 的 NDP 投毒覆盖 DNS 流量，
要么在 RA/DHCPv6 里把自己宣告成 DNS。

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
