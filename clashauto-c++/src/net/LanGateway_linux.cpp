// LanGateway 的 Linux 真实现（编译条件由 CMake 控制：仅 Linux 编入本文件）。
// 组合：IL2Endpoint(AF_PACKET 二层) + ArpSpoofer(双向 ARP 投毒/还原) + NetStack(lwIP 用户态栈)。
#include "LanGateway.h"

#include "ArpSpoofer.h"
#include "IL2Endpoint.h"
#include "NetStack.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>

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
} // namespace

struct LanGateway::Impl {
    IL2Endpoint *ep = nullptr;
    ArpSpoofer *arp = nullptr;
    NetStack *net = nullptr;
    LanGateway *owner = nullptr;

    QString ifname, localIp, localMac, gatewayIp, gatewayMac;
    quint16 socksPort = 0;
    bool stackReady = false; // ep 打开 + net 初始化成功

    // 被劫持设备：src MAC(6 字节) → ip（帧过滤 + 记账）。
    QHash<QByteArray, QString> victimByMac;
    QHash<QString, QString> victimMacStr; // ip → mac 串（disable 用）

    // 崩溃恢复清单文件路径。
    QString statePath() const
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        return QDir(dir).filePath("gateway_active.json");
    }
    void persist() const
    {
        QJsonArray arr;
        for (auto it = victimMacStr.constBegin(); it != victimMacStr.constEnd(); ++it) {
            QJsonObject o;
            o["ip"] = it.key();
            o["mac"] = it.value();
            arr.append(o);
        }
        QJsonObject root;
        root["ifname"] = ifname;
        root["localMac"] = localMac;
        root["gatewayIp"] = gatewayIp;
        root["gatewayMac"] = gatewayMac;
        root["victims"] = arr;
        QFile f(statePath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
            f.close();
        }
    }
    void clearState() const { QFile::remove(statePath()); }
};

LanGateway::LanGateway(QObject *parent) : QObject(parent), d(new Impl)
{
    d->owner = this;
}

LanGateway::~LanGateway()
{
    disableAll();
    delete d;
}

void LanGateway::configure(const QString &ifname, const QString &localIp, const QString &localMac,
                           const QString &gatewayIp, const QString &gatewayMac, quint16 socksPort)
{
    d->ifname = ifname;
    d->localIp = localIp;
    d->localMac = localMac;
    d->gatewayIp = gatewayIp;
    d->gatewayMac = gatewayMac;
    d->socksPort = socksPort;

    // 首次或换网卡：建立/重建协议栈。有活动劫持时不贸然重建（避免断流）。
    if (d->stackReady && !d->victimByMac.isEmpty())
        return;

    if (!d->ep) {
        d->ep = createL2Endpoint(this);
        if (!d->ep)
            return; // 平台不支持
    }
    QString err;
    if (!d->ep->isOpen()) {
        if (!d->ep->open(ifname, &err)) {
            emit deviceError(QString(), QStringLiteral("打开网卡失败: ") + err);
            return;
        }
    }
    if (!d->net) {
        d->net = new NetStack(d->ep, socksPort, this);
        if (!d->net->init(d->ep->localMac(), &err)) {
            emit deviceError(QString(), QStringLiteral("协议栈初始化失败: ") + err);
            d->net->deleteLater();
            d->net = nullptr;
            return;
        }
        // 二层帧过滤：只把被劫持设备发来的帧喂进用户态栈。
        connect(d->ep, &IL2Endpoint::frameReceived, this, [this](const QByteArray &frame) {
            if (frame.size() < 12)
                return;
            const QByteArray src = frame.mid(6, 6);
            if (d->victimByMac.contains(src))
                d->net->inputFrame(frame);
        });
    }
    if (!d->arp)
        d->arp = new ArpSpoofer(d->ep, this);
    d->arp->configure(localMac, gatewayIp, gatewayMac);
    d->stackReady = true;
}

bool LanGateway::isAvailable() const
{
    return d->ep != nullptr && d->ep->isOpen() && d->stackReady;
}

bool LanGateway::enableDevice(const QString &mac, const QString &ip, const QString &socksUser,
                              QString *err)
{
    if (!d->stackReady) {
        if (err)
            *err = QStringLiteral("网关未就绪（需要 root/CAP_NET_RAW，或网卡未配置）");
        return false;
    }
    const QByteArray mb = macBytes(mac);
    if (mb.isEmpty() || ip.isEmpty()) {
        if (err)
            *err = QStringLiteral("设备 MAC/IP 非法");
        return false;
    }
    if (d->victimMacStr.contains(ip))
        return true; // 已在劫持

    d->net->addDevice(ip, mb, socksUser);
    d->arp->startSpoof(mac, ip);
    d->victimByMac.insert(mb, ip);
    d->victimMacStr.insert(ip, mac);
    d->persist();
    emit statusChanged();
    return true;
}

void LanGateway::disableDevice(const QString &mac)
{
    const QByteArray mb = macBytes(mac);
    const QString ip = d->victimByMac.value(mb);
    if (ip.isEmpty())
        return;
    if (d->arp)
        d->arp->stopSpoof(mac); // 内部会 heal（还原 ARP）
    if (d->net)
        d->net->removeDevice(ip);
    d->victimByMac.remove(mb);
    d->victimMacStr.remove(ip);
    if (d->victimMacStr.isEmpty())
        d->clearState();
    else
        d->persist();
    emit statusChanged();
}

void LanGateway::disableAll()
{
    if (d->arp)
        d->arp->healAll();
    if (d->net) {
        const QStringList ips = d->victimMacStr.keys();
        for (const QString &ip : ips)
            d->net->removeDevice(ip);
    }
    d->victimByMac.clear();
    d->victimMacStr.clear();
    d->clearState();
    emit statusChanged();
}

void LanGateway::recoverFromCrash()
{
    QFile f(d->statePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QJsonArray victims = root["victims"].toArray();
    if (victims.isEmpty()) {
        d->clearState();
        return;
    }
    // 用上次留下的网卡/网关信息，给这些设备发还原 ARP（先修复被投毒的缓存，避免设备断网）。
    const QString ifname = root["ifname"].toString();
    const QString localMac = root["localMac"].toString();
    const QString gatewayIp = root["gatewayIp"].toString();
    const QString gatewayMac = root["gatewayMac"].toString();
    IL2Endpoint *ep = createL2Endpoint(this);
    if (ep && ep->open(ifname, nullptr)) {
        ArpSpoofer healer(ep, this);
        healer.configure(localMac, gatewayIp, gatewayMac);
        for (const QJsonValue &v : victims) {
            const QJsonObject o = v.toObject();
            healer.startSpoof(o["mac"].toString(), o["ip"].toString());
            healer.stopSpoof(o["mac"].toString()); // startSpoof 建档、stopSpoof 立刻 heal
        }
    }
    if (ep)
        ep->deleteLater();
    d->clearState();
}

QStringList LanGateway::activeDevices() const
{
    return d->victimMacStr.values();
}
