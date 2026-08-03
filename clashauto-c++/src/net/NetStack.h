#pragma once

// 用户态 TCP/IP 栈（基于 vendored lwIP，PIMPL 把 lwIP 头藏进 .cpp）。
//
// 职责：被劫持设备的以太帧经 LanGateway 过滤后送入 inputFrame()：
//   · IP/TCP 帧 → 交 lwIP(ethernet_input→ip4_input，accept-all 补丁使其终结发往任意公网 IP 的连接)
//     → 每条 TCP 连接经 Socks5Tcp 拨 mihomo 网关口(带每设备用户名) → 双向桥接字节。
//   · IP/UDP 帧 → 不进 lwIP，直接解析，经 Socks5Udp 转发（含 DNS）；回程手工封包发回设备。
//   · lwIP 出网(linkoutput) → 序列化成以太帧(dst=设备MAC，已由静态 ARP 表解析) → IL2Endpoint::send。
//
// **一个栈、多张网卡**：lwIP 只能有一个实例（lwip_init 全局、ARP 表全局、PCB 链全局），但一个实例
// 可以挂多个 netif。所以「同时代理有线 A 路由 + WiFi B 路由」的做法是 addNic() 一张卡一个 netif，
// 而不是开多个 NetStack。每个 netif 用**本机在该网卡上的真实 IP + 真实掩码**：
//   · 出方向 ip4_route(目的=设备IP) 靠子网匹配挑中正确的 netif（若像早期那样给 0.0.0.0/0 占位掩码，
//     两张卡都「匹配一切」，ip4_route 永远返回链表里第一张 → B 网段设备的回包会从 A 网卡发出去）；
//   · etharp_add_static_entry 内部也走 ip4_route，静态 ARP 因此自动落到正确的 netif 上。
// 每 netif 的二层端点/本机 MAC 放在 netif->state 里，C 回调从那里取上下文（不再用全局单例）。
//
// 线程：NetStack 自身不做线程化——它假定「由**单一**线程拥有并调用」（NO_SYS 单线程 lwIP：
// lwip_init/ARP 表/PCB 链全局，g_impl / ConnWatch 等哨兵也都依赖单线程前提）。全进程只有一个
// NetStack 实例，那个实例始终在某一个线程上：在正式 App 里是 LanGateway 的专用工作线程
// （见 LanGateway_linux.cpp 的线程模型），在 headless 自测里是主线程。两者从不并存 → 单线程前提恒成立。
// init() 创建的 QTimer 挂在**调用 init 的那个线程**的事件循环上周期 sys_check_timeouts()。
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
//   ── 2026-08-03 复核（同一套 netns 假外网夹具，但用的是当天的代码）────────────
//     被代理设备 .203 → netns 里的 iperf3（10.99.0.2:5202），12s：
//         吞吐 832 Mb/s   核心 1.58 核·秒   App 0.33 核·秒   合计 1.91 核·秒
//         折算 1.91 / 10.01 Gbit = **0.19 核/Gbps**（原记录 0.24~0.25）
//     变好的主要是 **App 那一档**：0.06 → 0.033 核/Gbps，与「日志不再逐条进模型 + 页脚节流」
//     那次改动（满负载下 App 27.9% → 4.7%）方向一致。核心侧 0.158 核/Gbps。
//     ★ 这个数与本文件后面「核心自身 0.38 核/Gbps」那条不是一回事，别混：那条量的是
//       **HTTP 混合口**的中继（curl -x 127.0.0.1:7890），这条量的是 **TPROXY 入站**；
//       两者差 2.4 倍尚未逐项拆开，在没拆清之前把 0.38 当 HTTP 代理路径的上界看。
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

class NetStack final : public QObject
{
    Q_OBJECT
public:
    explicit NetStack(quint16 socksPort, QObject *parent = nullptr);
    ~NetStack() override;

    // 初始化 lwIP + catch-all TCP 监听 + 定时器泵。全进程只能有一个实例。失败置 *err。
    bool init(QString *err);

    // 挂一张网卡：ep 为其二层端点，localMac6/localIp/netmask 为本机在这张卡上的信息。
    // localIp/netmask 必须有效——它们决定出方向选哪个 netif（见文件头说明）。
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
    // 一或多个全局/隐私地址，每个都调一次）。装一条 nd6 **静态邻居**项（v6↔mac，回程二层寻址靠它，
    // 见 nd6.c 的 Coast 补丁）+ 记录 mihomo 身份。from = 学到该帧的网卡端点（决定装到哪个 netif）。
    // 幂等：同址重复调用只刷新。
    void addDeviceV6(IL2Endpoint *from, const QString &ip6, const QByteArray &mac6,
                     const QString &socksUser, bool reject = false);
    void removeDeviceV6(const QString &ip6);

    // 送入一个「已确认属于某被劫持设备」的以太帧（含 14 字节以太头）。
    // from = 收到该帧的二层端点，用来定位注入哪个 netif（也决定 UDP 回程从哪张卡发出）。
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
