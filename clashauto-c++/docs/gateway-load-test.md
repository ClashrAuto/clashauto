# 网关饱和压测：瓶颈在哪，以及"多进程"该不该做

这份是为了回答一个反复被问的问题——**"把核心改成多进程，把延迟降到最低"**。

> ## ⚠️ 2026-07-31 重测：本文档 2026-07-30 那版的**性能结论全部作废**
>
> 旧版量到的「74% 失败率、connMs 大量 ≥100ms、直连比网关快 4.5 倍」有**两个独立的致命缺陷**，
> 两个都会让数字整体失真，不是修正系数能救的：
>
> 1. **代码 bug**：出站会丢掉设备的首个数据段（`write()` 早于 `connectTo()`，commit `2ede7ea`）。
>    那不是压测才暴露的竞态——被代理设备的**所有** TCP 都 100% 命中。旧版量的大半是这个 bug。
> 2. **测量台缺陷**：靶服务器放在**派自己的地址**（`192.168.31.66`）上。
>    `ip route get 192.168.31.66 from 192.168.20.239 iif eth0` → **`local … dev lo`** ——
>    内核把它当发给自己的包**本地直投**，压根没进 FORWARD，**coast 全程被短路**。
>    实证：客户端打了 13 万条连接，而 coast 自己的 `cc=` 只记了 19 条。
>    （这正是 rig 笔记里早写过的坑的 v4 版本：「目标别放本机地址，内核会本地直投短路掉 coast」，
>    我自己又踩了一遍。）
>
> 旧版**三个 bug 修复**（accept 内同步拨号导致 lwIP 断言崩溃、断言消息被 stdout 缓冲吞掉、
> fd 软限制 1024）**依然成立**——它们各有独立可复现的证据（崩溃栈、消息从无到有、fd 顶死曲线），
> 与被作废的性能数字无关。下面是重建测试台之后的结果。

## 测试台（重建后）

| | 配置 |
|---|---|
| 网关 | 树莓派 5，`coastcore: true`（进程内内核），被劫持设备 `192.168.20.239` |
| 靶服务器 | **纯 C**、epoll、`SO_REUSEPORT`×4、backlog 4096（不再用 python——它自己就是变量） |
| 靶地址 | **独立 netns `10.99.0.2`** ——**不是**派上的本机地址，内核无法本地直投 |
| 强制走 coast | `iptables -I FORWARD -s 192.168.20.239 -d 10.99.0.2 -j DROP`（coast 出站是本机发起、走 OUTPUT，不受影响） |
| 负载客户端 | **纯 C**、单线程 epoll、无线程无 GIL；维持 N 条「connect→GET→读到 EOF→关闭」 |

**先证明测试台本身可信**（旧版最大的教训就是没做这步）：

| 对照 | 结果 |
|---|---|
| 靶服务器上限（派本机直打 netns） | **17567/s，0% 失败** |
| 客户端直打（.239 → 派同网段，不经网关） | 14391 / 13973 / 14022 /s ⇒ **3.0% 离散**，0% 失败 |

两条都远高于被测对象，且可重复性满足「三次跑差异 <10%」——**靶机和客户端都不是瓶颈**。

## 核心对照：同一客户端、同一靶机、同一条路，只换「谁在转发」

这是本次最有价值的一个设计：`.239 → 10.99.0.2` 这条流，
**内核转发**（coast 停、放行 FORWARD）vs **coast 转发**（coast 跑、FORWARD DROP）。
客户端/靶机/路径/L2 全都相同，只剩一个变量。

| | 吞吐（收敛后） | 失败率 |
|---|---|---|
| **内核转发**（理想透明网关上限） | 16281 / 16111 /s | **0%** |
| **coast 转发**（coastcore 进程内） | 2683 / 2917 /s | **0%** |

**⇒ coast 约为内核转发的 1/5.6。** 而**失败率是 0%**——这和旧版的 60~74% 是两回事：
`2ede7ea` 之后**正确性问题已经没有了**，剩下的是纯粹的吞吐/延迟差距。

轻载单条延迟：经网关 **10~14 ms** vs 内核转发 **0.5 ms**。

## 结论：多进程解决不了这个问题（数据反对它）

最忙的 10 秒窗口（2861 accepts/s）里：

```
rx=143493(14318/s)  tx=116801(11654/s)  tcpAcc=28611
rxdrop=0  drains=0  defer=0  txdrop=0  backlogPeakKB=0
pump=401  late=0  maxLagMs=0        pumpLag=287/114/0/0/0/0
CPU：单核的 12~18%，整机 92~96% 空闲
```

- **泵一拍都没迟到**（`late=0`、`maxLagMs=0`），收发**零丢弃、零积压**；
- **CPU 只用了一个核的 15% 左右**，Pi5 还有三个核完全空闲。

**多进程/多线程复制的是数据面，而数据面既没饱和也不吃 CPU。** 慢的不是"算不过来"，
是**每条连接在等**——这类问题加进程只会让每个进程各自维护一份出站资源和一份 lwIP 池，更碎片化。
**要降延迟，得找到"在等什么"，不是加并行度。**

> ## ✅ 2026-07-31 续：真因已定位并修复（下文「真正的瓶颈」一节的**猜测部分**据此订正）
>
> 那一节把现象记对了，**机理猜错了一半**。加了按 TCP 状态的 pcb 普查 + 四条静默路径的计数器
> 之后（`pcbst=` / `synHitTw=` / `twRecyc=` / `twKill=` / `allocFail=`，见 NetStack 的
> `lwipStatsLine()` 与 `lwip_port/coast_lwip_diag.h`），真机给出的是：
>
> - 那 ~2000 个 pcb **全是 TIME_WAIT**（`pcbst=…tw1984…`）。FIN_WAIT/LAST_ACK/CLOSING
>   **全程为 0** ⇒ **我们自己的关闭路径没有洞**，上一版怀疑的「半关闭连接回收不掉」不成立。
> - **停摆时 `allocFail=0`、`twKill=0`**：SYN **根本没走到 `tcp_alloc`**。上一版猜的
>   「池里没有可杀的 TIME_WAIT → tcp_alloc 返回 NULL → 丢 SYN」是错的。
> - 真正吞掉 SYN 的是 **`tcp_timewait_input()`**：设备复用了一个还在 TIME_WAIT 里的四元组，
>   而 SYN 的序号（随机 ISN）落在旧连接的接收窗**之外** → 原生 lwIP 既不 RST 也不交给监听 pcb，
>   **直接 return**。停摆窗口的日志是 `tcpAcc=0 tw2048 synHitTw=64` —— 64 正是客户端的并发数，
>   一条不落全被吞。设备只能按 RTO 重传**同一个四元组**，再次命中，再次被吞。
> - **它为什么不会自愈**：TIME_WAIT 表只在「有新连接来 → tcp_alloc 池满 → tcp_kill_timewait」
>   时才被回收。SYN 全被吞 ⇒ 没有新连接 ⇒ 没人回收 ⇒ 2048 条按默认 `TCP_MSL` 冻结 **120 秒**。
>   **停摆本身维持着造成停摆的状态** —— 这就是那个活锁，实测单次持续 10~11 秒。
> - 这还**不只是压测现象**：正常速率下它表现为「每秒上百条连接白等一个 RTO」的隐形税。
>   修复前实测 **22% 的新连接**（`twRecyc≈660/s ÷ 2950 conn/s`）命中的就是这条路。
>
> **复现条件（上一版没复现出来的原因）**：单跑一轮不一定触发，**背靠背连跑**才稳定触发 ——
> 第一轮结束时冻结的 2048 条 TIME_WAIT 还有 120 秒寿命，正好等着第二轮的端口撞上来。
> 实测：第 1 轮干净，第 2、3 轮各停摆 10~11 秒。
>
> **修了三处，每处单独复测**（详见 `lwipopts.h` 的 `TCP_MSL` / `MEMP_NUM_TCP_PCB` 两段注释与
> `tcp_in.c` 的 Coast「TIME_WAIT 复用」补丁）：
>
> | # | 改动 | 三轮背靠背吞吐 | 停摆 | `connMs≥100ms` |
> |---|---|---|---|---|
> | — | 修复前 | 2478 / **1601** / **1612** = 1897/s | **10~11 秒 ×2** | 36 + 235 次超时 |
> | A | `TCP_MSL` 60000 → **500**（TIME_WAIT 120s → 1.2s） | 2339 / 2808 / 2677 = **2607/s** | 无（仅 <1s 抖动） | 294 |
> | B | 只放大池子 `MEMP_NUM_TCP_PCB` 2048 → 4096 | 1648 / 1799 / 1712 = **1720/s** ⚠️**负优化** | 无 | 2144 |
> | C | +`tcp_in.c` TIME_WAIT 复用（RFC 1122 §4.2.2.13） | 2968 / 2766 / 2713 = **2816/s** | 无 | **0** |
>
> **B 是这次最反直觉的一条，务必别再"顺手调大"**：上一版列的修复方向②「池子跟着连接速率走」
> **实测是负优化**。池容量 = TIME_WAIT 表的**长度上限**，而 `tcp_input` 对**每个 SYN** 都要把整条
> `tcp_tw_pcbs` 走一遍（无 MRU、无提前退出），更长的表还意味着**更大的四元组撞车面**——
> 撞车率从 7% 涨到 11%，吞吐反而掉了 34%。只有在 C 把撞车的代价消掉之后，放大池子才转为
> 净收益（它换来的是 `tcp_alloc` 那条会**静默杀活连接**的降级链彻底不再触发：
> 终态 `twKill=laKill=prioKill=allocFail=0`）。
>
> 上一版列的修复方向③「别让 tcp_kill_timewait 成为每连接 O(池大小) 的开销」**方向对但不是主因**：
> 它实测 5.6M 次链表节点访问/秒，确实贵，但 B 把它归零后吞吐反而更差 —— 说明**限制吞吐的是
> 撞车导致的 RTO 空等，不是扫描的 CPU**。终态它自然归零了（池子不再满员）。
>
> **没修好的**：轻载单条延迟 **11 → 13 ms**（无改善）。见文末「仍未解决」。

## 真正的瓶颈：lwIP 的 PCB 池被打爆，网关在持续压力下**完全停止接受新连接**

按秒采客户端 socket 状态（并发 64，25 秒）：

```
 1s ESTAB=36 SYN-SENT=28
 …
18s ESTAB=42 LAST-ACK=23 SYN-SENT=25
19s SYN-SENT=64      ← 从这里开始
20s SYN-SENT=64
 …                     ★ 全部 64 条卡在 SYN-SENT，直到压测结束都没恢复
25s SYN-SENT=64
```

**全员 SYN-SENT = 网关一个 SYN-ACK 都不回了。** 对上网关侧的池计数：

```
pcb=1479/2048 → 1664/2048 → 1985/2048 → 2048/2048 → 2048/2048 → 2044/2048（空载时仍然 2044！）
                                          ↑ 打满         格式：used/最高水位/err
```

关键矛盾：**客户端自始至终只开着 64 条 socket，而 lwIP 侧攥着 ~2000 个 PCB。**
也就是说 lwIP 为**客户端早已关闭**的连接留了大量 PCB。`MEMP_NUM_TCP_PCB = 2048`，
而 lwIP 是这里的**主动关闭方**（靶服务器 `Connection: close` 先发 FIN），
每条连接结束都会留一个 TIME_WAIT PCB；lwIP 默认 `TCP_MSL = 60s` ⇒ TIME_WAIT 停留 **120 秒**。
按 ~2000 conn/s 算，池子**不到 1 秒就填满**。

`err=0` 说明 lwIP 靠 `tcp_kill_timewait()` 一直在回收（每来一条新连接就把 2048 条的 tw 链表走一遍），
但一旦某个时刻池里**没有**处于 TIME_WAIT 的项可杀（都卡在半关闭态），
`tcp_alloc()` 直接返回 NULL → **SYN 被静默丢弃** → 客户端只能等 RTO 重传 → 全员 SYN-SENT。

旁证：客户端全程有 20~30 条停在 **LAST-ACK**（等 coast 发最后那个 ACK），
说明关闭握手的收尾本身也在拖——这同时是轻载 10~14ms 延迟的嫌疑点。

**压力一停就恢复**（压测后单条 curl 立刻 200 / 11ms），所以不是死锁，是**持续churn 下的活锁**。

### 这才是"完全替换 mihomo"的真正拦路虎

一个家用网关平时到不了 2000 conn/s，但**"持续新建连接就会停止服务、停手才恢复"是个功能性缺陷**，
不是性能调优项。修的方向（按性价比排）：

1. **给 TIME_WAIT 定上限**：`TCP_MSL` 对局域网网关来说 60s 毫无必要（lwIP 默认值是按广域网定的）；
2. **池子跟着连接速率走**：`MEMP_NUM_TCP_PCB` 2048 配上 2×MSL 的驻留时间，量纲上就对不齐；
3. **别让 `tcp_kill_timewait` 成为每连接 O(池大小) 的开销**；
4. 查清 LAST-ACK 堆积——半关闭连接的回收路径。

**这四条都有数据支撑，多进程没有。**

## 复现

```bash
# 派上：靶服务器放进独立 netns（关键：不能是派的本机地址，否则内核本地直投短路 coast）
ip netns add srv; ip link add veth-h type veth peer name veth-s
ip link set veth-s netns srv; ip addr add 10.99.0.1/24 dev veth-h; ip link set veth-h up
ip netns exec srv ip addr add 10.99.0.2/24 dev veth-s
ip netns exec srv ip link set veth-s up; ip netns exec srv ip route add default via 10.99.0.1
systemd-run --unit=httpsrv2 ip netns exec srv /root/httpsrv 8000 4
iptables -I FORWARD 1 -s 192.168.20.239 -d 10.99.0.2 -j DROP   # 掐死内核转发，逼流量走 coast

# 设备侧
ip route replace 10.99.0.0/24 via 192.168.20.91 dev ens18
/root/load 10.99.0.2 8000 64 10          # 纯 C 压测客户端

# 判瓶颈（缺一不可）
tail -1 logs/gateway-diag.log | tr ' ' '\n' | grep -E '^pcb=|^pumpLag=|^connMs=|^cc=|^rxdrop='
ss -tan 'dst 10.99.0.2' | awk 'NR>1{c[$1]++} END{for(s in c) print s, c[s]}'   # 全员 SYN-SENT = 网关已停摆
```

**做对照，而且要做两个**：①靶机上限（本机直打）②内核转发同一条流。
少任何一个，"网关慢 N 倍"都不成立——这正是旧版翻车的地方。

**★ 判"网关是不是又在静默吞 SYN"，只看诊断行这几栏**（都是 2026-07-31 加的）：

```
pcbst=synr…/est…/fw1_…/fw2_…/cw…/closing…/lastack…/tw…/bound…   # pcb 按 TCP 状态普查
backlog=<accepts_pending>/<backlog>        # 顶到 backlog ⇒ tcp_listen_input 静默丢 SYN
synHitTw= twRecyc= twRst=                  # SYN 撞上 TIME_WAIT 四元组：吞掉 / 回收 / 回 RST
twKill= laKill= prioKill= allocFail=       # tcp_alloc 降级链：杀 TW / 杀 LAST_ACK / **杀活连接** / 丢 SYN
twScan=                                    # tcp_kill_timewait 累计扫过的链表节点数
```

判读口径：`tw` 占满池子 = TIME_WAIT 堆积（主动关闭方的正常代价，看 `TCP_MSL` 与池容量是否配平）；
`fw1/fw2/lastack/closing` 堆积 = **我们自己的关闭路径有洞**，那才是真 bug；
`tcpAcc=0` 且 `synHitTw`/`synDropBacklog` 有值 = 网关正在静默吞 SYN；
`prioKill`/`allocFail` 非 0 = 已经开始伤活连接。

**★ 复现停摆必须背靠背连跑 3 轮**（单轮常常干净）：上一轮冻结的 TIME_WAIT 表要等下一轮的
端口撞上来才发作。

`httpsrv.c` / `load.c` 两个工具的源码在 `tools/gwbench/`。

## 仍未解决

**轻载单条延迟 11~13 ms（内核转发 0.5 ms），本次修复没有改善**，且它与 PCB 池无关——
轻载下根本没有 TIME_WAIT、没有扫描。定位线索已经有了：coast 自己的 `connMs=`
（`connectTo` → `established` 的墙钟）在**空载**下也大量落在 1~10 ms 桶，而出站那一跳
是本机 veth 上的一次普通 TCP connect（内核只要 0.1 ms 量级）。**所以时间花在
「拨出站」这一段，不在 lwIP**。下一步该量的是 CoastCore 出站的 `connectTo` 内部
（是不是有定时器/事件循环往返被算进来了），而不是继续调 lwIP。
