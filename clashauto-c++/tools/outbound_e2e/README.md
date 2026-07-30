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
| REALITY (自建 TLS1.3 + uTLS) | ✅ 已验（对真 xray 25.6.8；**dest 必须支持 TLS 1.3**，见下） |
| Hysteria2 / TUIC | ❌ 需 msquic（`COAST_HAVE_QUIC`）；且 TUIC 的 token 还缺 msquic 的 keying-material exporter |
| VLESS over ws/grpc | ❌ 未覆盖（ws 传输本身有独立实现，grpc 尚未实现） |

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
