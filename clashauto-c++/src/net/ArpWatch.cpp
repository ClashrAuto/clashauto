#include "ArpWatch.h"

#include <QHostAddress>

#include <cstring>

// ———————————————————————— 帧偏移常量（与 ArpSpoofer/NdpSpoofer 一致）————————————————————————
namespace {
// 以太
constexpr int kEthLen = 14;
constexpr int kOffEthDst = 0;
constexpr int kOffEthSrc = 6;
constexpr int kOffEthType = 12;
// ARP（IPv4）
constexpr int kArpMinLen = 42;
constexpr int kOffArpOper = 20;
constexpr int kOffArpSha = 22; // sender MAC（6）
constexpr int kOffArpSpa = 28; // sender IPv4（4）
constexpr int kOffArpTpa = 38; // target IPv4（4）
// IPv6 / NDP
constexpr int kOffIp6NextHdr = kEthLen + 6; // 20
constexpr int kOffIp6Src = kEthLen + 8;     // 22
constexpr int kOffIcmp6Type = kEthLen + 40; // 54
constexpr int kOffNaTarget = kOffIcmp6Type + 8; // 62（NA target）
constexpr int kNaOptStart = kOffNaTarget + 16;  // 78
constexpr int kRaOptStart = kOffIcmp6Type + 16; // 70
constexpr quint8 kNextHdrIcmp6 = 58;
constexpr quint8 kIcmp6TypeRa = 134;
constexpr quint8 kIcmp6TypeNa = 136;
constexpr quint8 kNdpOptSlla = 1; // Source Link-Layer Address
constexpr quint8 kNdpOptTlla = 2; // Target Link-Layer Address

bool isZeroMac(const QByteArray &m) { return m.size() != 6 || m == QByteArray(6, char(0)); }
} // namespace

ArpWatch::ArpWatch(QObject *parent) : QObject(parent) { m_clock.start(); }

// ———————————————————————— 真相表配置 ————————————————————————
void ArpWatch::setLocal(const QString &localMac, const QString &localIp4, const QString &localGlobal6)
{
    m_localMac = macBytes(localMac);
    m_localIp4 = ip4ToU32(localIp4);
    m_localGlobal6 = ip6Bytes(localGlobal6);
}

void ArpWatch::setGateway(const QString &gatewayIp4, const QString &gatewayMac,
                          const QString &routerLinkLocal6, const QString &routerMac6)
{
    m_gatewayIp4 = ip4ToU32(gatewayIp4);
    m_gatewayMac = macBytes(gatewayMac);
    m_routerLL6 = ip6Bytes(routerLinkLocal6);
    m_routerMac6 = macBytes(routerMac6);
}

void ArpWatch::addVictim(const QString &mac, const QString &ip4)
{
    const QByteArray m = macBytes(mac);
    if (m.size() != 6)
        return;
    const quint32 v = ip4ToU32(ip4);
    if (v)
        m_victimV4.insert(v, m);
}

void ArpWatch::removeVictim(const QString &mac)
{
    const QByteArray m = macBytes(mac);
    if (m.size() != 6)
        return;
    for (auto it = m_victimV4.begin(); it != m_victimV4.end();) {
        if (it.value() == m)
            it = m_victimV4.erase(it);
        else
            ++it;
    }
    for (auto it = m_victimV6.begin(); it != m_victimV6.end();) {
        if (it.value() == m)
            it = m_victimV6.erase(it);
        else
            ++it;
    }
}

void ArpWatch::addVictimV6(const QString &mac, const QString &ip6)
{
    const QByteArray m = macBytes(mac);
    const QByteArray a = ip6Bytes(ip6);
    if (m.size() == 6 && a.size() == 16)
        m_victimV6.insert(a, m);
}

void ArpWatch::clearVictims()
{
    m_victimV4.clear();
    m_victimV6.clear();
}

// ———————————————————————— 观察入口 ————————————————————————
void ArpWatch::observe(const QByteArray &frame)
{
    if (m_localMac.size() != 6) // 未配置
        return;
    const int len = frame.size();
    if (len < kEthLen)
        return;
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    if (f[kOffEthType] == 0x08 && f[kOffEthType + 1] == 0x06)
        observeArp(f, len);
    else if (f[kOffEthType] == 0x86 && f[kOffEthType + 1] == 0xDD && len > kOffIcmp6Type
             && f[kOffIp6NextHdr] == kNextHdrIcmp6)
        observeNdp(f, len);
}

// ———————————————————————— IPv4 ARP ————————————————————————
void ArpWatch::observeArp(const uchar *f, int len)
{
    if (len < kArpMinLen)
        return;
    const QByteArray sha(reinterpret_cast<const char *>(f + kOffArpSha), 6); // sender MAC = 声明者
    if (isZeroMac(sha) || sha == m_localMac) // 我们自己发的（投毒/还原/抢答）→ 忽略，见类注释「铁律」
        return;
    const quint32 spa = (quint32(f[kOffArpSpa]) << 24) | (quint32(f[kOffArpSpa + 1]) << 16)
                        | (quint32(f[kOffArpSpa + 2]) << 8) | quint32(f[kOffArpSpa + 3]);
    const quint32 tpa = (quint32(f[kOffArpTpa]) << 24) | (quint32(f[kOffArpTpa + 1]) << 16)
                        | (quint32(f[kOffArpTpa + 2]) << 8) | quint32(f[kOffArpTpa + 3]);
    const QByteArray ethDst(reinterpret_cast<const char *>(f + kOffEthDst), 6);

    // ① 声明「网关 IP 在别的 MAC」= 有人冒充网关。
    if (m_gatewayIp4 && spa == m_gatewayIp4) {
        if (m_gatewayMac.size() == 6 && sha == m_gatewayMac)
            return; // 真网关的正常 ARP
        const QString gwIp = QHostAddress(spa).toString();
        // 进一步判：这条毒是不是冲着我某台被劫持设备来的（tpa 命中 victim）→ 设备被争抢；
        // 否则（针对本机 / 广播 / 段内其它）→ 别人在代理我这段网络（含我）。
        auto vit = m_victimV4.find(tpa);
        if (vit != m_victimV4.end())
            maybeAlert(DeviceContended, sha, gwIp, vit.value());
        else
            maybeAlert(SelfImpersonated, sha, gwIp, {});
        return;
    }
    // ② 声明「本机 IP 在别的 MAC」= 有人冒充我（抢发往本机的流量）。
    if (m_localIp4 && spa == m_localIp4) {
        maybeAlert(SelfImpersonated, sha, QHostAddress(spa).toString(), {});
        return;
    }
    // ③ 声明「我某台 victim 的 IP 在别的 MAC」= 有人抢我的设备（偷其回程）。
    auto vit = m_victimV4.find(spa);
    if (vit != m_victimV4.end() && sha != vit.value())
        maybeAlert(DeviceContended, sha, QHostAddress(spa).toString(), vit.value());
}

// ———————————————————————— IPv6 NDP ————————————————————————
void ArpWatch::observeNdp(const uchar *f, int len)
{
    const quint8 type = f[kOffIcmp6Type];
    if (type == kIcmp6TypeRa) {
        // RA = 路由器自我通告。冒充者用它把「路由器在我 MAC」塞给设备。
        if (m_routerLL6.size() != 16)
            return;
        const QByteArray srcLL(reinterpret_cast<const char *>(f + kOffIp6Src), 16);
        if (srcLL != m_routerLL6) // 只关心冒充「我们盯的那个默认路由器」
            return;
        // 声明的 MAC：优先 RA 里的 Source Link-Layer Address 选项，缺则以太源 MAC。
        QByteArray mac = ndpOptMac(f, len, kRaOptStart, kNdpOptSlla);
        if (mac.size() != 6)
            mac = QByteArray(reinterpret_cast<const char *>(f + kOffEthSrc), 6);
        if (mac == m_localMac || (m_routerMac6.size() == 6 && mac == m_routerMac6))
            return; // 我们自己 / 真路由器
        maybeAlert(SelfImpersonated, mac, QHostAddress(srcLL).toString(), {});
        return;
    }
    if (type == kIcmp6TypeNa) {
        if (len < kNaOptStart)
            return;
        const QByteArray target(reinterpret_cast<const char *>(f + kOffNaTarget), 16);
        // 声明的 MAC：优先 NA 的 Target Link-Layer Address 选项，缺则以太源 MAC。
        QByteArray mac = ndpOptMac(f, len, kNaOptStart, kNdpOptTlla);
        if (mac.size() != 6)
            mac = QByteArray(reinterpret_cast<const char *>(f + kOffEthSrc), 6);
        if (isZeroMac(mac) || mac == m_localMac)
            return; // 我们自己发的投毒/还原 NA
        // 冒充 v6 路由器？
        if (m_routerLL6.size() == 16 && target == m_routerLL6) {
            if (m_routerMac6.size() == 6 && mac == m_routerMac6)
                return; // 真路由器
            maybeAlert(SelfImpersonated, mac, QHostAddress(target).toString(), {});
            return;
        }
        // 冒充本机全局 v6？
        if (m_localGlobal6.size() == 16 && target == m_localGlobal6) {
            maybeAlert(SelfImpersonated, mac, QHostAddress(target).toString(), {});
            return;
        }
        // 冒充我某台 victim 的 v6？
        auto vit = m_victimV6.find(target);
        if (vit != m_victimV6.end() && mac != vit.value())
            maybeAlert(DeviceContended, mac, QHostAddress(target).toString(), vit.value());
    }
}

QByteArray ArpWatch::ndpOptMac(const uchar *f, int len, int optStart, quint8 wantType)
{
    int p = optStart;
    while (p + 2 <= len) {
        const quint8 type = f[p];
        const int len8 = f[p + 1];
        if (len8 == 0)
            break; // 非法（0 长选项）—— 防死循环
        const int optLen = len8 * 8;
        if (p + optLen > len)
            break;
        if (type == wantType && optLen >= 8)
            return QByteArray(reinterpret_cast<const char *>(f + p + 2), 6);
        p += optLen;
    }
    return {};
}

// ———————————————————————— 去抖 + 首报门槛 ————————————————————————
void ArpWatch::maybeAlert(int kind, const QByteArray &offMac6, const QString &subjectIp,
                          const QByteArray &subjectMac6)
{
    const QString offMac = macToStr(reinterpret_cast<const uchar *>(offMac6.constData()));
    const QString key = QString::number(kind) + '|' + offMac + '|' + subjectIp;
    const qint64 now = m_clock.elapsed();

    // 老化清理（表偏大时才做，平时零开销）。
    if (m_tracks.size() > kPruneAt) {
        for (auto it = m_tracks.begin(); it != m_tracks.end();) {
            if (now - it.value().firstMs > kPruneMs)
                it = m_tracks.erase(it);
            else
                ++it;
        }
    }

    Track &t = m_tracks[key];
    if (t.firstMs == 0 || now - t.firstMs > kWindowMs) { // 开新窗口
        t.firstMs = now;
        t.hits = 0;
    }
    ++t.hits;
    if (t.hits < kMinHits)
        return; // 还没连看够，滤瞬态
    if (t.lastAlertMs != 0 && now - t.lastAlertMs < kRealertMs)
        return; // 重报节流
    t.lastAlertMs = now;

    QString subMac;
    if (subjectMac6.size() == 6 && !isZeroMac(subjectMac6))
        subMac = macToStr(reinterpret_cast<const uchar *>(subjectMac6.constData()));
    emit alert(kind, offMac, subjectIp, subMac);
}

// ———————————————————————— 小工具 ————————————————————————
QByteArray ArpWatch::macBytes(const QString &mac)
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

quint32 ArpWatch::ip4ToU32(const QString &ip)
{
    if (ip.isEmpty())
        return 0;
    bool ok = false;
    const quint32 v = QHostAddress(ip).toIPv4Address(&ok);
    return ok ? v : 0;
}

QByteArray ArpWatch::ip6Bytes(const QString &ip)
{
    if (ip.isEmpty())
        return {};
    QHostAddress a(ip);
    if (a.protocol() != QAbstractSocket::IPv6Protocol)
        return {};
    Q_IPV6ADDR raw = a.toIPv6Address();
    return QByteArray(reinterpret_cast<const char *>(raw.c), 16);
}

QString ArpWatch::macToStr(const uchar *p)
{
    static const char hex[] = "0123456789abcdef";
    QString s;
    s.reserve(17);
    for (int i = 0; i < 6; ++i) {
        if (i)
            s += ':';
        s += QLatin1Char(hex[p[i] >> 4]);
        s += QLatin1Char(hex[p[i] & 0xF]);
    }
    return s;
}
