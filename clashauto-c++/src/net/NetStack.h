#pragma once

// 用户态 TCP/IP 栈（TCP 由 Rust 的 smoltcp 实现，见 rust/coaststack + src/net/coaststack.h；
// PIMPL 把实现细节藏进 .cpp）。
//
// ★ **只有 Windows 编 NetStack.cpp**：Linux 走 TPROXY、macOS 走 pf rdr，内核就把流量接管了，
//   不需要用户态栈。非 Windows 链接的是 NetStack_stub.cpp —— init() 直接返回失败，**不降级**。
//   （2026-08 之前这里是 vendored lwIP、三端共用；移除的全过程见 docs/lwip-alternatives.md。）
//
// 职责：被劫持设备的以太帧经 LanGateway 过滤后送入 inputFrame()，按协议**三分**：
//   · IP/TCP 帧 → 喂给 smoltcp（catch-all：终结发往任意公网 IP:port 的连接）
//     → 每条 TCP 连接经 Socks5Tcp 拨 mihomo 网关口(带每设备用户名) → 双向桥接字节。
//   · IP/UDP 帧 → **不进栈**，直接解析，经 Socks5Udp 转发（含 DNS 劫持）；回程手工封包发回设备。
//     这套手工 NAT 从来就没走过任何用户态 TCP 栈，换栈/删 lwIP 时一行都没动。
//   · 其余（ICMP 等）→ 丢弃。栈只实现 TCP，喂别的进去只会让它 rx_dropped++。
//     代价是被劫持设备 ping 不通公网（那本来也只是我们代答，并不真探测远端可达性）。
//   · 栈的出帧回调 → 按 nic id 找到二层端点 → IL2Endpoint::send（回程 dst MAC 由引擎从设备
//     实帧自学，不需要外部注入静态 ARP/邻居）。
//
// **一个 NetStack、多张网卡**：addNic() 一张卡一份上下文，Rust 侧每张卡一个独立的 smoltcp
// Interface 实例。所以「同时代理有线 A 路由 + WiFi B 路由」是多次 addNic，而不是开多个 NetStack。
// 每张卡要给**本机在该网卡上的真实 IP + 真实掩码**：栈据它判断哪些目的是本网段的，
// 掩码给错会让 B 网段设备的回包从 A 网卡发出去。
//   （lwIP 时代这件事更脆：所有 netif 挂在一条全局链上，靠 ip4_route 按子网挑，
//     给 0.0.0.0 占位掩码就会让第一张卡「匹配一切」。独立实例天然没有这个坑。）
//
// 线程：NetStack 自身不做线程化——它假定「由**单一**线程拥有并调用」。这也是 coaststack 的硬
// 要求（见 coaststack.h「线程模型」：所有 coast_* 调用与回调都在同一线程）。全进程只有一个
// NetStack 实例，那个实例始终在某一个线程上：在正式 App 里是 LanGateway 的专用工作线程
// （见 LanGateway_linux.cpp 的线程模型），在 headless 自测里是主线程。两者从不并存 → 单线程前提恒成立。
// init() 创建的 QTimer 挂在**调用 init 的那个线程**的事件循环上周期 coast_stack_poll()。
// 所有公开方法（init/addNic/removeNic/addDevice/removeDevice/inputFrame）都只能在这同一个线程上调用。
//
// ── 单线程会不会饿死其他设备？实测：不会（但尾延迟有毛刺）──────────────────────
// 「所有设备共用一个 lwIP 线程」天然让人担心队头阻塞：一台设备灌满带宽时，别的设备的小请求
// 是不是要排队。真机测过（树莓派网关 9 台劫持；一台设备 8 路并发下载做饱和源，另一台**有线**
// 设备同时打 RTT 与 generate_204 小请求；DIRECT 出站，故不含代理节点波动）：
//                     探针RTT均值   RTT最大   小请求p50/p90   coast CPU
//     ① 空载            0.302 ms    0.416     64 / 80 ms       4%
//     ② 重压中          0.579 ms    5.128     60 / 64 ms      77%   ← 0.77 核，接近单线程上限
//     ③ 重压中(复测)    0.301 ms    0.402     63 / 65 ms      24%
//     ④ 卸载后          0.331 ms    0.536     64 / 72 ms       4%
// 结论：**小请求的 p50/p90 全程不劣化**（60~64ms，重压时甚至略优于空载——泵被唤醒得更频繁，
// 小包反而更快被服务到），RTT 中位数也不变。所以在 0.77 核这个量级上没有饥饿问题。
// 唯一可见代价是重压期 RTT **最大值** 0.42→5.1 ms（约 12 倍尾部毛刺，仅极少数包）：这正是
// 「泵在一拍里处理大批数据包时，偶发让小包多等一拍」的表征，属单线程设计的固有尾部特征。
// 要它彻底消失得把数据面多线程化或换 TPROXY（内核转发），但 p50 不受影响意味着**体感上不需要**。
// ★ 上面那组用的是公网下载做负载，受外部站点限速影响、压不到满速。后来改用**内网满速加压**
//   重测（在网关上建一个 netns 假外网 203.0.113.0/24，被劫持设备 iperf3 打它 —— 目的地不是
//   本机 LAN 网段也不是本机地址，必须真穿数据面；诊断计数 rx=13262 fed=13239 bypLan=0 证实
//   确实全喂进了 lwIP），拿到了**真正的单线程上限**：
//
//   ── lwIP vs TPROXY 同链路对照（千兆链路，纯链路 iperf3 基准 941 Mb/s 零重传）──────
//     数据面      吞吐        coast CPU   核心 CPU   合计      折算
//     lwIP       944 Mb/s    0.90 核      ~0        0.90 核   0.95 核/Gbps
//     TPROXY     940 Mb/s    0.06 核      0.18 核   0.24 核   0.25 核/Gbps
//   → **TPROXY 省 3.7 倍 CPU。**
//
//   ── 2026-08-04：**核心自带 TUN 的两个栈 system vs gvisor，实测等价** ─────────────
//     背景：目标是「去掉 lwIP」，一条备选路是让核心的 TUN inbound 接手。直觉上
//     `stack: system`（内核栈）该比 `stack: gvisor`（Go 用户态栈）快，值得换。**实测否决。**
//     先澄清两个我一度搞错、写进过结论的事实：
//       · `system` 栈**不需要任何 build tag**。sing-tun 的 NewStack() 按字符串分发，
//         `with_gvisor` 只保护 gvisor/mixed（我们仓库里它另外只管 tailscale 出站）。
//         所以**现有发布核心直接就能跑 system 栈**，不必重编 —— 三端(Win/Linux/macOS)已各自验证起得来。
//       · 但"能跑"不等于"更快"。
//     Windows 真机 A/B（同机、同订阅、同节点 JP-2、两份配置只差 stack/网卡/网段/端口，
//     且**每次都先读 /configs 确认 tun.enable=true 才开量**，串行跑避开 Wintun 残留冲突）：
//         栈        TTFB 中位   吞吐(3 轮, Mb/s)          中位      CPU
//         system    0.185s      2251 / 2743 / 2793       2743      0.45 核/Gbps
//         gvisor    0.176s      2455 / 2666 / 2732       2666      0.45 核/Gbps
//     → **区间完全重叠、CPU 折算相同 = 在噪声范围内等价。** 换栈拿不到任何收益。
//     ★ 教训：第一次单轮采样得到"gvisor 快 27%"，复测 3 轮就抹平了 —— 吞吐**单次不可信**，
//       这条与 [[throughput-test-pitfalls]] 里记的是同一件事，又踩了一次。
//     ★ 另一个真机坑（不是我们的 bug，但会挡住任何双实例对照）：Windows 上第二个 TUN 实例
//       恒报 `set ipv4 address: The object already exists`，即使给了不同的 device 名与
//       inet4-address 网段。这是 Wintun 适配器残留对象的已知问题，clash-verge-rev /
//       nekoray / hiddify / v2rayN 都有同样的 issue。**双实例 TUN 对照在 Windows 上做不了，
//       必须串行。**
//     ★ 结论：TUN 换栈不是出路。真要在 Windows 上超过这个量级（~2.7 Gbps / 0.45 核·Gbps），
//       得走内核重定向（WFP ALE_CONNECT_REDIRECT，让 Windows 自己的 TCP 栈搬数据），
//       而那需要内核 callout 驱动 + EV 证书附件签名 —— 见 net/WfpRedirect.h。
//
//   ── 2026-08-04：**撤回**上一版写在这里的「lwIP 每条 TCP 连接白送一个上游 RTT」──────
//     那条结论（Windows lwIP 建连 142~178ms、"建连时间 = f(上游 RTT)"）是**错的**，已删。
//     错因是取样没验成立：量的时候靶机的 ARP 表其实还指向**真路由器**（Coast 报了
//     `[RESUME] ... armed`，但进程随后就退了，投毒并未生效），所以那 142~178ms 量到的是
//     **设备直连公网的真实 RTT**，与 lwIP 无关。
//     补测（先用 `ip neigh` 确认网关 MAC 已翻成本机网卡，再量）：
//         lwIP 建连(出海目标)  = 1.6~2.9 ms
//         lwIP 建连(国内目标)  = 1.6~2.9 ms
//     → **lwIP 的 SYN-ACK 是就地立刻回的**，和 lwIP 源码里 tcp_listen_input() 的行为一致，
//       也和"我们代码里没有任何 tcp_backlog_delayed"这一点自洽。lwIP 在建连延迟上没有额外开销。
//     ★ 教训（与 L2Endpoint_win.cpp 里那条同源）：**网关类 A/B 必须先证明"接管真的生效"**，
//       判据是靶机侧 `ip neigh show <网关IP>` 的 lladdr 已变成本机网卡 MAC ——
//       只看日志里的 `armed` 不够，进程可能 armed 完就退了。
//
//   ── 2026-08-03 复核（同一套 netns 假外网夹具，但用的是当天的代码）────────────
//     被代理设备 .203 → netns 里的 iperf3（10.99.0.2:5202），12s：
//         吞吐 832 Mb/s   核心 1.58 核·秒   App 0.33 核·秒   合计 1.91 核·秒
//         折算 1.91 / 10.01 Gbit = **0.19 核/Gbps**（原记录 0.24~0.25）
//     变好的主要是 **App 那一档**：0.06 → 0.033 核/Gbps，与「日志不再逐条进模型 + 页脚节流」
//     那次改动（满负载下 App 27.9% → 4.7%）方向一致。核心侧 0.158 核/Gbps。
//     ★ 这个数与本文件后面「核心自身 0.38 核/Gbps」那条不是一回事，别混：那条量的是
//       **HTTP 混合口**的中继（curl -x 127.0.0.1:7890），这条量的是 **TPROXY 入站**。
//
//   ── 2026-08-03 把那个差距拆开了：**入站类型决定用不用 splice，差 6.7 倍** ──────────
//     同一个源（netns 假外网 10.99.0.2 的 HTTP）、同一个 400MB 文件，把混合口**限速到与
//     设备侧同一档**以消除"速率不同导致每 Gbps 成本不可比"，各测两遍：
//
//       路径                     吞吐            核心 CPU        折算
//       设备 → TPROXY 入站       0.932 Gbps      0.11 核·秒      **0.030 核/Gbps**
//       本机 → 混合口(限同速)    0.924 Gbps      0.65 核·秒      **0.193 核/Gbps**
//       设备 → TPROXY 入站       0.933 Gbps      0.10 核·秒      0.028 核/Gbps
//       本机 → 混合口(限同速)    0.924 Gbps      0.68 核·秒      0.202 核/Gbps
//       本机 → 混合口(不限速)    6.010 Gbps      0.61 核·秒      0.178 核/Gbps  ← 回环能到 6 Gbps
//
//     机制用 strace 计数验证（同样限速 30MB/s，各抓一段）：
//       TProxy 入站： **splice 1223 次**，read/write 各不到 100
//       混合口入站：  splice 0 次，**read 13017 / write 13543 次**
//     Go 的 io.Copy 在 TCP→TCP 之间会用 **splice(2) 内核零拷贝**；TPROXY 入站交给核心的就是
//     一对裸 TCPConn，splice 用得上。HTTP 入站要解析请求头，连接被 bufio 包过一层，
//     splice 用不了 → 退回用户态逐块 read/write 搬运。这就是 6.7 倍的来源。
//
//     ★ 对预算的影响：**网关（TPROXY）路径上核心侧只要 0.03 核/Gbps**，不是先前记的 0.38。
//       0.38/0.19 那两个数分别是「HTTP 代理路径」和「早先一次含 App 的合计」，别拿去推网关。
//       万兆折算（本机 ARM）：TPROXY 数据面 ~0.03（内核转发） + 核心 ~0.03 ≈ **0.3~0.6 核**。
//       **但这是 chain=DIRECT 的下限**，不是生产口径 —— 见下一段。
//
//   ── ★ 生产口径：**走真实节点（加密）时贵 2.4 倍，且 splice 彻底失效**（2026-08-03 实测）──
//     加密路径以前从没量过。为了不碰生产网关，在同一台 Pi 上另起两个**裸 mihomo 实例**
//     （不经 App、不碰 nft/ARP，对网关零影响）：一个开 shadowsocks 入站当"节点"，
//     一个当客户端；再加一个纯 DIRECT 的客户端做对照。同一个 HTTP 源、同一份 400MB 文件：
//
//       路径                  吞吐         客户端 CPU     折算            服务端 CPU
//       直连(无加密)          5.783 Gbps   0.58 核·秒     0.169 核/Gbps   0.00
//       经 ss 节点(aes-128-gcm) 2.045 Gbps   1.57 核·秒   **0.463 核/Gbps** 0.68
//       直连(无加密)          5.351 Gbps   0.69 核·秒     0.203 核/Gbps   0.00
//       经 ss 节点(加密)      2.234 Gbps   1.50 核·秒   **0.444 核/Gbps** 0.67
//
//     · 加密把每 Gbps 成本从 ~0.19 抬到 ~0.45（**2.4 倍**），吞吐从 5.5 Gbps 掉到 ~2.1 Gbps；
//       此时客户端已接近跑满一个核 —— 也就是**这台 ARM 上加密中继约 2.1 Gbps/核**。
//     · 服务端（=远端节点）另付 ~0.67 核·秒，那是对端的成本，不算在我们头上。
//     · 直连那一档（0.169~0.203）与上面「混合口 0.19」一致，交叉验证了两次测量。
//
//     ★★ **万兆的生产口径因此是 ~4.8 核**（10 Gbps × 0.48 核/Gbps），不是上面那个 0.3~0.6 核。
//        本机只有 4 核 —— 这台 Pi 做不到加密万兆，瓶颈是加解密而不是数据面。
//
//     ── 但它**能跨核扩展**，是"核不够"而不是"结构不行"（并发扩展实测）──────────────
//       同一夹具，把并发数拉上去（父进程墙钟计时；CPU 含客户端/服务端/整机三笔账）：
//
//         流数   吞吐        客户端    服务端   两者合计   整机忙/空闲（共 4 核）
//          1    1.910 Gbps  0.89 核   0.39 核   1.28 核    2.57 / 1.35
//          4    3.227 Gbps  1.54 核   0.81 核   2.35 核    3.60 / 0.34
//          8    3.239 Gbps  1.53 核   0.82 核   2.35 核    3.65 / 0.30
//
//       · **每 Gbps 成本全程恒定 ~0.48**（1/2/4/8 流分别 0.486/0.475/0.487/0.473），
//         客户端 CPU 随并发从 0.89 涨到 1.54 核 —— 加密中继确实在多核上并行；
//       · 吞吐停在 3.24 Gbps 的原因是**整机没核了**：4 核里只剩 0.3 核空闲。
//         整机忙(3.60) 比客户端+服务端(2.35) 高出的那 ~1.25 核，是 python HTTP 源（单线程吃满
//         一个核）、回环/网卡的内核开销，以及**同机还跑着生产网关**（coast + core）。
//         也就是说这个 3.24 Gbps 是**测试台的天花板，不是核心的天花板**。
//
//       结论修正：不是"加密路径不能上万兆"，而是"**按 0.48 核/Gbps 备核**"——十兆需要约 4.8 核
//       的中继 CPU（还没算内核网络栈），Pi 5 的 4 核不够，8 核以上的 x86 才谈得上。
//       ARM 加密扩展是有的（/proc/cpuinfo Features 含 aes/pmull/sha2），2.1 Gbps/核 已经是
//       硬件加速后的数字，别指望换实现再翻倍。
//       让尽量多的流量走 DIRECT（splice 那条快路，0.03 核/Gbps）仍是最省的方向。
//
//   ── ★ 三端横比：核心中继的每 Gbps 成本（2026-08-03，**严格同条件**）───────────────
//     同一个源（Pi 上的 HTTP，400MB）、同样经各自本机的混合口(DIRECT)、同样跑到 ~0.93 Gbps
//     （千兆链路封顶），各测 3 次：
//
//       平台                              核心 CPU        折算
//       x86 Linux（8 核 QEMU 虚机）        2.33~2.35 核·秒  **0.693~0.699 核/Gbps**
//       Windows（x86 i7 Comet Lake，8 核） 2.61~2.92 核·秒  **0.778~0.871 核/Gbps**
//
//     → **Windows 比同架构 Linux 贵约 1.2 倍**，差距不大，大体是系统调用开销的量级。
//
//     ★ 别拿本文件前面那个「混合口 0.19 核/Gbps」去和这两个比 —— 那次的目标在 netns 里，
//       走 veth/回环，**核心出站不经物理网卡**；这里两端都过真实网卡。同一台 Pi 换成过网卡
//       之后成本会明显上去。我一度据「Pi 0.19 vs Windows 0.82」推断「Windows 慢 4 倍、
//       多半是 Go 在 Windows 上没有 splice」，补了同架构的 x86 Linux 之后这个推断不成立：
//       splice 在混合口这条路上**本来就没被用到**（strace 已证实是 read/write），
//       真正的差异来自 CPU 与路径，不是 OS 缺少零拷贝。
//
//   ★★ 对「万兆」目标的硬结论：**lwIP 架构上不可能达到万兆。** 它跑满千兆就已吃掉 0.9 个核，
//      而整个 lwIP 是**单线程**的（本文件顶部那段前提），单核封顶 ≈ 1.05 Gbps —— 万兆需要在
//      一个线程里做到约 9.5 核的工作量，物理上做不到。加机器、换 CPU 都不解决，因为瓶颈是
//      「所有流量串行过一个线程」这个结构本身。
//      TPROXY 那条路是 0.25 核/Gbps，万兆约需 2.5 核，且内核转发天然按 CPU 队列并行 —— 可行。
//      所以要上万兆，方向只有「TPROXY / 内核转发」，不是优化 lwIP 的常数因子。
//
//   ── ★ 补齐第三项：**核心自身的代理开销**（2026-08-03 实测，同一台 Pi，有线千兆）──────
//     上面两行只算到「把包交给核心」为止，没算核心把它转发出去要花多少。单独量了一次：
//     在网关上 curl 局域网里的 HTTP 源（400MB range），直连 vs 经本机核心的混合口 7890，
//     两端完全相同、唯一差别是多一跳代理；三次重复几乎逐位一致：
//
//       直连          112.2 MB/s = 0.94 Gbps（千兆线速）    ——
//       经核心 7890   112.1 MB/s = 0.94 Gbps               核心 36% 单核 → 0.38 核/Gbps
//
//     两点结论：
//     ① **吞吐零损失** —— 走一趟代理并不降速，千兆下代理与直连打平（差 0.1 MB/s，在噪声内）；
//     ② 代价全在 CPU：**0.38 核/Gbps**。这一项比 TPROXY 数据面的 0.25 还大，也就是说
//        **数据面换成 TPROXY 之后，瓶颈已经从我们的数据面挪到了核心本身**。
//        万兆折算：TPROXY 2.5 核 + 核心 3.8 核 ≈ 6.3 核（本机 ARM；x86 单核更快，会低一些）。
//        继续优化 lwIP/数据面的常数因子已经不是主要矛盾了。
//     ★ 这个数是**不含加密**的下限：目标是局域网地址、路由判到 DIRECT，没有 TLS/传输层加密。
//       走真实节点时还要叠上加解密，别把 0.38 当成有节点场景的预算。
#include <QByteArray>
#include <QObject>
#include <QString>

class IL2Endpoint;
class QHostAddress;

/// 归因用的分级短路开关（详见 NetStack.cpp 里 g_benchStage 的说明）。
/// ★ 只为测量存在，生产恒为 0。之所以要个 setter 而不是只读 env：
///   短路必须在**握手完成之后**才打开 —— STAGE>=2 会关掉 poll，
///   而三次握手本身就要 poll，开在前面等于连连接都建不起来（第一版就是这么废掉的）。
void coastSetBenchStage(int stage);

class NetStack final : public QObject
{
    Q_OBJECT
public:
    explicit NetStack(quint16 socksPort, QObject *parent = nullptr);
    ~NetStack() override;

    // 初始化 lwIP + catch-all TCP 监听 + 定时器泵。全进程只能有一个实例。失败置 *err。
    bool init(QString *err);

    // 挂一张网卡：ep 为其二层端点，localMac6/localIp/netmask 为本机在这张卡上的信息。
    // localIp/netmask 必须有效——栈据它们判断本网段（见文件头说明）。
    bool addNic(IL2Endpoint *ep, const QByteArray &localMac6, const QString &localIp,
                const QString &netmask, QString *err);
    // 摘掉一张网卡（网卡消失/重配时）。其上的设备静态 ARP 由 removeDevice 各自清理。
    void removeNic(IL2Endpoint *ep);
    bool hasNic(IL2Endpoint *ep) const;

    // 登记/注销被劫持设备：静态 ARP(ip↔mac) + 记录其 mihomo 身份用户名。
    void addDevice(const QString &ip, const QByteArray &mac6, const QString &socksUser,
                   bool reject = false);
    void removeDevice(const QString &ip);

    // IPv6 版：登记设备的某个 v6 地址（从它发出的实帧里学到的，一台设备可能有多个：链路本地 +
    // 一或多个全局/隐私地址，每个都调一次）。现在只做一件事：**记录 mihomo 身份**。
    // 回程的二层寻址（邻居 + 每设备一条 /128 直连路由）由引擎从设备实帧自学，不再需要外部注入
    // ——lwIP 时代这里要装一条 nd6 静态邻居项。`from` 因此已无用，保留是为了不动调用方。
    // 幂等：同址重复调用只刷新。
    void addDeviceV6(IL2Endpoint *from, const QString &ip6, const QByteArray &mac6,
                     const QString &socksUser, bool reject = false);
    void removeDeviceV6(const QString &ip6);

    // 当前**实际生效**的 TCP 数据面："smoltcp"，或 "none"（init 失败 / 本平台是桩）。
    // ★ 存在的理由是测试可证伪性，不是好看：catch-all 的对外行为对任何实现都一样，
    //   同一个自测在哪条路上都会 PASS —— 没有这个标识就无法证明测的是哪一条。
    //   它曾经用于 smoltcp↔lwIP 的 A/B 断言（那个 A/B 抓到过真缺陷，见 R24）；lwIP 移除后
    //   只剩一个值，但**不删** —— 桩平台会诚实地报 "none"，诊断日志也该记它。
    const char *activeTcpStack() const;

    // 送入一个「已确认属于某被劫持设备」的以太帧（含 14 字节以太头）。
    // from = 收到该帧的二层端点，用来定位喂给哪张卡（也决定 UDP 回程从哪张卡发出）。
    void inputFrame(IL2Endpoint *from, const QByteArray &frame);

    // 前置声明公开（定义仍在 .cpp）：让 .cpp 内的自由函数/lwIP C 回调（在匿名命名空间里）
    // 能命名该类型。不暴露任何实现细节。
    struct Impl;
    struct Nic;

private:
    // UDP（含 DNS）不走 lwIP：手工解析设备发出的 UDP → Socks5Udp 转发；回程手工封包发回设备。
    // NAT 粒度是「每个设备源端口一条独立的 SOCKS UDP 关联」，所以回程不需要（也无法）靠
    // (fromIp,fromPort) 反查设备端口——vport 由「回包从哪条关联进来」直接给出。细节见 .cpp。
    void handleUdpFrame(Nic *nic, const QByteArray &frame, int ihl);
    // IPv6 UDP NAT：与 v4 版结构对称，复用同一套 UdpFlow/LRU/老化机制；帧封装与校验和用 v6 伪首部。
    void handleUdpFrame6(Nic *nic, const QByteArray &frame);
    // v4/v6 共用：回程 UDP 到设备。按会话记录的 v4/v6 走对应的封包/校验和。
    void onUdpResponse(const QString &victimIp, quint16 vport, const QHostAddress &fromIp,
                       quint16 fromPort, const QByteArray &payload);
    // DNS 劫持：把设备的 :53 查询转投 mihomo 的 DNS(127.0.0.1:1053) 而非原样中继到设备配置的 DNS
    //（常是网关/路由器 IP，经用户态栈中继到它走不通 → 名字解析时断时通）。见 .cpp。v6=true 按 v6 回封。
    void hijackDns(const QString &victimIp, quint16 vport, const QHostAddress &origServer,
                   const QByteArray &query, bool v6);
    // 那条常驻 DNS socket 的收包处理：按事务 ID 找回上下文并回封给设备。见 .cpp。
    void onDnsResponse();

    Impl *d;
};
