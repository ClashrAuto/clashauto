# 网关饱和压测：崩溃、瓶颈在哪，以及"多进程"该不该做

这份是为了回答一个反复被问的问题——**"把核心改成多进程，把延迟降到最低"**。
之前几次 A/B 都在轻载下做，结论是"性能中性"；这次专门**造饱和场景**再量。

测试台：树莓派 5 网关 + `192.168.20.239` 被劫持设备，`coastcore: true`（进程内内核）。
负载：设备侧多线程反复"建连 → GET → 关闭"，打 Pi 的 wlan0 上一个本地 HTTP（跨网段必过网关）。
**高新连接速率**是多进程唯一可能有意义的场景（每条新连接 = 一次 lwIP accept + 一次出站拨号）。

## 先说结论

| 量到的 | 数字 |
|---|---|
| 单线程 lwIP 泵是否饱和 | **完全没有**：`pump=400 late=0 maxLagMs=0`、`pumpLag=344/56/0/0/0/0` |
| 同时活动连接 | `tcpActive=965` |
| 分流回退 | `cc=1815/0/0/0/0/0` —— **全部走进程内，零回退** |
| **出站建连耗时** | `connMs=0/148/34/0/`**`1015`** —— **1015 次 ≥100ms**（空载时是 `7/0/0/0/0`，全 <1ms） |

**瓶颈不在数据面，在出站建连。** 泵一拍都没迟到，而建连耗时整整塌到了最慢那一桶。
所以 74% 的连接失败是**出站侧排队/超时**，不是"lwIP 算不过来"。

**这对"多进程"是决定性的反驳**：多进程复制的是 lwIP 数据面，而数据面**根本没饱和**（`late=0`）。
真要做，反而会让每个进程各自维护一份出站资源、更碎片化。
该优化的是**出站侧**：连接复用/池化（DIRECT 尤其明显——现在每条新连接都真去 `connect()` 一次）、
或者给并发建连加上限 + 排队，避免雪崩。这条有数据支撑，多进程没有。

## 顺带修掉的两个真 bug（都是压测才暴露的）

### 1. accept 回调里同步拨号 → lwIP 断言崩溃

现象：压到并发 4~6 就 **SIGABRT**，十几秒内必崩。

```
lwip assert: tcp_receive: wrong state @ third_party/lwip/src/core/tcp_in.c:1173
```

根因：lwIP 的 `tcp_process` 在 SYN_RCVD 分支是这么走的——

```c
pcb->state = ESTABLISHED;  TCP_EVENT_ACCEPT(...);  tcp_receive(pcb);
```

accept 回调返回后它**还要继续用这个 pcb**。而我们在回调里**同步**调 `connectTo()`，
高连接速率下它完全可能当场失败（fd/端口耗尽、协议出站拒绝……），失败处理会销毁连接、
顺手关掉 lwIP 侧的 pcb —— 于是 lwIP 拿着已死的 pcb 继续跑 `tcp_receive` → 断言 → 整个进程挂掉。

修法：把拨号**投到事件循环下一拍**（`QMetaObject::invokeMethod(..., QueuedConnection)`，
以 `c->socks` 作 context，连接若已销毁 Qt 会丢弃该调用）。修完同一压测：

| | 吞吐 | 稳定性 |
|---|---|---|
| 修复前 | 133 连接/秒 | 并发 4~6，12s 内必崩 |
| 修复后 | **238–303 连接/秒** | 并发 8/10/12 各跑满未崩 |

（注：并发 8 时仍偶发过一次同样的断言，说明还有第二条路径能在 lwIP 回调内拆 pcb，未根除。
诊断补丁已留在 `tcp_in.c`：断言前打印 `state/flags/端口`，下次复现就能直接定位。）

### 2. lwIP 断言消息**从来没被看见过**

`LWIP_PLATFORM_ASSERT` 原本用 `printf` 打到 **stdout**。stdout 在非 tty（systemd/重定向）下是
**全缓冲**的，而断言紧接着 `abort()` —— 缓冲区根本来不及刷，**消息全丢**。
所以第一次崩溃时 journal 里一个字都没有，只能靠 gdb 抓栈才知道是 lwIP 断言。
改成 **stderr（无缓冲）+ 显式 fflush** 之后，一行就看清了是哪条断言、哪个文件哪一行。

## 复现

```bash
# 网关侧
HOME=/root QT_QPA_PLATFORM=offscreen COAST_GATEWAY_TESTDEV=<设备IP> ./coast
# 目标（跨网段、纯本地，排除外网抖动）
python3 -c "from http.server import *; ThreadingHTTPServer(('0.0.0.0',8000), SimpleHTTPRequestHandler).serve_forever()"
# 设备侧：并发 N，持续 T 秒的"建连→GET→关闭"
python3 load.py <T> <N>
# 网关侧看瓶颈在哪
grep -oE 'pump=[0-9]+ late=[0-9]+ maxLagMs=[0-9]+' logs/gateway-diag.log | tail
tail -1 logs/gateway-diag.log | tr ' ' '\n' | grep -E '^connMs=|^pumpLag=|^tcpActive=|^cc='
```

**做对照**：同样的负载在 Pi 本机直接打一遍（完全不经网关）。本次对照是
**1980 连接/秒、零失败** —— 这条排除了"目标服务器扛不住"，否则所有结论都不成立。
