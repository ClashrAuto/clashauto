# mihomo / sing-box 是怎么做的 —— 以及我们为什么更费劲

2026-07-31 读了两边源码（mihomo `Alpha` 分支、sing-box `main`）后的对比。写下来是因为
这个结论直接决定「环路排除」和「obfs/grpc 支持」两块该往哪走，不写下来下次还得再读一遍。

> 顺带一条踩坑记录：`git clone https://github.com/MetaCubeX/mihomo.git` **不指定分支会拿到别的东西**
> （默认分支不是代理核心）。要 `-b Alpha`。核对办法：`go.mod` 首行应是 `module github.com/metacubex/mihomo`。

## 一、他们的核心机制：Go 的 `Control` 钩子

Go 的 `net.Dialer.Control` / `net.ListenConfig.Control` 会在 **socket 建好之后、connect/bind 之前**
把裸 fd 回调给你。两边都用它，做法几乎一样：

```go
// mihomo component/dialer/bind_linux.go
err = c.Control(func(fd uintptr) { innerErr = unix.BindToDevice(int(fd), ifaceName) })
// mihomo component/dialer/mark_linux.go → sockopt.RawConnMark(c, mark)

// sing-box common/dialer/default.go:76-84
dialer.Control   = control.Append(dialer.Control, bindFunc)
listener.Control = control.Append(listener.Control, bindFunc)   // ★ listener 那条是关键
```

**关键在 `listener.Control`。** quic-go 接受一个 `net.PacketConn`，而两边都把自己的 dialer 交给它：

```go
// mihomo adapter/outbound/hysteria2.go:223
PacketListener: outbound.dialer,     // QUIC 的 UDP socket 由它们自己建
```

于是 **QUIC 的 UDP socket 和普通 TCP 走同一条钩子**，SO_MARK / BindToDevice 自动覆盖，
不需要任何 QUIC 专属的特殊处理。

同一个前提还顺带解决了另外两件事：
- **salamander 混淆**：是个 `PacketConn` 包装层 —— 拥有 PacketConn 就能在数据报进出时套一层。
- **grpc/h2 传输**：stream 包装层，同理。

## 二、我们的处境：拿不到那个前提

| | mihomo / sing-box | Coast |
|---|---|---|
| 普通 socket 拿 fd | `Control` 回调，天然有 | Qt 到 `connectToHost` 才建 socket → 只能绕成「先 `bind()` 逼出 fd 再打选项」（`SelfRouteGuard::prepareSocket`） |
| **QUIC 的 UDP socket** | **自己创建，同一条钩子** | **msquic 自持，完全够不着**；msquic 只给 `QUIC_PARAM_CONN_LOCAL_ADDRESS`（钉源地址），**不接受外部 socket** |
| 环路排除机制数 | **一套**覆盖全部 | **两套**：socket 选项 + 专为 QUIC 的 `ip rule from <物理IP>`（Linux） |
| obfs / grpc | 包装层，自然实现 | 包装不了 → 只能守卫回退核心 |

**结论：他们的做法更好，但优势不在技巧，在架构前提 —— 出站栈拥有自己的 socket。**
用 msquic 就注定拿不到这个前提。

## 三、由此暴露的风险（**尚未验证，别当已解决**）

Linux 上那条补丁 `ip rule from <物理IP> lookup main pref 90` 之所以成立，靠的是 Linux
**按源地址做策略路由**的能力。**Windows / macOS 没有等价物** —— 那边我们只设了
`QUIC_PARAM_CONN_LOCAL_ADDRESS`，而「绑了源地址」是否足以让路由避开 TUN，**没有验证**。

⚠️ 所以 **Windows / macOS 上 Hy2 + TUN 很可能仍然环路**，只是没人测过。

## 四、建议的方向：换一条与 socket 归属无关的路

> **✅ 2026-07-31 已落地**：/32 主机路由方案已实现（`TunSession.cpp` + `LocalTunService` ①.5 +
> `SelfRouteGuard::physicalGateway()/systemDnsServers()`），Linux（Pi，tcpdump A/B 对照）与
> macOS（route -n get A/B 对照）真机验证通过，原 socket 机制降级为保险层。
> 证据与局限的完整记录在 [mihomo-replacement-gap.md](mihomo-replacement-gap.md) 环路一节。
> Windows 仍只编过；macOS 的真 QUIC 流量验证也还欠着（无 msquic 本地构建）。

与其继续追「怎么摸到 msquic 的 fd」，不如学 WireGuard 客户端：
**TUN 起来时，给每个代理服务器 IP 加一条经物理网关的 /32（v6 /128）主机路由。**

- **协议无关**：不管 socket 是谁建的、能不能打标，查表时 /32 永远比 TUN 的 `0.0.0.0/1` 更具体
- **三平台同形**：`ip route add` / `route add` / `netsh` 各一行，不依赖 SO_MARK、
  `ip rule from`、`IP_UNICAST_IF` 这些各不相同、且其中两处没验过的能力
- 服务器地址本来就在 `ProxyConfig` 里；域名解析结果要缓存 —— **顺带把「DNS 查询本身也可能被 TUN 抓走」
  这个隐患一并收掉**（那条今天验伪过一次，但只是当时不是主因，隐患仍在）

代价：节点切换 / 订阅更新时要同步维护那组路由。但相比现在「三平台三套机制、其中两套无证据」，
这个换法把不确定性收敛到一处，且那一处是**可以在任意一台机器上直接验的**（`ip route get` 即可）。
