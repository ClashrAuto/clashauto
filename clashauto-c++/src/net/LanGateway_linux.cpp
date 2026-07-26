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
#include "IL2Endpoint.h"
#include "NetStack.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <cstdio>
#include <memory>

namespace {
// COAST_GATEWAY_DEBUG=1 → 把「收到的设备帧走到哪一步」打到 stderr（配合 L2Endpoint 的 [WINL2]
// 探针，定位「设备流量为 0」断在收包 / victim 过滤 / 同网段旁路 / 喂进 lwIP 的哪一环）。
bool gwDbgOn()
{
    static const bool on = qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG");
    return on;
}
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
} // namespace

// 一张物理网卡的运行时套件：二层端点 + ARP 投毒器 + 该卡的拓扑数值。（活在工作线程上。）
struct GwNic {
    LanGateway::NicSpec spec;
    IL2Endpoint *ep = nullptr;
    ArpSpoofer *arp = nullptr;
    quint32 localIp4 = 0, netMask4 = 0, gatewayIp4 = 0;
    bool ready = false;      // ep 已打开且 netif 已挂上协议栈
    int victims = 0;         // 这张卡上正在被劫持的设备数（>0 时不重建，避免断流）
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
    bool enableDeviceLocal(const QString &mac, const QString &ip, const QString &socksUser,
                           QString *err);
    void disableDeviceLocal(const QString &mac);
    void disableAllLocal();
    void recoverLocal();
    void teardownLocal(); // 还原全部 ARP + 销毁 NetStack/端点/ArpSpoofer（幂等）。退出/析构走它。

signals:
    void deviceError(const QString &mac, const QString &message);
    void statusChanged();

private:
    bool availableLocal() const;
    GwNic *nicForIp(const QString &ip) const;
    void pushMacFilter(GwNic *n) const;
    QString statePath() const;
    void persist() const;
    void clearState() const { QFile::remove(statePath()); }
    // 把当前状态刷进 GUI 可读的快照（每次改状态后调用一次）。
    void publishSnapshot();

    NetStack *m_net = nullptr;               // 共用（lwIP 单实例，多 netif）
    QHash<QString, GwNic *> m_nics;          // ifname → 套件
    quint16 m_socksPort = 0;
    QHash<quint64, QString> m_victimByMac;   // src MAC 打包键 → ip（帧过滤 + 记账）
    QHash<QString, QString> m_victimMacStr;  // ip → mac 串（disable 用）
    QHash<QString, QString> m_victimNic;     // ip → ifname（disable/持久化找回对应网卡）
    std::shared_ptr<GwShared> m_shared;
    bool m_torndown = false;
    // 诊断计数（COAST_GATEWAY_DEBUG）：设备帧在 frameReceived 里各分支的去向。
    long long m_dbgDropNonVictim = 0, m_dbgBypassLan = 0, m_dbgFedLwip = 0, m_dbgArpAnswered = 0;
};

// 把「属于这张卡的」被劫持设备源 MAC 集合推给它的二层端点，装成内核态源 MAC 过滤（收方优化）。
void GatewayWorker::pushMacFilter(GwNic *n) const
{
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

// 按 IP 找它属于哪张已就绪网卡（同网段判定）。找不到返回 nullptr。
GwNic *GatewayWorker::nicForIp(const QString &ip) const
{
    const quint32 v = ipToU32(ip);
    if (!v)
        return nullptr;
    for (GwNic *n : m_nics) {
        if (n->ready && n->netMask4 && (v & n->netMask4) == (n->localIp4 & n->netMask4))
            return n;
    }
    return nullptr;
}

bool GatewayWorker::availableLocal() const
{
    if (!m_net)
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

void GatewayWorker::configureLocal(const QVector<LanGateway::NicSpec> &specs, quint16 socksPort)
{
    m_socksPort = socksPort;

    // 协议栈是共用的，先起来（lwIP 单实例；每张卡随后各挂一个 netif）。都在工作线程上创建。
    QString err;
    if (!m_net) {
        m_net = new NetStack(socksPort, this);
        if (!m_net->init(&err)) {
            emit deviceError(QString(), QStringLiteral("协议栈初始化失败: ") + err);
            delete m_net;
            m_net = nullptr;
            return;
        }
    }

    QSet<QString> seen;
    for (const LanGateway::NicSpec &spec : specs) {
        if (spec.ifname.isEmpty() || spec.localIp.isEmpty() || spec.localMac.isEmpty())
            continue;
        seen.insert(spec.ifname);
        GwNic *n = m_nics.value(spec.ifname);
        if (!n) {
            n = new GwNic;
            n->spec = spec;
            m_nics.insert(spec.ifname, n);
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
        if (!m_net->hasNic(n->ep)
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
            // 源 MAC 原地打包成 quint64 查表：不做 frame.mid(6, 6)，省掉每帧一次堆分配。
            if (!m_victimByMac.contains(macKey(f + 6))) {
                // 帧到了这一层但源 MAC 不在被劫持名单里 → 这里丢弃。若「设备流量为 0」且这个
                // 计数在涨，说明设备的帧收到了、但它的源 MAC 和台账里的 MAC 对不上（随机 MAC /
                // 台账 MAC 有误），是 victim 匹配的问题，不是抓包的问题。
                if (gwDbgOn() && (m_dbgDropNonVictim++ % 200) == 0)
                    std::fprintf(stderr, "[GW] drop non-victim src=%02x:%02x:%02x:%02x:%02x:%02x "
                                         "(count=%lld)\n",
                                 f[6], f[7], f[8], f[9], f[10], f[11], m_dbgDropNonVictim),
                        std::fflush(stderr);
                return;
            }
            // ARP 抢答：被劫持设备一问「谁是网关?」就同步回「网关在本机 MAC」，赶在真网关前面。
            // 这是「时通时不通」的针对性修复——周期重发压不住真网关的正确应答，必须一问就抢答。
            // 抢答后仍把该 ARP 喂给 lwIP（它据此维护设备 MAC 映射，无副作用）。
            if (frame.size() >= 14 && f[12] == 0x08 && f[13] == 0x06 && n->arp) {
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
                    const bool sameSubnet = (dst & n->netMask4) == (n->localIp4 & n->netMask4);
                    if (sameSubnet && dst != n->gatewayIp4) {
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
            if (gwDbgOn() && (m_dbgFedLwip++ % 100) == 0)
                std::fprintf(stderr, "[GW] -> lwIP fed=%lld\n", m_dbgFedLwip), std::fflush(stderr);
            m_net->inputFrame(n->ep, frame);
        });
        if (!n->arp) {
            n->arp = new ArpSpoofer(n->ep, this);
            n->arp->configure(spec.localMac, spec.gatewayIp, spec.gatewayMac);
        }
        n->ready = true;
        // 刚就绪、还没劫持任何设备：先装「全丢」内核过滤，避免混杂模式下整段流量白白进用户态。
        pushMacFilter(n);
    }

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
            if (m_net)
                m_net->removeNic(n->ep);
            delete n->ep;
        }
        if (n->arp)
            delete n->arp;
        m_nics.remove(ifn);
        delete n;
    }

    publishSnapshot();
}

bool GatewayWorker::enableDeviceLocal(const QString &mac, const QString &ip,
                                      const QString &socksUser, QString *err)
{
    if (!m_net || !availableLocal()) {
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
    if (m_victimMacStr.contains(ip))
        return true; // 已在劫持

    // 按设备 IP 落在哪张卡的子网里选那套 {端点, ArpSpoofer}——这就是多网卡同时可代理的入口。
    GwNic *n = nicForIp(ip);
    if (!n) {
        if (err)
            *err = QStringLiteral("该设备不在任何已就绪网卡的网段内");
        return false;
    }

    m_net->addDevice(ip, mb, socksUser);
    n->arp->startSpoof(mac, ip);
    ++n->victims;
    m_victimByMac.insert(macKey(mb), ip);
    m_victimMacStr.insert(ip, mac);
    m_victimNic.insert(ip, n->spec.ifname);
    pushMacFilter(n); // 该卡新增一台设备：重推内核过滤，放行这台的源 MAC
    persist();
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
    GwNic *n = m_nics.value(m_victimNic.value(ip));
    if (n) {
        if (n->arp)
            n->arp->stopSpoof(mac); // 内部会 heal（还原 ARP）
        if (n->victims > 0)
            --n->victims;
    }
    if (m_net)
        m_net->removeDevice(ip);
    m_victimByMac.remove(key);
    m_victimMacStr.remove(ip);
    m_victimNic.remove(ip);
    if (n)
        pushMacFilter(n); // 该卡移除一台设备：重推内核过滤（可能变回「全丢」）
    if (m_victimMacStr.isEmpty())
        clearState();
    else
        persist();
    publishSnapshot();
    emit statusChanged();
}

void GatewayWorker::disableAllLocal()
{
    // 每张卡都要还原：漏掉任何一张，那张卡上的设备会一直用着被投毒的 ARP → 断网。
    // 全程在工作线程上同步执行：端点 send() 是裸的 pcap_sendpacket/::sendto，本函数返回时还原 ARP 帧
    // 已经写到网卡上了。LanGateway::disableAll 用 BlockingQueued 投过来，于是「disableAll() 返回」==
    // 「还原已发出」，进程退出前必然完成还原（constraint 2）。
    for (GwNic *n : m_nics) {
        if (n->arp)
            n->arp->healAll();
        n->victims = 0;
    }
    if (m_net) {
        const QStringList ips = m_victimMacStr.keys();
        for (const QString &ip : ips)
            m_net->removeDevice(ip);
    }
    m_victimByMac.clear();
    m_victimMacStr.clear();
    m_victimNic.clear();
    // 所有设备清空后，每张卡都重推（现在集合都空了 → 全部装「全丢」，收方彻底静默）。
    for (GwNic *n : m_nics)
        pushMacFilter(n);
    clearState();
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
    // 用上次留下的网卡/网关信息，给这些设备发还原 ARP（先修复被投毒的缓存，避免设备断网）。
    // 在工作线程上开临时端点、healAll——纯 send，不需要跑事件循环。端点 parent=worker、也在工作线程析构。
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
        // 同步 delete（工作线程上），把临时端点的通知器一并在本线程销毁；不用 deleteLater 免得
        // 悬到不确定时点。healer 是栈对象，其析构再 healAll 一次（集合已空 → no-op）。
        delete ep;
    }
    clearState();
}

void GatewayWorker::teardownLocal()
{
    if (m_torndown)
        return;
    m_torndown = true;
    // 先还原全部 ARP（constraint 2 对析构/退出同样成立），再销毁数据面对象——全在工作线程上。
    disableAllLocal();
    for (GwNic *n : m_nics) {
        if (n->ep) {
            disconnect(n->ep, nullptr, this, nullptr);
            if (m_net)
                m_net->removeNic(n->ep);
            delete n->ep; // 端点+通知器在工作线程析构（constraint 9）
        }
        if (n->arp)
            delete n->arp; // ArpSpoofer 的 QTimer 在工作线程析构
        delete n;
    }
    m_nics.clear();
    delete m_net; // NetStack 的 QTimer + 所有 Socks5*/lwIP 结构在工作线程析构
    m_net = nullptr;
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
    d->thread->start();
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
                              QString *err)
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
        [this, &ok, &e, &mac, &ip, &socksUser] {
            ok = d->worker->enableDeviceLocal(mac, ip, socksUser, &e);
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
