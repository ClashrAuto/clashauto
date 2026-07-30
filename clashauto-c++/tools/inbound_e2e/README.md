# inbound_e2e —— 本机入站的「无 mihomo」端到端验证台

回答一个具体问题：**本机自己的流量，能不能全程走进程内引擎、一个字节都不经 mihomo？**

`MixedInbound::selfTest()`（`COAST_INBOUND_SELFTEST=1`）用的是直连出站**桩**，只验协议解析与转发。
这个台子换成**真正的生产路径**：

```
本地客户端 → MixedInbound → CoreDialerFactory（真拨号工厂，读 ProxyConfig 快照分流）→ DirectOutbound
```

## 为什么它的 PASS 有意义

关键在两行设置：

```cpp
CoreDialerFactory factory(&store, /*fallback=*/nullptr);
factory.setStrict(true);
```

`CoreDialerFactory` 平时判不了就**静默回退 mihomo** —— 那会把「离完全替换还差多少」藏起来
（这正是当初加 `cc=` 分原因记账的理由）。这里把回退这条路彻底堵死：没有 fallback，且开严格模式，
**任何一次想回退核心的企图都会当场变成连接失败**。所以 PASS 就等于「这条路上没有 mihomo」。

## 跑

```bash
cmake -S . -B b -G Ninja -DOPENSSL_ROOT_DIR=<openssl>   # MinGW 自带：C:/Qt/Tools/mingw1310_64/opt
cmake --build b && ./b/ibh
```

自带靶服务器，**不需要任何节点、订阅或外网**。三个用例：SOCKS5 CONNECT、HTTP CONNECT、
HTTP 绝对形式（顺带验证请求行被正确改写成源形式——靶机会把收到的请求行回显出来）。

## 范围

只走 DIRECT，因此不编 QUIC（msquic 是可选依赖）。要连协议出站一起验，用
[`../outbound_e2e`](../outbound_e2e)（那边有完整的 msquic 探测与真服务端拨通用例）。

背景与整体缺口见 [`../../docs/mihomo-replacement-gap.md`](../../docs/mihomo-replacement-gap.md)。
