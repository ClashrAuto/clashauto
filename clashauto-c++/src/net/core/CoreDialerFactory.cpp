#include "CoreDialerFactory.h"

#include "DirectOutbound.h"
#include "ProxyConfig.h"
#include "proto/OutboundRegistry.h"

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

// 把 router 给出的节点名解析成一个具体的内层 IOutboundTcp。parent 传包装器自身，随其一同析构。
IOutboundTcp *makeInnerTcp(const QString &node, ProxyConfigStore *store, OutboundFactory *fallback,
                           QObject *parent)
{
    if (node.isEmpty()) {
        return fallback->createTcp(parent); // 无路由 = 照旧走 mihomo（默认路径，行为不变）
    }
    if (node == QStringLiteral("DIRECT")) {
        return new DirectOutboundTcp(parent); // 进程内直连
    }
    // 具名节点：查快照拿到它的完整参数（type/server/port/cipher/…）。
    if (store) {
        if (const std::shared_ptr<const ProxyConfig> cfg = store->current()) {
            if (const ProxyNode *n = cfg->nodeByName(node)) {
                if (n->isDirect()) {
                    return new DirectOutboundTcp(parent); // 内建直连
                }
                // 协议出站：查注册表。已注册该 type 就用进程内协议实现；createTcp 可能返回 null
                // （creator 为空 / 内部拒绝），那时照样回退 fallback，绝不缺失功能。
                if (OutboundRegistry::instance().has(n->type)) {
                    if (IOutboundTcp *proto = OutboundRegistry::instance().createTcp(*n, parent)) {
                        return proto;
                    }
                }
            }
        }
    }
    // 未注册的协议 / 查不到节点 / 协议实现拒绝 → 回退 fallback（拨 mihomo），保证功能不缺失。
    return fallback->createTcp(parent);
}

IOutboundUdp *makeInnerUdp(const QString &node, ProxyConfigStore *store, OutboundFactory *fallback,
                           QObject *parent)
{
    if (node.isEmpty()) {
        return fallback->createUdp(parent);
    }
    if (node == QStringLiteral("DIRECT")) {
        return new DirectOutboundUdp(parent);
    }
    if (store) {
        if (const std::shared_ptr<const ProxyConfig> cfg = store->current()) {
            if (const ProxyNode *n = cfg->nodeByName(node)) {
                if (n->isDirect()) {
                    return new DirectOutboundUdp(parent);
                }
                // 协议 UDP 出站：注册表里该协议的 UdpCreator 为空（协议不支持 UDP）时 createUdp 返回
                // null，落到下面回退 fallback。
                if (OutboundRegistry::instance().has(n->type)) {
                    if (IOutboundUdp *proto = OutboundRegistry::instance().createUdp(*n, parent)) {
                        return proto;
                    }
                }
            }
        }
    }
    // 未注册 / 该协议不支持 UDP / 查不到节点 → 回退 fallback。
    return fallback->createUdp(parent);
}

// 一条 TCP 连接的路由包装器。connectTo 时才据 router 选内层，之后全权委托内层。
class RoutingOutboundTcp final : public IOutboundTcp
{
public:
    RoutingOutboundTcp(CoreDialerFactory::Router router, ProxyConfigStore *store,
                       OutboundFactory *fallback, QObject *parent)
        : IOutboundTcp(parent), m_router(std::move(router)), m_store(store), m_fallback(fallback)
    {
    }

    void connectTo(const QString &dstHost, quint16 dstPort, const QString &user) override
    {
        const QString node = m_router ? m_router(dstHost, user) : QString();
        m_inner = makeInnerTcp(node, m_store, m_fallback, this);
        // 内层信号 → 原样转发为本对象（基类）信号，NetStack 连的是本对象。
        connect(m_inner, &IOutboundTcp::established, this, [this] { emit established(); });
        connect(m_inner, &IOutboundTcp::dataReceived, this,
                [this](const QByteArray &d) { emit dataReceived(d); });
        connect(m_inner, &IOutboundTcp::failed, this,
                [this](const QString &r) { emit failed(r); });
        connect(m_inner, &IOutboundTcp::closed, this, [this] { emit closed(); });
        connect(m_inner, &IOutboundTcp::upstreamBytesWritten, this,
                [this](qint64 n) { emit upstreamBytesWritten(n); });
        // 连接建立前若已被要求暂停读，补设到内层（正常时序里 NetStack 不会在 connectTo 前调，
        // 但保持无副作用地承接）。
        if (m_readPausedPending) {
            m_inner->setReadPaused(true);
        }
        m_inner->connectTo(dstHost, dstPort, user);
    }

    void write(const QByteArray &data) override
    {
        if (m_inner) {
            m_inner->write(data);
        }
    }
    void write(const char *data, qsizetype size) override
    {
        if (m_inner) {
            m_inner->write(data, size);
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
    qint64 bytesToWrite() const override { return m_inner ? m_inner->bytesToWrite() : 0; }
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
    CoreDialerFactory::Router m_router;
    ProxyConfigStore *m_store = nullptr;
    OutboundFactory *m_fallback = nullptr;
    IOutboundTcp *m_inner = nullptr;   // parent=this，随本对象析构；connectTo 前为空
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
    RoutingOutboundUdp(CoreDialerFactory::Router router, ProxyConfigStore *store,
                       OutboundFactory *fallback, QObject *parent)
        : IOutboundUdp(parent), m_router(std::move(router)), m_store(store), m_fallback(fallback)
    {
    }

    void associate(const QString &user) override
    {
        // dstHost 传空：UDP 会话此刻无单一目的地（见类注释）。
        const QString node = m_router ? m_router(QString(), user) : QString();
        m_inner = makeInnerUdp(node, m_store, m_fallback, this);
        connect(m_inner, &IOutboundUdp::ready, this, [this] { emit ready(); });
        connect(m_inner, &IOutboundUdp::datagramReceived, this,
                [this](const QHostAddress &ip, quint16 port, const QByteArray &d) {
                    emit datagramReceived(ip, port, d);
                });
        connect(m_inner, &IOutboundUdp::failed, this, [this](const QString &r) { emit failed(r); });
        connect(m_inner, &IOutboundUdp::closed, this, [this] { emit closed(); });
        m_inner->associate(user);
    }

    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override
    {
        if (m_inner) {
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
    CoreDialerFactory::Router m_router;
    ProxyConfigStore *m_store = nullptr;
    OutboundFactory *m_fallback = nullptr;
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
    return new RoutingOutboundTcp(m_router, m_store, m_fallback, parent);
}

IOutboundUdp *CoreDialerFactory::createUdp(QObject *parent)
{
    return new RoutingOutboundUdp(m_router, m_store, m_fallback, parent);
}
