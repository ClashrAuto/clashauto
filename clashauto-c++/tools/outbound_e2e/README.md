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

## 覆盖范围

| 协议 | 状态 |
|---|---|
| Shadowsocks (AEAD) | ✅ 已验 |
| VMess (VMessAEAD) | ✅ 已验 |
| Trojan (over TLS) | ✅ 已验 |
| VLESS (over TLS) | ✅ 已验 |
| REALITY | ❌ 需真 xray 服务端（mihomo 的 reality inbound 与 xray 的 AuthKey 核验路径不等价） |
| Hysteria2 / TUIC | ❌ 需 msquic（`COAST_HAVE_QUIC`）；且 TUIC 的 token 还缺 msquic 的 keying-material exporter |
| VLESS over ws/grpc | ❌ 未覆盖（ws 传输本身有独立实现，grpc 尚未实现） |

## REALITY 现状：**对真 xray 复现失败（未解决）**

本目录已含可复现的 REALITY 用例（`main.cpp` 的 `rl` 节点 + `xray-reality.json`）。跑法：

```bash
# 1) 取 xray（该网络直连 GitHub 被墙，用 ghfast.top 镜像）
curl -sfL -o x.zip "https://ghfast.top/https://github.com/XTLS/Xray-core/releases/download/v25.6.8/Xray-linux-arm64-v8a.zip"
unzip -o x.zip && chmod +x xray
# 2) 生成密钥，把 private 填进 xray-reality.json，public/shortId 填进 main.cpp 的 rl 节点
./xray x25519
./xray run -c xray-reality.json        # 监听 127.0.0.1:18392，dest/serverNames = www.baidu.com
# 3) 跑 harness
cmake -B build -G Ninja && cmake --build build && ./build/obh
```

### 已查明的事实（2026-07-30，Pi5 + xray 25.6.8）

- 其余四协议同轮全部 PASS，**只有 REALITY 失败**，客户端报
  `utls: 收到明文 alert（握手被拒？）`。
- loopback 抓包（`tcpdump -i lo 'tcp port 18392'`）显示：客户端发出 **258 字节 ClientHello**，
  xray 回 **7 字节**后立刻 FIN。该 7 字节解出来是
  **`15 03 01 00 02 02 46` = fatal alert `protocol_version`(0x46)**。
- **把线上真实 ClientHello 逐扩展解析过，结构完全合法**：
  `session_id` 32B ✓、`cipher_suites` 4 项 ✓、`key_share` 含 x25519(0x001d) 32B ✓、
  **`supported_versions` = `04 0a0a 0304`（长度 4，含 TLS 1.3 = 0x0304）✓**、
  扩展总长与实际一致、无错位（parser 走到末尾对齐）。
- xray 侧即便 `"show": true` + `loglevel: debug`，日志里**没有任何 REALITY 诊断行** →
  拒绝发生在 **REALITY 逻辑之前的 TLS 层**（alert 记录版本 0x0301 也符合 Go 在版本协商前发 alert 的行为）。

### ★ 两次对照实验（第二次推翻了第一次的结论——测试台本身不可信）

**对照 1：openssl（不带 REALITY 认证的标准 TLS1.3 客户端）**

```bash
openssl s_client -tls1_3 -servername www.baidu.com -connect 127.0.0.1:18392
```
→ 收到**一模一样**的 `protocol version` alert（number 70 = 0x46），服务端同时打出：
```
REALITY remoteAddr: ...  hs.c.conn == conn: false / forwarded SNI: www.baidu.com
REALITY remoteAddr: ...  hs.c.handshakeStatus: false
[Info] transport/internet/tcp: REALITY: processed invalid connection
```
由此**先**得出结论：该 alert 不是「TLS 版本没协商上」，而是 REALITY 对「认证未通过」的正常处置。

**对照 2（决定性）：xray 自己当 REALITY 客户端**（`client.json`：socks 10808 → vless+reality → 18392）

```bash
./xray run -c client.json &
curl -x socks5h://127.0.0.1:10808 http://www.baidu.com/
```
→ **xray 自己的客户端也连不上**（`HTTP=000`）。它客户端侧的中间量全都正常（日志里
`hello.SessionId[:16]: [25 6 8 0 …ts… 104 108 14 240 0 0 0 0]` = 版本 25.6.8 + 时间戳 + shortId `686c0ef0`、
`uConn.AuthKey[:16]` 也打出来了），但**服务端对它一条 REALITY 日志都没有**，客户端反复重试到
`context canceled`。此时服务端仍是健康的（同一时刻 openssl 再探，照样打出 invalid connection 日志），
且 dest 可达（`https://www.baidu.com` = 200、TCP 443 通）。

**⇒ 因此：这个测试台没有一个「已知可用」的 REALITY 参照连接。** 对照 1 的结论（「我们的认证没被接受」）
**不成立**——它是拿一个**根本不带认证**的客户端做对照推出来的；既然连 xray 官方客户端在本台也连不上，
就无法把失败归因到我们的实现。**在建立起可信参照之前，不要去改 UtlsClient 的认证代码**（那等于
对着不可信的 oracle 瞎改，很可能把本来正确的实现改坏）。

### 下一步（按优先级）

1. **先把参照跑通**：目标是「xray 客户端 ↔ xray 服务端」在本机能取到网页。可试：
   换 `dest`/`serverNames`（用支持 HTTP/2 且证书链干净的站点，如 `www.microsoft.com:443`）、
   去掉/调整 `spiderX`、确认 `xver`、或把服务端换到非 loopback 地址（REALITY 对 dest 的
   转发与 SNI 一致性较敏感）。**只有这一步绿了，才有资格判我们的实现对不对。**
2. 参照跑通后，再用它做**逐字节对照**：xray 客户端日志会打出 `hello.SessionId[:16]` 与
   `uConn.AuthKey[:16]`；给我们的 `applyRealityAuth` 加同样的临时输出，比对
   ECDH 共享密钥 → `HKDF-SHA256(salt=CH.random[:20], info="REALITY")` → `AES-256-GCM(nonce=random[20:32],
   aad=session_id 清零后的整条 CH 握手消息, pt=16B[版本3+保留1+时间戳4BE+shortId8])` → 32B session_id。
   （已核对过 xray v25.6.8 的 `UClient` 源码，上述序列与我们的实现**在算法层面一致**，
   所以真出问题多半在某个偏移/长度/字节序的细节，而不是整体思路。）
3. 认证过了以后，才轮到验证证书 AuthKey 核验（`verifyRealityCertificate`）与 HRR/KeyUpdate 等。
