# CoastCore 移植现状 / 万兆可行性 / 真机验证清单

> 2026-08-04。接续 `coastcore-merge-plan.md`（那份是**计划**，五个阶段现已全部落地，
> 本文是**现状与欠账**）。性能数据的来源见 `gateway-bottleneck-audit.md`。

## 1. 五个阶段：全部落地

| 阶段 | 内容 | 提交 |
|---|---|---|
| 1 | 搬进程内出站引擎，**不接线** | `5042439` |
| 2a | `NetStack` 改用 `IOutboundTcp` 接口 | `83a84d9` |
| 2b | `LanGateway::setCoastCore` 网关侧接线 | `fc3cd90` |
| 2c | 配置快照 + `coastcore` 开关 | `efff210` |
| 3a | fake-ip 反查（域名类流量的前提） | `637ee33` |
| 3b | 进程内 DNS（`:53` 自己答） | `1daaabd` |
| 4 | UDP 出站也走工厂 | `642b2f1` |
| 5 | 进程内 TUN 搬入，**默认不启用** | `564723e` |
| — | OpenSSL 静态链（ss/vmess/reality） | `0656c69` |
| — | 拆掉 lwIP 时代的单例判重 | `63769f3` |
| — | **端到端自测**（下方 §3 的重点） | `a8e8833` |

★ 移植中有**三处「换栈把上游正在解决的问题直接消掉」**，都没照抄上游的解法：
1. lwIP 回调可能同步销毁上下文 → smoltcp 是 poll 模型，那套 `ConnWatch`/`ERR_ABRT` 不需要
2. 「网关开着时点『增强』被拒」→ 根因是 lwIP 全局单例，已随 lwIP 删除（并加了双实例护栏）
3. `LocalTunService` 的整套「借共享栈」机制（`sharedInstance`/`aboutToDestroy`）→ 同上，退回自建

★ 也有**一处刻意不照抄**：上游进程内 DNS 的 `forwardDns` 是每条查询一个 `QUdpSocket` +
   一个 5s 定时器；v1.1 早在 DNS 洪水那次换成了共享 socket + 事务 ID 多路复用
   （旧写法实测把一台设备的 DNS 变成拖垮所有设备的放大器）。照抄等于把修好的坑重挖。

## 2. ⛔ 万兆：这条路在 Windows 上不通

用已有实测数拉的账：

| 环节 | 每 Gbps 成本 | 万兆需要 | 出处 |
|---|---|---|---|
| **Windows 用户态数据面**（修完之后） | **2.2 核** | **22 核** | `gateway-bottleneck-audit.md` §9（FakeEp，CPU 直通故可信） |
| Linux TPROXY（内核转发） | 0.25 核 | 2.5 核 | `NetStack.h` 千兆对照 |
| mihomo 中继 | 0.38（直连）/ 0.48（加密） | 3.8~4.8 核 | 同上 |

**三条硬约束，本机一条都不满足：**

1. **物理链路**：本机网卡是 QEMU 模拟的 e1000，**1 Gbps**。
   PCI ID 是决定性证据：`VEN_8086&DEV_100E`（82540EM）+ `SUBSYS_...1AF4`（Red Hat/Qumranet）。
   直通的话这里会是真实型号与真实子系统 ID。（CPU 确实是直通的真 i7-10700F —— 所以
   **CPU-bound 的测量可信，网卡相关的不可信**，这个区分很重要。）
2. **数据面**：Windows 用户态 2.2 核/Gbps，且**单线程**——聚合实测天花板 **1080 Mb/s**，加核摊不开。
3. **中继**：万兆经 mihomo 就是 3.8~4.8 核，占掉半台 8 核机器。

**「进不进内核都要万兆」两条都算过：**
- 不进内核（进程内出站）：省掉中继那 4 核，但我们自己的数据面还是 2.2 核/Gbps → 22 核
- 进内核：Linux TPROXY 2.5 + 中继 4.5 ≈ 7 核，**8 核能跑**；但 **Windows 没有等价物**
  （WFP 内核重定向卡在 EV 证书 + 微软 attestation 签名，见 `WfpRedirect.h` —— 资质问题，非代码）

⇒ **要万兆只有一条现成的路：网关跑 Linux + TPROXY + 真 10G 网卡。**
  代码已经有（`gatewayTproxy: true` 是 Linux 默认），缺的是硬件。

## 3. ★ 欠账清单（照实写，别当已完成）

### 3.1 `coastcore: true` 只跑过合成对端

`COAST_GW_COASTCORE_SELFTEST` 证明了那五个阶段的代码**会被执行**（`ccInProcess` 涨、
SOCKS 零回退），这是它第一次被真正执行。但它用的是**合成设备 + 内建 DIRECT 出站**，
**没有真实节点、没有真实设备、没有真实流量**。离"能用"还差得远。

### 3.2 CI 从没跑过

- OpenSSL 那段是**推测** runner 上有 `C:\Program Files\OpenSSL`，没验证过
- arm64 的「找不到就降级」路径也没验证过
- 静态链在 **MSVC** 上是否同样零 DLL 依赖，只在 MinGW 上验过

### 3.3 阶段 5 的 TUN 只编过、没跑过

`LocalTunService::selfTest()` 会**真的接管默认路由**，需要 root + 可牺牲网络的机器。

### 3.4 真机验证：步骤与判据

这是现在最该做的一步，且**我在这台机器上做不了**（需要真设备接进来）。

1. `config.yaml` 写 `coastcore: true`，用**真实订阅**
2. 劫持一台真设备，正常上网若干分钟
3. 看 `<userDir>/logs/gateway-diag.log` 的 `cc=` 一栏：
   `cc=<进程内>/<无路由>/<节点缺>/<协议缺>/<UDP不支持>/<严格拒绝>`

**判据**：
- `ccInProcess` 明显 > 0 → 进程内出站真的在用
- **其余五栏接近 0** → 才说明「不依赖核心」成立；哪一栏大就查哪一类
- `fakeipResolved` > 0 → 域名类流量真的进了进程内（恒 0 = 只有 IP 直连类有效）
- `dnsLocal=<fake>/<forward>` 前者 > 0 → DNS 也不经核心了

4. 想把差距**逼出来**：临时开 `coastcore_strict: true`——判不了的直接失败而不是静默回退。
   静默回退会把差距藏起来，这正是严格模式存在的理由。

⚠️ 别在没有真机数据之前把 `coastcore` 默认值改成 true。当前默认 false 是刻意的。
