# CoastCore 出站协议「真机拨通」验证台

验证**进程内出站**（`src/net/core/proto/*`）是否真的能按协议线格式拨通 —— 编译通过 ≠ 线格式对，
这一层只有对着真服务端跑才验得出来（Shadowsocks 就是这么抓到 `established` 早发一步的 bug 的，
见 commit `290476c`）。

## 思路

不依赖任何外部机场节点（免费节点常年不稳、协议还未必是我们支持的那几种），而是**用 mihomo 自己当
参照服务端**：起若干 inbound listener（ss / vmess / trojan / vless）+ `MATCH,DIRECT`，
再让 CoastCore 的对应出站去拨它、取一个真实网页。链路：

```
harness(只含出站层, Qt+OpenSSL, 不含 lwIP)
   → CoastCore 的 ShadowsocksOutbound / VmessOutbound / TrojanOutbound / VlessOutbound
   → mihomo(参照服务端, 本机 18388~18391)
   → DIRECT → 目标站 → 200 回包解密
```

mihomo 是这些协议的权威实现，能被它接受并成功回包，就说明我们的握手/加密/分帧是对的。

## 用法（在能跑 Linux + Qt6 + OpenSSL 的机器上，例如树莓派测试台）

```bash
# 1) 证书（trojan/vless 的 inbound 需要 TLS）
#    ★ 必须放在 mihomo 的 -d 家目录内：它有 SAFE_PATHS 限制，
#      否则报 "path is not subpath of home directory or SAFE_PATHS"
mkdir -p /tmp/nodesrv2
openssl req -x509 -newkey rsa:2048 -keyout /tmp/nodesrv2/key.pem -out /tmp/nodesrv2/cert.pem \
  -days 365 -nodes -subj "/CN=test.local" -addext "subjectAltName=DNS:test.local,IP:127.0.0.1"

# 2) 起两个参照服务端（配置见本目录 nodesrv.yaml / nodesrv2.yaml）
CORE=~/.local/share/Coast/command/core
systemd-run --unit=nodesrv  "$CORE" -d /tmp/nodesrv  -f nodesrv.yaml
systemd-run --unit=nodesrv2 "$CORE" -d /tmp/nodesrv2 -f nodesrv2.yaml
ss -ltn | grep -E ':1838[89]|:1839[01]'   # 期望 4 个都在听

# 3) 编 harness 并跑（CMakeLists 见本目录）
cmake -B build -G Ninja && cmake --build build && ./build/obh
```

期望输出（fails=0）：

```
SS      PASS  est=1 bytes=5746  status='HTTP/1.1 200 OK'
VMESS   PASS  est=1 bytes=20130 status='HTTP/1.1 200 OK'
TROJAN  PASS  est=1 bytes=1670  status='HTTP/1.1 200 OK'
VLESS   PASS  est=1 bytes=1670  status='HTTP/1.1 200 OK'
== fails=0 ==
```

## 踩过的坑（照抄能省很多时间）

- **harness 必须定义 `COAST_HAVE_OPENSSL`**：ss / vmess / reality 的注册在 `OutboundRegistry.cpp`
  里被这个宏门控（见 CMakeLists 的 `COAST_HAVE_OPENSSL`），不定义就表现为「未注册该 type」。
- **证书路径受 mihomo SAFE_PATHS 限制**，见上。
- **mihomo 不允许「裸 vless」inbound**（`disallow using Vless without any certificates/...`），
  必须给 certificate + private-key（本台就是这么做的，出站侧 `tls=true` + `skipCertVerify=true`）。
- 远端别用 `pkill -f "<含 /tmp/nodesrv 的串>"` 停服务：那个模式会**匹配到自己这条 ssh 命令行**、
  把当前 shell 一起杀掉（表现为命令莫名截断）。用 `systemctl stop <unit>`。
- **harness 自己的坑**：用例的超时守卫不能用 `QTimer::singleShot(ms, lambda捕获&loop)` ——
  用例提前成功后它并不会被取消，稍后会在**下一个用例**的事件循环里触发，去 quit 一个早已析构的
  `QEventLoop` → **段错误**。加了耗时更长的 UDP 用例后才炸出来（栈顶是 `QEventLoop::exit`，
  第 1 帧却是上一个用例的 lambda）。改用**栈上的 QTimer**，随本帧销毁而取消。

## 覆盖范围

| 协议 | 状态 |
|---|---|
| Shadowsocks (AEAD) | ✅ 已验 |
| VMess (VMessAEAD) | ✅ 已验 |
| Trojan (over TLS) | ✅ 已验 |
| VLESS (over TLS) | ✅ 已验 |
| REALITY (自建 TLS1.3 + uTLS) | ✅ 已验（对真 xray 25.6.8；**dest 必须支持 TLS 1.3**，见下） |
| Hysteria2 TCP (QUIC + HTTP/3 认证) | ✅ 已验（对**官方 hysteria 2.10.0 服务端**；不能拿 mihomo 当服务端，见下） |
| Hysteria2 UDP (QUIC 数据报中继) | ✅ 已验（经中继查真 DNS，校验 ID/QR/ANCOUNT） |
| TUIC | ⛔ 上游卡住：token 要 msquic 的 keying-material exporter，**msquic 最新发行版 v2.5.9 没有**（见下） |
| VLESS over ws/grpc | ❌ 未覆盖（ws 传输本身有独立实现，grpc 尚未实现） |

## QUIC 系（Hysteria2 / TUIC）

### 怎么跑

```bash
# 0) msquic：官方 deb **只装 .so、不装头**，头要自己从对应 tag 取
curl -O https://packages.microsoft.com/debian/12/prod/pool/main/libm/libmsquic/libmsquic_2.5.9_arm64.deb
dpkg -i libmsquic_2.5.9_arm64.deb
mkdir -p /opt/msquic/include /opt/msquic/lib
for f in msquic.h msquic_posix.h quic_platform_posix.h quic_sal_stub.h; do
  curl -sfLo /opt/msquic/include/$f     https://raw.githubusercontent.com/microsoft/msquic/v2.5.9/src/inc/$f
done
ln -sf /usr/lib/aarch64-linux-gnu/libmsquic.so.2 /opt/msquic/lib/libmsquic.so

# 1) Hysteria2 参照服务端 = **官方 hysteria**（见 hy2srv.yaml；证书复用 /tmp/nodesrv3/）
curl -Lo hysteria "https://github.com/apernet/hysteria/releases/download/app%2Fv2.10.0/hysteria-linux-arm64"
chmod +x hysteria && systemd-run --unit=hy2srv ./hysteria server -c hy2srv.yaml   # :18395

# 2) TUIC 参照服务端 = mihomo 的 tuic listener（nodesrv3.yaml，:18394）

# 3) 编 + 跑
cmake -B build -G Ninja -DCOAST_MSQUIC_ROOT=/opt/msquic && cmake --build build
LD_LIBRARY_PATH=/opt/msquic/lib ./build/obh
```

期望：`QPACK PASS` + `HY2 PASS` + `HY2UDP PASS`（TUIC 见下，会 SKIP）。

`HY2UDP` 是经 Hy2 的 UDP 中继向真 DNS 服务器（默认 223.5.5.5，可用 `./build/obh <dns-ip>` 覆盖）
发一条查询，校验回包的 ID/QR/ANCOUNT —— 一次同时验了「请求出得去」和「回包按源地址还原得回来」。

排 UDP 问题时的参照（**先确认它是绿的再怀疑自己**）：官方 hysteria 客户端自带 UDP 转发，
```yaml
# hy2cli.yaml
server: 127.0.0.1:18395
auth: hy2pass123
tls: { sni: test.local, insecure: true }
udpForwarding:
  - { listen: 127.0.0.1:15353, remote: 223.5.5.5:53 }
```
`./hysteria client -c hy2cli.yaml` 然后 `dig www.baidu.com @127.0.0.1 -p 15353`，
服务端应打出 `UDP request ... reqAddr: 223.5.5.5:53`。

### ★★ 坑一：msquic 的 2^60 off-by-one —— **不能拿 mihomo/sing-box 当 hy2 服务端**

一开始我用 mihomo 的 `hysteria2` listener 当参照，结果 **QUIC 握手就崩**，服务端**一行日志都没有**
（因为它的 `Accept` 根本没返回）。查下来是 msquic 的 bug：

- `msquic/src/core/quicdef.h`：`QUIC_TP_MAX_STREAMS_MAX = ((1ULL << 60) - 1)`，
  `crypto_tls.c` 里 `if (InitialMaxBidiStreams > QUIC_TP_MAX_STREAMS_MAX) → 错误`；
- `metacubex/sing-quic` 的 hy2 服务端：`quicConfig.MaxIncomingStreams = 1 << 60`（**正好 2^60**）；
- RFC 9000 §4.6 说的是「**大于** 2^60」才非法 —— **2^60 本身合法**。msquic 把合法值判死了，
  且 `main` 分支至今未修。

影响范围（对用户）：**真实 hy2 节点不受影响** —— 官方 hysteria 服务端用的是
`defaultMaxIncomingStreams = 1024`（`apernet/hysteria` 的 `core/server/config.go`）。
只有当对端服务端是 **mihomo / sing-box** 时才会撞上，那种情况下 hy2 节点会**回退到 mihomo 内核**，
功能不受损。所以参照服务端必须用官方 hysteria，测试台上这一条不是我们的 bug。

排查顺序（血泪）：先拿**纯 msquic 探针**（几十行 C，见下）打一遍，把「msquic 能不能和 quic-go 握手」
和「我们的封装对不对」切开。当时探针的结果是：TUIC listener ✅、公网 Cloudflare HTTP/3 ✅、
故意写错 ALPN ✅ 报 `no_application_protocol`、**只有 hy2 listener ❌** —— 一眼定位到是服务端配置差异。

### 真机上抓到的三个我们自己的 bug（都只在真服务端下暴露）

1. **入向流额度通告成 0**（`QuicTransport::openConnection` 传了 `nullptr` 设置）。
   `PeerUnidiStreamCount` 是「允许**对端**向我们开多少条流」，msquic 默认 **0**；而 HTTP/3 要求
   服务器能开它自己的控制流。表现极具迷惑性：**QUIC 握手成功、认证请求也送达了**，服务端却
   一声不响地关连接。quic-go 的帧日志（`QUIC_GO_LOG_LEVEL=debug`）一眼看穿：
   ```
   Processed Transport Parameters: ... MaxUniStreamNum: 0
   server <- StreamFrame{StreamID: 2 ...}   ← 我们的控制流收到了
   server <- StreamFrame{StreamID: 0 ...}   ← 认证请求也收到了
   Closing connection with error: Application error 0x100 (local)
   ```
   修法：显式设 `PeerUnidiStreamCount=16`（双向仍保持 0），并**接住**对端开过来的流（`sDrainPeerStream`）。
2. **QPACK 解码碰到解不开的 Huffman 就整块放弃**。hysteria 的认证响应里有 `Hysteria-UDP` 这类头，
   排在 `:status` 前面 → 整个字段块解析失败。修法：内容解不开**不算结构错**，按长度跳过继续找。
3. **QUIC 数据报的 QUIC_BUFFER 放在了栈上**（UDP 不通的真凶，最难查的一个）。
   msquic 要求「载荷 **和 QUIC_BUFFER 数组本身**」都活到发送完成 —— `QuicStream::send` 早就把两者
   一起放进堆上的 `SendCtx` 了，唯独 `sendDatagram` 只把载荷放堆上、`QUIC_BUFFER` 留在栈里。
   函数一返回栈被复用，msquic 按那个指针取 Buffer/Length，**发出去的是垃圾**。
   现象为什么这么难查：QUIC 层一切正常（`DatagramSend` 返回成功，msquic 还报告对端 **ACK 了**），
   而 hysteria2 服务端的 `ParseUDPMessage` 解不开就**静默跳过**（官方代码注释原文：
   "Invalid message, this is fine - just wait for the next"）→ **两头都没有一行日志**，
   只表现为「UDP 就是不通」。
   定位路径：`COAST_HY2_DEBUG` 确认我们确实发了、字节也对 → `COAST_QUIC_DEBUG` 确认数据报被 ACK
   → 官方 hysteria 客户端做参照（`udpForwarding` + dig）确认服务端 UDP 是好的 → 只剩「我们发的
   内容不是我们以为的内容」这一种可能 → 查生命周期。
4. **Huffman 数字表写错**（最隐蔽的一个）。原实现假设 `'0'..'9'` 都是 5 位码、码值等于数字 ——
   但 RFC 7541 附录 B 里 5 位码只有 `'0''1''2'`，接着是 `'a''c''e''i''o''s''t'`；`'3'..'9'` 是 **6 位**码。
   于是 hy2 认证成功回的 `:status=233`（Huffman 编码 `13 2c ff`）被解成失败，
   **服务端日志明明写着 `client connected`，客户端却报 `auth rejected (status=-1)`**。
   现已按 RFC 抄正，并加了 KAT 自检 `hysteria2QpackSelfTest()`（harness 里作为 `QPACK` 一项先跑）。

### TUIC：**上游卡住**，不是我们的问题

TUIC v5 的 `Authenticate` token = 用 **TLS keying material exporter** 以 UUID 为 label、密码为 context 导出。
msquic 的对应 API `ConnectionExportKeyingMaterial` 在头文件里明写着 `// Available from v2.6`，
而 **msquic 最新 tag 是 v2.5.9**（`packages.microsoft.com` 上也只到 2.5.9）。所以：

- CMake 的 `COAST_HAVE_QUIC_KEYING` 探测（真编一次）在现有 msquic 上必然失败 → **tuic 不注册 → 回退 mihomo**；
- 退而求其次也不行：msquic 的 `QUIC_TLS_SECRETS` 只给 client random + 握手/应用流量密钥，
  **没有 `exporter_master_secret`**，而后者无法从流量密钥反推 —— 导不出 token。

结论：TUIC 的代码已就位并能编译，**等 msquic 2.6 发布即可开**；在此之前 harness 里显示 `SKIP`。

### 纯 msquic 探针（切开「msquic 的问题」还是「我们的问题」）

`qprobe.c`（不入库，几十行）：`MsQuicOpen2` → `ConfigurationOpen(alpn)` → `ConnectionStart`，
只打印 `CONNECTED` 或 `SHUTDOWN_INITIATED_BY_TRANSPORT` 的 `Status/ErrorCode`。
排 QUIC 问题时**先跑它**：能握手 = 问题在我们的协议层；握不上 = 问题在 msquic/服务端配置。
现在 `QuicTransport` 自己也会把这两个数翻成人话了（`quic 握手失败: ALPN 协商失败 (status=0x..., err=...)`、
`对端关闭: H3_NO_ERROR (app err=0x100)`），多数情况不必再动探针。

## REALITY：**已验证通过** ✅（含根因复盘）

最终结果（Pi5 + xray 25.6.8，五协议一次跑完）：

```
SS      PASS  HTTP/1.1 200 OK
VMESS   PASS  HTTP/1.1 200 OK
TROJAN  PASS  HTTP/1.1 200 OK
VLESS   PASS  HTTP/1.1 200 OK
REALITY PASS  HTTP/1.1 200 OK      ← 自建 TLS1.3 + uTLS 指纹 + REALITY 认证 + 证书 AuthKey 核验 全通
== fails=0 ==
```

xray 服务端侧对我们这条连接的确认（配置里 `"show": true`）：

```
hs.c.ClientShortId: [104 108 14 240 0 0 0 0]   ← 我们注入的 session_id 被正确解出
hs.c.conn == conn: true                         ← REALITY 认证通过（不是被丢去伪装站）
hs.handshake() err: <nil>
hs.readClientFinished() err: <nil>              ← 我们的 TLS1.3 密钥调度 / Finished 正确
hs.c.handshakeStatus: true                      ← 握手完成
```

### ★★ 根因复盘：**dest 必须支持 TLS 1.3**（前几轮都栽在这里）

之前反复失败、并且把我们自己的实现怀疑了两轮，真正原因是**测试台的 `dest` 配成了 `www.baidu.com`
—— 它不支持 TLS 1.3**：

```bash
openssl s_client -tls1_3 -servername www.baidu.com -connect www.baidu.com:443
#  → tlsv1 alert protocol version (alert 70)          ← 与客户端收到的 alert 一模一样
openssl s_client -tls1_3 -servername dl.google.com -connect dl.google.com:443
#  → New, TLSv1.3, Cipher is TLS_AES_256_GCM_SHA384   ✓
```

REALITY 的工作方式是把客户端握手**转发给 dest**；dest 不支持 TLS 1.3 时必然失败，客户端收到的
`protocol_version` alert **是 dest 回的、经 REALITY 转发回来**，与客户端实现无关。
**教训：排查 REALITY 之前，先用 `openssl s_client -tls1_3` 确认 dest 支持 1.3。**

`www.microsoft.com` 虽支持 1.3 但**同样不可用**：服务端日志出现 `Server Hello: 127`
（HelloRetryRequest）+ `Certificate: 8273`（证书链过大），最终 `handshakeStatus: false`。
**实测可用：`dl.google.com`（最干净）、`www.apple.com`、`cloudflare.com`。**

### 复现方法

```bash
# 1) 取 xray（GitHub 直连被墙时用 ghfast.top 镜像）
curl -sfL -o x.zip "https://ghfast.top/https://github.com/XTLS/Xray-core/releases/download/v25.6.8/Xray-linux-arm64-v8a.zip"
unzip -o x.zip && chmod +x xray
# 2) 生成密钥：private 填进 xray-reality.json，public + shortId 填进 main.cpp 的 rl 节点
./xray x25519
# 3) 起服务端（本目录配置的 dest 已是 dl.google.com）
./xray run -c xray-reality.json                  # 监听 127.0.0.1:18392
#    可选先验参照：./xray run -c xray-reality-client.json   # socks 10808
#    curl -x socks5h://127.0.0.1:10808 http://www.baidu.com/   → 期望 200
# 4) 跑 harness（五协议）
cmake -B build -G Ninja && cmake --build build && ./build/obh
```

### 排查 REALITY 的正确顺序（血泪总结）

1. `openssl s_client -tls1_3 -servername <dest> -connect <dest>:443` —— **先确认 dest 支持 TLS 1.3**；
2. 用 **xray 官方客户端**（`xray-reality-client.json`）打自己的服务端，确认**参照可用**（HTTP=200
   且服务端日志 `handshakeStatus: true`）——**参照没绿之前，不要动我们的加密代码**；
3. 才轮到跑我们的 harness；失败时看服务端 `show: true` 的 `hs.c.ClientShortId` / `hs.c.conn == conn`
   判断是「认证没过」还是「认证过了但 TLS 后续有问题」。
