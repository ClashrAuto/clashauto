# 网关性能实测：CoastCore 进程内出站 vs 回退核心（A/B）

回答最初那个问题——**「网关延迟和吞吐要上去，是不是必须用 C++ 重写 mihomo」**。
结论先说：**在这台测试台上，答案是「不是」**。数据如下，包括反例。

测试环境：树莓派 5（aarch64，四核）作网关，`192.168.20.239`（Ubuntu 虚机）作被劫持设备。
两种模式**拨同一个节点**（本机 mihomo 的 ss listener :18388 → DIRECT），
唯一差别就是「CoastCore 进程内 SS」还是「回退拨 7899 交给核心」。
目标是 Pi 的 **wlan0 地址 `192.168.31.66:8000`** 上的 `python3 -m http.server`——
**跨网段**（不会触发同网段旁路，必然过网关）但**全程本地**，把外网抖动降到最低。

## 结果

| 指标 | coastcore OFF（回退核心） | coastcore ON（进程内 SS） |
|---|---|---|
| 50 MB 下载吞吐 | **110.6 MB/s**（0.452 s） | **110.6 MB/s**（0.452 s） |
| TCP 建连（中位） | 0.24 ms | 0.26 ms |
| 首字节 TTFB（中位 / p90） | 0.86 / 1.12 ms | 0.95 / 1.38 ms |

**吞吐完全一致（≈880 Mbps，接近千兆线速）；延迟同在亚毫秒量级，差异在噪声内。**

> ⚠️ 中途一次测量曾得到「ON 只有 45.5 MB/s」，后来查明是**测量脚本自己的问题**：
> 归因采样用 200 次 `ss -tnp | grep` 高频轮询，在四核 Pi 上抢掉了可观 CPU。
> 换成「只测下载 + 低频采样」后两种模式都是 110.6 MB/s。**测性能时先确认测量工具本身不是负载。**

## 埋点计数器怎么说（`<userDir>/logs/gateway-diag.log`）

```
pumpLag=398/2/0/0/0/0   maxLagMs=0        ← lwIP 泵几乎从不迟到，≥50ms 桶全 0
connMs=7/0/0/0/0                          ← 7 次出站建连**全部 <1ms**
rxBatch=36/45/21/5/1/57                   ← 末桶(32+ 帧/唤醒)有 57 个样本 → RX 批量在生效
fpb=1                                     ← TX 每 syscall 1 帧（轻载下不聚批，符合设计）
```

三条要点：

1. **单线程 lwIP 不是瓶颈**：`pumpLag` 几乎全落在「不迟到」桶、`maxLagMs=0`。
   → 之前判定「lwIP 分片多线程不可行、且先量再决定」是对的；这台机器上根本没有可省的排队。
2. **出站建连开销可忽略**（`connMs` 全 <1ms）→ 这解释了为什么「省掉一次环回 SOCKS5 握手」
   在这里量不出收益：那一跳本身就是亚毫秒。
3. **RX 已在批量**（主路径是 TPACKET_v3 mmap 环），不是逐包收。

## 该怎么理解这个结论

- **进程内出站是「性能中性」的**：不会更慢（吞吐一致、延迟同量级），所以从性能角度**可以安全启用**；
  但也**不要期待它在这种链路上带来可感知的提速**。
- **最初的前提（「必须重写 mihomo 才能提升延迟/吞吐」）在本测试台上不成立**：
  数据面能跑满千兆、工作线程远未饱和、出站建连 <1ms。瓶颈不在这里。
- 真要看到进程内的收益，得是**那一跳真的开始疼**的场景，例如：
  · 极高的新连接速率（每条连接都要一次环回 SOCKS5 握手 + 一份双向 TCP 状态机）；
  · 网关 CPU 本身吃紧（少一份内核↔用户态往返和一次多余拷贝就有意义）；
  · 跨机部署（出站不在本机，环回假设不成立）。
  这些都不是本测试台的形态，**所以本文只给出「中性」这个结论，不做外推**。

## 进程内出站真正带来的价值（与性能无关）

同一批工作里，真正兑现的是**能力**而不是速度：

- 自主的**热重载**（不可变快照 + 原子换手，在途连接留旧快照）；
- 自主的 **fake-ip 反查**（旁听 DNS 应答，把假 IP 还原成域名再拨——见 `DnsResolver`）；
- 自主的**分流**（`RuleEngine` 首命中 + 组名→节点解析；判不了就回退核心，零误路由）；
- 五种协议在进程内可用且**已真机验证**（SS / VMess / Trojan / VLESS / REALITY，见
  `tools/outbound_e2e/README.md`）。

## 复现方法

```bash
# Pi 上：节点服务端 + 本地跨网段 HTTP 目标
mihomo -d /tmp/nodesrv -f nodesrv.yaml          # ss :18388 → DIRECT（见 tools/outbound_e2e/）
mkdir -p /tmp/www && dd if=/dev/zero of=/tmp/www/big.bin bs=1M count=50
cd /tmp/www && python3 -m http.server 8000 --bind 0.0.0.0

# 起 Coast（headless）：HOME 必须设，否则 Qt 的数据目录会落到 /.local/share/Coast
#   见 tools/outbound_e2e/README 与 memory 里的踩坑记录
HOME=/root QT_QPA_PLATFORM=offscreen COAST_GATEWAY_TESTDEV=192.168.20.239 ./build/coast

# 核心配 mode=global + 一个 test-ss 节点(127.0.0.1:18388)，两种模式都拨它；
# 然后把 config.yaml 的 coastcore 在 false/true 间切换、各测一轮：
#   延迟: curl -w '%{time_connect} %{time_starttransfer}' http://192.168.31.66:8000/small.txt
#   吞吐: curl -w '%{speed_download}' http://192.168.31.66:8000/big.bin
# 归因（确认真走了进程内）: ss -tnp | grep 127.0.0.1:18388 | grep coast
#   —— 采样频率别太高，否则采样自己就成了负载（见上面那个 45.5 MB/s 的乌龙）。
```
