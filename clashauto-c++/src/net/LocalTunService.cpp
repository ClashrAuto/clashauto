#include "LocalTunService.h"

#include "IL2Endpoint.h"
#include "NetStack.h"
#include "TunEndpoint.h"
#include "TunSession.h"
#include "core/CoreDialerFactory.h"
#include "core/CoreRouter.h"
#include "core/ProxyConfig.h"
#include "core/SelfRouteGuard.h"

namespace {
// TUN 网卡上我们这一端的地址。198.18.0.0/15 是 RFC 2544 的**基准测试保留段**，现实网络里不会
// 被路由，撞车概率比 10./172./192.168. 低得多 —— mihomo 的 TUN 也用这一段。
constexpr const char *kTunIp = "198.18.0.1";
constexpr const char *kPeerIp = "198.18.0.2";
constexpr const char *kMask = "255.255.255.0";

// 各平台想要的网卡名。macOS 传空 = 让内核挑 utun 单元（真名之后从端点问回来）。
QString desiredIfname()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Coast"); // wintun 适配器别名，SelfRouteGuard 也按类型把它排除在物理出口外
#elif defined(Q_OS_LINUX)
    return QStringLiteral("coast0");
#else
    return {};
#endif
}
} // namespace

LocalTunService::LocalTunService(QObject *parent) : QObject(parent) {}

LocalTunService::~LocalTunService()
{
    teardown();
}

QString LocalTunService::ifname() const
{
    return m_ep ? m_ep->ifname() : QString();
}

bool LocalTunService::blockedByQuicNode(const std::shared_ptr<ProxyConfigStore> &store, QString *why)
{
    if (!store)
        return false;
    const auto cfg = store->current();
    if (!cfg)
        return false;
    for (const ProxyNode &n : cfg->nodes()) {
        const QString t = n.type.toLower();
        if (t == QLatin1String("hysteria2") || t == QLatin1String("hy2")
            || t == QLatin1String("tuic")) {
            if (why)
                *why = QObject::tr("当前配置里有 %1 节点（%2）。这类节点的连接由 msquic 内部建立，"
                                   "进程内 TUN 暂时无法把它排除在自己的路由之外，开启会导致整机断网。"
                                   "请先切换到非 QUIC 节点，或改用内核 TUN。")
                               .arg(n.type, n.name);
            return true;
        }
    }
    return false;
}

bool LocalTunService::start(std::shared_ptr<ProxyConfigStore> store,
                            std::shared_ptr<RuleEngine> rules,
                            int socksFallbackPort, QString *err)
{
    if (m_active) {
        if (err)
            *err = tr("进程内 TUN 已在运行");
        return false;
    }
    if (!store) {
        if (err)
            *err = tr("没有出站配置");
        return false;
    }
    // ★ 先拦 QUIC：让用户在**点下去之前**就知道，而不是网断了才发现。
    QString why;
    if (blockedByQuicNode(store, &why)) {
        if (err)
            *err = why;
        return false;
    }

    // ① 自身流量排除必须**先于**任何出站建立。顺序反了就会有一段"路由已接管、出站还没钉住"的
    //    窗口，那段时间里新建的连接直接进环路。
    const auto ifc = SelfRouteGuard::refreshPhysicalInterface();
    if (!ifc.valid()) {
        if (err)
            *err = tr("探不到物理出口网卡，开启进程内 TUN 会导致断网，已中止");
        return false;
    }
    m_guardWasEnabled = SelfRouteGuard::enabled();
    SelfRouteGuard::setEnabled(true);
    emit logged(tr("物理出口：%1 (ifIndex=%2)").arg(ifc.name).arg(ifc.ifIndex));

    // ② 设备层
    m_ep = createTunEndpoint(this);
    if (!m_ep || !m_ep->open(desiredIfname(), err)) {
        teardown();
        return false;
    }
    const QString dev = m_ep->ifname();
    if (dev.isEmpty()) {
        if (err)
            *err = tr("拿不到 TUN 网卡名");
        teardown();
        return false;
    }
    emit logged(tr("TUN 网卡：%1").arg(dev));

    // ③ 用户态栈 + 出站
    m_net = new NetStack(quint16(socksFallbackPort < 0 ? 0 : socksFallbackPort), this);
    if (!m_net->init(err)) {
        teardown();
        return false;
    }
    if (!m_net->addNic(m_ep, m_ep->localMac(), QString::fromLatin1(kTunIp),
                       QString::fromLatin1(kMask), err)) {
        teardown();
        return false;
    }
    // TUN 上只有「本机」一个来源，登记成一个静态邻居即可（没有 ARP，见 TunEndpoint.h）。
    m_net->addDevice(QString::fromLatin1(kPeerIp), coastcore::tunPeerMac(),
                     QStringLiteral("local"));

    m_store = std::move(store);
    m_factory = new CoreDialerFactory(m_store.get(), nullptr); // 无 parent，由 teardown 手动释放
    m_factory->setStrict(socksFallbackPort <= 0); // 无回退口 = 严格：判不了就失败，不静默改道
    m_factory->setRouter(coastcore::makeRouter(m_store, std::move(rules), false));
    m_net->setOutboundFactory(m_factory);

    connect(m_ep, &IL2Endpoint::frameReceived, m_net,
            [this](const QByteArray &f) { m_net->inputFrame(m_ep, f); });

    // ④ 最后才接管路由 —— 此刻栈已经能收包，接管的瞬间就有人处理。
    m_sess = new TunSession();
    TunSession::Config c;
    c.ifname = dev;
    c.addr4 = QString::fromLatin1(kTunIp);
    c.peer4 = QString::fromLatin1(kPeerIp);
    c.mask4 = QString::fromLatin1(kMask);
    c.takeDefault = true;
    if (!m_sess->start(c, err)) {
        emit logged(tr("路由接管失败，已回滚：%1").arg(*err));
        teardown();
        return false;
    }
    emit logged(m_sess->trace().join(QLatin1Char('\n')));

    m_active = true;
    emit activeChanged();
    return true;
}

void LocalTunService::stop()
{
    if (!m_active && !m_ep)
        return;
    teardown();
    emit activeChanged();
}

// 严格逆序拆。**每一步都要做**，中间失败不能中断 —— 半截状态（路由还给着一张已经没人读的 TUN）
// 比彻底没起来危险得多。
void LocalTunService::teardown()
{
    if (m_sess) {
        m_sess->stop(); // 先还路由：让流量立刻回到物理网卡
        delete m_sess;
        m_sess = nullptr;
    }
    if (m_net) {
        delete m_net;
        m_net = nullptr;
    }
    // ★ 必须**在 NetStack 之后**删：栈里存着这个工厂的裸指针，反过来就是悬垂访问。
    if (m_factory) {
        delete m_factory;
        m_factory = nullptr;
    }
    if (m_ep) {
        m_ep->close();
        m_ep->deleteLater();
        m_ep = nullptr;
    }
    m_store.reset();
    SelfRouteGuard::setEnabled(m_guardWasEnabled);
    m_active = false;
}
