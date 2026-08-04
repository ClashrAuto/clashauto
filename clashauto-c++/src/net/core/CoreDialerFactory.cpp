#include "CoreDialerFactory.h"

#include "../GatewayDiag.h"      // 回退原因记账：离「完全替换 mihomo」还差多少
#include "../InprocTelemetry.h"  // 控制面出口：连接快照 / 流量合计 / 回退原因日志

#include "DirectOutbound.h"
#include "ProxyConfig.h"
#include "proto/OutboundRegistry.h"

#include <QByteArray>

// ———————————————————————————————————————————————————————————————————————————
// 为什么需要「路由包装器」而不能在 createTcp/createUdp 里直接选实现：
//   OutboundFactory 的 create* 只拿得到 parent —— 目的地(dstHost/dstPort)与设备身份(user) 要等
//   NetStack 之后调 connectTo()/associate() 才知道（见 NetStack.cpp 的 ACCEPT 回调：先 createTcp、
//   connect 好信号、最后才 connectTo）。而 router 的判定恰恰依赖目的地/设备。
//   所以 create* 先返回一个**空壳包装器**，把「选谁」推迟到 connectTo/associate 那一刻再做：那时
//   router 有了 dstHost/user，包装器据此现造真正的内层出站（DirectOutbound 或 fallback 造的），
//   把内层信号原样转发上来，其余方法全部委托给内层。对 NetStack 而言这就是一个普通的 IOutbound*。
//
// 包装器不带 Q_OBJECT：它不新增任何信号/槽，只是**转发** IOutboundTcp/Udp 基类已声明的信号
//（emit 基类信号、以基类信号做 connect 都不需要派生类自己的元对象），因此无需 moc，也就不必在
//   .cpp 里 #include 一个 .moc。
// ———————————————————————————————————————————————————————————————————————————

namespace {

// makeInner* 的判定结果：除了内层出站对象本身，还带回「走没走进程内」与「为什么没走」——
// 前者决定这条连接登不登记进 InprocTelemetry（**只登记进程内的**，回退核心的由 mihomo 的
// /connections 自己报，两个集合不相交，UI 合并时才不会重复计数），后者是回退原因日志的素材
//（用户排查「为什么这个节点没走进程内」的唯一线索，此前只有 GatewayDiag 的匿名计数器）。
struct DialChoice {
    bool inproc = false; // out 是进程内实现（DIRECT/协议出站）；false = 回退核心 / 拒绝
    QString reason;      // 回退/拒绝的人话原因；inproc 时为空
};

// 合成「为什么这个节点走不了进程内」的原因文本。具体的传输守卫拒绝发生在各协议注册 lambda
// 里（只 qWarning 到 stderr），这里按节点参数复原同一判定的人话版本，不必侵入每个协议文件。
QString fallbackReasonFor(const ProxyNode &n)
{
    QStringList extras;
    if (!n.network.isEmpty() && n.network != QLatin1String("tcp"))
        extras << QStringLiteral("network=%1").arg(n.network);
    if (!n.plugin.isEmpty())
        extras << QStringLiteral("plugin=%1").arg(n.plugin);
    if (!n.obfs.isEmpty())
        extras << QStringLiteral("obfs=%1").arg(n.obfs);
    if (OutboundRegistry::instance().has(n.type)) {
        return extras.isEmpty()
                ? QStringLiteral("协议 %1 的该参数组合进程内未实现").arg(n.type)
                : QStringLiteral("协议 %1 的传输/插件进程内未实现（%2）")
                          .arg(n.type, extras.join(QStringLiteral(", ")));
    }
    return QStringLiteral("协议 %1 进程内未实现").arg(n.type);
}

// 把 router 给出的节点名解析成一个具体的内层 IOutboundTcp。parent 传包装器自身，随其一同析构。
IOutboundTcp *makeInnerTcp(const QString &node, ProxyConfigStore *store, OutboundFactory *fallback,
                           QObject *parent, bool strict, DialChoice *choice)
{
    if (node.isEmpty()) {
        ++GatewayDiag::c.fbNoRoute;
        choice->reason = QStringLiteral("路由未判定出目标节点");
        return strict ? nullptr : fallback->createTcp(parent); // 无路由 = 照旧走 mihomo
    }
    if (node == QStringLiteral("DIRECT")) {
        ++GatewayDiag::c.ccInProcess;
        choice->inproc = true;
        return new DirectOutboundTcp(parent); // 进程内直连
    }
    // 具名节点：查快照拿到它的完整参数（type/server/port/cipher/…）。
    if (store) {
        if (const std::shared_ptr<const ProxyConfig> cfg = store->current()) {
            if (const ProxyNode *n = cfg->nodeByName(node)) {
                if (n->isDirect()) {
                    ++GatewayDiag::c.ccInProcess;
                    choice->inproc = true;
                    return new DirectOutboundTcp(parent); // 内建直连
                }
                // 协议出站：查注册表。已注册该 type 就用进程内协议实现；createTcp 可能返回 null
                // （creator 为空 / 内部拒绝），那时照样回退 fallback，绝不缺失功能。
                if (OutboundRegistry::instance().has(n->type)) {
                    if (IOutboundTcp *proto = OutboundRegistry::instance().createTcp(*n, parent)) {
                        ++GatewayDiag::c.ccInProcess;
                        choice->inproc = true;
                        return proto;
                    }
                }
                ++GatewayDiag::c.fbProtoMissing; // 有这个节点，但协议没编进来/没注册
                choice->reason = fallbackReasonFor(*n);
            } else {
                ++GatewayDiag::c.fbNodeMissing;  // router 给了名字，快照里却没有
                choice->reason = QStringLiteral("配置快照里找不到节点 %1").arg(node);
            }
        }
    }
    if (choice->reason.isEmpty())
        choice->reason = QStringLiteral("无出站配置快照");
    // 未注册的协议 / 查不到节点 / 协议实现拒绝 → 回退 fallback（拨 mihomo），保证功能不缺失。
    // ★ 严格模式：**不回退**，返回 nullptr 让上层明确失败并记账 —— 这样「还差什么」会立刻暴露，
    //   而不是被静默回退掩盖。代价是这些连接会断，所以它是个显式的二级开关，默认不开。
    return strict ? nullptr : fallback->createTcp(parent);
}

IOutboundUdp *makeInnerUdp(const QString &node, ProxyConfigStore *store, OutboundFactory *fallback,
                           QObject *parent, bool strict, DialChoice *choice)
{
    if (node.isEmpty()) {
        ++GatewayDiag::c.fbNoRoute;
        choice->reason = QStringLiteral("路由未判定出目标节点");
        return strict ? nullptr : fallback->createUdp(parent);
    }
    if (node == QStringLiteral("DIRECT")) {
        choice->inproc = true;
        return new DirectOutboundUdp(parent);
    }
    if (store) {
        if (const std::shared_ptr<const ProxyConfig> cfg = store->current()) {
            if (const ProxyNode *n = cfg->nodeByName(node)) {
                if (n->isDirect()) {
                    choice->inproc = true;
                    return new DirectOutboundUdp(parent);
                }
                // 协议 UDP 出站：注册表里该协议的 UdpCreator 为空（协议不支持 UDP）时 createUdp 返回
                // null，落到下面回退 fallback。
                if (OutboundRegistry::instance().has(n->type)) {
                    if (IOutboundUdp *proto = OutboundRegistry::instance().createUdp(*n, parent)) {
                        choice->inproc = true;
                        return proto;
                    }
                }
                choice->reason = QStringLiteral("节点 %1（%2）的 UDP 进程内未实现").arg(node, n->type);
            } else {
                choice->reason = QStringLiteral("配置快照里找不到节点 %1").arg(node);
            }
        }
    }
    if (choice->reason.isEmpty())
        choice->reason = QStringLiteral("无出站配置快照");
    // 未注册 / 该协议不支持 UDP / 查不到节点 → 回退 fallback。严格模式下不回退（见 makeInnerTcp）。
    ++GatewayDiag::c.fbUdpUnsupported;
    return strict ? nullptr : fallback->createUdp(parent);
}

// 一条 TCP 连接的路由包装器。connectTo 时才据 router 选内层，之后全权委托内层。
class RoutingOutboundTcp final : public IOutboundTcp
{
public:
    RoutingOutboundTcp(bool strict, CoreDialerFactory::Router router, ProxyConfigStore *store,
                       OutboundFactory *fallback, const QString &tag, QObject *parent)
        : IOutboundTcp(parent), m_strict(strict), m_router(std::move(router)), m_store(store),
          m_fallback(fallback), m_tag(tag)
    {
    }

    ~RoutingOutboundTcp() override
    {
        // 兜底注销（closed/failed 已各注销一次，幂等）。**必须在析构里也做**：requestClose 的
        // 跨线程安全依赖「记录还在 ⇒ owner 还活着」这条不变量（见 InprocTelemetry.h 头注释）。
        if (m_connId)
            InprocTelemetry::instance().unregisterConn(m_connId);
    }

    void connectTo(const QString &dstHost, quint16 dstPort, const QString &user) override
    {
        const QString node = m_router ? m_router(dstHost, user) : QString();
        DialChoice choice;
        m_inner = makeInnerTcp(node, m_store, m_fallback, this, m_strict, &choice);
        if (!m_inner) {
            // 严格模式下 makeInner* 拒绝回退 → 这条连接明确失败。**故意不静默回退**：
            // 「还差什么」只有暴露出来才补得上（原因分布见 GatewayDiag 的 cc=… 那几栏）。
            ++GatewayDiag::c.ccStrictRefused;
            InprocTelemetry::instance().log(
                QStringLiteral("refuse:%1:%2").arg(node, choice.reason),
                QStringLiteral("[CoastCore] 严格模式拒绝 %1:%2（节点 %3）—— %4")
                        .arg(dstHost).arg(dstPort)
                        .arg(node.isEmpty() ? QStringLiteral("-") : node, choice.reason));
            QMetaObject::invokeMethod(this, [this] {
                emit failed(QStringLiteral("严格模式：该连接无法走进程内出站（已拒绝回退核心）"));
            }, Qt::QueuedConnection);
            return;
        }
        if (choice.inproc) {
            // 进程内出站：登记进控制面快照（UI 连接列表/流量合计都从这儿来）。
            // 回退核心的连接**不登记** —— mihomo 的 /connections 会报它，登了就是重复计数。
            m_connId = InprocTelemetry::instance().registerConn(
                    dstHost, dstPort, QStringLiteral("tcp"), node, user, m_tag, this,
                    [this] { closeTunnel(); });
        } else if (m_router) {
            // 回退核心（只在 coastcore 真开着、router 判过之后才值得说；router 都没装时全量
            // 回退是既定行为，不是诊断信息）。这行日志是用户回答「为什么这个节点没走进程内」
            // 的唯一线索，按「节点+原因」节流防刷屏。
            InprocTelemetry::instance().log(
                QStringLiteral("fb:%1:%2").arg(node, choice.reason),
                QStringLiteral("[CoastCore] 回退核心 %1:%2（节点 %3）—— %4")
                        .arg(dstHost).arg(dstPort)
                        .arg(node.isEmpty() ? QStringLiteral("-") : node, choice.reason));
        }
        // 内层信号 → 原样转发为本对象（基类）信号，NetStack 连的是本对象。
        // 顺带在转发点记控制面数据：下行字节（dataReceived）、上行字节（upstreamBytesWritten，
        // 即真正交给内核的量）、建立/失败日志、结束注销 —— 三条路（网关/TUN/本机入站）共用
        // 此包装器，控制面因此零侵入地覆盖全部进程内路径。
        connect(m_inner, &IOutboundTcp::established, this, [this, dstHost, dstPort, node] {
            if (m_connId)
                InprocTelemetry::instance().log(
                    QStringLiteral("est:%1").arg(node),
                    QStringLiteral("[CoastCore] 进程内出站建立 %1:%2 经 %3")
                            .arg(dstHost).arg(dstPort)
                            .arg(node.isEmpty() ? QStringLiteral("DIRECT") : node));
            emit established();
        });
        connect(m_inner, &IOutboundTcp::dataReceived, this, [this](const QByteArray &d) {
            if (m_connId)
                InprocTelemetry::instance().addDown(m_connId, d.size());
            emit dataReceived(d);
        });
        connect(m_inner, &IOutboundTcp::failed, this, [this, dstHost, dstPort, node](const QString &r) {
            if (m_connId) {
                InprocTelemetry::instance().log(
                    QStringLiteral("fail:%1:%2").arg(node, r),
                    QStringLiteral("[CoastCore] 进程内出站失败 %1:%2（节点 %3）—— %4")
                            .arg(dstHost).arg(dstPort)
                            .arg(node.isEmpty() ? QStringLiteral("DIRECT") : node, r));
                InprocTelemetry::instance().unregisterConn(m_connId);
            }
            emit failed(r);
        });
        connect(m_inner, &IOutboundTcp::closed, this, [this] {
            if (m_connId)
                InprocTelemetry::instance().unregisterConn(m_connId);
            emit closed();
        });
        connect(m_inner, &IOutboundTcp::upstreamBytesWritten, this, [this](qint64 n) {
            if (m_connId)
                InprocTelemetry::instance().addUp(m_connId, n);
            emit upstreamBytesWritten(n);
        });
        // 连接建立前若已被要求暂停读，补设到内层（正常时序里 NetStack 不会在 connectTo 前调，
        // 但保持无副作用地承接）。
        if (m_readPausedPending) {
            m_inner->setReadPaused(true);
        }
        // 拨号前就 write() 进来的上行字节，先原样交给内层再拨（内层的契约保证 connectTo 之前
        // 收到的字节会被缓冲、就绪后补发，见 IOutbound.h）。**必须在 connectTo 之前交接**：
        // connectTo 可能同步失败并让上层收掉这条连接，之后再碰本对象就没意义了。
        if (!m_preDial.isEmpty()) {
            m_inner->write(m_preDial);
            m_preDial.clear();
        }
        m_inner->connectTo(dstHost, dstPort, user);
    }

    void write(const QByteArray &data) override
    {
        if (m_inner) {
            m_inner->write(data);
        } else {
            // 还没选内层（NetStack 把拨号推迟了一拍，而设备的首个数据段先到了）：先存着。
            // 早先这里是**静默丢弃** —— HTTP 请求/TLS ClientHello 被吞掉，隧道建起来却不发
            // 一个字节，设备侧表现为 connect 成功后挂死。契约见 IOutbound.h。
            m_preDial += data;
        }
    }
    void write(const char *data, qsizetype size) override
    {
        if (!data || size <= 0) {
            return;
        }
        if (m_inner) {
            m_inner->write(data, size);
        } else {
            m_preDial.append(data, size); // 深拷贝：字节要留到 connectTo 之后（同上）
        }
    }
    void closeTunnel() override
    {
        if (m_inner) {
            m_inner->closeTunnel();
        } else {
            emit closed(); // 还没选内层就被关：直接收口
        }
    }
    bool isEstablished() const override { return m_inner && m_inner->isEstablished(); }
    qint64 bytesToWrite() const override
    {
        // 拨号前缓冲的字节也要算进水位，否则上层会误判「上行已排空」而把 lwIP 接收窗口开满。
        return qint64(m_preDial.size()) + (m_inner ? m_inner->bytesToWrite() : 0);
    }
    void setReadPaused(bool paused) override
    {
        if (m_inner) {
            m_inner->setReadPaused(paused);
        } else {
            m_readPausedPending = paused;
        }
    }
    bool isReadPaused() const override
    {
        return m_inner ? m_inner->isReadPaused() : m_readPausedPending;
    }

private:
    bool m_strict = false; // 严格模式：不回退核心，见 makeInnerTcp
    CoreDialerFactory::Router m_router;
    ProxyConfigStore *m_store = nullptr;
    OutboundFactory *m_fallback = nullptr;
    QString m_tag;                     // 入口标签（进快照 metadata.type）
    quint64 m_connId = 0;              // InprocTelemetry 句柄；0 = 未登记（回退核心的连接不登记）
    IOutboundTcp *m_inner = nullptr;   // parent=this，随本对象析构；connectTo 前为空
    QByteArray m_preDial;              // connectTo 前 write() 进来的上行字节（内层还没造出来）
    bool m_readPausedPending = false;  // connectTo 前暂存的暂停请求
};

// 一次 UDP 会话的路由包装器。associate 时选内层。
//   注意：UDP 会话在 associate 时还没有具体目的地（dstIp 要到 sendTo 才有，且一个会话复用于多目标），
//   所以按目的地做 UDP 分流与「每会话一个出站」的模型对不上。本单元先只按 user 路由（dstHost 传空），
//   足够支撑「整机/按设备走直连 or mihomo」。真要做「同一设备不同目的地 UDP 走不同出站」需要把会话按
//   目的地拆开，留到后续单元（TODO）。
class RoutingOutboundUdp final : public IOutboundUdp
{
public:
    RoutingOutboundUdp(bool strict, CoreDialerFactory::Router router, ProxyConfigStore *store,
                       OutboundFactory *fallback, const QString &tag, QObject *parent)
        : IOutboundUdp(parent), m_strict(strict), m_router(std::move(router)), m_store(store),
          m_fallback(fallback), m_tag(tag)
    {
    }

    ~RoutingOutboundUdp() override
    {
        if (m_connId) // 兜底注销，理由同 TCP 侧（requestClose 的不变量依赖它）
            InprocTelemetry::instance().unregisterConn(m_connId);
    }

    void associate(const QString &user) override
    {
        // dstHost 传空：UDP 会话此刻无单一目的地（见类注释）。
        const QString node = m_router ? m_router(QString(), user) : QString();
        DialChoice choice;
        m_inner = makeInnerUdp(node, m_store, m_fallback, this, m_strict, &choice);
        if (!m_inner) { // 严格模式：拒绝回退，明确失败（理由同 TCP 侧）
            ++GatewayDiag::c.ccStrictRefused;
            InprocTelemetry::instance().log(
                QStringLiteral("refuse-udp:%1:%2").arg(node, choice.reason),
                QStringLiteral("[CoastCore] 严格模式拒绝 UDP 会话（节点 %1）—— %2")
                        .arg(node.isEmpty() ? QStringLiteral("-") : node, choice.reason));
            QMetaObject::invokeMethod(this, [this] {
                emit failed(QStringLiteral("严格模式：该 UDP 会话无法走进程内出站（已拒绝回退核心）"));
            }, Qt::QueuedConnection);
            return;
        }
        if (choice.inproc) {
            // 目的地此刻未知（sendTo 才有），host 先空着、首个 sendTo 用 noteUdpDst 补。
            m_connId = InprocTelemetry::instance().registerConn(
                    QString(), 0, QStringLiteral("udp"), node, user, m_tag, this,
                    [this] { closeSession(); });
        } else if (m_router && !choice.reason.isEmpty()) {
            InprocTelemetry::instance().log(
                QStringLiteral("fb-udp:%1:%2").arg(node, choice.reason),
                QStringLiteral("[CoastCore] UDP 回退核心（节点 %1）—— %2")
                        .arg(node.isEmpty() ? QStringLiteral("-") : node, choice.reason));
        }
        connect(m_inner, &IOutboundUdp::ready, this, [this] { emit ready(); });
        connect(m_inner, &IOutboundUdp::datagramReceived, this,
                [this](const QHostAddress &ip, quint16 port, const QByteArray &d) {
                    if (m_connId)
                        InprocTelemetry::instance().addDown(m_connId, d.size());
                    emit datagramReceived(ip, port, d);
                });
        connect(m_inner, &IOutboundUdp::failed, this, [this, node](const QString &r) {
            if (m_connId) {
                InprocTelemetry::instance().log(
                    QStringLiteral("fail-udp:%1:%2").arg(node, r),
                    QStringLiteral("[CoastCore] 进程内 UDP 会话失败（节点 %1）—— %2")
                            .arg(node.isEmpty() ? QStringLiteral("DIRECT") : node, r));
                InprocTelemetry::instance().unregisterConn(m_connId);
            }
            emit failed(r);
        });
        connect(m_inner, &IOutboundUdp::closed, this, [this] {
            if (m_connId)
                InprocTelemetry::instance().unregisterConn(m_connId);
            emit closed();
        });
        m_inner->associate(user);
    }

    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override
    {
        if (m_inner) {
            if (m_connId) {
                InprocTelemetry::instance().addUp(m_connId, payload.size());
                // 快照里显示最近的目标；只在变化时写，别为每个包付一次锁+字符串的代价。
                if (dstPort != m_lastDstPort || dstIp != m_lastDstIp) {
                    m_lastDstIp = dstIp;
                    m_lastDstPort = dstPort;
                    InprocTelemetry::instance().noteUdpDst(m_connId, dstIp.toString(), dstPort);
                }
            }
            m_inner->sendTo(dstIp, dstPort, payload);
        }
    }
    void closeSession() override
    {
        if (m_inner) {
            m_inner->closeSession();
        } else {
            emit closed();
        }
    }
    bool isReady() const override { return m_inner && m_inner->isReady(); }

private:
    bool m_strict = false; // 严格模式：不回退核心，见 makeInnerTcp
    CoreDialerFactory::Router m_router;
    ProxyConfigStore *m_store = nullptr;
    OutboundFactory *m_fallback = nullptr;
    QString m_tag;                   // 入口标签（进快照 metadata.type）
    quint64 m_connId = 0;            // InprocTelemetry 句柄；0 = 未登记
    QHostAddress m_lastDstIp;        // noteUdpDst 的去重（见 sendTo）
    quint16 m_lastDstPort = 0;
    IOutboundUdp *m_inner = nullptr; // parent=this
};

} // namespace

CoreDialerFactory::CoreDialerFactory(ProxyConfigStore *store, OutboundFactory *fallback)
    : m_store(store), m_fallback(fallback)
{
}

CoreDialerFactory::~CoreDialerFactory()
{
    delete m_fallback; // 本类拥有 fallback
}

void CoreDialerFactory::setRouter(Router router)
{
    m_router = std::move(router);
}

IOutboundTcp *CoreDialerFactory::createTcp(QObject *parent)
{
    return new RoutingOutboundTcp(m_strict, m_router, m_store, m_fallback, m_tag, parent);
}

IOutboundUdp *CoreDialerFactory::createUdp(QObject *parent)
{
    return new RoutingOutboundUdp(m_strict, m_router, m_store, m_fallback, m_tag, parent);
}
