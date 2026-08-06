# TIDE 线格式规范

> **draft-00 · 未冻结 · 不要据此实现服务端**
>
> 本文是规范性文本，只描述"是什么"。每个字段"为什么存在"见 [`design.md`](design.md)，
> 建议先读完那篇——脱离取舍论证看本文，会觉得很多字段是多余的。

本文中 **MUST / MUST NOT / SHOULD / MAY** 按 RFC 2119 解释。

---

## 1. 术语与约定

| 术语 | 含义 |
|---|---|
| 外层信道 | 承载 TIDE 的加密通道：TLS 1.3（TCP 路径）或 QUIC-TLS（QUIC 路径） |
| 会话 (Session) | 由 Session ID 标识的逻辑连接，**独立于任何一条路径存在** |
| 路径 (Path) | 承载会话的一条具体传输连接 |
| 流 (Stream) | 会话内的一条应用层字节流，对应一个被代理的 TCP 连接 |

- 所有整数为**大端序**。
- `varint` 采用 QUIC 变长整数编码（RFC 9000 §16）：首字节高 2 位给出总长度 1/2/4/8 字节。
- `AEAD(k, plaintext)` 默认 ChaCha20-Poly1305；若两端均通告 AES-NI 支持则为 AES-256-GCM。

---

## 2. 帧格式

所有 TIDE 通信（握手之后）由帧构成，帧位于外层信道内部。

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     type      |     flags     |     length (varint)           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     stream_id (varint)                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          payload ...                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 padding ...（PAD 标志置位时）                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

`length` 覆盖 `payload + padding` 的总长，接收方 MUST 依据 `PAD` 标志与 `pad_len` 区分两者。

### 2.1 帧类型

| type | 名称 | 方向 | 说明 |
|---:|---|---|---|
| 0x01 | `HELLO` | C→S | 首次握手，见 §3.1 |
| 0x02 | `ACCEPT` | S→C | 握手响应 + 首批票据，见 §3.2 |
| 0x03 | `ZERO_RTT` | C→S | 0-RTT 握手，见 §3.3 |
| 0x10 | `STREAM_OPEN` | 双向 | 新建流，payload 为目标地址 |
| 0x11 | `STREAM_DATA` | 双向 | 流数据 |
| 0x12 | `STREAM_FIN` | 双向 | 单向关闭 |
| 0x13 | `STREAM_RST` | 双向 | 异常关闭，payload 为错误码 |
| 0x20 | `DATAGRAM` | 双向 | UDP 数据报，见 §5 |
| 0x30 | `PATH_PROBE` | 双向 | 路径质量探测 |
| 0x31 | `PATH_ACK` | 双向 | 探测响应，携带时间戳 |
| 0x32 | `PATH_MIGRATE` | 双向 | 声明将流迁移至另一路径 |
| 0x40 | `TICKET_REPLENISH` | S→C | 增量补充单次票据 |
| 0x50 | `PADDING` | 双向 | 纯填充帧，接收方 MUST 丢弃 |
| 0x5F | `CLOSE` | 双向 | 关闭会话 |

未知 type 的帧接收方 MUST 忽略并跳过（依 `length`），以便后续版本扩展。

### 2.2 标志位

| bit | 名称 | 含义 |
|---:|---|---|
| 0 | `PAD` | payload 之后有填充；填充区首字节为 `pad_len`（varint） |
| 1 | `END` | 本帧为该流的最后一帧（等价于紧跟 `STREAM_FIN`） |
| 2 | `PUSH` | 提示立即冲刷，不要等待聚合 |
| 3–7 | — | 保留，发送方 MUST 置 0，接收方 MUST 忽略 |

---

## 3. 握手

### 3.1 `HELLO`（首次连接，1-RTT）

在外层 TLS 握手完成后，作为**第一个应用数据记录**发送。

```
HELLO {
    version       : u8      = 0x00        // draft-00
    kem_share     : X25519_pub(32) || MLKEM768_ct(1088)
    client_random : 32 bytes
    sealed_auth   : AEAD(k_hs, auth_plain)
    early_data    : opt bytes             // 可携带首批载荷
}

auth_plain {
    user_id   : 16 bytes
    timestamp : u64        // Unix 秒
    cb_hash   : 32 bytes   // 见 §4
    flags     : u8         // bit0: 请求 bare 模式
}

k_hs = HKDF-Extract-Expand(
           salt = client_random,
           ikm  = X25519_shared || MLKEM_shared,
           info = "tide/draft-00 handshake" || transcript_hash )
```

服务端 MUST 校验 `timestamp` 在 ±120 秒容差内，且 `cb_hash` 与本连接实际的 TLS Exporter 相等。任一校验失败 → 走 §6 的失败关闭流程。

### 3.2 `ACCEPT`

```
ACCEPT {
    session_id  : 16 bytes
    mode        : u8            // 0=sealed, 1=bare
    ticket_base : u64           // 首张票据的 ticket_id
    ticket_count: u16           // 默认 1024
    ticket_seed : 32 bytes      // 票据密钥由此派生，见下
    server_data : opt bytes
}

ticket_key[i] = HKDF-Expand(ticket_seed, "tide/ticket" || u64(ticket_base + i), 32)
```

只下发一个 32 字节种子而非 1024 把密钥，是为了让 `ACCEPT` 保持小帧——整批密钥两端各自派生即可。

服务端 MUST NOT 在 `mode` 中通告 `bare`，除非外层信道提供 AEAD 且 §4 的信道绑定校验已通过。

### 3.3 `ZERO_RTT`（后续连接，0-RTT）

```
ZERO_RTT {
    ticket_id : u64
    nonce     : 12 bytes
    sealed    : AEAD(ticket_key, cb_hash || timestamp || early_data)
}
```

服务端处理流程（顺序 MUST 严格保持）：

1. 按 `ticket_id` 查该用户的**未消费票据位图**。
2. 若已消费或超出 `[base, base+count)` 范围 → **静默丢弃，转入 §6 失败关闭流程**。MUST NOT 返回任何区别于掩护站点的响应。
3. 置位（标记消费）。此步 MUST 在解密 `early_data` **之前**完成，且对同一 `ticket_id` 的并发请求 MUST 原子化，否则重放保护失效。
4. 派生会话密钥 `k_sess = HKDF(ticket_key || cb_hash)`，处理 `early_data`。

票据 MUST 在签发后 24 小时过期，即使未被消费。

> **多节点部署警告**：位图是**服务端硬状态**。若同一用户可能落在不同节点上，位图必须共享（Redis 或按 user_id 一致性哈希路由），否则重放保护在节点间失效——攻击者只需把重放流量投给另一个节点。这是本协议唯一显著增加运维负担的地方，实现前 MUST 先定方案。

---

## 4. 信道绑定

```
cb_hash = TLS-Exporter( label   = "tide-channel-binding",
                        context = empty,
                        length  = 32 )
```

QUIC 路径使用 QUIC-TLS 的等价导出器。

客户端 MUST 将 `cb_hash` 纳入 `sealed_auth`（§3.1）或 `sealed`（§3.3）的被认证数据；服务端 MUST 独立计算并比对。不匹配 → 失败关闭，MUST NOT 降级重试。

此机制使任何终止并重建外层 TLS 的中间实体（企业 MITM CA、CDN 回源、透明代理）都无法完成握手，同时是启用 `bare` 模式的前提。

---

## 5. UDP

```
DATAGRAM payload {
    assoc_id : varint       // 会话内的 UDP 关联标识
    addr     : SOCKS5 地址格式（ATYP + ADDR + PORT）
    data     : bytes
}
```

身份信息挂在**会话**上，而非每个数据报上。因此不存在 SOCKS5 UDP 中继那个已知问题——那里的中继是共享 socket、数据报不携带认证，客户端不得不在 ASSOCIATE 请求中申报真实来源地址来让服务端做 `addr → user` 归属（参见仓库根 `CLAUDE.md` 关于 Windows 侧 SOCKS UDP 的说明）。TIDE 中该问题在架构上不存在。

---

## 6. 失败关闭

任何认证失败（`sealed_auth` 校验失败、`cb_hash` 不匹配、票据已消费、时间戳超窗）时，服务端：

1. MUST NOT 返回任何 TIDE 帧或错误指示。
2. MUST 将该连接**已接收和后续全部字节**原样代理至掩护源站，直至任一端关闭连接。
3. MUST NOT 对该路径做特殊的超时、限速或日志分支处理。

> **关键**：不能"模拟"掩护站点的响应，必须**真的转发**。若认证失败路径耗时 0.1ms 而真实站点路径耗时 50ms，探测方只需测量响应时间分布即可区分两者，伪装随即全部作废。**时序是这里唯一真正难伪造的东西。**
>
> 因此掩护源站 MUST 真实可达且延迟合理（同机房或本机）。

---

## 7. 填充调度

发送方 MUST 按连接生命周期分三阶段调整填充（阈值为默认值，MAY 配置）：

| 阶段 | 触发条件 | 策略 |
|---|---|---|
| 1 · 判决窗口 | 前 64 KiB 或前 100 帧（先到为准） | 每帧填充至从 HTTPS 浏览包长分布采样的目标长度 |
| 2 · 衰减 | 其后 256 KiB | 填充概率自 1.0 线性衰减至 0 |
| 3 · 批量 | 之后 | 停止填充，MTU 满帧 |

接收方 MUST 无条件接受任何阶段的任意填充量——填充策略是**发送方本地决策**，不参与协商，以便未来单方面演进。

---

## 8. 多路径

- 会话可同时绑定多条路径。每条路径独立完成 §3 握手，并在 `HELLO`/`ZERO_RTT` 中携带已有 `session_id` 以加入现有会话。
- 调度器 SHOULD 默认自 TCP 路径起步，后台以 `PATH_PROBE` 低频探测 QUIC 路径。
- 当 TCP 路径丢包率持续超过 2% 时，SHOULD 将批量流迁移至 QUIC 路径，交互流保留在 TCP。
- 单条流的帧 SHOULD 保持路径亲和，以避免跨路径乱序开销；迁移经 `PATH_MIGRATE` 显式声明。
- QUIC 路径探测失败（UDP 被封锁）时，MUST 静默全量回落 TCP，不得向用户暴露错误。
- 拥塞控制每路径独立，SHOULD 使用 BBRv3。MUST NOT 默认启用无上界的激进拥塞控制。

---

## 9. 版本协商

`HELLO.version` 为单字节。服务端若不支持该版本，MUST 执行 §6 的失败关闭流程，**MUST NOT 返回版本错误**——任何区别性响应都是可被探测的指纹。

客户端据此无法区分"版本不支持"与"服务器不存在"，这是刻意的：可探测性的代价由运维承担（部署时确认版本匹配），而非由协议放宽。

---

## 10. 未定项（draft-00 待决）

以下是已知需要在 draft-01 前定稿的问题，**不是遗漏**：

1. §3.3 票据位图的多节点同步方案——单机与集群需要不同答案。
2. §7 阶段 1 所用 HTTPS 包长分布的具体拟合数据来源与更新机制。
3. `bare` 模式下 kTLS 的可用性探测与回退路径（内核版本、网卡支持差异）。
4. 票据耗尽（1024 张用完而未及时补充）时的降级行为——退回 1-RTT 还是阻塞等待补充。
5. `PATH_MIGRATE` 期间的流内乱序处理边界。
