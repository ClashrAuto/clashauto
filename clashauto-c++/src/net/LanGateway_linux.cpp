// LanGateway 的 Linux/mac/Windows 真实现（编译条件由 CMake 控制：Linux OR APPLE OR WIN32 都编入
// 本文件——文件名是历史遗留，实为 POSIX/Win 通用，二层差异全在 createL2Endpoint）。
// 组合：IL2Endpoint(二层) + ArpSpoofer(双向 ARP 投毒/还原) + NetStack(lwIP 用户态栈)。
//
// ——————————————————————————— 线程模型（本次重构核心）———————————————————————————
//
// 数据面（每包都要碰的东西）整体搬到一个专用 QThread 上：
//   · 二层端点 IL2Endpoint（QSocketNotifier/QWinEventNotifier —— 通知器必须在服务它的线程上创建，
//     所以端点的 open() 必须在工作线程跑）；
//   · NetStack（NO_SYS 的 lwIP + 每连接一个 Socks5Tcp / 每 UDP 流一个 Socks5Udp）——lwIP 完全不是
//     线程安全的，它的**所有**公开方法只能在工作线程上被调；
//   · ArpSpoofer（1500ms 定时器发 ARP）；
//   · 帧过滤 lambda（frameReceived → 旁路判断 → NetStack::inputFrame）。
// 这些全部由下面的 GatewayWorker 持有，GatewayWorker moveToThread 到 m_thread。
//
// GUI 线程只保留 LanGateway 这层「编排入口」。它的公共 API 一个字没改；跨线程的事都在这里消化：
//   · 会改状态的写方法（configure / enableDevice / disableDevice / disableAll / recoverFromCrash）
//     用 **BlockingQueuedConnection** 投到工作线程执行——保持原来的同步语义（尤其 enableDevice 要
//     同步返回 bool+错误串；disableAll 要在还原 ARP 真正发出去之后才返回，见下）。
//   · GUI 线程会读的只读方法（isAvailable / canProxy / activeDevices）**不跨线程**：工作线程每次改
//     状态后把一份快照写进 GwShared（互斥量保护），GUI 直接读快照。没有线程往返 ⇒ 不阻塞、不撕裂。
//   · 信号 deviceError / statusChanged 由工作线程 emit，经 Qt 队列连接转成 LanGateway 的同名信号在
//     GUI 线程重发（只携带 QString 值类型，不传裸指针）。
//
// —— 为什么不会互等死锁（改这块最容易写出双向阻塞）——
//   GUI→worker 是阻塞调用；而 worker 在处理这些调用时**从不反过来阻塞等 GUI**：它 emit 的信号都是
//   队列连接（只投递、不等待），写快照只碰一把自己的互斥量，端点 send / lwIP / QTcpSocket 都不发任何
//   跨线程 Qt 调用。于是「GUI 等 worker」+「worker 从不等 GUI」⇒ 无环 ⇒ 不可能死锁。唯一的外部调用者
//   只有 GUI 线程（DevicesController / main_qml 都在主线程），所以这个论证是完备的。
//   工作线程处理每个事件都是有界的（pcap 一次 drain 完当前缓冲、lwIP 一次泵完），不存在无界循环把
//   阻塞调用饿死——所以 GUI 的阻塞等待也总是有界的。
#include "LanGateway.h"

#include "ArpSpoofer.h"
#include "ArpWatch.h"
#include "GatewayPanic.h" // 崩溃兜底：上劫持时预存还原帧，进程被打死时由信号处理器裸发
#include "IL2Endpoint.h"
#include "NdpSpoofer.h"
#include "NetStack.h"
#include "TproxyRules.h" // TPROXY 数据面的内核规则（装/拆/设备集合）

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkInterface>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>

#include "GatewayDiag.h" // 帧分流计数（落盘采样；与 gwDbgOn 的 stderr 抽样打印互补）
#include <QVector>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility> // std::as_const

namespace {
// COAST_GATEWAY_DEBUG=1 → 把「收到的设备帧走到哪一步」打到 stderr（配合 L2Endpoint 的 [WINL2]
// 探针，定位「设备流量为 0」断在收包 / victim 过滤 / 同网段旁路 / 喂进 lwIP 的哪一环）。
bool gwDbgOn()
{
    static const bool on = qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG");
    return on;
}
// 空闲阈值：某 victim 超过这么久没发过帧、又忽然来帧 = 视为「唤醒」。它多半刚把网关重新解析过
// （或即将解析），触发 ArpSpoofer 的高频重投窗口抢在真网关前钉回投毒。取 8s：短于常见的邻居缓存
// 老化（Linux base_reachable_time≈30s、Win/mac NUD 类似量级），足以覆盖「表项刚老化就来流量」。
constexpr qint64 kWakeIdleMs = 8000;
} // namespace

namespace {
// "aa:bb:cc:dd:ee:ff" → 6 字节（非法返回空）。
QByteArray macBytes(const QString &mac)
{
    const QStringList parts = mac.split(':');
    if (parts.size() != 6)
        return {};
    QByteArray out(6, char(0));
    for (int i = 0; i < 6; ++i) {
        bool ok = false;
        out[i] = char(parts[i].toUInt(&ok, 16));
        if (!ok)
            return {};
    }
    return out;
}
// 6 字节 MAC → quint64 键（大端拼装，高 16 位恒为 0）。
// 为什么不用 QByteArray 当键：这是每帧都要做一次的查表，用 QByteArray 就得先
// frame.mid(6, 6) 深拷贝出 6 个字节（一次堆分配 + 引用计数块），高吞吐时纯浪费；
// MAC 只有 48 位，塞进 quint64 是无损的，可以直接从帧缓冲原地读出来，零分配。
inline quint64 macKey(const uchar *p)
{
    return (quint64(p[0]) << 40) | (quint64(p[1]) << 32) | (quint64(p[2]) << 24)
           | (quint64(p[3]) << 16) | (quint64(p[4]) << 8) | quint64(p[5]);
}
inline quint64 macKey(const QByteArray &mac)
{
    return mac.size() == 6 ? macKey(reinterpret_cast<const uchar *>(mac.constData())) : 0;
}
// 点分 IPv4 → 主机序 quint32（非法/空/非 IPv4 返回 0）。
quint32 ipToU32(const QString &ip)
{
    if (ip.isEmpty())
        return 0;
    bool ok = false;
    const quint32 v = QHostAddress(ip).toIPv4Address(&ok);
    return ok ? v : 0;
}

// —— IPv6 帧判定小工具（p6 指向 IPv6 头首字节，即以太头之后；仅在已确认 ethertype=0x86DD 时用）——
// 是否 NDP（ICMPv6 的 RS/RA/NS/NA，type 133~136）——NDP 由 NdpSpoofer 接管，不喂 lwIP。
inline bool ip6IsNdp(const uchar *p6, int ip6Len)
{
    if (ip6Len < 41)          // 40 头 + 至少 1 字节 ICMPv6 type
        return false;
    if (p6[6] != 58)          // next header = ICMPv6（NDP 无扩展头）
        return false;
    const quint8 t = p6[40];  // ICMPv6 type
    return t >= 133 && t <= 136;
}
inline bool ip6IsNs(const uchar *p6, int ip6Len)
{
    return ip6Len >= 41 && p6[6] == 58 && p6[40] == 135;
}
// 目的地址是「链路本地(fe80::/10) 或 组播(ff00::/8)」——本地链路流量/NDP，不出网，不喂 lwIP。
inline bool ip6DstIsLocalOrMulticast(const uchar *p6, int ip6Len)
{
    if (ip6Len < 40)
        return true; // 头都不全，当作不出网丢弃
    const uchar *dst = p6 + 24;
    if (dst[0] == 0xFF)                                   // 组播
        return true;
    if (dst[0] == 0xFE && (dst[1] & 0xC0) == 0x80)        // 链路本地
        return true;
    return false;
}
// 源地址是否「可作为设备的可路由身份」——即全局单播 / 唯一本地(ULA fc00::/7)，用于回程静态邻居。
// 排除链路本地、组播、未指定(::)。src 指向 16 字节源地址。
inline bool ip6SrcIsRoutable(const uchar *src)
{
    if (src[0] == 0xFF)                              // 组播
        return false;
    if (src[0] == 0xFE && (src[1] & 0xC0) == 0x80)   // 链路本地
        return false;
    bool allZero = true;
    for (int i = 0; i < 16; ++i)
        if (src[i] != 0) { allZero = false; break; }
    return !allZero;
}
// 解析 "2408:xxxx::/64" → 网络 16 字节 + 前缀长度。失败返回 false。
bool parseV6Prefix(const QString &cidr, QByteArray *netOut, int *lenOut)
{
    const int slash = cidr.indexOf('/');
    if (slash < 0)
        return false;
    bool ok = false;
    const int len = cidr.mid(slash + 1).toInt(&ok);
    if (!ok || len < 0 || len > 128)
        return false;
    QHostAddress a(cidr.left(slash));
    if (a.isNull() || a.protocol() != QAbstractSocket::IPv6Protocol)
        return false;
    const Q_IPV6ADDR v6 = a.toIPv6Address();
    *netOut = QByteArray(reinterpret_cast<const char *>(v6.c), 16);
    *lenOut = len;
    return true;
}
// dst16（16 字节）是否落在 (net,plen) 前缀内。net 为 16 字节网络部分。
bool ip6InPrefix(const uchar *dst16, const QByteArray &net, int plen)
{
    if (net.size() != 16 || plen < 0)
        return false;
    const uchar *n = reinterpret_cast<const uchar *>(net.constData());
    int fullBytes = plen / 8;
    for (int i = 0; i < fullBytes; ++i)
        if (dst16[i] != n[i])
            return false;
    const int rem = plen % 8;
    if (rem) {
        const uchar mask = static_cast<uchar>(0xFF << (8 - rem));
        if ((dst16[fullBytes] & mask) != (n[fullBytes] & mask))
            return false;
    }
    return true;
}
} // namespace

// 一张物理网卡的运行时套件：二层端点 + ARP 投毒器 + NDP(v6) 投毒器 + 该卡的拓扑数值。（活在工作线程上。）
struct GwNic {
    LanGateway::NicSpec spec;
    IL2Endpoint *ep = nullptr;
    ArpSpoofer *arp = nullptr;
    ArpWatch *watch = nullptr; // 被动 ARP/NDP 冲突监视（只读，检测本机被劫持/设备被争抢）
    NdpSpoofer *ndp = nullptr; // IPv6 邻居发现投毒器；v6 拓扑缺失时 configure 后自身 no-op
    quint32 localIp4 = 0, netMask4 = 0, gatewayIp4 = 0;
    QByteArray prefix6Net;     // v6 前缀网络部分（16 字节；空=未知，则不做 v6 同网段旁路）
    int prefix6Len = -1;       // v6 前缀长度（bit 数；<0=未知）
    // ★ 从线上 RA 学到的 v6 拓扑，**必须与 spec 分开存**：configureLocal 每轮都 `n->spec = spec`
    //   （扫描每 ~5s 一轮），学到的值若写进 spec 会被下一轮原样抹掉。取用时以 spec 为先、这里兜底
    //   （见 effectiveRouterLL6/Mac6）：扫描能拿到就用扫描的，拿不到（本机 accept_ra=0 等）才用
    //   从 RA 学的 —— 修的正是「本机没配上 v6 就以为链路上没有 v6」这个错误判据。
    QString learnedRouterLL6;
    QString learnedRouterMac6;
    QByteArray learnedPrefix6Net;
    int learnedPrefix6Len = -1;
    // v6 拓扑的**唯一取用口径**：扫描（本机路由表）优先，缺则用从 RA 学到的。
    // routerMac6 再缺则回落 v4 网关 MAC（同一路由器 v4/v6 常共用一个 NIC MAC）。
    QString effectiveRouterLL6() const
    {
        return spec.routerLinkLocal6.isEmpty() ? learnedRouterLL6 : spec.routerLinkLocal6;
    }
    QString effectiveRouterMac6() const
    {
        if (!spec.routerMac6.isEmpty())
            return spec.routerMac6;
        if (!learnedRouterMac6.isEmpty())
            return learnedRouterMac6;
        return spec.gatewayMac;
    }
    bool ready = false;      // ep 已打开且 netif 已挂上协议栈
    int victims = 0;         // 这张卡上正在被劫持的设备数（>0 时不重建，避免断流）
    int order = 0;           // configure 传入的 specs 次序（LanScanner 保证 index 0 = 主网卡）。
                             // nicForIp 在多张同网段网卡里据此确定性选主卡，不再靠 QHash 无序遍历。
};

// GUI 线程只读的一份状态快照。工作线程每次改状态后写、GUI 读，都在这把互斥量下——
// 不做线程往返，所以 isAvailable/canProxy/activeDevices 既不会阻塞也读不到撕裂的中间态。
// 用 shared_ptr 让 LanGateway 与工作线程共享同一实例，工作线程 deleteLater 的时机再晚也不悬垂。
struct GwSubnet {
    quint32 ip = 0;
    quint32 mask = 0;
};
struct GwShared {
    QMutex mutex;
    bool available = false;
    QStringList active;          // activeDevices()：被劫持 mac 列表
    QVector<GwSubnet> subnets;   // canProxy()：已就绪网卡的 (ip,mask)
};

// ———————————————————————— 工作线程对象：整个数据面都在它身上 ————————————————————————
// 它的所有方法（configureLocal/enableDeviceLocal/…）都**只在工作线程上**执行：由 LanGateway 经
// BlockingQueuedConnection 投过来。它创建的一切 QObject（NetStack、端点、ArpSpoofer、Socks5*）都
// parent 到自己 → 都活在工作线程上 → 也在工作线程上析构（constraint 9）。
class GatewayWorker : public QObject
{
    Q_OBJECT
public:
    explicit GatewayWorker(std::shared_ptr<GwShared> shared, QObject *parent = nullptr)
        : QObject(parent), m_shared(std::move(shared))
    {
    }
    ~GatewayWorker() override { teardownLocal(); }

    // 以下六个方法皆在工作线程执行。
    void configureLocal(const QVector<LanGateway::NicSpec> &specs, quint16 socksPort);
    bool enableDeviceLocal(const QString &mac, const QString &ip, const QString &socksUser, bool reject,
                           QString *err);
    void disableDeviceLocal(const QString &mac);
    void disableAllLocal();
    // 数据面模式。必须在 configureLocal 之前设——它按模式决定建不建 NetStack。
    void setDatapathLocal(const LanGateway::DatapathSpec &spec) { m_datapath = spec; }
    void recoverLocal();
    void teardownLocal(); // 还原全部 ARP + 销毁 NetStack/端点/ArpSpoofer（幂等）。退出/析构走它。

signals:
    void deviceError(const QString &mac, const QString &message);
    /// 从设备自己的帧里现学到它换了 v4 地址（DHCP 续约等）。上层据此更新台账并按新地址重挂劫持。
    void deviceIpChanged(const QString &mac, const QString &newIp);
    void statusChanged();
    void securityAlert(int kind, const QString &offenderMac, const QString &subjectIp,
                       const QString &subjectMac);

private:
    bool availableLocal() const;
    GwNic *nicForIp(const QString &ip) const;
    void pushMacFilter(GwNic *n) const;
    QString statePath() const;
    void persist() const;
    void clearState() const { QFile::remove(statePath()); }
    // 状态文件 = 「当前正在劫持」∪「上次遗留、还没还原成功」。两边都空才删文件，否则重写。
    // 所有原来写 persist()/clearState() 的地方都改走它，免得某一边把另一边的记录冲掉。
    void saveState() const;
    // 尝试还原 m_pendingHeal 里的遗留投毒记录；还原不了的留着，等网卡就绪后下一轮 configure 再试。
    void healPending();
    // 把当前状态刷进 GUI 可读的快照（每次改状态后调用一次）。
    void publishSnapshot();

    NetStack *m_net = nullptr;               // 共用（lwIP 单实例，多 netif）；**tproxy 模式下恒为 null**
    // —— 数据面模式 ——
    // tproxy 模式下不建 NetStack,数据帧根本不进用户态:内核转发 + nft TPROXY 直接投给核心。
    // 此时二层端点只用来收发 ARP/NDP（劫持与安全监视），内核过滤器也只放行这两类帧。
    LanGateway::DatapathSpec m_datapath;
    TproxyRules m_tproxy;
    // 把当前受害设备的 IP 集合推给 nft（谁被接管由这个集合决定）。lwIP 模式下是空操作。
    void syncTproxyDevices();
    QHash<QString, GwNic *> m_nics;          // ifname → 套件
    quint16 m_socksPort = 0;
    QHash<quint64, QString> m_victimByMac;   // src MAC 打包键 → ip（帧过滤 + 记账）
    // 被隔离（policy=reject）设备的 MAC 打包键。它们去局域网的流量按方向过滤，见 forwardIsolatedLan。
    QSet<quint64> m_isolatedMacs;
    bool isIsolatedMac(const uchar *mac6) const { return m_isolatedMacs.contains(macKey(mac6)); }
    // 本机**自己**的全部地址（所有网卡，不只我们接管的那几张）。用途见帧分流里的「本机地址旁路」。
    // 每轮 configureLocal（扫描约 5s 一轮）重建一次；查询在帧分流热路径上，故 v4 用哈希集、
    // v6 用定长数组的线性 memcmp（一台主机的 v6 地址数是个位数，比建 QByteArray 便宜得多）。
    QSet<quint32> m_localIp4;
    QVector<std::array<quint8, 16>> m_localIp6;
    void refreshLocalAddrs();
    bool isLocalIp6(const uchar *a) const
    {
        for (const auto &x : m_localIp6)
            if (std::memcmp(x.data(), a, 16) == 0)
                return true;
        return false;
    }
    void forwardIsolatedLan(GwNic *n, const QByteArray &frame, const uchar *f);
    void pushIsolatedMacs(GwNic *n) const;
    // src MAC 打包键 → 最后一次看到「台账上那个 IP」发帧的时刻（uptime ms）。
    // 用来把「换地址」和「一个 MAC 上挂着多个 IP」区分开，见 learnDeviceV4。
    QHash<quint64, qint64> m_victimIpSeenMs;
    QHash<QString, QString> m_victimMacStr;  // ip → mac 串（disable 用）
    QHash<QString, QString> m_victimNic;     // ip → ifname（disable/持久化找回对应网卡）
    QHash<quint64, qint64> m_lastSeenMs;     // src MAC 打包键 → 上次见到该 victim 帧的时刻（唤醒沿检测）
    QHash<quint64, QString> m_victimUserByMac; // src MAC 打包键 → mihomo 身份 user（v6 学习时补登记要用）
    QHash<quint64, QSet<QString>> m_victimV6ByMac; // src MAC 打包键 → 已学到并登记的设备 v6 地址集（去重/回收）
    // 上次异常退出遗留、**尚未还原成功**的投毒记录（每项自带 ifname/localMac/gatewayIp/gatewayMac/
    // routerLL6/routerMac6，与状态文件里的对象同形）。开机那一刻网卡常常还没就绪，一次还原打空是
    // 常态；留在这里 + 写回状态文件，等 configureLocal 拿到就绪网卡后重试，直到真的还原掉。
    QVector<QJsonObject> m_pendingHeal;
    void learnDeviceV6(GwNic *n, const uchar *f, const QByteArray &frame); // 从设备 v6 帧学它的地址
    void learnDeviceV4(GwNic *n, const uchar *f, const QByteArray &frame); // 从设备 v4 帧学它换没换地址
    // 从线上观察到的 RA 学 v6 路由器（不依赖本机 accept_ra / 路由表）。详见实现处的说明。
    void learnRouterFromRa(GwNic *n, const QByteArray &frame);
    std::shared_ptr<GwShared> m_shared;
    bool m_torndown = false;
    // 诊断计数（COAST_GATEWAY_DEBUG）：设备帧在 frameReceived 里各分支的去向。
    long long m_dbgDropNonVictim = 0, m_dbgBypassLan = 0, m_dbgFedLwip = 0, m_dbgArpAnswered = 0,
              m_dbgReassert = 0;
};

// 把「属于这张卡的」被劫持设备源 MAC 集合推给它的二层端点，装成内核态源 MAC 过滤（收方优化）。
// 把当前受害设备的 IP 推给 nft 的 proxied 集合。**谁被接管完全由这个集合决定**：
// 不在集合里的设备照常由内核转发（等价于「没开代理」），所以集合为空时装着规则也是安全的。
// lwIP 模式下是空操作。
void GatewayWorker::syncTproxyDevices()
{
    if (!m_datapath.tproxy || !m_tproxy.isInstalled())
        return;
    QStringList ips, macs;
    ips.reserve(m_victimMacStr.size());
    macs.reserve(m_victimMacStr.size());
    for (auto it = m_victimMacStr.constBegin(); it != m_victimMacStr.constEnd(); ++it) {
        ips.append(it.key());       // 这张表是 ip → mac
        macs.append(it.value());    // v6 兜底丢弃按 MAC 匹配（tproxy 下不学设备 v6 地址）
    }
    QString err;
    if (!m_tproxy.syncDevices(ips, macs, &err))
        emit deviceError(QString(), QStringLiteral("TPROXY 设备集合更新失败: ") + err);

    // 局域网隔离（policy=reject）：被隔离设备的 IP + 本机各网卡的网段。
    QStringList isoIps;
    for (auto it = m_victimMacStr.constBegin(); it != m_victimMacStr.constEnd(); ++it) {
        const QByteArray mb = macBytes(it.value());
        if (mb.size() == 6
            && m_isolatedMacs.contains(macKey(reinterpret_cast<const uchar *>(mb.constData()))))
            isoIps.append(it.key());
    }
    QStringList lans;
    for (GwNic *n : m_nics) {
        if (!n->ready || !n->netMask4)
            continue;
        int bits = 0; // 掩码一定是连续 1，数前导 1 即前缀长度
        for (quint32 m = n->netMask4; m & 0x80000000u; m <<= 1)
            ++bits;
        const quint32 net = n->localIp4 & n->netMask4;
        const QString cidr = QStringLiteral("%1.%2.%3.%4/%5")
                                 .arg((net >> 24) & 0xFF).arg((net >> 16) & 0xFF)
                                 .arg((net >> 8) & 0xFF).arg(net & 0xFF).arg(bits);
        if (!lans.contains(cidr))
            lans.append(cidr);
    }
    if (!m_tproxy.syncIsolation(isoIps, lans, &err))
        emit deviceError(QString(), QStringLiteral("TPROXY 隔离集合更新失败: ") + err);
}

void GatewayWorker::pushMacFilter(GwNic *n) const
{
    // ★ tproxy 模式：**一个数据帧都不要**。劫持仍然需要收发 ARP/NDP（抢答、反制、被争抢检测），
    //   但设备的数据帧由内核转发,进用户态纯属浪费——那正是这条数据面要省掉的东西。
    //   传空集合即可:下面那套通用布局在 n=0 时恰好退化成「只放行 ARP + NDP」（见该函数内注释）。
    if (m_datapath.tproxy) {
        if (n && n->ep)
            n->ep->setSourceMacFilter({});
        return;
    }
    if (!n || !n->ep)
        return;
    QVector<QByteArray> macs;
    for (auto it = m_victimMacStr.constBegin(); it != m_victimMacStr.constEnd(); ++it) {
        if (m_victimNic.value(it.key()) != n->spec.ifname)
            continue;
        const QByteArray mb = macBytes(it.value());
        if (mb.size() == 6)
            macs.append(mb);
    }
    n->ep->setSourceMacFilter(macs);
}

// 把当前的被隔离 MAC 集合推给这张卡的 ArpSpoofer（抢答时要据此判「是不是被禁设备在问」）。
void GatewayWorker::pushIsolatedMacs(GwNic *n) const
{
    if (!n || !n->arp)
        return;
    QVector<QByteArray> macs;
    for (quint64 k : m_isolatedMacs) {
        QByteArray m(6, '\0');
        for (int i = 0; i < 6; ++i)
            m[5 - i] = char((k >> (8 * i)) & 0xFF);
        macs.append(m);
    }
    n->arp->setIsolatedMacs(macs);
}

// 被隔离设备发往同网段的一帧：按方向放行或丢弃；放行的改写目的 MAC 后转发给真正的对端。
void GatewayWorker::forwardIsolatedLan(GwNic *n, const QByteArray &frame, const uchar *f)
{
    if (!n || !n->ep || !n->arp || frame.size() < 34)
        return;
    const int ihl = (f[14] & 0x0F) * 4;
    if (ihl < 20 || frame.size() < 14 + ihl)
        return;
    // ★ **源 IP 必须等于该设备自己的地址**，否则我们就是它的伪造跳板。
    //   这条路径会把放行的帧**改写源 MAC 为本机**再发出去，所以危害比普通伪造更重：
    //   接收方看到的是「来自网关机 MAC 的帧」，更可信，而且反查不到真凶 —— 一台被我们
    //   判定为「不可信、要禁网」的设备，反倒借我们的身份获得了更强的伪造能力。
    //   而它要过前面的方向过滤只需把 TCP 标志位带上 ACK，门槛为零。
    const quint32 src = (quint32(f[26]) << 24) | (quint32(f[27]) << 16)
                        | (quint32(f[28]) << 8) | quint32(f[29]);
    const QString claimed = m_victimByMac.value(macKey(f + 6));
    if (claimed.isEmpty() || ipToU32(claimed) != src) {
        ++GatewayDiag::c.dropNonVictim;
        return;
    }
    const uchar proto = f[23];
    bool allow = false;
    if (proto == 6) { // TCP：丢纯 SYN（A 在发起），其余放行
        if (frame.size() >= 14 + ihl + 14) {
            const uchar flags = f[14 + ihl + 13];
            const bool syn = (flags & 0x02) != 0;
            const bool ack = (flags & 0x10) != 0;
            allow = !(syn && !ack);
        }
    } else if (proto == 1) { // ICMP：丢 echo-request(8)，放行 echo-reply(0) 等
        if (frame.size() >= 14 + ihl + 1)
            allow = f[14 + ihl] != 8;
    }
    // UDP(17) 与其余协议：allow 保持 false —— 理由见调用处的论证。
    if (!allow) {
        ++GatewayDiag::c.dropNonVictim; // 复用「被丢弃」计数，避免为此新增一栏
        return;
    }
    const quint32 dst = (quint32(f[30]) << 24) | (quint32(f[31]) << 16)
                        | (quint32(f[32]) << 8) | quint32(f[33]);
    const quint32 dstU = dst;
    const QByteArray peer = n->arp->lanMac(dstU);
    if (peer.size() != 6) {
        // 查不到对端 MAC → 网关**自己主动解析一次**（who-has 的应答单播给我们，收得到），
        // 下一帧就能转发。首帧仍被丢，但 TCP 会重传 —— 与任何一次 ARP 冷启动同理，一个 RTT 的代价。
        //
        // ★ 为什么原来查不到：`learnLanMac` 只从**本机收得到的** ARP 帧学，而对端回给 A 的 reply
        //   是单播给 A 的，交换机不送到我们端口。resolveLanPeer 用本机身份重问一遍，应答就到我们这。
        //
        // ★ **验证到哪一步、诚实交代**：resolveLanPeer 编译通过、逻辑闭环，但我**没能在真机上
        //   端到端证明它闭环**。卡点在上游 —— 隔离抢答对同网段真实主机的竞速本就脆弱：抓包实测
        //   A 广播 who-has <真实主机 .2> 时，**真实 .2 硬件直接回、比我们早 ~77µs**（我们要经
        //   收帧→用户态判定→回帧一整圈），A 的投毒条目因此时有时无，转发路径没能稳定触发。
        //   这不是本函数的问题，是「隔离抢答面对真实主机」这一层的可靠性问题（抢网关能稳，是因为
        //   网关走软件转发、通常更慢）。要让隔离对真实对端也稳，得把抢答做得更激进（更短的连发
        //   窗口 / 收到真主机 reply 立刻反制），那是下一步。此处 resolveLanPeer 是正确的一环，
        //   但整条「B 主动访问 A」的回程要真正可靠，取决于上游抢答先稳下来。
        n->arp->resolveLanPeer(dstU, n->localIp4);
        return;
    }
    QByteArray out = frame;
    std::memcpy(out.data(), peer.constData(), 6);              // 目的 MAC = 对端真实 MAC
    std::memcpy(out.data() + 6, macBytes(n->spec.localMac).constData(), 6); // 源 MAC = 本机
    n->ep->send(out);
}

// 按 IP 找它属于哪张已就绪网卡（同网段判定）。找不到返回 nullptr。
GwNic *GatewayWorker::nicForIp(const QString &ip) const
{
    const quint32 v = ipToU32(ip);
    if (!v)
        return nullptr;
    // 多张网卡可能都命中同一网段(如一台机器有线+WiFi 接同一路由)。**必须确定性地选主网卡**——
    // 否则 QHash 无序遍历可能选到 WiFi 那张,把 ARP 投毒发错网卡(经 WiFi 发的投毒常被 AP 隔离/
    // 到不了有线侧设备),表现为「开了代理但设备没被劫持」。按 order(specs 次序,主网卡=0)取最小者。
    // 真机联调实证:同网段双卡下选错卡 → 投毒无效;选对主卡 → 稳定劫持。
    GwNic *best = nullptr;
    for (GwNic *n : m_nics) {
        if (!n->ready || !n->netMask4 || (v & n->netMask4) != (n->localIp4 & n->netMask4))
            continue;
        if (!best || n->order < best->order)
            best = n;
    }
    return best;
}

// 从设备发出的一帧 IPv6 里学它的可路由地址（全局/ULA），登记成 lwIP 的 nd6 静态邻居 + mihomo 身份。
// v6 地址是被动学来的（不做主动扫描），因为一台设备可能有多个（全局 + 若干隐私地址），且随时间轮换。
// 幂等 + 去重：每个 (mac, v6) 只登记一次；设备换隐私地址会新增一条，旧的留到 disable 时统一摘除。
// 从线上观察到的 Router Advertisement 学 v6 路由器（LL + MAC + on-link 前缀）。
//
// 为什么不能只靠 LanScanner 交上来的 spec：那份是解析**本机**的 `ip -6 route show default` 得到的，
// 而本机拿不拿得到 v6 默认路由取决于本机的 accept_ra。真机实证（这台树莓派网关）：
//   net.ipv6.conf.eth0.accept_ra = 0  → 本机永远没有 v6 默认路由 → routerLinkLocal6 恒空
//   → NdpSpoofer no-op → 被劫持设备照样通过 RA 拿到全局 v6 并直连出网，**整段绕过代理且无声**。
// 而 RA 本来就流经这条抓包链路（混杂模式 + BPF 放行全部 ICMPv6，本是给反制解毒用的），
// 直接从中学最可靠：它反映的是**链路上的事实**，与本机怎么配无关。
//
// 幂等 + 只在有变化时才动：RA 是周期广播（几十秒到几分钟一次），不能每来一条就重配一次投毒器。
void GatewayWorker::learnRouterFromRa(GwNic *n, const QByteArray &frame)
{
    if (!n || !n->ndp)
        return;
    QString ll, mac;
    QByteArray pfx;
    int pfxLen = -1;
    if (!NdpSpoofer::parseRouterAdvert(frame, &ll, &mac, &pfx, &pfxLen))
        return; // 不是可用的 RA（hop limit≠255 / lifetime==0 / 源地址非 LL 等，判据见解析函数）
    if (ll.isEmpty() || mac.isEmpty())
        return;

    // 记进 learned*（**不写 spec**：spec 每轮被扫描结果覆盖，见 GwNic 里的说明）。
    if (pfxLen > 0 && pfx.size() == 16) {
        n->learnedPrefix6Net = pfx;
        n->learnedPrefix6Len = pfxLen;
        if (n->prefix6Len < 0) { // 扫描没给前缀 → 这条立刻生效（v6 LAN 内直连旁路要用）
            n->prefix6Net = pfx;
            n->prefix6Len = pfxLen;
            qInfo() << "网关: 从 RA 学到 v6 前缀" << n->spec.ifname << pfxLen;
        }
    }

    // 取用口径没变 → 到此为止。绝大多数 RA（周期广播）走这条，零动作、不重配、不刷日志。
    const QString beforeLL = n->effectiveRouterLL6();
    const QString beforeMac = n->effectiveRouterMac6();
    n->learnedRouterLL6 = ll;
    n->learnedRouterMac6 = mac;
    const QString nowLL = n->effectiveRouterLL6();
    const QString nowMac = n->effectiveRouterMac6();
    if (beforeLL.compare(nowLL, Qt::CaseInsensitive) == 0
        && beforeMac.compare(nowMac, Qt::CaseInsensitive) == 0)
        return;

    const bool wasUnconfigured = beforeLL.isEmpty();
    n->ndp->configure(n->spec.localMac, nowLL, nowMac);
    qInfo() << "网关: 从 RA 学到 v6 路由器" << n->spec.ifname << nowLL << nowMac
            << (wasUnconfigured ? "(本机无 v6 默认路由，此前 v6 劫持一直是停用的)"
                                : "(拓扑变化，已重配)");

    // ★ 补投：学到之前 startSpoof 是被忽略的（NdpSpoofer 未配置时直接 return + 告警），
    //   所以这张卡上已经在劫持的设备必须在这里重新挂上 v6 投毒，否则要等到下次 enableDevice 才生效。
    for (auto it = m_victimMacStr.constBegin(); it != m_victimMacStr.constEnd(); ++it) {
        if (m_victimNic.value(it.key()) == n->spec.ifname)
            n->ndp->startSpoof(it.value());
    }
    // 拓扑变了要重写崩溃恢复清单，否则异常退出后按旧（空）拓扑还原不了 v6。
    persist();
}

// 从被劫持设备自己的 v4 帧里现学它当前的地址。
//
// ★ **为什么必须有它**：投毒帧是**按 victim 的 IP 单播**发出去的（见 ArpSpoofer::startSpoof），
//   而那个 IP 来自台账。设备换地址（DHCP 续约、断线重连、换网段）之后，台账要等一轮**全量
//   局域网扫描**才可能更新 —— 而扫描能不能发现新地址，又取决于本机 ARP 表里恰好有它。
//   真机实测：第一次换地址约 110s 后才跟上；第二次**过了 120s 仍没跟上**，本机 ARP 表里
//   压根没有新地址，设备侧的网关表项已经消失、成功率掉到 67.6%。
//   期间界面依然显示「已代理」—— 又一个「说在代理其实没有」的场景。
//
//   而我们其实**每一帧都看得到设备的源 IP**：数据面本来就是按 **MAC** 认设备的
//   （m_victimByMac），源 IP 就在帧里。所以根本不必等扫描，现学即可 —— 与上面 v6 那套
//   （learnDeviceV6）完全同一个思路。
//
// 判据从严，避免把一台设备的劫持挂到不相干的地址上：
//   · 必须是这张卡上**已在劫持**的 MAC（m_victimByMac 命中）；
//   · 新地址必须与旧地址**同一子网**（换网段属于重新入网，交给正常的发现流程，不在这里猜）；
//   · 0.0.0.0（DHCP DISCOVER 的源）与组播/广播源一律忽略。
// 命中就发信号，由上层去更新台账 + 按新地址重挂（拆旧的、上新的），本函数不直接动投毒器 ——
// 劫持的启停归 DevicesController 编排，这里只负责「看见了」。
void GatewayWorker::learnDeviceV4(GwNic *n, const uchar *f, const QByteArray &frame)
{
    if (frame.size() < 34)
        return;
    const quint16 ethType = (quint16(f[12]) << 8) | f[13];
    if (ethType != 0x0800)
        return;
    const quint32 src = (quint32(f[26]) << 24) | (quint32(f[27]) << 16)
                        | (quint32(f[28]) << 8) | quint32(f[29]);
    if (src == 0 || (src >> 28) == 0xE || src == 0xFFFFFFFFu)
        return; // 0.0.0.0 / 组播 / 广播源：不是设备的真实地址

    const quint64 vkey = macKey(f + 6);
    auto it = m_victimByMac.constFind(vkey);
    if (it == m_victimByMac.constEnd())
        return; // 不是我们在劫持的设备
    const QString oldIp = it.value();
    const QString newIp = QHostAddress(src).toString();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (newIp == oldIp) {
        m_victimIpSeenMs[vkey] = nowMs;   // 台账上那个地址仍在使用
        return;
    }
    // ★ **一个 MAC 上挂多个 IPv4 是常态，不能当成「换地址」**（静态 IP + DHCP、虚拟机宿主、
    //   Docker 网桥、临时别名都会这样）。没有这道判据时两个地址会轮流把台账改来改去，
    //   而每改一次上层都要按新地址**重挂一次接管**，在途连接跟着被打断。
    //   真机实测（Pi 网关、四设备并发）：给 ens18 加一个同 MAC 的第二 IP，该设备的
    //   p99 立刻从 **99ms 跳到 2518ms**、速率掉掉一半；把第二个 IP 删掉，同一份二进制
    //   立刻回到 99ms。这个现象被我当成「饱和下的公平性问题」追了好几轮，其实是这里。
    //
    //   判据用「旧地址是不是还在发包」而不是「有没有见过两个 IP」：真的换地址时旧地址
    //   必然不再出现（DHCP 续约到新址、重新入网），静默期一过自然就认了；而多 IP 设备的
    //   两个地址都在持续发包，永远过不了这道门 —— 正是我们想要的。
    //   3 秒：远大于正常设备的发包间隔（ARP/DNS/keepalive 都是秒级以内），
    //   又远小于用户对「换了地址多久能恢复」的容忍度。
    constexpr qint64 kIpMoveQuietMs = 3000;
    const qint64 lastSeenOld = m_victimIpSeenMs.value(vkey, 0);
    if (lastSeenOld != 0 && nowMs - lastSeenOld < kIpMoveQuietMs)
        return; // 旧地址还活着 → 这是同一台设备的另一个地址，不是搬家
    // 同子网才认（换网段不在这里猜）
    if (n->netMask4 != 0) {
        const quint32 oldRaw = QHostAddress(oldIp).toIPv4Address();
        if ((oldRaw & n->netMask4) != (src & n->netMask4))
            return;
    }
    // ★ 这个地址已经属于**另一台**正在被劫持的设备 → 一律不认。
    //   两种现实来源：① 某台设备的旧地址被 DHCP 分给了别人，两边短时都在用；
    //   ② 链路上出现了源 MAC 与源 IP 不一致的帧（网桥/虚拟网卡的转发、或伪造）。
    //   认了的话，我们会把 A 的劫持改挂到 B 的地址上 —— 轻则两台一起失灵，重则去投毒
    //   一台根本没开代理的机器。宁可这一次不学，等它自己再发一帧（正常设备每秒都在发）。
    //   真机就撞到过：rig 里有流量从另一张网卡出去，日志立刻出现
    //   「device 02:…:02 v4 changed .202 -> .231」这种张冠李戴。
    if (m_victimMacStr.contains(newIp))
        return;
    // ★ MAC 串**直接从帧里格式化**，别拿 oldIp 去 `m_victimMacStr` 反查 —— 那张表是 ip→mac，
    //   多设备时不同设备的记录会互相盖，反查回来的可能是**另一台设备**的 MAC，
    //   于是台账被改到不相干的设备上。第一版就这么写的，真机日志立刻露馅：
    //   「device v4 changed 192.168.20.202 -> 192.168.20.239 / -> .203」—— old 恒为某一台、
    //   new 却是别人的地址。帧里就有源 MAC，没有任何理由绕一圈。
    const uchar *sm = f + 6;
    const QString macStr = QString::asprintf("%02x:%02x:%02x:%02x:%02x:%02x",
                                             sm[0], sm[1], sm[2], sm[3], sm[4], sm[5]);
    if (gwDbgOn())
        std::fprintf(stderr, "[GW] device %s v4 changed %s -> %s\n", macStr.toUtf8().constData(),
                     oldIp.toUtf8().constData(), newIp.toUtf8().constData()),
            std::fflush(stderr);
    emit deviceIpChanged(macStr, newIp);
}

void GatewayWorker::learnDeviceV6(GwNic *n, const uchar *f, const QByteArray &frame)
{
    // tproxy 模式下**故意**不学 v6：现在的 nft 规则只覆盖 IPv4（见 TproxyRules），学了也没处用。
    // 等 v6 规则补上之后，这里要改成「学到就同步进 nft 的 v6 集合」，而不是继续沿用 m_net 判据。
    if (m_datapath.tproxy || !m_net || frame.size() < 14 + 40)
        return;
    const uchar *src = f + 14 + 8; // IPv6 源地址
    if (!ip6SrcIsRoutable(src))
        return;
    const quint64 vkey = macKey(f + 6);
    QSet<QString> &set = m_victimV6ByMac[vkey];
    // 上限兜底：隐私地址轮换极端情况下别让一台设备把 nd6 表（64 条）吃光。到顶就不再学新的。
    if (set.size() >= 16)
        return;
    Q_IPV6ADDR raw;
    std::memcpy(raw.c, src, 16);
    const QString ip6 = QHostAddress(raw).toString();
    if (set.contains(ip6))
        return;
    const QByteArray mb(reinterpret_cast<const char *>(f + 6), 6);
    // ★ reject 必须一起带上：这条是**运行时现学**的 v6 注册点，漏传就走默认 false ——
    //   一台设为「禁网」的设备，它的 v6 地址是这里学到的，注册时丢了 reject，
    //   NetStack 的 handleUdpFrame6 就查不到 dev.reject → **该设备的 IPv6 UDP 不被拦截**，
    //   「禁网」在 v6 上静默失效。而这条路径只在设备真发 v6 流量时才走，真机极难覆盖到。
    if (!m_net) // tproxy 模式没有用户态栈：设备身份由原始源 IP 承载,不需要在这里登记
        return;
    if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
        m_net->addDeviceV6(n->ep, ip6, mb, m_victimUserByMac.value(vkey),
                           m_isolatedMacs.contains(vkey));
    }
    if (n->watch) {
        const QString macStr = QStringLiteral("%1:%2:%3:%4:%5:%6")
                                   .arg(mb[0] & 0xFF, 2, 16, QChar('0')).arg(mb[1] & 0xFF, 2, 16, QChar('0'))
                                   .arg(mb[2] & 0xFF, 2, 16, QChar('0')).arg(mb[3] & 0xFF, 2, 16, QChar('0'))
                                   .arg(mb[4] & 0xFF, 2, 16, QChar('0')).arg(mb[5] & 0xFF, 2, 16, QChar('0'));
        n->watch->addVictimV6(macStr, ip6); // 也让监视器能检测「这台设备的 v6 被别人投毒」
    }
    set.insert(ip6);
    if (gwDbgOn())
        std::fprintf(stderr, "[GW] learned device v6 %s\n", ip6.toLatin1().constData()),
            std::fflush(stderr);
}

bool GatewayWorker::availableLocal() const
{
    // 「网关可用」= 数据面就绪 + 至少一张卡的二层端点开着。
    // 数据面就绪按模式分别判：lwIP 看用户态栈，tproxy 看内核规则装没装。
    // ★ 这里原本也只写 `!m_net`——与 enableDeviceLocal 里那处是同一个坑的两半：
    //   光修那一处没用,这条总闸照样会把 tproxy 模式判成"不可用"（UI 上也会显示网关不可用）。
    if (m_datapath.tproxy ? !m_tproxy.isInstalled() : !m_net)
        return false;
    for (GwNic *n : m_nics) {
        if (n->ready && n->ep && n->ep->isOpen())
            return true;
    }
    return false;
}

QString GatewayWorker::statePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return QDir(dir).filePath("gateway_active.json");
}

// 每条 victim 自带它那张卡的还原所需信息：多网卡下不能再靠一份全局 ifname/gateway。
void GatewayWorker::persist() const
{
    QJsonArray arr;
    for (auto it = m_victimMacStr.constBegin(); it != m_victimMacStr.constEnd(); ++it) {
        const QString ip = it.key();
        QJsonObject o;
        o["ip"] = ip;
        o["mac"] = it.value();
        const QString ifn = m_victimNic.value(ip);
        if (GwNic *n = m_nics.value(ifn)) {
            o["ifname"] = n->spec.ifname;
            o["localMac"] = n->spec.localMac;
            o["gatewayIp"] = n->spec.gatewayIp;
            o["gatewayMac"] = n->spec.gatewayMac;
            // v6 崩溃恢复：还原 NDP 要用。**写 effective 而不是 spec** —— 本机没有 v6 默认路由
            // 时 spec 恒空，只有从 RA 学到的那份才是真的；写空的话崩溃后就还原不了 v6 邻居缓存，
            // 被劫持设备的 v6 会一直指着本机（断网）直到表项老化。
            o["routerLL6"] = n->effectiveRouterLL6();
            o["routerMac6"] = n->effectiveRouterMac6();
        }
        arr.append(o);
    }
    // 还没还原成功的遗留记录一并写回：它们已经不在 m_victimMacStr 里，但设备的 ARP 仍指着本机，
    // 漏掉的话这次再被杀就彻底失联了（本进程还没来得及还原就没人知道要还原谁）。
    for (const QJsonObject &o : m_pendingHeal)
        arr.append(o);
    QJsonObject root;
    root["victims"] = arr;
    QFile f(statePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

void GatewayWorker::saveState() const
{
    if (m_victimMacStr.isEmpty() && m_pendingHeal.isEmpty())
        clearState();
    else
        persist();
}

void GatewayWorker::publishSnapshot()
{
    bool available = false;
    QVector<GwSubnet> subnets;
    for (GwNic *n : m_nics) {
        if (n->ready && n->ep && n->ep->isOpen()) {
            available = true;
            if (n->netMask4)
                subnets.append(GwSubnet{n->localIp4, n->netMask4});
        }
    }
    const QStringList active = m_victimMacStr.values();
    QMutexLocker lk(&m_shared->mutex);
    m_shared->available = available;
    m_shared->subnets = subnets;
    m_shared->active = active;
}

// 重建「本机自己的地址」集合。**必须枚举所有网卡**，不能只看我们接管的那几张：
// 会撞上这个问题的恰恰是没被接管的第二块网卡（无线、docker0、VPN tun、veth…）。
void GatewayWorker::refreshLocalAddrs()
{
    m_localIp4.clear();
    m_localIp6.clear();
    const auto all = QNetworkInterface::allAddresses();
    for (const QHostAddress &a : all) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol) {
            m_localIp4.insert(a.toIPv4Address());
        } else if (a.protocol() == QAbstractSocket::IPv6Protocol) {
            const Q_IPV6ADDR v6 = a.toIPv6Address();
            std::array<quint8, 16> x{};
            std::memcpy(x.data(), &v6, 16);
            m_localIp6.append(x);
        }
    }
}

void GatewayWorker::configureLocal(const QVector<LanGateway::NicSpec> &specs, quint16 socksPort)
{
    refreshLocalAddrs(); // 网卡/地址可能刚变（DHCP 续约、插拔、VPN 起落），每轮重建
    m_socksPort = socksPort;

    QString err;
    if (m_datapath.tproxy) {
        // TPROXY 数据面：**不建 NetStack**。规则装不上就必须原地失败并让上层退回 lwIP——
        // 半开着（规则装了一半 / 装了但核心没起监听）等于把被覆盖的设备**整台断网**,
        // 那比"这条新路暂时用不了"糟糕得多。
        if (!m_tproxy.isInstalled()) {
            TproxyRules::Spec ts;
            ts.tproxyPort = m_datapath.tproxyPort;
            ts.dnsPort = m_datapath.dnsPort;
            if (!m_tproxy.install(ts, &err)) {
                emit deviceError(QString(),
                                 QStringLiteral("TPROXY 规则装载失败（已保持未接管状态）: ") + err);
                return;
            }
        }
        syncTproxyDevices();
    } else if (!m_net) {
        // 协议栈是共用的，先起来（lwIP 单实例；每张卡随后各挂一个 netif）。都在工作线程上创建。
        m_net = new NetStack(socksPort, this);
        if (!m_net->init(&err)) {
            emit deviceError(QString(), QStringLiteral("协议栈初始化失败: ") + err);
            delete m_net;
            m_net = nullptr;
            return;
        }
    }

    QSet<QString> seen;
    int specIndex = -1;
    for (const LanGateway::NicSpec &spec : specs) {
        ++specIndex; // specs 的次序即优先级：LanScanner.physicalNics() 把主网卡(带默认路由的那张)
                     // 放在 index 0。多张同网段网卡时 nicForIp 据此选主卡。
        if (spec.ifname.isEmpty() || spec.localIp.isEmpty() || spec.localMac.isEmpty())
            continue;
        seen.insert(spec.ifname);
        GwNic *n = m_nics.value(spec.ifname);
        if (!n) {
            n = new GwNic;
            n->spec = spec;
            m_nics.insert(spec.ifname, n);
        }
        n->order = specIndex;
        // 拓扑数值每次都刷新（网关 MAC 常常是扫描几轮后才解析出来的）。
        n->spec = spec;
        n->localIp4 = ipToU32(spec.localIp);
        n->gatewayIp4 = ipToU32(spec.gatewayIp);
        n->netMask4 = ipToU32(spec.netmask);
        n->prefix6Net.clear();
        n->prefix6Len = -1;
        if (!spec.prefix6.isEmpty())
            parseV6Prefix(spec.prefix6, &n->prefix6Net, &n->prefix6Len); // 失败则保持未知（不旁路）
        if (n->prefix6Len < 0 && n->learnedPrefix6Len > 0) {
            n->prefix6Net = n->learnedPrefix6Net; // 扫描没给 → 用从 RA 学到的（别被本轮清空抹掉）
            n->prefix6Len = n->learnedPrefix6Len;
        }
        if (n->arp)
            n->arp->configure(spec.localMac, spec.gatewayIp, spec.gatewayMac);
        // v6 投毒器同样每轮刷新拓扑（路由器 LL/MAC 常是扫描几轮后才解析出来）。
        // 取用走 effectiveRouterLL6/Mac6：扫描拿不到时用从线上 RA 学到的，两者都没有才 no-op
        //（v6 劫持不生效，但绝不影响 v4）。
        if (n->ndp)
            n->ndp->configure(spec.localMac, n->effectiveRouterLL6(), n->effectiveRouterMac6());
        // 监视器的真相表同样每轮刷新（网关/路由器 MAC 常是扫描几轮后才解析出来）。
        if (n->watch) {
            n->watch->setLocal(spec.localMac, spec.localIp, spec.localGlobal6);
            n->watch->setGateway(spec.gatewayIp, spec.gatewayMac, n->effectiveRouterLL6(),
                                 n->effectiveRouterMac6());
        }

        if (n->ready)
            continue; // 已就绪：只刷新拓扑，不重建（重建会断掉这张卡上的活动劫持）
        if (!n->netMask4) {
            // 没有有效掩码就没法给 netif 定路由，也没法做同网段旁路——这张卡直接不启用。
            continue;
        }
        if (!n->ep) {
            n->ep = createL2Endpoint(this); // parent=worker → 端点及其通知器都在工作线程上
            if (!n->ep)
                break; // 平台不支持，后面几张也没必要试（仍要走下面的摘卡清理）
        }
        // ★ open() 在工作线程执行：QSocketNotifier/QWinEventNotifier 因此在工作线程创建、也在工作
        //   线程被服务（Qt 硬性要求通知器与服务它的线程同线程）。这正是把 configure 投到工作线程的
        //   根本原因之一。
        if (!n->ep->isOpen() && !n->ep->open(spec.ifname, &err)) {
            emit deviceError(QString(),
                             QStringLiteral("打开网卡失败(%1): ").arg(spec.ifname) + err);
            continue;
        }
        // ★ m_net 判空必须在**最前面**：tproxy 模式下它恒为空，而 && 是从左到右求值的。
        //   我第一版写成 `!m_net->hasNic(...) && m_net && ...`，hasNic 先解引用了空指针 → SIGSEGV。
        if (m_net && !m_net->hasNic(n->ep)
            && !m_net->addNic(n->ep, n->ep->localMac(), spec.localIp, spec.netmask, &err)) {
            emit deviceError(QString(),
                             QStringLiteral("协议栈挂载网卡失败(%1): ").arg(spec.ifname) + err);
            continue;
        }
        // 二层帧过滤：只把被劫持设备发来的帧喂进用户态栈（按这张卡的网段做旁路判断）。
        // context = this(worker)，sender = n->ep 也在工作线程 → 直连，lambda 在工作线程上跑。
        // ★ 这条「必须直连」不只是性能取向，是**正确性硬约束**：Linux 端点用 TPACKET_v3 收环，
        //   frame 是 QByteArray::fromRawData 指向 mmap 环内存的视图，槽一返回块就还给内核。
        //   一旦这里变成队列连接（改 connect 类型、或把端点/worker 拆到两个线程），Qt 会把这个
        //   「视图」拷进事件队列，等真正派发时那块内存早被新帧覆写 ⇒ 悬垂读。详见
        //   IL2Endpoint::frameReceived 的注释。下面这个 lambda 本身也只在槽内读 frame，不留存。
        connect(n->ep, &IL2Endpoint::frameReceived, this, [this, n](const QByteArray &frame) {
            if (frame.size() < 12)
                return;
            const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
            const bool isArp = frame.size() >= 14 && f[12] == 0x08 && f[13] == 0x06;
            // 被动安全监视：在任何 return/旁路之前先看一眼这帧的 ARP/NDP 声明。它只读、内部去抖，
            // 既看得到真网关广播、也看得到冲我而来的毒帧（本机自己发的帧按 sender MAC 被它排掉）。
            if (n->watch)
                n->watch->observe(frame);
            // ★ 反应式反制:真网关自己发的 ARP(它周期性广播 who-has,携带「网关 IP 在真 MAC」,
            //   设备一收到就把投毒解掉)→ 立刻把所有 victim 重投一轮盖回去。这是「时通时不通」的
            //   根治:周期重发跟网关的 ARP 刷新是 1:1 拉锯,必须一看到解毒就抢回(tcpdump 实证 UniFi
            //   每 ~1s 广播一次 who-has victim tell 网关IP)。过滤器已放行 ARP,所以这里收得到。
            if (isArp && n->arp) {
                const QByteArray gwMac = macBytes(n->spec.gatewayMac);
                if (gwMac.size() == 6 && std::memcmp(f + 6, gwMac.constData(), 6) == 0) {
                    n->arp->reassertNow();
                    if (gwDbgOn() && (m_dbgReassert++ % 20) == 0)
                        std::fprintf(stderr, "[GW] reassert on gateway ARP (count=%lld)\n",
                                     m_dbgReassert),
                            std::fflush(stderr);
                    return; // 网关帧不是设备流量,不喂 lwIP
                }
            }
            // v6 反制（与上面 ARP 反制对称）：真路由器自己发的 NA/RA 会把设备的 v6 邻居缓存解毒 →
            // 立刻把所有 victim 重投一轮盖回。路由器 MAC 优先用 routerMac6，缺则回落到 v4 网关 MAC
            //（同一台路由器 v4/v6 通常共用一个物理 NIC MAC）。它不是 victim 帧，必须在 victim 过滤前处理。
            // ★ 先学再反制：RA 是「链路上到底有没有 v6 路由器」的**第一手事实**，而本机的
            //   `ip -6 route` 只是二手（还受本机 accept_ra 影响，真机上就是 0）。这一步必须在
            //   下面的「源 MAC 是不是已知路由器」判断**之前**——不然在还没学到路由器 MAC 时，
            //   RA 会被那个判断挡掉，永远学不到，成死锁。
            if (NdpSpoofer::isRouterAdvert(frame))
                learnRouterFromRa(n, frame);
            if (n->ndp && NdpSpoofer::isRouterAdvertOrNa(frame)) {
                const QByteArray rmac = macBytes(
                    n->spec.routerMac6.isEmpty() ? n->spec.gatewayMac : n->spec.routerMac6);
                if (rmac.size() == 6 && std::memcmp(f + 6, rmac.constData(), 6) == 0) {
                    n->ndp->reassertNow();
                    return; // 路由器帧不是设备流量，不喂 lwIP
                }
            }
            // 源 MAC 原地打包成 quint64 查表：不做 frame.mid(6, 6)，省掉每帧一次堆分配。
            const quint64 vkey = macKey(f + 6);
            if (!m_victimByMac.contains(vkey)) {
                // 帧到了这一层但源 MAC 不在被劫持名单里 → 这里丢弃。若「设备流量为 0」且这个
                // 计数在涨，说明设备的帧收到了、但它的源 MAC 和台账里的 MAC 对不上（随机 MAC /
                // 台账 MAC 有误），是 victim 匹配的问题，不是抓包的问题。
                ++GatewayDiag::c.dropNonVictim;
                if (gwDbgOn() && (m_dbgDropNonVictim++ % 200) == 0)
                    std::fprintf(stderr, "[GW] drop non-victim src=%02x:%02x:%02x:%02x:%02x:%02x "
                                         "(count=%lld)\n",
                                 f[6], f[7], f[8], f[9], f[10], f[11], m_dbgDropNonVictim),
                        std::fflush(stderr);
                return;
            }
            // 唤醒沿检测（item 2/3）：某 victim 空闲 >kWakeIdleMs 后又发帧 —— 它的邻居缓存多半刚老化、
            // 正在/即将重新解析网关。立刻让 ArpSpoofer 进高频重投窗口，抢在真网关的解析应答前把「网关
            // 在本机」钉回去，治「空闲后首次访问先失败、再走真路由、再代理」的过渡。该 victim 的 who-has
            // 广播源 MAC 也走这条路（在下面 answerGatewayArp 之前），所以主动解析场景一并覆盖。
            // 每帧成本：一次时钟读 + 一次 QHash 查改；startBoost 只在稀有的唤醒沿才真正调用。
            {
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                qint64 &last = m_lastSeenMs[vkey];
                if (last != 0 && nowMs - last > kWakeIdleMs) {
                    if (n->arp)
                        n->arp->startBoost();
                    if (n->ndp)
                        n->ndp->startBoost(); // v6 邻居缓存唤醒沿同样要抢在真路由器前钉回
                    if (gwDbgOn())
                        std::fprintf(stderr, "[GW] wake edge (idle %llds) -> boost\n",
                                     static_cast<long long>((nowMs - last) / 1000)),
                            std::fflush(stderr);
                }
                last = nowMs;
            }
            // ———————————————— IPv6 数据路径（与下面 v4 的抢答/旁路/喂栈一一对应）————————————————
            // 走到这里 = 帧确来自被劫持设备。若是 v6 帧，整条 v6 逻辑在此闭环并 return，不落到 v4 代码。
            {
                const bool isV6 = frame.size() >= 14 && f[12] == 0x86 && f[13] == 0xDD;
                if (isV6) {
                    const uchar *p6 = f + 14;
                    const int ip6Len = int(frame.size()) - 14;
                    // NDP（NS/NA/RS/RA）：投毒器接管，绝不喂 lwIP（避免 lwIP 自己应答邻居发现）。
                    // 设备主动问「路由器 LL 在哪」→ 立刻抢答（对应 v4 的 answerGatewayArp）。
                    if (ip6IsNdp(p6, ip6Len)) {
                        if (ip6IsNs(p6, ip6Len) && n->ndp)
                            n->ndp->answerNeighborSolicit(frame);
                        return;
                    }
                    // 学习设备的可路由 v6 地址 → 登记回程静态邻居 + mihomo 身份（幂等去重）。
                    learnDeviceV6(n, f, frame);
                    // 旁路：链路本地/组播目的不出网（本地链路通信 / 残余 NDP），不喂用户态栈——
                    // 与 v4「同网段直连旁路」同精神：只有真出网（全局单播目的）才进 lwIP。
                    if (ip6DstIsLocalOrMulticast(p6, ip6Len))
                        return;
                    // 同前缀（同一 /64 内的另一台 LAN 设备的全局地址）也旁路——否则本机会把设备间的
                    // LAN 内 v6 直连也终结掉。仅在前缀已知时生效；未知时无法判定，只能放行进栈
                    //（记录在案：prefix6 发现失败的少见情况下，全局-到-全局的 LAN 内 v6 会被误代理）。
                    if (n->prefix6Len >= 0 && ip6Len >= 40
                        && ip6InPrefix(p6 + 24, n->prefix6Net, n->prefix6Len))
                        return;
                    // 本机自己的 v6 地址（任意网卡、任意前缀）——与上面 v4 那条同因同解：
                    // 内核已经收过一份，再进 lwIP 就是两套栈抢答。上面「同前缀」只挡得住本卡这一段，
                    // 挡不住无线/VPN 上那些属于**别的**前缀的本机全局地址。
                    if (ip6Len >= 40 && isLocalIp6(p6 + 24))
                        return;
                    ++GatewayDiag::c.fedLwip;
                    if (gwDbgOn() && (m_dbgFedLwip++ % 100) == 0)
                        std::fprintf(stderr, "[GW] -> lwIP fed(v6)=%lld\n", m_dbgFedLwip),
                            std::fflush(stderr);
                    if (m_net) // tproxy 模式没有用户态栈，这里恒为空
                        m_net->inputFrame(n->ep, frame);
                    return;
                }
            }
            // 设备换 IP（DHCP 续约/重连）时从它自己的帧里现学 —— 与上面的 v6 现学同精神。
            learnDeviceV4(n, f, frame);
            // ARP 抢答：被劫持设备一问「谁是网关?」就同步回「网关在本机 MAC」，赶在真网关前面。
            // 与上面的网关侧反制互补：这条管设备主动问的场景,上面那条管网关主动广播的场景。
            // 抢答后仍把该 ARP 喂给 lwIP（它据此维护设备 MAC 映射，无副作用）。
            if (isArp && n->arp) {
                // 顺手学一条「局域网 IP → 真实 MAC」：ARP 的 sender 字段就是现成的真相，
                // 隔离转发与撤销还原都要用它。
                if (frame.size() >= 42) {
                    const quint32 spa = (quint32(f[28]) << 24) | (quint32(f[29]) << 16)
                                        | (quint32(f[30]) << 8) | quint32(f[31]);
                    n->arp->learnLanMac(spa, QByteArray(reinterpret_cast<const char *>(f + 22), 6));
                }
                // 局域网隔离：被禁设备问「同网段的 X 在哪」→ 抢答本机，把它去 LAN 的流量引到
                // 我们手上（放不放由上面的方向过滤决定）。详见 ArpSpoofer 里的论证。
                n->arp->answerIsolationArp(frame, n->localIp4, n->netMask4, n->localIp4,
                                           n->gatewayIp4);
                // 反制真实主机对被隔离目标的抢先 reply（收得到就立刻盖回）——隔离对真实对端能否
                // 稳的关键。放在 answerIsolationArp 之后：request 归它、reply 归这条。
                n->arp->counterIsolationReply(frame);
                if (n->arp->answerGatewayArp(frame) && gwDbgOn()
                    && (m_dbgArpAnswered++ % 50) == 0)
                    std::fprintf(stderr, "[GW] answered gateway ARP (count=%lld)\n",
                                 m_dbgArpAnswered),
                        std::fflush(stderr);
            }
            // 同网段直连旁路：被劫持设备发往「本网段内（且非网关本身）」的 IPv4 帧不喂用户态栈，
            // 让它照常二层直达——设备回给本机 LAN IP 的包若被 lwIP 终结会触发 RST，导致本机无法
            // 直连该设备（SSH/网页/共享）；同理 LAN 内设备互访也不该被代理绕行。只有真正出网
            // （目的在子网外）或发往网关 IP（DNS/路由器后台等，其 ARP 被投毒必须由我们接管）的帧进栈。
            if (n->netMask4 != 0 && frame.size() >= 34) {
                const quint16 ethType = (quint16(f[12]) << 8) | f[13];
                if (ethType == 0x0800) { // IPv4
                    const quint32 dst = (quint32(f[30]) << 24) | (quint32(f[31]) << 16)
                                        | (quint32(f[32]) << 8) | quint32(f[33]);
                    // 广播 / 组播旁路（v6 侧早就有对应的 ip6DstIsLocalOrMulticast，v4 一直漏着）：
                    //   255.255.255.255（DHCP 续租、部分 SSDP/NetBIOS）与 224.0.0.0/4
                    //   （mDNS 224.0.0.251、SSDP 239.255.255.250、LLMNR、IGMP…）
                    // 都不是「出网」流量，代理不了也不该代理。以前它们会一路喂进 NetStack：
                    //   · 每个源端口建一条 UdpFlow → 一次 SOCKS5 UDP ASSOCIATE（含一条到 mihomo 的
                    //     TCP 控制连接），而每设备只有 kMaxUdpFlowsPerDevice=128 个名额 —— 一台爱
                    //     嚷嚷的设备光靠 LAN 发现就能把名额挤掉，把正在用的 QUIC 流顶出去；
                    //   · 对 policy=global 的设备还会命中 IN-USER 规则，把 LAN 发现包**发到代理节点**上。
                    // 这些帧本身是泛洪的、真交换机照样送到该去的地方，我们只是别再多此一举。
                    const bool bcastOrMcast = (dst == 0xFFFFFFFFu) || ((dst >> 28) == 0xE);
                    if (bcastOrMcast) {
                        ++GatewayDiag::c.bypassBcast;
                        if (gwDbgOn() && (m_dbgBypassLan++ % 200) == 0)
                            std::fprintf(stderr,
                                         "[GW] bypass bcast/mcast dst=%u.%u.%u.%u (count=%lld)\n",
                                         f[30], f[31], f[32], f[33], m_dbgBypassLan),
                                std::fflush(stderr);
                        return;
                    }
                    const bool sameSubnet = (dst & n->netMask4) == (n->localIp4 & n->netMask4);
                    // ★ 本机地址旁路（跨网段的那一档）。
                    //   下面同网段那条旁路的理由是「设备发回本机 LAN IP 的包若被 lwIP 终结会触发
                    //   RST」——但**本机在别的网段上的地址一样会踩到它**，而那一档一直漏着：
                    //   网关机器上的第二块网卡（无线）、docker0、VPN tun、veth… 任何一个都算。
                    //   这类帧的 dst MAC 是我们、dst IP 也是我们，宿主内核本来就会正确收下并处理；
                    //   我们再喂进 lwIP 就成了**两套栈同时应答**同一个连接。真机实测（树莓派同时接
                    //   有线 eth0 192.168.20.91 与无线 wlan0 192.168.31.66，另有 veth-h 10.99.0.1）：
                    //     · 被接管的设备 ping 10.99.0.1 / 192.168.31.66 → 每个请求收到 2 个应答
                    //       （ping 报 "+2 duplicates"）；同网段的 192.168.20.91 无重复（已被旁路）。
                    //     · TCP 更糟：三次握手能成，发出请求后直接 RST（"Connection reset by peer"），
                    //       且核心侧连一条日志都没有——因为压根不是核心的问题。
                    //   注意帧到达我们这里时**宿主内核已经收到过一份**（AF_PACKET 是旁路复制，原帧
                    //   照常进协议栈），所以旁路不会放宽任何访问权限：能不能访问由内核决定，与这里
                    //   放不放行无关。对 policy=reject 的隔离设备同理——它本来就拦不住内核那一份。
                    if (!sameSubnet && m_localIp4.contains(dst)) {
                        ++GatewayDiag::c.bypassLan;
                        return;
                    }
                    // ★ 局域网隔离（policy=reject 的设备）：按**方向**决定放不放。
                    //   依据是「A 主动发起的第一个包有可识别特征」：
                    //     · TCP 纯 SYN（有 SYN 无 ACK）= A 在发起 → 丢。既然纯 SYN 一律丢，
                    //       后续任何已建立连接**必然是 B 发起的**，放行安全 —— 不需要状态表。
                    //     · ICMP echo-request = A 主动探测 → 丢；echo-reply 放行（B 能 ping 通 A）。
                    //     · UDP 没有方向标记，而 B→A 的包在交换网络里我们根本看不见（交换机只送到
                    //       B 和 A 的端口），无从做有状态判断 → **全丢**。代价是 B 也用不了 A 上的
                    //       UDP 服务（mDNS/SSDP 之类）；在「A 不能主动做任何事」的要求下这是唯一
                    //       安全的取法。
                    //   放行的帧必须改写目的 MAC 为对端真实 MAC：设备是被我们骗了才发到本机的。
                    if (sameSubnet && dst != n->gatewayIp4 && isIsolatedMac(f + 6)) {
                        forwardIsolatedLan(n, frame, f);
                        return;
                    }
                    if (sameSubnet && dst != n->gatewayIp4) {
                        ++GatewayDiag::c.bypassLan;
                        // 同网段直连,放行给系统,不进 lwIP。若设备只访问 LAN、没真出网,这里会涨。
                        if (gwDbgOn() && (m_dbgBypassLan++ % 200) == 0)
                            std::fprintf(stderr, "[GW] bypass LAN dst=%u.%u.%u.%u (count=%lld)\n",
                                         f[30], f[31], f[32], f[33], m_dbgBypassLan),
                                std::fflush(stderr);
                        return;
                    }
                }
            }
            // 走到这里 = 真出网 / 发往网关的帧 → 喂进用户态栈。这个计数在涨却仍「设备流量 0」,
            // 那问题在 lwIP 之后(SOCKS/mihomo);它不涨,问题在它上面几道。
            ++GatewayDiag::c.fedLwip;
            if (gwDbgOn() && (m_dbgFedLwip++ % 100) == 0)
                std::fprintf(stderr, "[GW] -> lwIP fed=%lld\n", m_dbgFedLwip), std::fflush(stderr);
            if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
                m_net->inputFrame(n->ep, frame);
            }
        });
        if (!n->arp) {
            n->arp = new ArpSpoofer(n->ep, this);
            n->arp->configure(spec.localMac, spec.gatewayIp, spec.gatewayMac);
        }
        if (!n->ndp) {
            n->ndp = new NdpSpoofer(n->ep, this);
            n->ndp->configure(spec.localMac, n->effectiveRouterLL6(), n->effectiveRouterMac6());
        }
        if (!n->watch) {
            n->watch = new ArpWatch(this);
            n->watch->setLocal(spec.localMac, spec.localIp, spec.localGlobal6);
            n->watch->setGateway(spec.gatewayIp, spec.gatewayMac, n->effectiveRouterLL6(),
                                 n->effectiveRouterMac6());
            // 监视器活在工作线程 → 直连；它内部已去抖，这里原样上抛，由 GUI 线程队列接收。
            connect(n->watch, &ArpWatch::alert, this, &GatewayWorker::securityAlert);
        }
        n->ready = true;
        // 刚就绪、还没劫持任何设备：先装「全丢」内核过滤，避免混杂模式下整段流量白白进用户态。
        pushMacFilter(n);
    }
    syncTproxyDevices(); // 集合已清空 → nft 的 proxied 也要跟着空掉,否则设备仍被 TPROXY 截着

    // 消失的网卡（拔网线/断 WiFi）：没有活动劫持的直接摘掉，有的先留着等 disable 收尾。
    const QStringList known = m_nics.keys();
    for (const QString &ifn : known) {
        if (seen.contains(ifn))
            continue;
        GwNic *n = m_nics.value(ifn);
        if (!n || n->victims > 0)
            continue;
        if (n->ep) {
            // **先断信号再删**：帧过滤 lambda 捕获了 n。这里在工作线程上同步 delete（不是 GUI 线程），
            // 端点的通知器随之在工作线程被销毁——合法。delete 后 n->ep 立刻失效，先 disconnect 免得
            // 尚未 delete 之前再进一帧踩到正被拆的 n。
            disconnect(n->ep, nullptr, this, nullptr);
                if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
                    m_net->removeNic(n->ep);
                }
            delete n->ep;
        }
        if (n->arp)
            delete n->arp;
        if (n->watch)
            delete n->watch; // 只读监视器，无还原动作
        if (n->ndp)
            delete n->ndp; // NdpSpoofer 析构会 healAll（还原 v6 邻居缓存）
        m_nics.remove(ifn);
        delete n;
    }

    // 网卡就绪状态刚刷新过 → 顺手重试一次「上次遗留、还没还原成功」的投毒记录（开机第一发常
    // 因为网卡/WiFi 没就绪而打空，这里是它的重试点）。没有遗留时是个空调用。
    healPending();

    publishSnapshot();
}

bool GatewayWorker::enableDeviceLocal(const QString &mac, const QString &ip,
                                      const QString &socksUser, bool reject, QString *err)
{
    // 「就绪」按数据面分别判：lwIP 要有用户态栈，tproxy 要有装好的内核规则。
    //
    // ★ 这一行原本只写 `!m_net`，而 tproxy 模式下 m_net 恒为空 —— 于是**每一次接管都在第一行
    //   失败返回**，表现是「一台设备都劫持不了」，且没有任何崩溃或报错线索（错误信息还理直气壮
    //   地说"需要 root"）。第一版改造就栽在这里，查了一轮才定位到。
    //   更值得记的是：当时我写脚本审计"所有 m_net-> 是否都加了空指针守卫"，那个脚本把含
    //   `!m_net` 的行当成"已守卫"跳过了 —— 而这行的极性恰恰相反。**审计的启发式亲手把 bug
    //   藏了起来。** 加新的数据面时,要找的不只是"解引用点",还有这类**前置条件判据**。
    const bool datapathReady = m_datapath.tproxy ? m_tproxy.isInstalled() : (m_net != nullptr);
    if (!datapathReady || !availableLocal()) {
        if (err)
            *err = m_datapath.tproxy
                       ? QStringLiteral("TPROXY 规则未装载（需要 root 与 nftables/iproute2）")
                       : QStringLiteral("网关未就绪（需要 root/CAP_NET_RAW，或网卡未配置）");
        return false;
    }
    const QByteArray mb = macBytes(mac);
    if (mb.isEmpty() || ip.isEmpty()) {
        if (err)
            *err = QStringLiteral("设备 MAC/IP 非法");
        return false;
    }
    if (m_victimMacStr.contains(ip))
        return true; // 已在劫持

    // 按设备 IP 落在哪张卡的子网里选那套 {端点, ArpSpoofer}——这就是多网卡同时可代理的入口。
    GwNic *n = nicForIp(ip);
    if (!n) {
        if (err)
            *err = QStringLiteral("该设备不在任何已就绪网卡的网段内");
        return false;
    }
    // ★ 网关 MAC 还没解析出来（ArpSpoofer 未 configure）时**必须失败**。
    //   ArpSpoofer::startSpoof 在未配置时是**静默 no-op**：一帧毒都不发。这里若照常返回 true，
    //   这台设备就进了 activeDevices()，而 DevicesController::resumeProxies 见「已在劫持且 IP 没变」
    //   就直接跳过 —— 再也不会重试。结果是 UI 显示「代理中」、实际走的是直连（重启后首轮扫描时
    //   ARP 表里往往还没有网关 MAC，最容易踩）。宁可报错，让上层每轮扫描重试到拓扑齐了为止。
    if (!n->arp || !n->arp->configured()) {
        if (err)
            *err = QStringLiteral("网关 MAC 尚未解析，稍后自动重试");
        return false;
    }

        if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
            m_net->addDevice(ip, mb, socksUser, reject);
        }
    n->arp->startSpoof(mac, ip);
    if (n->ndp)
        n->ndp->startSpoof(mac); // v6 投毒只需 MAC（设备 v6 地址随后从实帧里被动学到）
    if (n->watch)
        n->watch->addVictim(mac, ip); // 登记进监视真相表：检测「这台设备被别人也在投毒」
    ++n->victims;
    const quint64 vkey = macKey(mb);
    m_victimByMac.insert(vkey, ip);
    // ★ 换址防抖的基线：此刻台账 IP 刚确立，视为"刚见过"。**必须初始化**，否则
    //   learnDeviceV4 的静默期判据 `m_victimIpSeenMs.value(vkey, 0)` 会拿到 0 → 判成
    //   「旧地址早已静默」→ 立即采纳新地址。于是一台设备若在**刚被劫持、台账 IP 还没
    //   来得及发帧**时就先发了另一个 IP 的帧（多 IP 主机很可能如此），防抖直接被绕过，
    //   又触发那个反复重挂的抖动（本会话 6600b09 修的正是它）。窗口窄但真实。
    m_victimIpSeenMs[vkey] = QDateTime::currentMSecsSinceEpoch();
    m_victimMacStr.insert(ip, mac);
    m_victimNic.insert(ip, n->spec.ifname);
    m_victimUserByMac.insert(vkey, socksUser); // v6 学习登记时补 mihomo 身份要用
    pushMacFilter(n); // 该卡新增一台设备：重推内核过滤，放行这台的源 MAC
    syncTproxyDevices(); // tproxy 模式：把它加进 nft 的 proxied 集合（lwIP 模式下空操作）
    // 局域网隔离集合：policy=reject 的设备进来，抢答与方向过滤都据此判定。
    if (reject)
        m_isolatedMacs.insert(vkey);
    else
        m_isolatedMacs.remove(vkey);
    pushIsolatedMacs(n);
    // ★ 已经学到的 v6 地址要按**新的** reject 重注册一遍：DeviceInfo 里的 reject 是注册时
    //   拍下的快照，策略切换（普通 ⇄ 禁网）只改 v4 那条的话，该设备旧的 v6 记录仍带着旧值 ——
    //   表现为「设成禁网了，它的 IPv6 UDP 还照走」。addDeviceV6 内部是 insert 覆盖，幂等。
    if (m_net) {
        for (const QString &ip6 : m_victimV6ByMac.value(vkey))
                if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
                    m_net->addDeviceV6(n->ep, ip6, mb, socksUser, reject);
                }
    }
    // 崩溃兜底：把这台设备的还原帧预存起来。进程若被 SIGSEGV/abort 打死，正常的 disableAll
    // 一步都走不到，只有这条路能把设备的网关 ARP 还回去（详见 GatewayPanic.h）。
    if (const IL2Endpoint::RawSendFn pfn = n->ep ? n->ep->panicSender() : nullptr)
        GatewayPanic::arm(vkey, pfn, n->ep->panicContext(), n->arp->healFrames(mac, ip));
    saveState();
    publishSnapshot();
    emit statusChanged();
    return true;
}

void GatewayWorker::disableDeviceLocal(const QString &mac)
{
    const QByteArray mb = macBytes(mac);
    if (mb.isEmpty())
        return; // 非法 MAC：macKey 会得到 0，和「全零 MAC」撞键，干脆挡在这里
    const quint64 key = macKey(mb);
    const QString ip = m_victimByMac.value(key);
    if (ip.isEmpty())
        return;
    GatewayPanic::disarm(key); // 先撤兜底：这台已经要正常还原了，别让崩溃处理器再对它发一遍
    GwNic *n = m_nics.value(m_victimNic.value(ip));
    if (n) {
        if (n->arp)
            n->arp->stopSpoof(mac); // 内部会 heal（还原 ARP）
            // 撤销隔离：把我们替它答过的那些同网段条目还原成真实 MAC。
            n->arp->healIsolation(macBytes(mac));
        if (n->watch)
            n->watch->removeVictim(mac); // 从监视真相表移除（v4/v6 地址一并清）
        if (n->ndp)
            n->ndp->stopSpoof(mac); // 内部会 heal（还原 v6 邻居缓存）
        if (n->victims > 0)
            --n->victims;
    }
    if (m_net) {
                if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
                    m_net->removeDevice(ip);
                }
        // 摘掉这台设备学到的所有 v6 地址（nd6 静态邻居 + devices 表 + v6 UDP 会话）。
        const QSet<QString> v6s = m_victimV6ByMac.value(key);
        for (const QString &ip6 : v6s)
                if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
                    m_net->removeDeviceV6(ip6);
                }
    }
    m_victimByMac.remove(key);
    m_isolatedMacs.remove(key);
    m_victimMacStr.remove(ip);
    m_victimNic.remove(ip);
    m_lastSeenMs.remove(key);
    m_victimIpSeenMs.remove(key); // 换址防抖基线：随设备一起清，别在 QHash 里越积越多
    m_victimUserByMac.remove(key);
    m_victimV6ByMac.remove(key);
    if (n)
        pushMacFilter(n); // 该卡移除一台设备：重推内核过滤（可能变回「全丢」）
    syncTproxyDevices(); // tproxy 模式：把它从 nft 的 proxied 集合里去掉
        pushIsolatedMacs(n);
    saveState();
    publishSnapshot();
    emit statusChanged();
}

void GatewayWorker::disableAllLocal()
{
    GatewayPanic::disarmAll(); // 崩溃兜底连同劫持一起撤（下面就要真正还原了）
    // 每张卡都要还原：漏掉任何一张，那张卡上的设备会一直用着被投毒的 ARP → 断网。
    // 全程在工作线程上同步执行：端点 send() 是裸的 pcap_sendpacket/::sendto，本函数返回时还原 ARP 帧
    // 已经写到网卡上了。LanGateway::disableAll 用 BlockingQueued 投过来，于是「disableAll() 返回」==
    // 「还原已发出」，进程退出前必然完成还原（constraint 2）。
    for (GwNic *n : m_nics) {
        if (n->arp)
            n->arp->healAll();
        if (n->ndp)
            n->ndp->healAll(); // 还原每台设备的 v6 邻居缓存（同 ARP，漏掉就 v6 断网）
        n->victims = 0;
    }
    if (m_net) {
        const QStringList ips = m_victimMacStr.keys();
        for (const QString &ip : ips)
            if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
                m_net->removeDevice(ip);
            }
        for (auto it = m_victimV6ByMac.constBegin(); it != m_victimV6ByMac.constEnd(); ++it)
            for (const QString &ip6 : it.value())
                if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
                    m_net->removeDeviceV6(ip6);
                }
    }
    m_victimByMac.clear();
    m_victimMacStr.clear();
    m_victimNic.clear();
    m_lastSeenMs.clear();
    m_victimUserByMac.clear();
    m_victimV6ByMac.clear();
    // 所有设备清空后，每张卡都重推（现在集合都空了 → 全部装「全丢」，收方彻底静默）。
    for (GwNic *n : m_nics)
        pushMacFilter(n);
    saveState(); // 还有没还原成功的遗留记录时不能删状态文件（下次启动还要靠它）
    publishSnapshot();
    emit statusChanged();
}

void GatewayWorker::recoverLocal()
{
    QFile f(statePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QJsonArray victims = root["victims"].toArray();
    if (victims.isEmpty()) {
        clearState();
        return;
    }
    // 先把记录**展平**（缺的字段从 root 兜底补齐），此后每条都自带完整拓扑，可以脱离 root 重试。
    static const char *kTopoKeys[] = {"ifname",   "localMac",  "gatewayIp",
                                      "gatewayMac", "routerLL6", "routerMac6"};
    m_pendingHeal.clear();
    for (const QJsonValue &v : victims) {
        QJsonObject o = v.toObject();
        for (const char *k : kTopoKeys) {
            if (!o.contains(k) && root.contains(k))
                o[k] = root[k];
        }
        if (o["mac"].toString().isEmpty() || o["ifname"].toString().isEmpty())
            continue; // 没有 MAC/网卡名 = 永远还原不了，别留着无限重试
        m_pendingHeal.append(o);
    }
    healPending();
}

// 尝试还原上次遗留的投毒记录。**还原不掉的绝不丢弃**——开机那一刻 WiFi 常常还没关联、网卡/Npcap
// 驱动还没就绪，这一发很容易打空；老代码无论成败都 clearState()，等于把「谁被投毒了」这份唯一的
// 记录扔掉，被劫持设备就会一直把网关指着本机 MAC（本机又不再转发）→ 连直连都断，只能等两侧邻居
// 缓存老化。现在留在 m_pendingHeal + 状态文件里，每次 configureLocal（每轮扫描）都重试一次。
void GatewayWorker::healPending()
{
    if (m_pendingHeal.isEmpty())
        return;

    QHash<QString, QVector<QJsonObject>> byIface;
    for (const QJsonObject &o : std::as_const(m_pendingHeal)) {
        // 这台设备现在又被我们正常劫持着 → 旧记录作废：再 heal 就把刚投的毒解掉了。
        const QByteArray mb = macBytes(o["mac"].toString());
        if (!mb.isEmpty() && m_victimByMac.contains(macKey(mb)))
            continue;
        byIface[o["ifname"].toString()].append(o);
    }

    QVector<QJsonObject> left;
    for (auto it = byIface.constBegin(); it != byIface.constEnd(); ++it) {
        const QString ifname = it.key();
        const QVector<QJsonObject> &group = it.value();
        const QJsonObject &first = group.constFirst();
        GwNic *n = m_nics.value(ifname);
        // 拓扑取用：状态文件优先，**缺的用这张卡当前的拓扑兜底**。网关 MAC 是扫描几轮后才解析出来
        // 的，上次退出时若还没解析到，文件里就是空的——只认文件的话这条记录永远还原不了、永远重试。
        const auto field = [&](const char *k) -> QString {
            const QString v = first[k].toString();
            if (!v.isEmpty() || !n)
                return v;
            if (std::strcmp(k, "localMac") == 0)
                return n->spec.localMac;
            if (std::strcmp(k, "gatewayIp") == 0)
                return n->spec.gatewayIp;
            if (std::strcmp(k, "gatewayMac") == 0)
                return n->spec.gatewayMac;
            if (std::strcmp(k, "routerLL6") == 0)
                return n->effectiveRouterLL6();
            if (std::strcmp(k, "routerMac6") == 0)
                return n->effectiveRouterMac6();
            return v;
        };
        // 这张卡已经开着就直接复用它的端点（省一次 pcap/AF_PACKET 打开，也避开同卡双开的坑）；
        // 否则临时开一个。临时端点在工作线程上开、也在工作线程上同步销毁（constraint 4/9）。
        IL2Endpoint *ep = nullptr;
        bool temporary = false;
        if (n && n->ready && n->ep && n->ep->isOpen()) {
            ep = n->ep;
        } else if (!m_nics.isEmpty() && !n) {
            // 已经配置过网卡了，而这张卡不在其中 = 它当前根本不存在（拔了/改名了）。别每轮扫描都
            // 去试开一次（那是白白的系统调用/pcap 打开）。记录留着：卡回来了自然会走上面那条分支。
            left += group;
            continue;
        } else {
            ep = createL2Endpoint(this);
            if (ep && !ep->open(ifname, nullptr)) {
                delete ep;
                ep = nullptr;
            }
            temporary = ep != nullptr;
        }
        if (!ep) {
            left += group; // 这张卡还没就绪：留着下轮再试，**不**清记录
            continue;
        }
        ArpSpoofer healer(ep, this);
        healer.configure(field("localMac"), field("gatewayIp"), field("gatewayMac"));
        // v6 也要还原：上次投毒过设备的 v6 邻居缓存，不还原同样会让设备 v6 断网。routerLL6 为空
        //（上次没有 v6 拓扑）→ NdpSpoofer no-op，heal 自然跳过。routerMac6 缺则回落 gatewayMac。
        NdpSpoofer ndpHealer(ep, this);
        const QString rmac6 = field("routerMac6").isEmpty() ? field("gatewayMac")
                                                            : field("routerMac6");
        ndpHealer.configure(field("localMac"), field("routerLL6"), rmac6);
        // 网关 MAC 没记下来（老版本状态文件 / 上次也没解析出来）→ healer 未配置，stopSpoof 是
        // 静默 no-op，等于**没还原**。这种也要留着重试：下一轮扫描后拓扑就有了。
        const bool healable = healer.configured();
        if (healable) {
            for (const QJsonObject &o : group) {
                healer.startSpoof(o["mac"].toString(), o["ip"].toString());
                healer.stopSpoof(o["mac"].toString()); // startSpoof 建档、stopSpoof 立刻 heal
                ndpHealer.startSpoof(o["mac"].toString());
                ndpHealer.stopSpoof(o["mac"].toString());
            }
        } else {
            left += group;
        }
        if (temporary)
            delete ep; // 同步 delete（工作线程上），通知器随之销毁；不用 deleteLater 免得悬到不确定时点
        if (gwDbgOn())
            std::fprintf(stderr, "[GW] healPending %s: %d 台 %s\n", ifname.toLatin1().constData(),
                         int(group.size()), healable ? "已还原" : "延后重试"),
                std::fflush(stderr);
    }

    m_pendingHeal = left;
    saveState();
}

void GatewayWorker::teardownLocal()
{
    if (m_torndown)
        return;
    m_torndown = true;
    // 先还原全部 ARP（constraint 2 对析构/退出同样成立），再销毁数据面对象——全在工作线程上。
    // disableAllLocal() 里已 disarmAll()：端点马上要被 delete，兜底表里绝不能留着它的 fd/句柄。
    disableAllLocal();
    for (GwNic *n : m_nics) {
        if (n->ep) {
            disconnect(n->ep, nullptr, this, nullptr);
                if (m_net) { // tproxy 模式没有用户态栈,这里恒为空
                    m_net->removeNic(n->ep);
                }
            delete n->ep; // 端点+通知器在工作线程析构（constraint 9）
        }
        if (n->arp)
            delete n->arp; // ArpSpoofer 的 QTimer 在工作线程析构
        if (n->watch)
            delete n->watch; // 只读监视器
        if (n->ndp)
            delete n->ndp; // NdpSpoofer 同理（析构 healAll 还原 v6）
        delete n;
    }
    m_nics.clear();
    delete m_net; // NetStack 的 QTimer + 所有 Socks5*/lwIP 结构在工作线程析构
    m_net = nullptr;
    // nft 规则/ip rule/路由表是**内核持久状态**,不拆掉的话:规则还在、却没有进程接管 TPROXY
    // 的流量 = 被覆盖的设备完全断网。析构里也有一道（TproxyRules 的析构调 remove），这里显式
    // 再来一次是因为 teardownLocal 是「还原一切」的正式出口,不该依赖对象生命周期的时机。
    m_tproxy.remove();
}

// ———————————————————————————— LanGateway（GUI 线程的编排入口）————————————————————————————
struct LanGateway::Impl {
    QThread *thread = nullptr;
    GatewayWorker *worker = nullptr;
    std::shared_ptr<GwShared> shared;

    // 工作线程是否可被阻塞调用（已 start、未 quit，且当前不在工作线程上——外部调用者恒为 GUI 线程）。
    bool workerReady() const
    {
        return worker && thread && thread->isRunning()
               && QThread::currentThread() != thread;
    }
};

LanGateway::LanGateway(QObject *parent) : QObject(parent), d(new Impl)
{
    d->shared = std::make_shared<GwShared>();
    d->thread = new QThread(this);
    d->thread->setObjectName(QStringLiteral("LanGatewayWorker"));
    d->worker = new GatewayWorker(d->shared); // 无 parent：随后 moveToThread
    d->worker->moveToThread(d->thread);
    // 线程干净退出后在**它自己的线程**上删 worker（constraint 9）。teardownLocal 已在退出前把
    // NetStack/端点/ArpSpoofer 全删干净，此刻 worker 已无子对象，deleteLater 只是回收空壳。
    connect(d->thread, &QThread::finished, d->worker, &QObject::deleteLater);
    // 工作线程 emit 的信号 → 队列连接 → 在 GUI 线程重发本类同名信号（只带 QString 值类型，constraint 7）。
    connect(d->worker, &GatewayWorker::deviceError, this, &LanGateway::deviceError);
    connect(d->worker, &GatewayWorker::statusChanged, this, &LanGateway::statusChanged);
    connect(d->worker, &GatewayWorker::securityAlert, this, &LanGateway::securityAlert);
    connect(d->worker, &GatewayWorker::deviceIpChanged, this, &LanGateway::deviceIpChanged);
    d->thread->start();
}

void LanGateway::shutdown()
{
    if (!d->workerReady())
        return;
    QMetaObject::invokeMethod(d->worker, [w = d->worker] { w->teardownLocal(); },
                              Qt::BlockingQueuedConnection);
}

LanGateway::~LanGateway()
{
    if (d->thread) {
        if (d->workerReady()) {
            // 阻塞投递 teardown：还原全部 ARP + 在工作线程上销毁数据面对象，之后再停线程（constraint 2/9）。
            QMetaObject::invokeMethod(d->worker, [w = d->worker] { w->teardownLocal(); },
                                      Qt::BlockingQueuedConnection);
        }
        d->thread->quit();
        d->thread->wait(); // 等 exec() 退出；退出途中处理 finished→deleteLater，把 worker 删在本线程上
        // d->worker 此刻已被 deleteLater 回收，置空避免误用。
        d->worker = nullptr;
        delete d->thread; // QThread 对象 affinity 属 GUI，本线程删除是标准做法
        d->thread = nullptr;
    }
    delete d;
}

void LanGateway::setDatapath(const DatapathSpec &spec)
{
    if (!d->workerReady())
        return;
    // 同步语义：紧跟其后的 configure() 必须已经看到新模式（configureLocal 按模式决定建不建 NetStack）。
    QMetaObject::invokeMethod(
        d->worker, [w = d->worker, spec] { w->setDatapathLocal(spec); },
        Qt::BlockingQueuedConnection);
}

void LanGateway::configure(const QVector<NicSpec> &nics, quint16 socksPort)
{
    if (!d->workerReady())
        return;
    // BlockingQueued：保持原来的同步语义（configure 返回后紧跟的 isAvailable/enableDevice 立刻能看到
    // 新配置）。它做的是开端点这类冷路径工作，代价与「重构前就在 GUI 线程同步跑」相同，无回归。
    QMetaObject::invokeMethod(
        d->worker, [w = d->worker, nics, socksPort] { w->configureLocal(nics, socksPort); },
        Qt::BlockingQueuedConnection);
}

bool LanGateway::isAvailable() const
{
    // 只读快照：无线程往返，不阻塞、不撕裂（constraint 5）。
    QMutexLocker lk(&d->shared->mutex);
    return d->shared->available;
}

bool LanGateway::canProxy(const QString &ip) const
{
    const quint32 v = ipToU32(ip);
    if (!v)
        return false;
    QMutexLocker lk(&d->shared->mutex);
    for (const GwSubnet &s : d->shared->subnets) {
        if (s.mask && (v & s.mask) == (s.ip & s.mask))
            return true;
    }
    return false;
}

bool LanGateway::enableDevice(const QString &mac, const QString &ip, const QString &socksUser,
                              bool reject, QString *err)
{
    if (!d->workerReady()) {
        if (err)
            *err = QStringLiteral("网关未就绪");
        return false;
    }
    // 同步语义（constraint 6）：BlockingQueued 让 lambda 在工作线程跑、GUI 阻塞等它完成，其间按引用
    // 捕获 ok/e/参数都安全（GUI 栈全程存活）。工作线程处理本调用时只 emit 队列信号 + 写快照，绝不反向
    // 阻塞等 GUI → 不可能互等死锁。
    bool ok = false;
    QString e;
    QMetaObject::invokeMethod(
        d->worker,
        [this, &ok, &e, &mac, &ip, &socksUser, reject] {
            ok = d->worker->enableDeviceLocal(mac, ip, socksUser, reject, &e);
        },
        Qt::BlockingQueuedConnection);
    if (err)
        *err = e;
    return ok;
}

void LanGateway::disableDevice(const QString &mac)
{
    if (!d->workerReady())
        return;
    QMetaObject::invokeMethod(d->worker, [w = d->worker, mac] { w->disableDeviceLocal(mac); },
                              Qt::BlockingQueuedConnection);
}

void LanGateway::disableAll()
{
    if (!d->workerReady())
        return;
    // 必须同步完成才返回（挂在 aboutToQuit 上，还原 ARP 要在进程退出前真正发出去，constraint 2）。
    QMetaObject::invokeMethod(d->worker, [w = d->worker] { w->disableAllLocal(); },
                              Qt::BlockingQueuedConnection);
    // ★ 这里**不**写诊断日志的收尾。disableAll 跑在 GUI 线程上，而工作线程的事件循环和 NetStack
    //   的泵此刻仍活着（disableAllLocal 只是撤了投毒，没有停线程）—— 在这里调 GatewayDiag::flush
    //   会和泵里的 sample() 并发碰同一批计数器/文件，正好违反 GatewayDiag 的单线程前提。
    //   收尾放在 NetStack 的析构里（那是在工作线程上跑的，见本文件的线程模型）。
}

void LanGateway::recoverFromCrash()
{
    if (!d->workerReady())
        return;
    // 启动时调用：临时端点/通知器必须在工作线程上开（constraint 4），故投到工作线程同步执行。
    QMetaObject::invokeMethod(d->worker, [w = d->worker] { w->recoverLocal(); },
                              Qt::BlockingQueuedConnection);
}

QStringList LanGateway::activeDevices() const
{
    QMutexLocker lk(&d->shared->mutex);
    return d->shared->active;
}

#include "LanGateway_linux.moc"
