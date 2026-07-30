# 「完全替换 mihomo 内核」还差什么

目标是让 CoastCore（进程内引擎）承担 mihomo 现在做的全部事情。这份是**按证据**列出的剩余缺口，
不是路线图愿望清单——每条都标了是**核查过**还是**估计**。

## 结论先行：最大的缺口是结构性的，不是功能性的

```
grep -rn "CoreDialerFactory" src/ --include=*.cpp --include=*.h
```
**`CoreDialerFactory` 只被 `LanGateway` 装配，别处一处都没有**（核查过）。

⇒ **CoastCore 今天只服务「局域网里被代理的设备」。本机自己的流量——系统代理 `:7890`、TUN——
100% 还是 mihomo 在跑。** 就算网关那条路做到完美，本机这条路也一步都没往前走。

这一条比下面所有条目加起来都重要：它决定了「替换 mihomo」目前**最多只完成了一半的入口**。

---

## 一、数据面：谁在搬运字节

| 路径 | 现状 | 缺口 |
|---|---|---|
| 局域网被代理设备 | ✅ **进程内**（NetStack + CoreDialerFactory） | 有活锁待修，见 [gateway-load-test.md](gateway-load-test.md) |
| **本机系统代理**（`:7890` 混合端口） | ❌ **mihomo** | 需要进程内 HTTP/SOCKS inbound 监听 |
| **本机 TUN** | ❌ **mihomo** | 需要进程内 TUN 读写 + 三层栈（lwIP 已有，缺设备侧接入） |

进程内**出站**已经相当完整（核查过，`src/net/core/`）：

- 协议 8 种：Hysteria2 / Reality / Shadowsocks / Trojan / TUIC / VLESS / VMess / Direct
- 传输 4 种：QUIC(msquic) / TLS / uTLS / WebSocket
- 规则引擎 `RuleEngine`、DNS `DnsResolver`+`DnsMessage`、配置快照 `ProxyConfig(Builder)`

**所以缺的不是"能不能拨出去"，而是"字节从哪进来"** —— 入站侧只有网关一个入口。
补齐本机入口，用的是同一套出站，边际成本比看上去小。

## 二、控制面：谁在提供状态

`ClashService` 是唯一的 mihomo REST 客户端。全仓库对它的调用（核查过）：

| 调用 | mihomo 端点 | 进程内是否已有等价物 |
|---|---|---|
| `refreshNodes` | `/proxies` | **部分**：`ProxyConfig` 已从 full.yaml 解析出节点与分组结构 |
| `selectNode` / `setSelectedGroup` | `/proxies/<group>` | ❌ 选择状态由 mihomo 持有（`cache.db` 的 `store-selected`） |
| `testDelays` | `/proxies/<n>/delay` | **部分**：`LatencyProbe` 自己 `connectToHost` 探测，但节点列表仍来自 mihomo |
| `setMode` | `/configs` | **部分**：`RuleEngine` 有完整规则匹配，缺 global/direct 模式开关 |
| `fetchConnections` / `closeConnection` / `clearConnections` | `/connections` | **部分**：网关侧 `NetStack` 自己就持有连接表 |
| 流量 | `/traffic` | **部分**：`GatewayDiag` 已有 rx/tx 计数 |
| 日志 | mihomo stdout | ❌ |
| `setMixedPort` | `/configs` | 随「本机 inbound」一起解决 |

**注意"部分"的含义**：这些等价物今天只覆盖**网关那条路**的数据。
本机流量的连接/流量/日志完全在 mihomo 里，进程内看不见。

## 三、优先级（我的判断，附理由）

1. **修网关活锁** —— 功能缺陷，不是调优项；持续新建连接会让网关停止服务。已在做。
2. **本机 inbound（HTTP/SOCKS 混合端口）** —— 打通第二个入口，复用已有的全部出站。
   这是让"替换"从一半走向完整的最短路径，且不碰 TUN 的系统权限问题。
3. **选择状态与节点列表搬进程内** —— 之后控制面才谈得上脱离 REST。
4. **TUN** —— 最后做：涉及各平台权限（macOS 要 root helper），且 inbound 打通后收益重叠。

**关于"多进程"**：真机数据已经否掉了它，见 [gateway-load-test.md](gateway-load-test.md)
——数据面 `late=0`、零丢弃、CPU 只占单核 15%，瓶颈不在并行度上。

## 四、明确不打算做的

- **不追求 mihomo 的全部配置兼容面**。目标是「Coast 自己生成的 full.yaml 里用到的那些」，
  不是做一个 mihomo 的完整替代实现。范围失控的风险比缺功能的风险大。
- **TUIC 暂时不动**：等 msquic 2.6 的 keying-material exporter（上游阻塞，见 memory）。
