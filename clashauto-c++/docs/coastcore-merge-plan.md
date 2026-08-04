# 把 `feat/coastcore-outbound` 并进 `v1.1` —— 调查结果与分阶段方案

> 2026-08-04。尝试过一次 `git merge`，**已中止**（树保持在 `8228075`，干净可构建）。
> 本文记录为什么直接合不了，以及可行的路径。结论先说：
> **这是一次 128 文件 / +23003 / −8360 的集成，不是解冲突；但它可以拆成 5 个独立可验证的阶段。**

## 1. 直接合并的实况

- 共同祖先 `f4e6375c`；`v1.1` 独有 **399** 个提交，`feat/coastcore-outbound` 独有 **109** 个。
- `git merge` 产生 **21 个冲突文件、47 处冲突块**。

其中三处是**结构性**的，不是文本冲突：

| # | 冲突 | 为什么不能机械解 |
|---|---|---|
| 1 | `NetStack.cpp` 有一个 **571 行**的冲突块 | 对面是整块 **lwIP 数据面**，而 v1.1 已经把它换成 smoltcp。这是**跨栈移植**，不是取哪一边 |
| 2 | CMake 目标改名 | 对面把 `clashauto-qml` 改成了 **`coast`**（36 处 vs 48 处引用）。两边的 `target_sources` 互相看不懂 |
| 3 | `GatewayDiag` 计数集不同 | 对面新增 `cc=/fbNoRoute/fbNodeMissing/…` 一整套回退记账；格式串与 `.arg()` 数量必须同步改，取任一边都会错位 |

另有 4 个 lwIP 文件是「v1.1 删了、对面改了」—— 这个好办，保持删除即可（lwIP 是**故意**移除的）。

## 2. ★ 关键的好消息：出站引擎与 NetStack **零代码耦合**

查证过：`clashauto-c++/src/net/core/**` 与 `IOutbound.h` 里，
**没有任何 `#include "NetStack.h"`、没有 `NetStack::`、没有 `NetStack *`** ——
那 10 个文件里出现的 "NetStack" 全在**注释**里。

⇒ 整套进程内出站（`CoreRouter` / `CoreDialerFactory` / `DirectOutbound` / `RuleEngine` /
  `DnsResolver` / `ProxyConfig*` / `proto/Hysteria2Outbound` / `proto/RealityOutbound` …）
  **可以独立搬过来**，不需要先把 NetStack 移植完。

这把「一次性大合并」变成了可以分期的工程。

## 3. 分阶段方案（每阶段独立可验证）

### 阶段 1：把出站引擎搬进来，但**不接线**
- 拿 `src/net/IOutbound.h` + `src/net/core/**`
- `Socks5Client.h/.cpp` 让 `Socks5Tcp/Socks5Udp` 实现 `IOutboundTcp/IOutboundUdp`
  （这两个文件在试合并时**自动合并成功**，无冲突）
- CMake 加源文件（注意目标名：v1.1 叫 `clashauto-qml`）
- **验收**：编译通过 + 现有六个 headless 自测全绿。**零行为变化**（没有构造点）

### 阶段 2：TCP 出站接线 —— **65% 的收益落在这里**
- `NetStack.cpp` 里 `SmolConn::socks` 的类型 `Socks5Tcp *` → `IOutboundTcp *`
- 由 `CoreDialerFactory` 创建，而不是 `new Socks5Tcp`
- 信号面两边一致（`established` / `upstreamBytesWritten` / `dataReceived` / `failed` / `closed`），
  所以桥接那 5 个 `connect` 基本不用改
- **验收**：`COAST_GW_THROUGHPUT` 的 ns/字节应从 ~17.6 掉下来（第十节归因说这一跳占 65%）；
  `COAST_SMOLGW_SELFTEST` 仍需绿

### 阶段 3：进程内 DNS
- 对面的 `answerDnsLocally` + fake-ip 反查改写（accept 里把 fake-ip 目的地换回域名）
- 这部分**耦合 NetStack**，要在 smoltcp 的 accept 路径上重写
- **验收**：`GatewayDiag` 的 `dnsLocal=…` 两栏 > 0

### 阶段 4：UDP 出站
- `IOutboundUdp` 接进现有那 700 行手工 NAT（UDP 路径与 TCP 栈无关，改动面小）

### 阶段 5：进程内 TUN（独立特性，可最后做）
- `LocalTunService` / `TunSession` / `TunEndpoint_{linux,win,mac}`
- 对面的 `260a9c6` 自己标着 **WIP、未验完**，别急着并

## 4. 不要做的事

- ❌ **不要 take-both 解 CMakeLists**：目标名不同，会把两套 `target_sources` 交织成编不过的东西
  （试过，就是这么炸的）
- ❌ **不要把 lwIP 文件合回来**：v1.1 删除 lwIP 是有实测依据的决定（见 `lwip-alternatives.md`）
- ❌ **不要一次性合完再编**：128 个文件同时动，编译错误会互相掩盖，定位成本极高

## 5. 当前状态

- 树在 `8228075`，**干净、可构建、六个自测全绿**
- 合并已 `--abort`，没有留下任何半成品
- `feat/coastcore-outbound` 分支原样保留
