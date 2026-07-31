#include "LocalTunService.h"

#include "IL2Endpoint.h"
#include "NetStack.h"
#include "TunEndpoint.h"
#include "TunSession.h"
#include "core/CoreDialerFactory.h"
#include "core/CoreRouter.h"
#include "core/ProxyConfig.h"
#include "core/ProxyConfigBuilder.h" // 自检可喂真实订阅：从 full.yaml 解析节点
#include "core/SelfRouteGuard.h"
#include "inbound/MixedInbound.h" // 出站探针：不碰 TUN，直接用本机入站验节点

#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QHostInfo>
#include <QProcess>
#include <QSet>
#include <QTimer>

#include <cstdio>

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
    // QUIC 系节点（hysteria2 / tuic）：**曾经在这里直接拒绝启动**，因为它们的 socket 由 msquic
    // 内部创建、SelfRouteGuard 那条 fd 路径够不着 → 开 TUN 必然环路 → 整机断网。
    //
    // 现在的环路排除是**主层 + 保险层**两级（详见 docs/mihomo-replacement-gap.md 环路一节）：
    //   · 主层（协议无关、三平台同形）：给每个代理服务器地址 + 系统 DNS 加一条经物理网关的
    //     /32(/128) 主机路由（①.5 收集材料，TunSession 安装）。查表时 /32 永远比 TUN 的 /1
    //     更具体 —— 不管 socket 是谁建的、能不能打标，都出物理口。msquic 自持 socket 的问题
    //     就此绕开。
    //   · 保险层（各平台原有机制，保留不删）：socket 选项（SO_MARK / IP_UNICAST_IF /
    //     IP_BOUND_IF）+ msquic 的 QUIC_PARAM_CONN_LOCAL_ADDRESS + Linux 的 pref 90 源地址
    //     规则（那条在 /32 拿到真机证据后可撤，见 TunSession.cpp）。
    //
    // ★ 告警照留，且按**证据强度**分平台说 —— 含糊的告警等于没有告警：
    //   · Linux：真机 tcpdump 证过保险层（修前 QUIC 握手包 coast0:5/eth0:0 → 修后 0/6）；
    //     /32 主层的验证见 docs（同样是 tcpdump 记数）。
    //   · Windows / macOS：/32 主层机制相同，但**尚无本平台的真机 QUIC 验证**。
    //   真出问题的表现是整机断网，自救办法：关掉「增强」，或 config.yaml 写 coastcore: false。
    QString why;
    if (blockedByQuicNode(store, &why)) {
#if defined(Q_OS_LINUX)
        emit logged(tr("提示：配置里有 QUIC 类节点。本平台已通过 /32 主机路由 + 策略路由"
                       "把它们排除在 TUN 之外（真机抓包验证过）。"));
#else
        emit logged(tr("⚠ 配置里有 QUIC 类节点（Hysteria2/TUIC）。已按 /32 主机路由方案把代理"
                       "服务器排除在 TUN 之外（与 Linux 同一机制），但本平台**尚无**真机 QUIC "
                       "验证。若开启后整机断网，请关掉「增强」，或在 config.yaml 写 "
                       "coastcore: false。"));
#endif
    }

    // ① 自身流量排除必须**先于**任何出站建立。顺序反了就会有一段"路由已接管、出站还没钉住"的
    //    窗口，那段时间里新建的连接直接进环路。
    const auto ifc = SelfRouteGuard::refreshPhysicalInterface();
    if (!ifc.valid()) {
        if (err)
            *err = tr("探不到物理出口网卡，开启进程内 TUN 会导致断网，已中止");
        return false;
    }
    emit logged(tr("物理出口：%1 (ifIndex=%2)").arg(ifc.name).arg(ifc.ifIndex));

    // ①.5 /32 主机路由的材料：物理网关 + 要排除的地址集合（代理服务器 + 系统 DNS）。
    //    ★ 域名解析**必须发生在接管路由之前** —— 接管之后再解析，DNS 查询自己就会被 TUN
    //      抓走（/1 覆盖一切），解析永远回不来。所以这一整段都在任何路由改动之前完成，
    //      结果作为快照传给 TunSession（生命周期同一次 TUN 会话；运行中订阅变化不热更新，
    //      TUN 重启时按新配置重建 —— 这是已知局限，见 docs/mihomo-replacement-gap.md）。
    const QString gw4 = SelfRouteGuard::physicalGateway(false);
    const QString gw6 = SelfRouteGuard::physicalGateway(true);
    QStringList excl4, excl6;
    auto addExcl = [&excl4, &excl6](const QHostAddress &a) {
        if (a.isNull() || a.isLoopback() || a.isLinkLocal())
            return;
        // v4-mapped v6（::ffff:1.2.3.4）按 v4 记，路由命令认的是纯 v4 形态
        bool isV4 = false;
        const quint32 v4 = a.toIPv4Address(&isV4);
        if (isV4 || a.protocol() == QAbstractSocket::IPv4Protocol) {
            const QString s = QHostAddress(v4).toString();
            if (!excl4.contains(s))
                excl4 << s;
        } else {
            const QString s = a.toString();
            if (!excl6.contains(s))
                excl6 << s;
        }
    };
    int nDomains = 0, nSkipped = 0;
    {
        QSet<QString> seen;
        const auto snap = store->current();
        const QVector<ProxyNode> nodes = snap ? snap->nodes() : QVector<ProxyNode>();
        for (const ProxyNode &n : nodes) {
            if (n.isDirect() || n.server.isEmpty() || seen.contains(n.server))
                continue;
            seen.insert(n.server);
            const QHostAddress lit(n.server);
            if (!lit.isNull()) {
                addExcl(lit);
                continue;
            }
            ++nDomains;
            const QHostInfo hi = QHostInfo::fromName(n.server); // 阻塞解析——此刻路由还没动，安全
            if (hi.error() != QHostInfo::NoError || hi.addresses().isEmpty()) {
                // 解析不出：跳过并记日志，不让整个 TUN 启动失败 —— 这个节点开 TUN 后会环路，
                // 但用户多半也选不中它（连不上），比「因为一个死节点整个开关打不开」强。
                ++nSkipped;
                emit logged(tr("⚠ 代理服务器域名解析失败，无法为它加排除路由：%1（%2）")
                                    .arg(n.server, hi.errorString()));
                continue;
            }
            for (const QHostAddress &a : hi.addresses())
                addExcl(a);
        }
    }
    const int nProxy4 = excl4.size(), nProxy6 = excl6.size();
    const QStringList dns = SelfRouteGuard::systemDnsServers();
    for (const QString &d : dns)
        addExcl(QHostAddress(d));
    emit logged(tr("排除集合：v4 ×%1 + v6 ×%2（代理 %3/%4，域名 %5 个、失败 %6 个；DNS %7 个）"
                   "，物理网关 %8%9")
                        .arg(excl4.size())
                        .arg(excl6.size())
                        .arg(nProxy4)
                        .arg(nProxy6)
                        .arg(nDomains)
                        .arg(nSkipped)
                        .arg(dns.size())
                        .arg(gw4.isEmpty() ? tr("<v4 未探到>") : gw4,
                             gw6.isEmpty() ? QString() : QStringLiteral(" / ") + gw6));
    if (!excl4.isEmpty() && gw4.isEmpty()) {
        // ★ 没有网关，/32 装不出来 → 主层缺位 → QUIC 等打不了标的出站必然环路。
        //   宁可拒绝开启，也不开出一个「看着开了、实则整机断网」的状态。
        if (err)
            *err = tr("探不到物理默认网关（默认路由可能是 on-link），无法给 %1 个代理/DNS "
                      "地址加排除路由，开启进程内 TUN 会导致断网，已中止")
                           .arg(excl4.size());
        return false;
    }
    if (!excl6.isEmpty() && gw6.isEmpty())
        emit logged(tr("提示：有 %1 个 v6 地址但没探到 v6 网关，跳过 v6 排除路由"
                       "（当前 TUN 只接管 v4 默认路由，v6 流量本就不进 TUN）")
                            .arg(excl6.size()));

    m_guardWasEnabled = SelfRouteGuard::enabled();
    SelfRouteGuard::setEnabled(true);

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
    // 无 parent：**所有权交给 NetStack**（setOutboundFactory 取得所有权，~NetStack 会删它）。
    // 我们只留一个裸指针用于 setStrict/setRouter，teardown 里置空即可，千万别自己再删一遍。
    m_factory = new CoreDialerFactory(m_store.get(), nullptr);
    m_factory->setStrict(socksFallbackPort <= 0); // 无回退口 = 严格：判不了就失败，不静默改道
    m_factory->setRouter(coastcore::makeRouter(m_store, std::move(rules), false));
    m_net->setOutboundFactory(m_factory);

    connect(m_ep, &IL2Endpoint::frameReceived, m_net,
            [this](const QByteArray &f) { m_net->inputFrame(m_ep, f); });

    // ④ 最后才接管路由 —— 此刻栈已经能收包，接管的瞬间就有人处理。
    m_sess = new TunSession();
    TunSession::Config c;
    c.ifname = dev;
    // ★★ 两端地址**不能同号**，而且方向容易搞反（第一版就反了，表现是路由全配对但 curl 恒 000）：
    //   · 内核那侧的 TUN 网卡 = lwIP 眼里的「那台设备」 → 拿 **kPeerIp**；
    //     内核按 0.0.0.0/1 把包投进 coast0 时，源地址就选它，正对上 addDevice(kPeerIp)。
    //   · lwIP 的 netif = 「网关」 → 拿 **kTunIp**（上面 addNic 用的就是它）。
    //   写成两边都用 kTunIp 的话，addDevice 登记的那台设备根本不存在，进来的帧一律不认。
    //   已验通的 tools/gwbench/tunstack.cpp 就是这个分工（内核 kAppIp / lwIP kTunIp）。
    c.addr4 = QString::fromLatin1(kPeerIp);
    c.peer4 = QString::fromLatin1(kTunIp); // macOS 点对点的对端 = 我们（网关侧）
    c.mask4 = QString::fromLatin1(kMask);
    c.takeDefault = true;
    // /32 主机路由材料（①.5 备好的快照）。运行中订阅/节点变化不会热更新这组路由 ——
    // 重开 TUN 才重建（已知局限，文档有记）。
    c.physIfname = ifc.name;
    c.gateway4 = gw4;
    c.gateway6 = gw6;
    c.hostRoutes4 = excl4;
    c.hostRoutes6 = excl6;
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

// ———————————————————————— 整体自检 ————————————————————————
//
// ★★ 这里有一个必须避开的坑，是 TUN 端到端第一次调试时栽过的：
//   **绝不能用 QProcess::waitForFinished() 等 curl。** 那个调用只泵子进程自己的 IO，
//   **不跑主事件循环**，而 TUN fd 的 QSocketNotifier 和 lwIP 的定时器泵全挂在主事件循环上。
//   于是 curl 跑的那几秒里一帧都没被从 TUN 读走 → 必然超时 → 看起来像"TUN 不通"，
//   实际是测试驱动自己把链路掐死了（当时 A/B 实测：阻塞版 rx=1、非阻塞版 rx=61）。
//   所以这里用 QEventLoop + finished 信号，全程让事件循环转着。
// 出站探针（COAST_OUTBOUND_PROBE=1）——**完全不碰 TUN**。
//
// 存在的理由是一次真实的误判：我拿真实订阅测「TUN + Hy2」，两组都 curl=000，差点就去改
// TUN/QUIC 那边的代码 —— 可我**从没验证过这些节点经我们的进程内出站单独能不能用**。
// 前提没验就测组合，失败了根本分不清是哪一层。这个探针就是补那个前提：
//   起本机混合入站(127.0.0.1:<port>) → 用真实订阅的指定节点做出站 → curl -x 打过去
// 链路里没有 TUN、没有 lwIP、没有路由改动。它不通 = 节点或出站实现的问题，与 TUN 无关。
//
// 严格模式(fallback=0)：想回退 mihomo 就当场失败，所以 PASS 即证明是我们自己的出站拨通的。
int LocalTunService::outboundProbe()
{
    const QString yamlPath = qEnvironmentVariable("COAST_TUNSERVICE_YAML");
    const QString wantNode = qEnvironmentVariable("COAST_TUNSERVICE_NODE");
    const QString target = qEnvironmentVariableIsSet("COAST_TUNSERVICE_TARGET")
            ? qEnvironmentVariable("COAST_TUNSERVICE_TARGET")
            : QStringLiteral("http://223.5.5.5/");
    if (yamlPath.isEmpty() || wantNode.isEmpty()) {
        std::fprintf(stderr, "用法：COAST_OUTBOUND_PROBE=1 COAST_TUNSERVICE_YAML=<full.yaml> "
                             "COAST_TUNSERVICE_NODE=<节点名> [COAST_TUNSERVICE_TARGET=<url>]\n");
        return 2;
    }
    QFile yf(yamlPath);
    if (!yf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "打不开 %s\n", qUtf8Printable(yamlPath));
        return 2;
    }
    const QString yaml = QString::fromUtf8(yf.readAll());
    yf.close();
    // 让入站把每条会话的收口原因打出来 —— curl 只给一个 000，真实错误（TLS 校验失败 / 认证被拒 /
    // 连接被拒 / 传输不支持回退核心…）全在这条日志里。不设的话探针只能告诉你「不通」，不告诉你「为什么」。
    qputenv("COAST_INBOUND_VERBOSE", "1");
    auto cfg = coastcore::buildProxyConfig(yaml, wantNode, ProxyConfig::Mode::Global);
    const ProxyNode *hit = nullptr;
    if (cfg) {
        for (const ProxyNode &n : cfg->nodes()) {
            if (n.name == wantNode) { hit = &n; break; }
        }
    }
    if (!hit) {
        std::fprintf(stderr, "解析不出节点「%s」——测下去只会测到别的东西\n", qUtf8Printable(wantNode));
        return 2;
    }
    std::fprintf(stderr, "=== 出站探针（无 TUN）：节点「%s」type=%s server=%s:%u 目标=%s ===\n",
                 qUtf8Printable(hit->name), qUtf8Printable(hit->type), qUtf8Printable(hit->server),
                 unsigned(hit->port), qUtf8Printable(target));

    auto store = std::make_shared<ProxyConfigStore>();
    store->reload(cfg);
    auto *factory = new CoreDialerFactory(store.get(), nullptr);
    factory->setStrict(true); // 想回退 mihomo 就当场失败
    factory->setRouter(coastcore::makeRouter(store, nullptr, false));

    MixedInbound inbound(factory);
    const quint16 port = 17891;
    if (!inbound.listen(port)) {
        std::fprintf(stderr, "入站监听 %u 失败\n", unsigned(port));
        delete factory;
        return 2;
    }

    QProcess p;
    QEventLoop loop;
    QObject::connect(&p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop,
                     &QEventLoop::quit);
    p.start(QStringLiteral("curl"),
            {QStringLiteral("-s"), QStringLiteral("-o"), QStringLiteral("/dev/null"),
             QStringLiteral("-w"), QStringLiteral("%{http_code}"), QStringLiteral("--max-time"),
             QStringLiteral("15"), QStringLiteral("-x"),
             QStringLiteral("http://127.0.0.1:%1").arg(port), target});
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();
    const QString code = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    const bool ok = !code.isEmpty() && code != QStringLiteral("000");
    std::fprintf(stderr, "经该节点 curl=%s  %s\n", qUtf8Printable(code),
                 ok ? "✓ 出站本身没问题" : "✗ 出站就不通 —— 与 TUN 无关");
    inbound.stop();
    delete factory;
    std::fprintf(stderr, "=== 出站探针 %s ===\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int LocalTunService::selfTest()
{
    const QString target = qEnvironmentVariableIsSet("COAST_TUNSERVICE_TARGET")
            ? qEnvironmentVariable("COAST_TUNSERVICE_TARGET")
            : QStringLiteral("http://223.5.5.5/");

    auto httpCode = [](const QString &url, int timeoutSec) -> QString {
        QProcess p;
        QEventLoop loop;
        QObject::connect(&p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop,
                         &QEventLoop::quit);
        p.start(QStringLiteral("curl"),
                {QStringLiteral("-s"), QStringLiteral("-o"), QStringLiteral("/dev/null"),
                 QStringLiteral("-w"), QStringLiteral("%{http_code}"), QStringLiteral("--max-time"),
                 QString::number(timeoutSec), url});
        QTimer::singleShot(timeoutSec * 1000 + 3000, &loop, &QEventLoop::quit); // 硬兜底
        loop.exec();                                                            // ★ 事件循环全程在转
        return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    };

    std::fprintf(stderr, "=== LocalTunService 自检：目标 %s ===\n", qUtf8Printable(target));
    const QString before = httpCode(target, 8);
    std::fprintf(stderr, "起服务前： curl=%s\n", qUtf8Printable(before));
    if (before.isEmpty() || before == QStringLiteral("000")) {
        std::fprintf(stderr, "✗ 前提不成立：起服务前本来就连不通目标，后面测什么都没意义\n");
        return 2; // ★ 先验前提。少了这一步，"失败"可能只是环境本来就没网
    }

    // 出站配置：默认只装内建 DIRECT（严格模式下 PASS 即证明整条链路在进程内）。
    //
    // ★ 也可以喂**真实订阅**，用来验「TUN 开着时某个具体协议节点会不会环路」——
    //   这是 Hy2 那条唯一的验法：msquic 的 socket 我们够不着，只有真跑一次才知道
    //   QUIC_PARAM_CONN_LOCAL_ADDRESS 是不是真的把它钉住了。
    //     COAST_TUNSERVICE_YAML=<full.yaml 路径>   —— 从中解析 proxies
    //     COAST_TUNSERVICE_NODE=<节点名>           —— 选它并走 Global 模式（全部流量都过这个节点）
    //   两者都给才生效；给了但解析不出该节点会**直接失败**而不是悄悄回落 DIRECT ——
    //   否则「测了个寂寞」：以为在测 Hy2，其实一直在测直连。
    auto store = std::make_shared<ProxyConfigStore>();
    const QString yamlPath = qEnvironmentVariable("COAST_TUNSERVICE_YAML");
    const QString wantNode = qEnvironmentVariable("COAST_TUNSERVICE_NODE");
    if (!yamlPath.isEmpty() && !wantNode.isEmpty()) {
        QFile yf(yamlPath);
        if (!yf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::fprintf(stderr, "✗ 打不开 %s\n", qUtf8Printable(yamlPath));
            return 2;
        }
        const QString yaml = QString::fromUtf8(yf.readAll());
        yf.close();
        auto cfg = coastcore::buildProxyConfig(yaml, wantNode, ProxyConfig::Mode::Global);
        if (!cfg) {
            std::fprintf(stderr, "✗ 解析 %s 失败\n", qUtf8Printable(yamlPath));
            return 2;
        }
        // 前提检查：那个节点必须真的在解析结果里，且不是被降级成了 DIRECT。
        const ProxyNode *hit = nullptr;
        for (const ProxyNode &n : cfg->nodes()) {
            if (n.name == wantNode) {
                hit = &n;
                break;
            }
        }
        if (!hit) {
            std::fprintf(stderr, "✗ 解析出 %d 个节点，但没有「%s」——测下去只会测到别的东西\n",
                         int(cfg->nodes().size()), qUtf8Printable(wantNode));
            return 2;
        }
        std::fprintf(stderr, "出站：Global 模式，节点「%s」type=%s server=%s:%u（共解析出 %d 个节点）\n",
                     qUtf8Printable(hit->name), qUtf8Printable(hit->type),
                     qUtf8Printable(hit->server), unsigned(hit->port), int(cfg->nodes().size()));
        store->reload(cfg);
    } else {
        QVector<ProxyNode> nodes;
        nodes.push_back(ProxyNode::direct());
        store->reload(std::make_shared<const ProxyConfig>(nodes, QStringLiteral("DIRECT"),
                                                          ProxyConfig::Mode::Direct));
    }

    LocalTunService svc;
    QObject::connect(&svc, &LocalTunService::logged, [](const QString &l) {
        std::fprintf(stderr, "  | %s\n", qUtf8Printable(l));
    });

    QString err;
    if (!svc.start(store, nullptr, 0, &err)) { // fallback=0 → 严格：想回退核心就当场失败
        std::fprintf(stderr, "✗ 起服务失败：%s\n", qUtf8Printable(err));
        return 1;
    }

    const QString during = httpCode(target, 10);
    const bool ok = (during == before);
    std::fprintf(stderr, "接管中： curl=%s %s\n", qUtf8Printable(during),
                 ok ? "✓ 经 TUN→NetStack→进程内出站 绕回来了" : "✗ 不通（或返回码变了）");

    svc.stop();
    const QString after = httpCode(target, 8);
    std::fprintf(stderr, "停服务后： curl=%s %s\n", qUtf8Printable(after),
                 after == before ? "✓ 路由已还原" : "✗ 没还原干净");

    const bool pass = ok && after == before;
    std::fprintf(stderr, "=== LocalTunService 自检 %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
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
        delete m_net; // ★ 它会**连带删掉出站工厂**（见下）
        m_net = nullptr;
    }
    // ★★ 这里**绝不能**再 delete m_factory —— NetStack::setOutboundFactory 的契约是
    //   「取得所有权」，~NetStack 里就有一句 delete d->factory。第一版在这里又删了一遍，
    //   结果是停服务时 SIGSEGV（真机自检当场崩在这）。只置空，不释放。
    m_factory = nullptr;
    if (m_ep) {
        m_ep->close();
        m_ep->deleteLater();
        m_ep = nullptr;
    }
    m_store.reset();
    SelfRouteGuard::setEnabled(m_guardWasEnabled);
    m_active = false;
}
