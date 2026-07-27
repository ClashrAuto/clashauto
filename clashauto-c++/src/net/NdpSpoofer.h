#pragma once

// NDP 双向投毒（中间人）—— ArpSpoofer 的 **IPv6 对应物**。IPv6 没有 ARP，「谁在哪个 MAC」全靠
// 邻居发现（NDP，RFC 4861 的 ICMPv6 NS/NA/RS/RA）。透明网关要把被劫持设备的 v6 出网流量引到本机，
// 就得让设备**误以为默认路由器（v6 网关）在本机 MAC 上**。
//
// 原理（与 ArpSpoofer 逐条对应）：
//   (a) 周期性给 victim 发**伪造的非请求邻居通告 NA**（ICMPv6 type=136）：target = v6 默认路由器的
//       **链路本地地址**（fe80::…），TargetLinkLayerAddress 选项填**本机 MAC**，置 Override(O) 标志
//       → 设备把「路由器在本机 MAC」覆盖进邻居缓存，出网 v6 于是发给我们。
//       ★ 送达手段（关键、故意为之）：这条 NA 的 **L3 目的写 ff02::1（all-nodes 组播）**，但 **L2 目的
//         写成 victim 的单播 MAC**。于是只有被劫持的那台设备收到并处理它（它是 all-nodes 组成员），
//         别的设备一无所知——既拿到了「非请求 NA 也能覆盖」的效果（Override），又不会污染整段网络的
//         v6（对比真发 L2 组播会把在线所有设备的默认路由都改到本机）。这正是 IPv4 侧「gratuitous/
//         request 只发给 victim」思路的 v6 版。**不需要知道设备的 v6 地址**，只要它的 MAC。
//   (b) 抢答设备主动发来的**邻居请求 NS**（type=135，问「路由器 fe80:: 在哪」）：立刻回一帧 solicited
//       NA（target=路由器 LL、TLLA=本机 MAC、S+O 置位）单播给它，赶在真路由器应答前。对应
//       ArpSpoofer::answerGatewayArp。
//   (c) 一看到**真路由器自己**发的 NA/RA（把设备解毒），立刻把所有 victim 重投一轮盖回去。对应
//       ArpSpoofer::reassertNow。
//
// 为什么**不投毒路由器**（与 IPv4 的 (b) 发给网关那步的取舍）：本网关是终结式代理——设备的 v6 连接
// 在 lwIP 里终结、由 mihomo 从本机另开出去，回程落在**本机/mihomo 的 socket** 上，不经「路由器→设备」
// 那条 LAN 路径；而 lwIP 回包给设备是**直连二层**（NetStack 用 nd6 静态邻居项把 设备v6→设备MAC 钉死，
// 见 nd6.c 的 Coast 补丁）。所以回程根本不需要路由器把包送到我们这——只投毒设备一侧即可。（记录在案，
// 这是相对 ArpSpoofer「双向都投」的一处**有意简化**。）
//
// 安全红线：**必须可靠还原**（同 ArpSpoofer）。停止/析构/退出时给每个 victim 重发数遍「路由器在真实
// 路由器 MAC」的 NA（heal），否则设备的 v6 邻居缓存仍指向本机 → 该设备 v6 断网。
//
// 本类**跨平台可编译**：只拼 QByteArray 以太帧交给 IL2Endpoint::send()，不含任何系统调用。端点由外部
// 持有（本类不拥有）。未配置（缺路由器 LL / 路由器 MAC 等）时所有操作 no-op + qWarning。
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class IL2Endpoint;
class QTimer;

class NdpSpoofer : public QObject
{
    Q_OBJECT
public:
    explicit NdpSpoofer(IL2Endpoint *endpoint, QObject *parent = nullptr);
    ~NdpSpoofer() override; // 析构必须 healAll()

    // 配置本机拓扑。localMac/routerMac = "aa:bb:..:ff"；routerLinkLocal = v6 默认路由器的**链路本地**
    // 地址串（"fe80::…"）。解析失败视为未配置（后续操作 no-op）。网段/路由器变化时可重配。
    void configure(const QString &localMac, const QString &routerLinkLocal, const QString &routerMac);

    // 开始劫持某设备：只需其 MAC（投毒 NA 靠 L2 单播送达，不需要设备的 v6 地址）。立即投一次。
    void startSpoof(const QString &victimMac);
    // 停止劫持某设备：立刻还原（heal）其 v6 邻居缓存，从集合移除。
    void stopSpoof(const QString &victimMac);
    // 给所有被劫持设备发还原 NA 后清空集合、停表。退出/急停调用。
    void healAll();

    // ———————————— 从线上观察到的 RA 学 v6 路由器 ————————————
    // 为什么需要：本机的 v6 默认路由**不能**当作「链路上有没有 v6 路由器」的判据。真机实证
    // （树莓派网关）：`net.ipv6.conf.eth0.accept_ra = 0` 时本机永远拿不到 v6 默认路由，于是
    // LanScanner 交上来的 routerLinkLocal6 恒为空 → 本类 no-op；而被劫持设备照样通过 RA 拿到
    // 全局地址和 v6 默认路由，它的 v6 流量**整段绕过代理，且完全无声**。这正是 v6 劫持要根治的
    // 那类「时通时不通」。网关本来就开着混杂模式、BPF 也已放行全部 ICMPv6（反制解毒要用），
    // **RA 本来就流经抓包链路** —— 直接从中学，就彻底不依赖本机的 accept_ra / 路由表了。
    //
    // 解析一帧 RA，成功返回 true 并填出参（任一出参可为 nullptr）：
    //   routerLL  —— 路由器的**链路本地**地址串（取自 IPv6 源地址；非 fe80::/10 一律拒绝）
    //   routerMac —— 路由器 MAC：优先取 RA 里的 Source Link-Layer Address 选项(type 1)，
    //                缺失则回落到以太头源 MAC
    //   prefixNet/prefixLen —— Prefix Information 选项(type 3) 里的**on-link** 前缀（供 LAN 内
    //                v6 直连旁路用）。没有该选项时不填（prefixLen 置 -1），不影响前两项。
    // 拒绝条件（按 RFC 4861 的安全要求 + 本用途的语义）：
    //   · 不是 RA / 帧太短 / 选项越界；
    //   · IPv6 跳数限制 != 255（NDP 强制，防跨网段伪造）；
    //   · Router Lifetime == 0 —— 这台只提供前缀信息、**不是默认路由器**，拿它当投毒目标是错的。
    static bool parseRouterAdvert(const QByteArray &frame, QString *routerLL, QString *routerMac,
                                  QByteArray *prefixNet, int *prefixLen);

    // 只判「是不是 RA」（parseRouterAdvert 的轻量前置判据，避免对每个 NA 都走完整解析）。
    static bool isRouterAdvert(const QByteArray &frame);

    // 抢答：若 frame 是被劫持设备发出的「路由器 LL 在哪?」邻居请求 NS，立刻回一帧 solicited NA
    //（路由器 LL 在本机 MAC）。返回 true = 确实是问路由器且已抢答。契约同 ArpSpoofer::answerGatewayArp
    //（源 MAC 已由二层过滤限定为被劫持设备）。
    bool answerNeighborSolicit(const QByteArray &frame);

    // 一看到真路由器发的 NA/RA（会把设备解毒）就把所有 victim 重投一轮盖回。自带 ~50ms 节流。
    // 对应 ArpSpoofer::reassertNow。
    void reassertNow();

    // 唤醒沿高频重投（对应 ArpSpoofer::startBoost）：某 victim 空闲后又活跃 → 邻居缓存多半刚老化，
    // 立刻推一轮 + 进入 50ms×N 高频窗口，压过唤醒期真路由器的应答。
    void startBoost();

    // 判定一帧是否是「真路由器发来的 NDP（NA/RA）」——供 LanGateway 决定要不要 reassert。
    // 只看 ethertype=0x86DD + ICMPv6 type ∈ {134 RA,136 NA}；调用方另按源 MAC=路由器 MAC 过滤。
    static bool isRouterAdvertOrNa(const QByteArray &frame);

    QStringList victims() const; // 当前被劫持的 victim MAC 列表（小写）

private:
    void tick();                 // 周期给所有 victim 重投毒
    void boostTick();            // 唤醒沿高频窗口内重投
    void sendSpoof(const QByteArray &victimMac6); // 给一个 victim 发一帧投毒 NA
    void healOne(const QByteArray &victimMac6);    // 给一个 victim 发数遍还原 NA
    bool configured() const;
    bool hasVictimMac(const QByteArray &mac6) const;

    static QByteArray macToBytes(const QString &);  // "aa:bb:.." → 6 字节（非法返回空）
    static QByteArray ip6ToBytes(const QString &);  // "fe80::.." → 16 字节（非法返回空）

    // 拼一帧完整以太 + IPv6 + ICMPv6 NA（type=136）。所有 QByteArray 长度须正确：mac=6，ip6=16。
    // flags：bit7=Router(R) bit6=Solicited(S) bit5=Override(O)。tllaMac=目标链路层地址选项里的 MAC。
    static QByteArray buildNa(const QByteArray &ethDst, const QByteArray &ethSrc,
                              const QByteArray &srcIp6, const QByteArray &dstIp6,
                              const QByteArray &targetIp6, const QByteArray &tllaMac, quint8 flags);
    // IPv6 伪首部 + ICMPv6 报文的 16 位反码校验和。
    static quint16 icmp6Checksum(const QByteArray &srcIp6, const QByteArray &dstIp6,
                                 const uchar *icmp, int icmpLen);

    IL2Endpoint *m_endpoint = nullptr; // 不拥有
    QTimer *m_timer = nullptr;
    QTimer *m_boostTimer = nullptr;
    int m_boostRemaining = 0;
    QElapsedTimer m_lastReassert;
    // configure() 的日志去重：只在「配好↔没配好」翻转时说一句（它每轮扫描都会被调用）。
    bool m_lastConfigOk = false;
    bool m_configLogged = false;
    QByteArray m_localMac;    // 6 字节（本机 = 冒充的“路由器”）
    QByteArray m_routerLL;    // 16 字节（v6 默认路由器的链路本地地址）
    QByteArray m_routerMac;   // 6 字节（真实路由器 MAC，heal 时用）
    QHash<QString, QByteArray> m_victims; // key = 小写 victimMac，value = 6 字节 MAC
    // victim mac6(6B) → 上次抢答其 NS 时学到的**链路本地源地址**(16B)。heal 时要向它**单播**一条
    // solicited NA 才能覆盖设备「已 REACHABLE」的网关表项（组播 solicited NA 是非法帧、会被丢）。
    QHash<QByteArray, QByteArray> m_victimLL;
};
