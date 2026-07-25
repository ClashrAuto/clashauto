// LanGateway 的 Linux 真实现（编译条件由 CMake 控制：仅 Linux 编入本文件）。
// 组合：IL2Endpoint(AF_PACKET 二层) + ArpSpoofer(双向 ARP 投毒/还原) + NetStack(lwIP 用户态栈)。
#include "LanGateway.h"

#include "ArpSpoofer.h"
#include "IL2Endpoint.h"
#include "NetStack.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QVector>

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
} // namespace

// 一张物理网卡的运行时套件：二层端点 + ARP 投毒器 + 该卡的拓扑数值。
struct GwNic {
    LanGateway::NicSpec spec;
    IL2Endpoint *ep = nullptr;
    ArpSpoofer *arp = nullptr;
    quint32 localIp4 = 0, netMask4 = 0, gatewayIp4 = 0;
    bool ready = false;      // ep 已打开且 netif 已挂上协议栈
    int victims = 0;         // 这张卡上正在被劫持的设备数（>0 时不重建，避免断流）
};

struct LanGateway::Impl {
    NetStack *net = nullptr;         // 共用（lwIP 单实例，多 netif）
    LanGateway *owner = nullptr;
    QHash<QString, GwNic *> nics;    // ifname → 套件
    quint16 socksPort = 0;

    // 被劫持设备：src MAC 打包成的 quint64 键 → ip（帧过滤 + 记账，见 macKey 注释）。
    QHash<quint64, QString> victimByMac;
    QHash<QString, QString> victimMacStr; // ip → mac 串（disable 用）
    QHash<QString, QString> victimNic;    // ip → ifname（disable/持久化要找回对应那张卡）

    // 把「属于这张卡的」被劫持设备源 MAC 集合推给它的二层端点，装成内核态源 MAC 过滤（收方优化）。
    // 多网卡：只推 victimNic 记为这张卡的设备。集合为空 → 端点装「全丢」过滤（这张卡当前没有劫持）。
    // 装不上（平台不支持/失败）无所谓——frameReceived 的 lambda 仍按 victimByMac 在用户态兜底过滤。
    // 这里要的是真正的 6 字节 MAC，而 victimByMac 的键已经是打包过的整数，所以改从 victimMacStr
    // （ip → mac 串）走一遍 macBytes()——冷路径（只在 configure/enable/disable 时跑），无所谓开销。
    void pushMacFilter(GwNic *n) const
    {
        if (!n || !n->ep)
            return;
        QVector<QByteArray> macs;
        for (auto it = victimMacStr.constBegin(); it != victimMacStr.constEnd(); ++it) {
            if (victimNic.value(it.key()) != n->spec.ifname)
                continue;
            const QByteArray mb = macBytes(it.value());
            if (mb.size() == 6)
                macs.append(mb);
        }
        n->ep->setSourceMacFilter(macs);
    }

    // 按 IP 找它属于哪张已就绪网卡（同网段判定）。找不到返回 nullptr。
    GwNic *nicForIp(const QString &ip) const
    {
        const quint32 v = ipToU32(ip);
        if (!v)
            return nullptr;
        for (GwNic *n : nics) {
            if (n->ready && n->netMask4 && (v & n->netMask4) == (n->localIp4 & n->netMask4))
                return n;
        }
        return nullptr;
    }

    // 崩溃恢复清单文件路径。
    QString statePath() const
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        return QDir(dir).filePath("gateway_active.json");
    }
    // 每条 victim 自带它那张卡的还原所需信息：多网卡下不能再靠一份全局 ifname/gateway。
    void persist() const
    {
        QJsonArray arr;
        for (auto it = victimMacStr.constBegin(); it != victimMacStr.constEnd(); ++it) {
            const QString ip = it.key();
            QJsonObject o;
            o["ip"] = ip;
            o["mac"] = it.value();
            const QString ifn = victimNic.value(ip);
            if (GwNic *n = nics.value(ifn)) {
                o["ifname"] = n->spec.ifname;
                o["localMac"] = n->spec.localMac;
                o["gatewayIp"] = n->spec.gatewayIp;
                o["gatewayMac"] = n->spec.gatewayMac;
            }
            arr.append(o);
        }
        QJsonObject root;
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

void LanGateway::configure(const QVector<NicSpec> &specs, quint16 socksPort)
{
    d->socksPort = socksPort;

    // 协议栈是共用的，先起来（lwIP 单实例；每张卡随后各挂一个 netif）。
    QString err;
    if (!d->net) {
        d->net = new NetStack(socksPort, this);
        if (!d->net->init(&err)) {
            emit deviceError(QString(), QStringLiteral("协议栈初始化失败: ") + err);
            d->net->deleteLater();
            d->net = nullptr;
            return;
        }
    }

    QSet<QString> seen;
    for (const NicSpec &spec : specs) {
        if (spec.ifname.isEmpty() || spec.localIp.isEmpty() || spec.localMac.isEmpty())
            continue;
        seen.insert(spec.ifname);
        GwNic *n = d->nics.value(spec.ifname);
        if (!n) {
            n = new GwNic;
            n->spec = spec;
            d->nics.insert(spec.ifname, n);
        }
        // 拓扑数值每次都刷新（网关 MAC 常常是扫描几轮后才解析出来的）。
        n->spec = spec;
        n->localIp4 = ipToU32(spec.localIp);
        n->gatewayIp4 = ipToU32(spec.gatewayIp);
        n->netMask4 = ipToU32(spec.netmask);
        if (n->arp)
            n->arp->configure(spec.localMac, spec.gatewayIp, spec.gatewayMac);

        if (n->ready)
            continue; // 已就绪：只刷新拓扑，不重建（重建会断掉这张卡上的活动劫持）
        if (!n->netMask4) {
            // 没有有效掩码就没法给 netif 定路由，也没法做同网段旁路——这张卡直接不启用。
            continue;
        }
        if (!n->ep) {
            n->ep = createL2Endpoint(this);
            if (!n->ep)
                break; // 平台不支持，后面几张也没必要试（仍要走下面的摘卡清理）
        }
        if (!n->ep->isOpen() && !n->ep->open(spec.ifname, &err)) {
            emit deviceError(QString(),
                             QStringLiteral("打开网卡失败(%1): ").arg(spec.ifname) + err);
            continue;
        }
        if (!d->net->hasNic(n->ep)
            && !d->net->addNic(n->ep, n->ep->localMac(), spec.localIp, spec.netmask, &err)) {
            emit deviceError(QString(),
                             QStringLiteral("协议栈挂载网卡失败(%1): ").arg(spec.ifname) + err);
            continue;
        }
        // 二层帧过滤：只把被劫持设备发来的帧喂进用户态栈（按这张卡的网段做旁路判断）。
        connect(n->ep, &IL2Endpoint::frameReceived, this, [this, n](const QByteArray &frame) {
            if (frame.size() < 12)
                return;
            const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
            // 源 MAC 原地打包成 quint64 查表：不做 frame.mid(6, 6)，省掉每帧一次堆分配。
            if (!d->victimByMac.contains(macKey(f + 6)))
                return;
            // 同网段直连旁路：被劫持设备发往「本网段内（且非网关本身）」的 IPv4 帧不喂用户态栈，
            // 让它照常二层直达——设备回给本机 LAN IP 的包若被 lwIP 终结会触发 RST，导致本机无法
            // 直连该设备（SSH/网页/共享）；同理 LAN 内设备互访也不该被代理绕行。只有真正出网
            // （目的在子网外）或发往网关 IP（DNS/路由器后台等，其 ARP 被投毒必须由我们接管）的帧进栈。
            if (n->netMask4 != 0 && frame.size() >= 34) {
                const quint16 ethType = (quint16(f[12]) << 8) | f[13];
                if (ethType == 0x0800) { // IPv4
                    const quint32 dst = (quint32(f[30]) << 24) | (quint32(f[31]) << 16)
                                        | (quint32(f[32]) << 8) | quint32(f[33]);
                    const bool sameSubnet = (dst & n->netMask4) == (n->localIp4 & n->netMask4);
                    if (sameSubnet && dst != n->gatewayIp4)
                        return; // LAN 内直连，放行给系统
                }
            }
            d->net->inputFrame(n->ep, frame);
        });
        if (!n->arp) {
            n->arp = new ArpSpoofer(n->ep, this);
            n->arp->configure(spec.localMac, spec.gatewayIp, spec.gatewayMac);
        }
        n->ready = true;
        // 刚就绪、还没劫持任何设备：先装「全丢」内核过滤，避免混杂模式下整段流量白白进用户态。
        // （后续 enable/disable 会按这张卡的最新 victim 集合重推。）
        d->pushMacFilter(n);
    }

    // 消失的网卡（拔网线/断 WiFi）：没有活动劫持的直接摘掉，有的先留着等 disable 收尾。
    const QStringList known = d->nics.keys();
    for (const QString &ifn : known) {
        if (seen.contains(ifn))
            continue;
        GwNic *n = d->nics.value(ifn);
        if (!n || n->victims > 0)
            continue;
        if (n->ep) {
            // **先断信号再删**：帧过滤 lambda 捕获了 n，而 deleteLater 要到下一轮事件循环才生效——
            // 这中间若再收到一帧，就会用到已 delete 的 n（use-after-free）。
            disconnect(n->ep, nullptr, this, nullptr);
            if (d->net)
                d->net->removeNic(n->ep);
            n->ep->deleteLater();
        }
        if (n->arp)
            n->arp->deleteLater();
        d->nics.remove(ifn);
        delete n;
    }
}

bool LanGateway::isAvailable() const
{
    if (!d->net)
        return false;
    for (GwNic *n : d->nics) {
        if (n->ready && n->ep && n->ep->isOpen())
            return true;
    }
    return false;
}

bool LanGateway::canProxy(const QString &ip) const
{
    return d->nicForIp(ip) != nullptr;
}

bool LanGateway::enableDevice(const QString &mac, const QString &ip, const QString &socksUser,
                              QString *err)
{
    if (!d->net || !isAvailable()) {
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

    // 按设备 IP 落在哪张卡的子网里选那套 {端点, ArpSpoofer}——这就是多网卡同时可代理的入口。
    GwNic *n = d->nicForIp(ip);
    if (!n) {
        if (err)
            *err = QStringLiteral("该设备不在任何已就绪网卡的网段内");
        return false;
    }

    d->net->addDevice(ip, mb, socksUser);
    n->arp->startSpoof(mac, ip);
    ++n->victims;
    d->victimByMac.insert(macKey(mb), ip);
    d->victimMacStr.insert(ip, mac);
    d->victimNic.insert(ip, n->spec.ifname);
    d->pushMacFilter(n); // 该卡新增一台设备：重推内核过滤，放行这台的源 MAC
    d->persist();
    emit statusChanged();
    return true;
}

void LanGateway::disableDevice(const QString &mac)
{
    const QByteArray mb = macBytes(mac);
    if (mb.isEmpty())
        return; // 非法 MAC：macKey 会得到 0，和「全零 MAC」撞键，干脆挡在这里
    const quint64 key = macKey(mb);
    const QString ip = d->victimByMac.value(key);
    if (ip.isEmpty())
        return;
    GwNic *n = d->nics.value(d->victimNic.value(ip));
    if (n) {
        if (n->arp)
            n->arp->stopSpoof(mac); // 内部会 heal（还原 ARP）
        if (n->victims > 0)
            --n->victims;
    }
    if (d->net)
        d->net->removeDevice(ip);
    d->victimByMac.remove(key);
    d->victimMacStr.remove(ip);
    d->victimNic.remove(ip);
    if (n)
        d->pushMacFilter(n); // 该卡移除一台设备：重推内核过滤（可能变回「全丢」）
    if (d->victimMacStr.isEmpty())
        d->clearState();
    else
        d->persist();
    emit statusChanged();
}

void LanGateway::disableAll()
{
    // 每张卡都要还原：漏掉任何一张，那张卡上的设备会一直用着被投毒的 ARP → 断网。
    for (GwNic *n : d->nics) {
        if (n->arp)
            n->arp->healAll();
        n->victims = 0;
    }
    if (d->net) {
        const QStringList ips = d->victimMacStr.keys();
        for (const QString &ip : ips)
            d->net->removeDevice(ip);
    }
    d->victimByMac.clear();
    d->victimMacStr.clear();
    d->victimNic.clear();
    // 所有设备清空后，每张卡都重推（现在集合都空了 → 全部装「全丢」，收方彻底静默）。
    for (GwNic *n : d->nics)
        d->pushMacFilter(n);
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
    // 多网卡：victim 按 ifname 分组，每张卡各开一次端点还原自己那批。
    // 兼容旧格式（字段在 root 上、只有一张卡）：取不到 per-victim 字段就回落到 root。
    QHash<QString, QVector<QJsonObject>> byIface;
    for (const QJsonValue &v : victims) {
        const QJsonObject o = v.toObject();
        const QString ifn = o.contains("ifname") ? o["ifname"].toString()
                                                 : root["ifname"].toString();
        byIface[ifn].append(o);
    }
    for (auto it = byIface.constBegin(); it != byIface.constEnd(); ++it) {
        const QString ifname = it.key();
        if (ifname.isEmpty())
            continue;
        const QVector<QJsonObject> &group = it.value();
        const QJsonObject &first = group.constFirst();
        const auto field = [&](const char *k) {
            return first.contains(k) ? first[k].toString() : root[k].toString();
        };
        IL2Endpoint *ep = createL2Endpoint(this);
        if (ep && ep->open(ifname, nullptr)) {
            ArpSpoofer healer(ep, this);
            healer.configure(field("localMac"), field("gatewayIp"), field("gatewayMac"));
            for (const QJsonObject &o : group) {
                healer.startSpoof(o["mac"].toString(), o["ip"].toString());
                healer.stopSpoof(o["mac"].toString()); // startSpoof 建档、stopSpoof 立刻 heal
            }
        }
        if (ep)
            ep->deleteLater();
    }
    d->clearState();
}

QStringList LanGateway::activeDevices() const
{
    return d->victimMacStr.values();
}
