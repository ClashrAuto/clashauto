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
