# 网关性能审计：到底堵在哪个环节（2026-08-04 重审）

> 起因：`lwip-alternatives.md` R19–R21 的结论是「Windows 网关的绑定约束是 **Npcap 发送路径**」。
> 那个结论**在当时是对的**（测的是 lwIP + Npcap）。但 lwIP 已经在今天被移除、smoltcp 成为唯一
> 数据面 —— 所以这次不看旧结论，从代码重新走一遍每个包的实际路径。
>
> **结果是绑定约束换人了。** 今天堵在这里的不是 Npcap，是一个此前没人量过的环节。

---

## 结论先行

| 环节 | 实测/推算上限 | 是不是今天的瓶颈 |
|---|---|---|
| **泵周期量化（25 ms）** | 📏 **≈21 Mb/s / 连接，上下行都是** | ★ **是。且远比其它项紧** |
| Npcap TX（修完 loopback） | 📏 572 Mb/s | 否 —— 现在够不着它 |
| smoltcp CPU | 📏 0.21 核/Gbps | 否 |
| 核心中继（DIRECT） | 📏 0.38 核/Gbps | 否 |

📏 = 实测。泵量化那一行来自本次新增的 `measure_realistic_upstream_ceiling`
（`rust/coaststack/src/e2e.rs`），可复现：`cargo test --release measure_realistic -- --nocapture`。

> ### ⚠️ 这个数被我改了三次，过程保留 —— 它本身就是结论的一部分
> 前两版都错，而且方向相反；第三版才收敛。
>
> | 版本 | 测法 | 结果 | 错在哪 |
> |---|---|---|---|
> | 1 | `measure_bytes_per_poll_cycle` | 41.9 Mb/s | 灌**出窗**数据 → 量到的是收缓冲容量(128 KiB)，不是设备被允许发的量。**偏乐观 2×** |
> | 2 | `measure_realistic_upstream_ceiling` | 20.6 Mb/s | 合成 SYN **不带 WS 选项** → 窗口缩放没生效，被 RFC 封在 65535。**前提错了，数字却撞对了** |
> | 3 | `measure_ceilings_with_window_scaling` | **20.8 / 21.0 Mb/s** | 带 WS。中途还踩了一次：对 SYN-ACK 的 window 套了 shift，而 **RFC 7323 规定 SYN 段的窗口不缩放** → 灌超量 → 假设备不重传 → 流停摆，量出个假的 "0.8 Mb/s" |
>
> ```
> [WS  ] SYN-ACK window=65535 shift=2 → 我方有效接收窗口 262140 B
> [UP  ] 4222320 B / 65 拍  → 20.8 Mb/s/连接（最大有效窗口 66832 B）
> [DOWN] 上线 19660800 B / 300 拍 → 21.0 Mb/s/连接
> ```
>
> **三条路的失效模式互不相同，却落在同一点（≈64 KiB/拍）** —— 这比任何单条测量都可信。
>
> ★ 关键的反直觉之处：**窗口缩放开不开，最终数字几乎不变**（20.6 → 20.8）。
> 因为绑定约束不是窗口的**上限**，而是**每个 poll 周期能腾出/补上多少** ——
> 收缓冲要等 `recved()` 才释放、发缓冲要等 ACK 被处理才腾空，而这两件事都只在 poll 里发生。
> 所以哪怕把 `RX_BUF` 和窗口都调大，只要拍子还是 25 ms，稳态仍是一拍一窗。
> 这也说明**单靠调大缓冲救不了**，必须动拍子。

---

## 一、根因：整条数据面被量化到 40 Hz

### 代码事实（三处，可逐一核对）

1. **喂帧不处理**。`engine::input()`（`engine.rs:322`）只做「学邻居 → 改写端口 → `push_rx`」，
   **不调 `iface.poll()`**。帧就躺在收队列里。
2. **只有一个地方真正推进协议栈**：`poll_collect()`（`engine.rs:493`）—— 收包解析、生成 ACK、
   产生 `ConnData` 事件、`drain_tx` 出帧，全在它里面。
3. **它唯一的调用点是 25 ms 的定时器**：`NetStack.cpp:1006`，泵的 lambda 里。
   `coast_conn_send()` / `coast_conn_recved()`（`engine.rs:761/779`）也都**只动 socket 缓冲、
   不出帧** —— 下行数据写进去、窗口还回去，都要等下一拍才变成线上的包。

于是每个包的实际时序变成：

```
设备发包 ──► Npcap(事件驱动,快) ──► 收队列 ┅┅ 最多等 25 ms ┅┅► poll: 解析+交给 SOCKS
SOCKS 回数据 ──► coast_conn_send(只写缓冲) ┅┅ 最多等 25 ms ┅┅► poll: drain_tx 才出帧
C++ 还窗口 ──► coast_conn_recved(只动缓冲) ┅┅ 最多等 25 ms ┅┅► poll: 窗口更新 ACK 才上线
```

### 实测代价

```
[UP  ] 4222320 B / 65 拍  → 20.8 Mb/s/连接
[DOWN] 上线 19660800 B / 300 拍 → 21.0 Mb/s/连接
```

上行 64958 B/拍、下行 65536 B/拍 —— **两个方向都是一拍搬一个窗口**，
所以这不是"上行有个坑"，是**整条数据面**被同一个拍子卡住。

每拍搬运量 = **通告窗口**，与窗口/RTT 的经典公式一致 —— 只不过这里的「RTT」是
**我们自己的泵周期**，不是链路 RTT。设备与网关同在局域网、真实 RTT 亚毫秒，
却被我们自己的 25 ms 拍子当成了往返时延。

~~**通告窗口只有 65535，没开窗口缩放**~~ —— **这条已证伪**。smoltcp 0.12 支持 WS
（`socket/tcp.rs` 的 `remote_win_scale` / `remote_win_shift`，SYN-ACK 会回选项），
实测设备带 WS 时它回 shift=2。之所以第二版量到 65535，是**我的合成 SYN 没带 WS 选项**。
带上之后有效窗口 262140 B，而吞吐**几乎没变**（20.6 → 20.8）—— 见上面那段反直觉说明。

延迟侧同样：每一个方向的每一跳都 **+12.5 ms 均值 / +25 ms 最坏**。一次 TCP 握手经网关要多
25–50 ms，网页首字节直接慢一档。

### 为什么以前没暴露

- **lwIP 是同步的**：`nif.input(p, &nif)` 当场走完 `ethernet_input → ip4_input → tcp_input →
  回调`，数据立刻交给 SOCKS，`tcp_output` 就地出帧。`pumpToLwip` / `giveBackRecvWindow` /
  `closeConn` 各自直接调 `flushNicTx` —— 那 **6 个 flushTx 收口点**存在的理由正是「出帧发生在
  很多时刻，25 ms 的泵只能当兜底」。
- 代码里甚至写着这句警告：*「别拿它当主路（25 ms 的延迟对 TCP 自时钟是致命的）」*
  —— 而 smoltcp 桥接把**所有**路径都变成了那条兜底路。
- **默认值挡住了它**：`COAST_STACK` 一直默认 `lwip`，smoltcp 只被自测和一次真机试跑碰过。
  自测是 `input(); poll();` 成对调用的，**结构上不可能发现这个问题**。
- ★ 今天移除 lwIP 之后，**这条路成了唯一的默认路径**。

---

## 二、修它之前必须一起修的一处：`pump_conns` 每拍重拷整个积压

`engine.rs:620-637`：

```rust
let avail = s.recv_queue();
if avail > c.peeked {
    let mut tmp = vec![0u8; avail];          // 按**整个积压**分配
    let got = s.peek_slice(&mut tmp)...;     // 把整个积压拷一遍
    buf.copy_from_slice(&tmp[c.peeked..got]);// 只为取出末尾那几百字节
}
```

因为背压要求「先不 recv」，`c.peeked` 会一直攒着；每有新数据到达，就把**已经交出去过的那部分
再拷一遍**。峰值 128 KiB × 每拍 × 每条连接。

今天 40 Hz，代价是 5 MB/s/连接 —— 难看但不致命。**一旦按下面的方案改成按需 poll（上千次/秒），
它会立刻变成 CPU 瓶颈**：把一个延迟问题换成一个 CPU 问题。所以两件事必须同批做。

（当前 smoltcp 0.12 没有带偏移的 peek。最低成本的缓解是把 `tmp` 挪进 `Conn` 复用，先消掉每拍
的堆分配；彻底消掉那次拷贝需要另设计，不该塞进同一次改动里。）

---

## 三、修法（建议形状，尚未实施）

保留 25 ms 定时器**只当兜底**（重传/超时仍需要它），另加「有活就 poll」：

```cpp
// 把一次事件循环内产生的所有工作合并成一次 poll —— 不是每帧一次（那是 O(连接数)/帧）。
void smolSchedulePoll(NetStack::Impl *d) {
    if (d->pollScheduled) return;
    d->pollScheduled = true;
    QMetaObject::invokeMethod(d->owner, [d] {
        d->pollScheduled = false;
        coast_stack_poll(d->smol, d->smolClock.elapsed());
        flushNicTx(d);
    }, Qt::QueuedConnection);
}
```

调用点正好对应 lwIP 那 6 个 flushTx 收口点的角色：
- `inputFrame()` 末尾（收帧批 → 合并成一次）
- `smolPumpToStack()` 里 `coast_conn_send` 之后（下行数据到了）
- `smolFlushRecvWindow()` 里 `coast_conn_recved` 之后（窗口还了，ACK 要立刻上线）

⚠️ 收帧是**整批**从 RX 线程投递过来的（`RxWorker` 一次 posted event 一批），所以「每批一次
poll」天然成立，不会退化成每帧一次。

**验证判据**（别只看能不能跑）：三条测量的语义都会变 —— 它们现在都假设「一拍 25 ms」，
改完之后该测的是**单位时间的 poll 次数**与端到端吞吐。
★ 三条都**不要删**：它们的失效模式互不相同，正是靠这一点才收敛出可信的数。
把它们改成"给定 N 次 poll/秒，吞吐是多少"，仍是无真机条件下唯一能证伪的量。

### ⚠️ 这些数全部来自合成对端，**没有一条来自真实 TCP**

三次返工都是同一类错误：**用合成对端模拟真实 TCP 比看上去难**（出窗判定、
SYN 段窗口不缩放、丢包重传……每一条错了都能量出一个看着很像真的假数）。
所以：

- **机制**（整条数据面量化到 40 Hz）—— 确定，三处代码事实可独立核对，不依赖任何测量。
- **数字**（≈21 Mb/s/连接）—— 三个模型收敛，可信，但**仍是合成的**。
- **真机没测**：真实设备 + 真实链路 + 真实 SOCKS/核心叠加之后是多少，没量过；
  N 条连接是否干净地 ×N，也没量过（浏览器多连接场景可能没这么难看，
  但单流下载/测速会直接撞上）。

真要定案，正确做法是让**真实内核 TCP** 当对端 —— `GatewaySelfTest.cpp` 里那套
TAP + 静态邻居 + 真实 curl 的脚手架就是干这个的（现在是 Linux-only，而 NetStack 已收进
Windows；这正是"TPROXY/pf 没有端到端自测"那个已知缺口的另一面）。
真机 A/B 前照例先确认接管生效（靶机 `ip neigh` 的网关 lladdr 已翻成本机 MAC）。

---

## 四、顺带记下的次要项（都不是当前瓶颈，别顺手优化）

- **上行每字节约 8 次拷贝**：Npcap 内核缓冲 → `RxWorker` 深拷 → `engine::input` 的 `frame.to_vec()`
  → smoltcp 收缓冲 → `tmp` → `buf` → C++ `QByteArray` → `QTcpSocket` 写缓冲。
  其中 `tmp`/`buf` 那两次是上面第二节那个问题，其余属于跨线程/跨语言边界的固有成本。
- **SOCKS 环回**：每个字节多穿两次内核边界（写环回 socket + 核心读）。这是「用户态栈 + SOCKS」
  架构的固有代价，也正是 `npcap-alternatives.md` 里「让 Windows 转发进核心 TUN」那条路
  能一次性省掉的部分。
- **`NetStack.h` 里那组"单线程会不会饿死"的实测**（p50 不劣化、仅尾部毛刺）是 **lwIP 时代**
  测的，同步模型下成立。smoltcp + 40 Hz 量化之后那组数据**不再适用**，需要重测。

---

## 五、诊断盲区（这次审计暴露的元问题）

`GatewayDiag` 现有的计数器（`pump` / `late` / `maxLagMs` / `usPerTx` / `fpb` / `tcpXmit` …）
**一个都看不见这个瓶颈**：

- `pump`/`late`/`maxLagMs` 量的是「泵有没有按时到」—— 泵**准时**得很，问题是它**太稀**；
- `usPerTx`/`fpb` 量的是 Npcap 侧，而现在根本喂不满它；
- 没有任何一个计数器回答「一个包从进收队列到被处理等了多久」。

这套指标是围绕 lwIP 的**同步**模型设计的，换成 poll 模型之后语义就漏了。
**建议补一条**：poll 周期内处理的帧数 / 距上次 poll 的间隔（即"每拍吃进多少"），
它正是本次量出来的那个量，且能在生产日志里直接看出天花板。

---

## 附：这次审计推翻了什么、没推翻什么

- **没推翻** R19–R21：那三轮测的是 lwIP + Npcap，当时 lwIP 同步、确实是 Npcap 更紧。
  那个 `PacketSetLoopbackBehavior` 的 2.6× 依然是真实收益。
- **推翻的是它的时效性**：R22 的 smoltcp 移植引入了一个更紧的约束，而移植的验证
  （功能自测 + ABI 自测）**结构上无法发现它** —— 自测总是 `input(); poll();` 成对调用。
- **教训**：换执行模型（同步 → poll）时，功能等价不等于性能等价。旧的性能结论
  **必须连同"它成立的前提"一起复核**，而不是继承。
