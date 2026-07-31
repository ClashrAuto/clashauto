#include "DevicesController.h"
#include "../AppConfig.h"
#include "../ClashService.h"
#include "../ConfigBuilder.h" // localGlobal6Prefixes()：与生成配置同一份实现
#include "../CoreController.h"
#include "../DeviceStore.h"
#include "../HistoryStore.h"
#include "../LanScanner.h"
#include "../net/LanGateway.h"
#include "../net/core/ProxyConfig.h"        // ProxyConfigStore/ProxyConfig（灰度：进程内出站快照）
#include "../net/core/ProxyConfigBuilder.h" // coastcore::buildProxyConfig
#include "../net/core/CoreRouter.h"        // 分流判定：与网关共用同一份实现
#include "../net/core/CoreDialerFactory.h"  // 本机入站的拨号工厂
#include "../net/inbound/MixedInbound.h"     // 本机 HTTP/SOCKS 入站
#include "../net/Socks5Client.h"             // Socks5OutboundFactory：入站的回退工厂
#include "../net/core/RuleEngine.h"
#include "../net/core/DnsResolver.h"         // DNS 旁听器（灰度：学核心 fake-ip → 域名）
#include "../net/GatewayDiag.h"              // cc= 分项计数：本机入站「进程内 vs 回退」的唯一读数

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QHostAddress>
#include <QRegularExpression>
#include <QTcpSocket>

#include <cstdio>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <algorithm>

DevicesController::DevicesController(DeviceStore *store, ClashService *clash, CoreController *core,
                                    LanGateway *gateway, HistoryStore *history,
                                    const AppConfig &config, QObject *parent)
    : QObject(parent), m_store(store), m_clash(clash), m_core(core), m_gateway(gateway),
      m_history(history), m_configDir(config.configDir), m_coastCore(config.coastcore),
      // coastcore_inbound 只当**开关**用（>0=开）：实际监听端口一律归一成 mixedPort（端口换位，
      // 见 startLocalInbound）。旧配置里写的 7891 之类具体数字在这里被吸收，不再各起各的端口。
      m_coastStrict(config.coastcoreStrict),
      m_inboundPort(config.coastcoreInboundPort > 0 ? config.mixedPort : 0),
      m_mixedPort(config.mixedPort)
{
    m_scanner = new LanScanner(this);

    // 灰度：进程内出站快照持有者 + 规则引擎（app 生命周期常驻；与网关工作线程共享同一实例）。
    m_pcfgStore = std::make_shared<ProxyConfigStore>();
    m_ruleEngine = std::make_shared<RuleEngine>();
    // DNS 旁听器：本对象持有、与网关 worker 共享。只被数据面调它的同步方法（内部有锁），
    // 异步解析(resolveReal)本单元不用，故不涉及跨线程 QDnsLookup 的问题。
    m_dnsResolver = std::make_shared<DnsResolver>();

    connect(m_scanner, &LanScanner::discovered, this, &DevicesController::onDiscovered);
    connect(m_scanner, &LanScanner::scanningChanged, this, [this](bool s) {
        if (s != m_scanning) {
            m_scanning = s;
            emit scanningChanged();
        }
        emit topologyChanged(); // 扫描完拓扑已更新（SSDP 名称迟到也借此轻触发）
    });
    // 台账变化（编辑/发现/离线）→ 刷新列表模型 + 详情。
    connect(m_store, &DeviceStore::changed, this, [this] {
        refreshModel();
        rebuildSelected();
    });
    // 新设备提醒：首轮扫描后再出现的新设备 → 发信号（main 连托盘）。
    connect(m_store, &DeviceStore::deviceAdded, this, &DevicesController::onDeviceAdded);

    // 网关自身的错误（打不开网卡 / 协议栈起不来）此前无人接听：LanGateway::deviceError 一直是
    // 悬空信号，用户只看得到 enableDevice 那句泛化的「网关未就绪（需要 root/CAP_NET_RAW…）」——
    // Windows 上真正的原因「未检测到 Npcap」就这么被吞了。转成 gatewayError 送到设备页浮动提示。
    // ensureGatewayConfigured() 每轮扫描都会重试 open，同一句错误只报一次，避免刷屏。
    if (m_gateway) {
        connect(m_gateway, &LanGateway::deviceError, this,
                [this](const QString &, const QString &msg) {
                    if (msg == m_lastGatewayErr)
                        return;
                    m_lastGatewayErr = msg;
                    emit gatewayError(msg);
                });
        // 邻居安全监视：ArpWatch 检测到「有人代理本机 / 抢我劫持的设备」→ 横幅 + 徽标 + 托盘。
        connect(m_gateway, &LanGateway::securityAlert, this, &DevicesController::onSecurityAlert);
    }
    // ★ 内核是被劫持设备的**唯一出口**。投毒一旦上了，设备的每个包（包括最终会命中 DIRECT 规则的
    //   那些）都必须走 lwIP → SOCKS 127.0.0.1:7899 → mihomo 才出得去——它没有「绕过我们直连」这条
    //   路，因为本机就是它的默认网关。所以内核没起来/挂了却还劫持着 = 该设备**彻底断网，连直连都断**。
    //   规则：内核不在跑就不上劫持（resumeProxies 开头拦掉）；跑起来了立刻补上；停了/崩了立刻全撤，
    //   让设备的 ARP 还原回真网关、掉回真直连。
    if (m_core) {
        m_coreUp = m_core->isRunning();
        connect(m_core, &CoreController::statusChanged, this,
                [this](bool, bool, bool core) { onCoreRunningChanged(core); });
    }

    // 灰度：进程内出站热更新。切节点 / 改模式 / 换订阅最终都会经核心热重载后由 pollNodes 反映到
    // nodesUpdated（selected/mode 变化）。仅在灰度开着时才据变化重建快照 + 重新推给网关。默认关时
    // 这个回调直接 return，一步不动 —— 零行为变化。
    if (m_clash) {
        connect(m_clash, &ClashService::nodesUpdated, this,
                [this](const QVector<NodeInfo> &, const QString &selected) {
                    if (!m_coastCore)
                        return;
                    // 组映射也要比：用户在组内换了节点时 mode/selected 可能都没变，但 Rule 模式
                    // 该拨的叶子已经不同了——不比这一项就会继续拨旧节点。
                    if (selected == m_ccSelected && m_clash->mode() == m_ccMode
                        && groupFingerprint(m_clash->groupLeafMap()) == m_ccGroupFp)
                        return; // 三项都没变 → 无需重建（避免每轮轮询白重建）
                    refreshCoastCore();
                });
    }

    m_secClock.start();
    m_secTimer = new QTimer(this);
    m_secTimer->setInterval(30000); // 30s 巡检一次，把停了的威胁清出横幅/徽标
    connect(m_secTimer, &QTimer::timeout, this, &DevicesController::sweepSecurityAlerts);

    // 读持久化的「新设备提醒」偏好（org/app 已在 main 设为 Coast）。
    m_newDeviceAlert = QSettings().value(QStringLiteral("devices/newDeviceAlert"), true).toBool();

    m_livenessTimer = new QTimer(this);
    m_livenessTimer->setInterval(5000);
    connect(m_livenessTimer, &QTimer::timeout, this, [this] {
        QStringList ips;
        for (const DeviceRecord &d : m_store->devices())
            if (!d.ip.isEmpty())
                ips << d.ip;
        m_scanner->refreshLiveness(ips);
    });

    m_connTimer = new QTimer(this);
    m_connTimer->setInterval(1000);
    connect(m_connTimer, &QTimer::timeout, this, &DevicesController::pollConnections);

    refreshModel();

    // 重启后自动恢复代理。「代理网络」开关是**持久**的（存在 coast.db 的 device 表），劫持是**运行时**的：
    // 上次退出时 aboutToQuit→disableAll() 把 ARP 全还原了，新进程里没有任何东西把它们重新劫持，
    // 于是开关明明还开着、设备却在走直连。而扫描/热更新只在设备页可见时才跑，用户不进那个页面
    // 就永远不会恢复。这里在启动后主动扫一轮（只在确实有设备开着代理时才扫，别给普通用户平白
    // 加一轮网段探测），拿到 IP/MAC/网关拓扑后由 onDiscovered → resumeProxies() 重新上劫持。
    if (hasProxiedDevices()) {
        QTimer::singleShot(2000, this, [this] {
            if (m_active)
                return; // 用户已经先一步进了设备页，那边已经扫过
            scan();
            m_livenessTimer->start(); // 之后跟着 IP/在线态变化持续补劫持
        });
    }

    // 灰度：按 AppConfig.coastcore 初始把开关意图推给网关一次。默认关时 setCoastCore(false,...) 是
    // no-op（网关根本不建 CoreDialerFactory）——**只在开着时**才建快照 + 装工厂，确保默认关=零行为变化。
    if (m_coastCore) {
        rebuildCoastCoreConfig();
        if (m_gateway)
            m_gateway->setCoastCore(true, m_coastStrict, m_pcfgStore, m_ruleEngine, m_dnsResolver);
        startLocalInbound();
    }
}

// 本机 HTTP/SOCKS 混合入站 —— CoastCore 的**第二个入口**。
//
// 在此之前进程内出站只服务局域网里被代理的设备，本机自己的流量 100% 走 mihomo；出站侧其实早就齐了
// （8 协议 + QUIC/TLS/uTLS/WS + 规则 + DNS），缺的一直是「字节从哪进来」。见 docs/mihomo-replacement-gap.md。
//
// 复用**同一个** ProxyConfigStore / RuleEngine 和**同一份**分流实现（coastcore::makeRouter），
// 所以本机流量与网关流量的选路结果必然一致 —— 不存在「两套逻辑要同步」。
//
// ★ 端口换位（2026-07-31 起）：入站占 **mixedPort**（系统代理指向的那个口，不用改指向），
//   mihomo 的 mixed-port 挪到 **mixedPort+1**，只作为进程内不支持的节点（grpc/salamander/tuic…）
//   的回退出口。切换顺序是安全性的全部：
//   ① 先试绑 mixedPort（核心没跑/没绑上时零窗口拿下）；
//   ② 绑不上且核心在跑 → 先把核心挪到 mixedPort+1 并**确认新口起来**，再抢绑；
//   ③ 还绑不上 = 端口在别的程序手里 → **回滚**核心回原端口，一切保持原样。
//   任何失败路径都不允许停在「系统代理指着的端口没人听」——那等于整机断网。
void DevicesController::startLocalInbound()
{
    if (m_localInbound || m_inboundPort <= 0 || !m_pcfgStore || m_switching)
        return;
    m_switching = true;
    m_switchError.clear();

    const bool swap = (m_inboundPort == m_mixedPort); // 现约定恒真；防御旧值走非换位路径
    const int corePort = swap ? m_mixedPort + 1 : m_mixedPort; // mihomo（回退出口）所在的端口

    // 回退工厂 = 拨 mihomo 的混合端口（与网关那条路的「每设备 SOCKS」不同：本机没有设备身份）。
    // 换位后 mihomo 在 corePort —— 回退口必须跟着挪，否则「不支持的节点回退核心」整条断掉，
    // 而那正是订阅里 reality+grpc / salamander 这些节点唯一的出路。
    auto *factory = new CoreDialerFactory(
        m_pcfgStore.get(), new Socks5OutboundFactory(static_cast<quint16>(corePort)));
    factory->setStrict(m_coastStrict);
    factory->setRouter(coastcore::makeRouter(m_pcfgStore, m_ruleEngine, false));
    auto *inbound = new MixedInbound(factory, this);

    // ① 先试绑：这就是「确认我们真能拿到端口」那一步。核心没跑（冷启动）或它其实没绑上
    //   mixedPort（此前被别人占了）时，这里直接成功，全程零窗口。
    bool listening = inbound->listen(static_cast<quint16>(m_inboundPort));
    bool coreMoved = false;

    if (!listening && swap && m_core && m_core->isRunning()) {
        // ② 端口在别人手里，而核心正在跑 —— 大概率是 mihomo 按 full.yaml 绑着 mixed-port。
        //    先把它挪到 corePort（重生成 full.yaml + PUT 热重载会重建 listener 完成换绑），
        //    **确认新口真的起来了**再去抢绑原端口。
        qInfo() << "本机入站: 端口换位，先把核心 mixed-port 挪到" << corePort;
        m_core->setCoreMixedPort(corePort);
        m_core->rebuildConfig();
        coreMoved = true;
        if (!waitTcpPortOpen(static_cast<quint16>(corePort), 6000)) {
            // 热重载没把新口立起来（核心版本不支持端口热换绑？）→ 重启核心兜底。
            qWarning() << "本机入站: 热重载后" << corePort << "未就绪，重启核心兜底";
            m_core->stopCore();
            m_core->startCore();
            waitTcpPortOpen(static_cast<quint16>(corePort), 8000);
        }
        // mihomo 已让出 mixedPort → 抢绑（旧 listener 的释放可能略滞后，重试 ~3s）。
        // 20ms 粒度重试：核心释放端口到我们接手之间是**唯一**一段「7890 无人监听」的窗口
        //（那期间新连接会被拒），粒度就是窗口长度的上界，所以别用 100ms 这种省事的值。
        QElapsedTimer t;
        t.start();
        while (!listening && t.elapsed() < 3000) {
            listening = inbound->listen(static_cast<quint16>(m_inboundPort));
            if (!listening)
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        }
        if (!listening) {
            // ③ 核心让出来了还绑不上 = 端口在**别的程序**手里。回滚：核心退回原端口，
            //    一切保持原样。绝不停在「核心在 corePort、系统代理指着的 mixedPort 没人听」。
            m_core->setCoreMixedPort(0);
            m_core->rebuildConfig();
            // 注意措辞：这里只能确认「原端口有人应答」，**不能**断定应答者就是核心 —— 抢走端口的
            //   那个程序同样会应答。要紧的不变量恰恰是「有人服务」，不是「谁在服务」。
            const bool served = waitTcpPortOpen(static_cast<quint16>(m_mixedPort), 6000);
            coreMoved = false;
            qWarning() << "本机入站: 端口" << m_inboundPort << "被其他程序占用，已回滚"
                       << (served ? "（原端口仍有人服务）"
                                  : "（⚠ 原端口无人应答——核心回绑失败且占用者也不在了）");
        }
    }

    if (!listening) {
        m_switchError = QStringLiteral("端口 %1 被其他程序占用，未切换（原持有者继续服务）")
                            .arg(m_inboundPort);
        emit gatewayError(m_switchError);
        delete inbound; // 从未监听成功，无会话在途
        delete factory;
        m_inboundPort = 0; // 运行态归零：开关必须反映真实状态（配置里的意图由调用方决定去留）
        m_switching = false;
        emit localInboundEnabledChanged();
        emit localInboundStatusChanged();
        emit allInProcessChanged();
        return;
    }

    m_localInbound = inbound;
    m_localInboundFactory = factory; // MixedInbound 不持有工厂，得由我们管生命周期

    if (swap && m_core) {
        // 让核心（正在跑的和**下一次启动的**）都知道 mixed-port 已挪位。冷启动路径全靠这一行：
        // 构造函数 → startLocalInbound（此刻核心还没起）→ 600ms 后 autoStartCore →
        // startCore 里 ensureFullConfig 才写 full.yaml —— mixed-port 必须在那之前定好（时序要求）。
        m_core->setCoreMixedPort(corePort);
        if (!coreMoved && m_core->isRunning())
            m_core->rebuildConfig(); // 首绑就成功但核心在跑 = 它本来就没绑上 mixedPort，把配置也挪正
    }

    qInfo() << "本机入站: HTTP/SOCKS 已在 127.0.0.1:" << m_inboundPort
            << "启用（进程内引擎；核心回退口" << corePort << "）";
    // ★ 只在 listen **成功之后**才告诉 CoreController。端口换位下系统代理本来就指 mixedPort，
    //   指向不变；这行让 proxyPort() 的内部状态与实况一致（非换位的旧路径仍靠它改指向）。
    if (m_core)
        m_core->setLocalInboundPort(m_inboundPort);
    // 读数刷新：只在入站开着时跑，关掉就停（别让一个 1s 定时器白白常驻）。
    if (!m_inboundStatTimer) {
        m_inboundStatTimer = new QTimer(this);
        m_inboundStatTimer->setInterval(1000);
        connect(m_inboundStatTimer, &QTimer::timeout, this, [this] {
            emit localInboundStatusChanged();
            // ★ 本机入站这条路**没有任何「进程内 vs 回退核心」的读数**：GatewayDiag 的采样由
            //   NetStack 的泵驱动，而本机入站根本没有 NetStack。于是「切过去了，但真的是我们
            //   在拨吗」只能靠猜 —— 测试也就无法证伪（curl 200 在回退核心时同样成立）。
            //   COAST_INBOUND_VERBOSE=1 时按变化打一行分项计数，让这个问题有直接答案。
            static const bool verbose = qEnvironmentVariableIsSet("COAST_INBOUND_VERBOSE");
            if (!verbose)
                return;
            const auto &c = GatewayDiag::c;
            static qint64 last = -1;
            const qint64 sum = c.ccInProcess + c.fbNoRoute + c.fbNodeMissing + c.fbProtoMissing
                               + c.fbUdpUnsupported + c.ccStrictRefused;
            if (sum == last)
                return;
            last = sum;
            std::fprintf(stderr,
                         "[INBOUND] cc=%lld/%lld/%lld/%lld/%lld/%lld "
                         "(进程内/无路由/无节点/缺协议/UDP不支持/严格拒绝) sessions=%d/%llu\n",
                         static_cast<long long>(c.ccInProcess), static_cast<long long>(c.fbNoRoute),
                         static_cast<long long>(c.fbNodeMissing),
                         static_cast<long long>(c.fbProtoMissing),
                         static_cast<long long>(c.fbUdpUnsupported),
                         static_cast<long long>(c.ccStrictRefused),
                         m_localInbound ? m_localInbound->activeSessions() : 0,
                         static_cast<unsigned long long>(m_localInbound ? m_localInbound->totalSessions() : 0));
            std::fflush(stderr);
        });
    }
    m_inboundStatTimer->start();
    m_switching = false;
    emit allInProcessChanged();
}

// 等 127.0.0.1:port 能接受 TCP 连接（核心换绑端口后的确认）。期间用 ExcludeUserInputEvents
// 泵事件循环 —— rebuildConfig 的 PUT /configs 得经事件循环才发得出去；用户输入被排除，
// 切换过程不会被再点一次开关打断（另有 m_switching 双保险）。
bool DevicesController::waitTcpPortOpen(quint16 port, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, port);
        if (probe.waitForConnected(250)) {
            probe.abort();
            return true;
        }
    }
    return false;
}

// 「策略组 → 叶子」的落盘缓存。**这是让用户手选在核心缺席时不丢的唯一办法。**
//
// 这张表平时来自核心 REST，是权威的：它含用户在 UI 里手选的节点，并由核心自己的
// store-selected 持久化在 cache.db 里。核心一停，我们就只剩「从 full.yaml 解析出的默认结构」
// ——每组取第一个成员——那会把用户选的节点悄悄换成别的，属于**静默误路由**，比连不上更糟。
// 所以每次拿到权威表就存一份，核心不在时重放。
QString DevicesController::groupMapCachePath() const
{
    return QDir(m_configDir).filePath(QStringLiteral("groupmap.json"));
}

void DevicesController::saveGroupMapCache(const QHash<QString, QString> &map) const
{
    QJsonObject o;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        o.insert(it.key(), it.value());
    QFile f(groupMapCachePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.close();
    }
}

QHash<QString, QString> DevicesController::loadGroupMapCache() const
{
    QHash<QString, QString> map;
    QFile f(groupMapCachePath());
    if (!f.open(QIODevice::ReadOnly))
        return map;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        const QString v = it.value().toString();
        if (!v.isEmpty())
            map.insert(it.key(), v);
    }
    return map;
}

// 一行可读状态，给设置页那一行用。没开时返回空串（QML 侧据此隐藏）。
QString DevicesController::localInboundStatus() const
{
    if (!m_localInbound)
        return QString();
    const auto human = [](quint64 b) {
        if (b < 1024)
            return QStringLiteral("%1 B").arg(b);
        if (b < 1024ull * 1024)
            return QStringLiteral("%1 KB").arg(b / 1024.0, 0, 'f', 1);
        if (b < 1024ull * 1024 * 1024)
            return QStringLiteral("%1 MB").arg(b / (1024.0 * 1024), 0, 'f', 1);
        return QStringLiteral("%1 GB").arg(b / (1024.0 * 1024 * 1024), 0, 'f', 2);
    };
    return tr("%1 活跃 · 累计 %2 · ↑%3 ↓%4")
        .arg(m_localInbound->activeSessions())
        .arg(m_localInbound->totalSessions())
        .arg(human(m_localInbound->bytesUp()), human(m_localInbound->bytesDown()));
}

// 反向切回。端口换位模式下顺序同样是安全性的全部：先停入站（mixedPort 释放）→ 把核心的
// mixed-port 换回 mixedPort 并**确认它真的接手了**（系统代理一直指着这个口）→ 接不回来就把
// 入站抢回来继续服务。宁可停在「仍是进程内」，也绝不留一个「系统代理指着空端口」的终态。
bool DevicesController::stopLocalInbound(bool restoreCore)
{
    const bool swap = m_localInbound && m_localInbound->boundPort() == m_mixedPort;
    // ★★ 放手之前先问一句「有没有人能接」。核心既没在跑、又没装（下载失败/被删）时，放手 =
    //     mixedPort 从此无人监听，而系统代理正指着它 —— 本机直接断网。这种情况**拒绝切回**，
    //     保持进程内入站继续服务并把原因摆出来（开关随之留在「开」，与真实状态一致）。
    if (swap && restoreCore && m_core && !m_core->isRunning() && !m_core->isCoreInstalled()) {
        m_switchError = QStringLiteral("mihomo 内核不可用，无法把端口 %1 交还——已保持进程内入站")
                            .arg(m_mixedPort);
        qWarning() << m_switchError;
        emit gatewayError(m_switchError);
        emit allInProcessChanged();
        return false; // 调用方须放弃整个关闭动作（见头文件）
    }
    // ★ 把「生成 full.yaml + mihomo -t 校验」提前到**入站还在服务**的此刻做完（见
    //   CoreController::prewarmConfig）：7890 的无人监听窗口 = 我们放手 → 核心接手，
    //   而那次校验子进程原本正落在窗口里，真机实测让窗口长达约 1 秒。
    if (swap && m_core && restoreCore && m_core->isRunning()) {
        m_core->setCoreMixedPort(0);
        m_core->prewarmConfig();
    }
    // 换位模式下 proxyPort() 换手前后都是 mixedPort（系统代理指向不变），这行只是内部状态归位；
    // 旧的非换位路径仍靠它把系统代理从入站口改回核心口 —— 顺序必须在拆入站之前。
    if (m_core && m_localInbound)
        m_core->setLocalInboundPort(0);
    if (m_inboundStatTimer)
        m_inboundStatTimer->stop();
    if (m_localInbound) {
        m_localInbound->stop();
        m_localInbound->deleteLater();
        m_localInbound = nullptr;
    }
    delete m_localInboundFactory; // 必须在入站销毁之后：在途连接的出站对象由工厂造出
    m_localInboundFactory = nullptr;

    if (swap && m_core) {
        m_core->setCoreMixedPort(0); // 下一次 ensureFullConfig（含冷启动）就写回 mixedPort
        if (restoreCore) {
            m_switching = true;
            bool ok = false;
            if (m_core->isRunning()) {
                m_core->rebuildConfig(); // full.yaml 回 mixedPort + force 热重载换绑
                ok = waitTcpPortOpen(static_cast<quint16>(m_mixedPort), 6000);
            }
            if (!ok) {
                // 核心没在跑（用户停过/崩了）或热重载没换绑成功 —— 起/重启它。**不做无谓的等待**：
                // 没在跑时直接启动，别先白等 6 秒（那 6 秒里 mixedPort 是空的）。
                qWarning() << "本机入站: 核心未接回" << m_mixedPort << "，重启核心兜底";
                if (m_core->isRunning())
                    m_core->stopCore();
                m_core->startCore();
                ok = waitTcpPortOpen(static_cast<quint16>(m_mixedPort), 8000);
            }
            m_switching = false;
            if (!ok) {
                // 核心接不回端口（第三方趁窗口抢了？核心起不来？）——回抢端口继续服务。
                m_switchError = QStringLiteral("核心未能接回端口 %1，已恢复进程内入站继续服务")
                                    .arg(m_mixedPort);
                qWarning() << m_switchError;
                emit gatewayError(m_switchError);
                m_inboundPort = m_mixedPort;
                startLocalInbound();
                persistLocalInbound(m_inboundPort); // 与真实状态保持一致
                emit localInboundEnabledChanged();
            }
        }
    }
    emit localInboundStatusChanged();
    emit allInProcessChanged();
    // 入站被「抢回来继续服务」时同样算没停成 —— 调用方据此保持开关为「开」。
    return m_localInbound == nullptr;
}

DevicesController::~DevicesController()
{
    // 退出路径不做「归还端口 + 等核心接手」那套（核心马上要整个停掉，白等几秒拖慢退出）；
    // 只拆入站与工厂。下次冷启动会按 config 里的意图重新走完整时序。
    stopLocalInbound(false); // m_localInbound 有 parent 会自动收，但工厂是裸指针，且必须晚于入站销毁
}

bool DevicesController::hasProxiedDevices() const
{
    if (!m_store)
        return false;
    for (const DeviceRecord &d : m_store->devices())
        if (d.proxyEnabled)
            return true;
    return false;
}

void DevicesController::onCoreRunningChanged(bool up)
{
    if (up == m_coreUp)
        return;
    m_coreUp = up;
    if (!up) {
        // 内核停了/崩了：立刻撤销全部劫持并还原 ARP。台账里的「代理网络」开关**不动**——它是持久
        // 意图，内核回来时 resumeProxies 会自动补上。宁可让设备掉回真直连，也不能把它悬在一个
        // 没有出口的劫持里（那是彻底断网，比不代理糟得多）。
        // ★ coastcore 开着时**不撤**：进程内引擎就是出口，设备不会悬在「没有出口的劫持」里。
        //   （判不了的少数连接会回退到已经不在的 mihomo 而失败——那是可见的个别失败，
        //   远好于把全部设备踢回真直连、彻底绕过代理。严格模式下连这种回退都没有。）
        if (m_coastCore) {
            emit gatewayError(QStringLiteral("核心已停止，但进程内内核开着——设备代理继续由本程序接管"));
            m_resumeErr.clear();
            return;
        }
        if (m_gateway && !m_gateway->activeDevices().isEmpty()) {
            m_gateway->disableAll();
            m_armedIp.clear();
            emit gatewayError(QStringLiteral("核心已停止，已暂时撤销设备代理（设备恢复直连）"));
        }
        m_resumeErr.clear();
        return;
    }
    // 内核回来了：不必等下一轮扫描，立刻用现有拓扑把劫持补回去。首轮扫描之前拓扑还是空的
    //（网关都没配置），这时候试只会白报一句「网关未就绪」——交给 onDiscovered 那次就行。
    if (m_firstScanDone)
        resumeProxies();
}

// ———————————————————————— 系统睡眠 / 唤醒 ————————————————————————
// 本机一睡，用户态栈就停了转发，可设备的 ARP 还钉在本机 MAC 上 —— 合盖睡眠 = 那台设备直接断网，
// 而且比关机高频得多（关机路径早就有还原，睡眠这条一直是空的）。
// 挂起前必须**同步**撤劫持 + 发还原 ARP：disableAll() 是 BlockingQueued，返回时帧已经写到网卡上。
void DevicesController::handleSleep()
{
    if (!m_gateway || m_gateway->activeDevices().isEmpty())
        return;
    std::fprintf(stderr, "[POWER] sleep: withdrawing %lld device proxy(ies)\n",
                 static_cast<long long>(m_gateway->activeDevices().size()));
    std::fflush(stderr);
    m_gateway->disableAll();
    m_armedIp.clear();
    m_resumeErr.clear();
    // 台账里的「代理网络」开关不动 —— 那是持久意图，醒来由 handleWake 补回去。
}

void DevicesController::handleWake()
{
    if (!hasProxiedDevices())
        return;
    std::fprintf(stderr, "[POWER] wake: rescanning to re-arm device proxy\n");
    std::fflush(stderr);
    // 睡了一觉，IP/网关/网卡都可能变了（换了网络、DHCP 续租），必须重扫拓扑而不是直接上劫持。
    // 扫描结果经 onDiscovered → ensureGatewayConfigured + resumeProxies 完成补挂。
    scan();
    if (!m_livenessTimer->isActive())
        m_livenessTimer->start();
}

void DevicesController::resumeProxies()
{
    if (!m_gateway)
        return;
    // ★ 内核不在跑就一台都不上（见构造函数里那段说明）。开关仍然开着，等 onCoreRunningChanged
    //   在内核起来时补。少了这道门禁，开机时序（劫持 2s 就绪、内核可能还在加载 rule-provider，
    //   或正等 TUN 的 UAC 确认）就会把设备劫持到一个空出口上，表现为「重启后设备全网断」。
    //   ★ coastcore 开着时**不适用**：那条数据面（DNS + TCP + UDP）整条都在本进程内，
    //     进程内引擎自己就是出口，不存在「空出口」。此前这道门禁不看 coastcore，于是 mihomo
    //     一停就把劫持全撤——哪怕进程内引擎完全有能力接管。那正是「完全替换 mihomo」真正的
    //     拦路虎：数据面早就不需要核心了，app 却仍把核心存活当成能否代理的前提。
    if (m_core && !m_core->isRunning() && !m_coastCore) {
        if (qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG"))
            std::fprintf(stderr, "[RESUME] 内核未运行，本轮不上劫持\n"), std::fflush(stderr);
        return;
    }
    const QStringList active = m_gateway->activeDevices();
    for (const DeviceRecord &d : m_store->devices()) {
        if (!d.proxyEnabled || !d.proxyable() || !d.online || d.ip.isEmpty())
            continue;
        const bool armed = active.contains(d.mac);
        if (armed && m_armedIp.value(d.mac) == d.ip)
            continue; // 已在劫持，且还是同一个 IP —— 无事可做
        if (armed)
            m_gateway->disableDevice(d.mac); // IP 变了：按旧 IP 的劫持早已无效，拆了重上
        // 真正上第一台之前补一次配置重生成：full.yaml 里的 coast-gateway listener(7899) + 每设备
        // IN-USER 规则是 ConfigBuilder 从 coast.db 读出来生成的。内核若是在设备开关落库**之前**
        // 起来的（或用的是一份旧 full.yaml），那份配置里根本没有网关口 → 拨 7899 直接被拒 → 设备
        // 全断。每会话只做一次，避免每轮扫描都热重载。
        if (!m_resumeConfigSynced && m_core) {
            m_resumeConfigSynced = true;
            m_core->rebuildConfig();
        }
        QString err;
        const bool ok = m_gateway->enableDevice(d.mac, d.ip, DeviceStore::socksUser(d.mac), &err);
        // 恢复劫持的成败在 headless（网关联调、树莓派）下**没有任何出口**——gatewayError 只连到
        // 设备页的浮动提示。一行 stderr，定位「重启后没接上代理」时这是唯一凭据。
        if (qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG")) {
            std::fprintf(stderr, "[RESUME] %s ip=%s -> %s%s\n", d.mac.toUtf8().constData(),
                         d.ip.toUtf8().constData(), ok ? "armed" : "FAILED: ",
                         ok ? "" : err.toUtf8().constData());
            std::fflush(stderr);
        }
        if (ok) {
            m_armedIp.insert(d.mac, d.ip);
            m_resumeErr.remove(d.mac);
        } else {
            m_armedIp.remove(d.mac);
            // 每轮发现都会重试，同一条原因只报一次，免得 5s 刷一次提示条。
            if (!err.isEmpty() && m_resumeErr.value(d.mac) != err) {
                m_resumeErr.insert(d.mac, err);
                emit gatewayError(err);
            }
        }
    }
}

QString DevicesController::localIp() const { return m_scanner ? m_scanner->localIp() : QString(); }
QString DevicesController::gatewayIp() const { return m_scanner ? m_scanner->gatewayIp() : QString(); }
int DevicesController::deviceCount() const { return m_store ? m_store->devices().size() : 0; }

// 概览条上的「今日总量」：直接把台账里每台设备的 todayUp/Down 加起来（十几台设备，随 UI 绑定
// 每秒求一次和的开销可以忽略；也省得再维护一份会和台账走神的缓存）。
// 注意这里天然只统计**归属到设备**的流量：归不到任何设备的连接不进任何一台的 today 计数。
double DevicesController::totalTodayUp() const
{
    qint64 sum = 0;
    if (m_store)
        for (const DeviceRecord &d : m_store->devices())
            sum += d.todayUp;
    return static_cast<double>(sum);
}

double DevicesController::totalTodayDown() const
{
    qint64 sum = 0;
    if (m_store)
        for (const DeviceRecord &d : m_store->devices())
            sum += d.todayDown;
    return static_cast<double>(sum);
}
bool DevicesController::gatewayReady() const { return m_gateway && m_gateway->isAvailable(); }

void DevicesController::ensureGatewayConfigured()
{
    if (!m_gateway || !m_scanner)
        return;
    // 用当前扫描到的拓扑配置网关。**每张物理网卡都配**——有线接 A 路由、WiFi 接 B 路由时，
    // 两个网段的设备都要能代理；LanGateway 按设备 IP 落在哪张卡的网段自动选对应那套。
    // 网关 MAC 需从 ARP 表拿到；扫描过至少一轮后才有值（每轮都会重新 configure 刷新）。
    QVector<LanGateway::NicSpec> specs;
    const QVector<LanScanner::NicInfo> nics = m_scanner->physicalNics();
    specs.reserve(nics.size());
    for (const LanScanner::NicInfo &n : nics) {
        LanGateway::NicSpec s;
        s.ifname = n.name;
        s.localIp = n.ip;
        s.localMac = n.mac;
        s.gatewayIp = n.gatewayIp;
        s.gatewayMac = n.gatewayMac;
        s.netmask = n.netmask;
        s.routerLinkLocal6 = n.routerLinkLocal6; // IPv6 拓扑（可空 → 该卡 v6 劫持 no-op）
        s.routerMac6 = n.routerMac6;
        s.localGlobal6 = n.localGlobal6;
        s.prefix6 = n.prefix6;
        specs.append(s);
    }
    m_gateway->configure(specs, DeviceStore::kGatewayPort);
    // 起来了就清掉去重记忆：之后再坏（拔网卡等）还要能重新报一次。
    if (m_gateway->isAvailable())
        m_lastGatewayErr.clear();
}

void DevicesController::onDeviceAdded(const QString &mac)
{
    // 首轮扫描的设备在 mergeDiscovered 内触发（此时 m_firstScanDone 仍为 false）→ 不提醒，避免刷屏。
    if (!m_firstScanDone || !m_newDeviceAlert)
        return;
    const DeviceRecord *d = m_store->find(mac);
    if (d && !d->isSelf)
        emit newDeviceFound(d->displayName());
}

void DevicesController::setNewDeviceAlert(bool on)
{
    if (on == m_newDeviceAlert)
        return;
    m_newDeviceAlert = on;
    QSettings().setValue(QStringLiteral("devices/newDeviceAlert"), on);
    emit newDeviceAlertChanged();
}

// ———————————————————————— 邻居安全监视（ArpWatch） ————————————————————————
// kind 与 ArpWatch::Kind 对齐（不引 net 头，避免 UI 层依赖数据面实现）。
namespace {
constexpr int kKindSelfImpersonated = 0; // 有人代理本机
constexpr int kKindDeviceContended = 1;  // 我劫持的设备被别人也在抢
} // namespace

void DevicesController::onSecurityAlert(int kind, const QString &offenderMac,
                                        const QString &subjectIp, const QString &subjectMac)
{
    const qint64 now = m_secClock.elapsed();
    const QString key = QString::number(kind) + '|' + offenderMac + '|' + subjectIp;

    SecAlert &a = m_secAlerts[key];
    const bool isNew = a.firstMs == 0;
    a.kind = kind;
    a.offenderMac = offenderMac;
    a.subjectIp = subjectIp;
    a.mac = subjectMac;
    a.lastMs = now;
    if (isNew)
        a.firstMs = now;
    // 被抢设备的展示名（有 mac 才查得到；查不到就留空，QML 回落到 IP/MAC）。
    if (!subjectMac.isEmpty()) {
        const DeviceRecord *d = m_store->find(subjectMac);
        a.name = d ? d->displayName() : QString();
    }

    // 托盘提醒：同一威胁 30 分钟内只弹一次（m_secLastNotify 不随 dismiss 清）。
    qint64 &lastNotify = m_secLastNotify[key];
    if (lastNotify == 0 || now - lastNotify >= kSecRenotifyMs) {
        lastNotify = now;
        QString title, body;
        if (kind == kKindDeviceContended) {
            title = tr("设备被争抢");
            const QString who = a.name.isEmpty() ? (subjectIp.isEmpty() ? subjectMac : subjectIp)
                                                 : a.name;
            body = tr("%1 也在劫持你代理的设备「%2」").arg(offenderMac, who);
        } else {
            title = tr("检测到 ARP 欺骗");
            body = tr("%1 正在冒充网关或本机，可能在监听/代理你的流量").arg(offenderMac);
        }
        emit securityAlertRaised(title, body);
    }

    refreshContended();
    if (!m_secTimer->isActive())
        m_secTimer->start();
    emit securityChanged();
}

void DevicesController::sweepSecurityAlerts()
{
    const qint64 now = m_secClock.elapsed();
    bool changed = false;
    for (auto it = m_secAlerts.begin(); it != m_secAlerts.end();) {
        if (now - it.value().lastMs > kSecTtlMs) {
            it = m_secAlerts.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    // 顺便老化通知去重表（放宽到 2× 重报间隔），别让它无界增长。
    for (auto it = m_secLastNotify.begin(); it != m_secLastNotify.end();) {
        if (now - it.value() > kSecRenotifyMs * 2)
            it = m_secLastNotify.erase(it);
        else
            ++it;
    }
    if (m_secAlerts.isEmpty())
        m_secTimer->stop();
    if (changed) {
        refreshContended();
        emit securityChanged();
    }
}

void DevicesController::refreshContended()
{
    QSet<QString> macs;
    for (const SecAlert &a : m_secAlerts)
        if (a.kind == kKindDeviceContended && !a.mac.isEmpty())
            macs.insert(a.mac);
    m_model.setContended(macs);
}

QVariantList DevicesController::securityAlerts() const
{
    // 按首次发现时间稳定排序，避免横幅里几条告警每次刷新乱跳。
    QVector<const SecAlert *> v;
    v.reserve(m_secAlerts.size());
    for (auto it = m_secAlerts.constBegin(); it != m_secAlerts.constEnd(); ++it)
        v.append(&it.value());
    std::sort(v.begin(), v.end(),
              [](const SecAlert *a, const SecAlert *b) { return a->firstMs < b->firstMs; });
    QVariantList out;
    out.reserve(v.size());
    for (const SecAlert *a : v) {
        QVariantMap m;
        m.insert(QStringLiteral("kind"), a->kind);
        m.insert(QStringLiteral("offenderMac"), a->offenderMac);
        m.insert(QStringLiteral("subjectIp"), a->subjectIp);
        m.insert(QStringLiteral("mac"), a->mac);
        m.insert(QStringLiteral("name"), a->name);
        out.append(m);
    }
    return out;
}

void DevicesController::dismissSecurityAlerts()
{
    if (m_secAlerts.isEmpty())
        return;
    m_secAlerts.clear(); // 威胁若仍在，下次 ArpWatch 观察到会重新加入（但托盘去重表还在，不会又弹）
    m_secTimer->stop();
    refreshContended();
    emit securityChanged();
}

void DevicesController::onDiscovered(const QVector<DeviceRecord> &devices)
{
    m_store->mergeDiscovered(devices);
    m_firstScanDone = true; // 首轮已并完 → 之后 deviceAdded 才提醒
    // 离线判定：本轮快照没刷新到、且 lastSeen 超 15s 的设备置离线（宽限 15s≈3 轮 5s 热更新，
    // 避免单次探测丢包造成的在线/离线抖动）。先收集 mac 再统一标记，避免边遍历边改的再入。
    // 本机（当前仍存在的网卡 MAC）永不判离线：程序在跑，本机必然在线；它的在线态不该依赖
    // ARP/探测（开增强模式时正是这里把本机判成了掉线）。网卡被拔掉 → 不在 localMacs 里 → 照常判离线。
    const QDateTime now = QDateTime::currentDateTime();
    const QSet<QString> selfMacs = m_scanner ? m_scanner->localMacs() : QSet<QString>();
    QStringList stale;
    for (const DeviceRecord &d : m_store->devices())
        if (d.online && !selfMacs.contains(d.mac) && d.lastSeen.isValid()
            && d.lastSeen.secsTo(now) > 15)
            stale << d.mac;
    for (const QString &mac : stale)
        m_store->markOffline(mac);
    // 扫描后拓扑（含网关 MAC）已知 → 配置网关，使 gatewayReady 反映真实可用性。
    ensureGatewayConfigured();
    syncLanPrefixRules();
    // 台账里开着代理、当前却没在劫持的设备（重启后 / 换了 IP）在这里补上。
    resumeProxies();
    // mergeDiscovered/markOffline 会 emit changed → refreshModel/rebuildSelected 已连；此处补拓扑通知。
    emit topologyChanged();
    emit overviewChanged();
}

// 本机 IPv6 前缀变了 → 重新生成 full.yaml（+ 热重载）。每轮扫描调一次。
//
// 为什么非有这一步不可：ConfigBuilder::applyPrivateNetworkRules() 会把本机各网卡的 v6 全局单播
// 前缀写成 IP-CIDR6 DIRECT 规则。v4 那三段 RFC1918 是常量、永远不会过期，v6 却是路由器 RA 下发的
// **全局**前缀（家宽上就是 240e:/2408: 这种），换 Wi-Fi、换 ISP、运营商重新委派前缀都会变。
// 而 full.yaml 只在设置/规则/订阅/设备开关变动时才重建 —— 不补这一刀，配置里那条前缀就会在
// 你换网络的那一刻起一直是旧的：被代理设备发往新网关 v6 地址的流量匹配不上，落 MATCH 出海。
//
// 挂在 onDiscovered 上而不是新起一个轮询：扫描本来就在跑，且**恰好**在需要它的场景下跑
//（有设备开着代理时 m_livenessTimer 常驻，见构造函数里那段自动恢复代理的注释）。没有被代理设备时
// 不扫也无所谓 —— 那时没人经网关出网，这条规则也就没有受众。
//
// 只在**集合真的变了**时才重建：rebuildConfig 会热重载核心（clearConnections 开着时还会断连接），
// 每轮扫描都来一次是灾难。首轮只记基线不重建 —— 那份 full.yaml 是核心启动前刚生成的，本来就是新的。
void DevicesController::syncLanPrefixRules()
{
    const QStringList now = ConfigBuilder::localGlobal6Prefixes();
    if (!m_lanPrefixSynced) {
        m_lanPrefixSynced = true;
        m_lanPrefixes6 = now;
        return;
    }
    if (now == m_lanPrefixes6) {
        return;
    }
    m_lanPrefixes6 = now;
    if (m_core) {
        m_core->rebuildConfig();
    }
}

void DevicesController::refreshModel()
{
    m_model.setDevices(m_store->devices());
    emit overviewChanged();
}

void DevicesController::scan()
{
    if (m_scanner) {
        m_lastScan.restart();
        m_scanner->scanFull();
    }
}

void DevicesController::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active) {
        // 进页面扫一次——但**别每次切回来都重扫**。全量扫描是这个页面最重的动作（整段网段的
        // TCP 探测），来回点几次导航就会反复触发；在此期间 5s 的 liveness 热更新已经在维持
        // 在线态/流量了，30s 内切回来没必要再来一轮。
        if (!m_lastScan.isValid() || m_lastScan.elapsed() > kRescanMinIntervalMs)
            scan();
        m_livenessTimer->start();
        m_connTimer->start();
        m_clock.restart();
    } else {
        m_connTimer->stop();
        // 还有设备开着代理时，5s 的在线态热更新**不能停**：设备换 IP / 掉线再上线之后要靠它
        // 触发 resumeProxies() 把劫持重新挂上。页面一关就停，等于代理只在设备页开着时才自愈。
        if (!hasProxiedDevices())
            m_livenessTimer->stop();
    }
}

void DevicesController::select(const QString &mac)
{
    if (mac == m_selectedMac)
        return;
    m_selectedMac = mac;
    const DeviceRecord *d = m_store->find(mac);
    m_connModel.setSourceIp(d ? d->ip : QString());
    // 网关代理的连接 sourceIP=127.0.0.1,还要靠 dev-<mac> 用户名归属(与 IP 取或)。
    m_connModel.setUser(d ? DeviceStore::socksUser(d->mac) : QString());
    refreshTopDomains(true); // 换设备：立刻重查，别让详情先闪一下上一台的域名
    rebuildSelected();
    emit selectedChanged();
}

void DevicesController::rebuildSelected()
{
    m_selectedDevice.clear();
    const DeviceRecord *d = m_selectedMac.isEmpty() ? nullptr : m_store->find(m_selectedMac);
    if (d) {
        m_selectedDevice["mac"] = d->mac;
        m_selectedDevice["ip"] = d->ip;
        m_selectedDevice["name"] = d->displayName();
        m_selectedDevice["alias"] = d->alias;
        m_selectedDevice["autoName"] = d->autoName;
        m_selectedDevice["model"] = d->model;
        m_selectedDevice["vendor"] = d->vendor;
        m_selectedDevice["typeKey"] = DeviceStore::typeKey(d->effectiveType());
        m_selectedDevice["typeOverride"] = DeviceStore::typeKey(d->typeOverride);
        m_selectedDevice["online"] = d->online;
        m_selectedDevice["isSelf"] = d->isSelf;
        m_selectedDevice["isGateway"] = d->isGateway;
        m_selectedDevice["proxyEnabled"] = d->proxyEnabled;
        m_selectedDevice["policyMode"] = DeviceStore::modeKey(d->policyMode);
        m_selectedDevice["policyTarget"] = d->policyTarget;
        m_selectedDevice["rateUp"] = static_cast<double>(d->rateUp);
        m_selectedDevice["rateDown"] = static_cast<double>(d->rateDown);
        m_selectedDevice["connCount"] = d->connCount;
        m_selectedDevice["sessionUp"] = static_cast<double>(d->sessionUp);
        m_selectedDevice["sessionDown"] = static_cast<double>(d->sessionDown);
        m_selectedDevice["totalUp"] = static_cast<double>(d->totalUp);
        m_selectedDevice["totalDown"] = static_cast<double>(d->totalDown);
        m_selectedDevice["todayUp"] = static_cast<double>(d->todayUp);
        m_selectedDevice["todayDown"] = static_cast<double>(d->todayDown);
        m_selectedDevice["firstSeen"] = d->firstSeen.isValid()
                                            ? d->firstSeen.toString("yyyy-MM-dd") : QString();
        m_selectedDevice["onlineSince"] = d->onlineSince.isValid()
                                              ? d->onlineSince.toMSecsSinceEpoch() : 0;
        // 能否开代理：本机/网关不可，跨网段（另一张网卡那边的网络）也不可——二层端点只绑主网卡。
        m_selectedDevice["proxyable"] = d->proxyable();
        m_selectedDevice["inLanSubnet"] = d->inLanSubnet;
        // 开关为什么不能开（QML 据此给一句人话）：""=可开 / self / gateway / foreign / offline
        m_selectedDevice["proxyBlockReason"] =
            d->isSelf      ? QStringLiteral("self")
            : d->isGateway ? QStringLiteral("gateway")
            : !d->inLanSubnet ? QStringLiteral("foreign")
            : (!d->online || d->ip.isEmpty()) ? QStringLiteral("offline")
                                             : QString();

        // Top 域名 + 近 7 天用量：都来自历史库（跨重启保留），按 TTL 节流，见 refreshTopDomains。
        refreshTopDomains(false);
        m_selectedDevice["topDomains"] = m_topDomains;
        m_selectedDevice["recentDays"] = m_recentDays;
    }
    emit selectedChanged();
}

// 「常用域名」和「近 7 天用量」都来自历史库。rebuildSelected 每秒都会跑（流量聚合完就刷一次
// 详情），每秒查两次 SQL 纯属浪费——按 5s TTL 节流；换选中设备时 force=true 立刻重查，
// 否则点开一台设备会先看到上一台的数据。
void DevicesController::refreshTopDomains(bool force)
{
    if (!m_history || m_selectedMac.isEmpty()) {
        m_topDomains.clear();
        m_recentDays.clear();
        return;
    }
    if (!force && m_topDomainsMac == m_selectedMac && m_topDomainsAge.isValid()
        && m_topDomainsAge.elapsed() < kTopDomainsTtlMs)
        return;

    m_topDomainsMac = m_selectedMac;
    m_topDomainsAge.restart();

    m_topDomains.clear();
    for (const HistoryStore::DomainTotal &d : m_history->topDomains(m_selectedMac, 7, 5)) {
        QVariantMap e;
        e["host"] = d.host;
        e["bytes"] = static_cast<double>(d.bytes);
        m_topDomains.append(e);
    }
    m_recentDays.clear();
    for (const HistoryStore::DayTotal &d : m_history->dailyTraffic(m_selectedMac, 7)) {
        QVariantMap e;
        e["day"] = d.day;
        e["up"] = static_cast<double>(d.up);
        e["down"] = static_cast<double>(d.down);
        m_recentDays.append(e);
    }
}

void DevicesController::setProxyEnabled(const QString &mac, bool on)
{
    const DeviceRecord *d = m_store->find(mac);
    if (!d)
        return;
    // 关闭永远允许（离线设备也得能撤销之前开的代理）；开启才做这些校验。
    if (on) {
        if (d->isSelf || d->isGateway) {
            emit gatewayError(d->isSelf
                                  ? QStringLiteral("本机不需要（也不能）代理自己")
                                  : QStringLiteral("网关是路由器本身，劫持它会打瘫整个网络"));
            return;
        }
        if (!d->inLanSubnet) {
            emit gatewayError(QStringLiteral("该设备不在本机任何一张网卡的网段内，无法代理"
                                            "（透明网关只能劫持与本机同网段的设备）"));
            return;
        }
        if (!d->online || d->ip.isEmpty()) {
            emit gatewayError(QStringLiteral("设备已离线，等它上线后再开启代理"));
            return;
        }
    }
    const QString ip = d->ip;
    m_store->setProxyEnabled(mac, on);
    m_store->save(); // 立刻落盘，供 ConfigBuilder 从库里读出来生成网关 listener + auth/IN-USER
    if (m_core)
        m_core->rebuildConfig(); // 重生成 full.yaml + 热重载 mihomo（网关口 + 每设备身份就绪）
    if (m_gateway) {
        ensureGatewayConfigured();
        if (on) {
            QString err;
            if (m_gateway->enableDevice(mac, ip, DeviceStore::socksUser(mac), &err)) {
                m_armedIp.insert(mac, ip); // 记住劫持所用的 IP（resumeProxies 据此发现 IP 变了）
                m_resumeErr.remove(mac);
            } else {
                m_armedIp.remove(mac);
                if (!err.isEmpty())
                    emit gatewayError(err);
            }
        } else {
            m_gateway->disableDevice(mac);
            m_armedIp.remove(mac);
            m_resumeErr.remove(mac);
        }
    }
    emit topologyChanged(); // gatewayReady 可能变化
}

void DevicesController::setAlias(const QString &mac, const QString &alias)
{
    m_store->setAlias(mac, alias.trimmed());
}

void DevicesController::setTypeOverride(const QString &mac, const QString &typeKey)
{
    m_store->setTypeOverride(mac, DeviceStore::typeFromKey(typeKey));
}

void DevicesController::setPolicy(const QString &mac, const QString &modeKey, const QString &target)
{
    m_store->setPolicy(mac, DeviceStore::modeFromKey(modeKey), target.trimmed());
    m_store->save();             // 立刻落盘，供 ConfigBuilder
    if (m_core)
        m_core->rebuildConfig(); // 生成 IN-USER 规则 + 热重载
}

void DevicesController::closeDeviceConnections(const QString &mac)
{
    const DeviceRecord *d = m_store->find(mac);
    if (!d || !m_clash)
        return;
    const QString ip = d->ip;
    const QString user = DeviceStore::socksUser(mac); // 网关代理连接靠它归属(sourceIP=127.0.0.1)
    m_clash->fetchConnections([this, ip, user](QJsonArray arr) {
        for (const QJsonValue &v : arr) {
            const QJsonObject meta = v.toObject().value("metadata").toObject();
            const bool mine = meta.value("sourceIP").toString() == ip
                              || (!user.isEmpty() && meta.value("inboundUser").toString() == user);
            if (mine)
                m_clash->closeConnection(v.toObject().value("id").toString());
        }
    });
}

void DevicesController::exportCsv()
{
    const QString path = QFileDialog::getSaveFileName(nullptr, QStringLiteral("导出设备列表"),
                                                      QStringLiteral("devices.csv"),
                                                      QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit gatewayError(QStringLiteral("导出失败：无法写入 ") + path);
        return;
    }
    auto q = [](const QString &s) -> QString { // CSV 转义：含逗号/引号/换行则加引号并转义引号
        if (s.contains(',') || s.contains('"') || s.contains('\n'))
            return '"' + QString(s).replace('"', "\"\"") + '"';
        return s;
    };
    QTextStream out(&f); // Qt6 QTextStream 默认 UTF-8
    out << "name,ip,mac,type,vendor,model,online,proxied,totalDown,totalUp,firstSeen\n";
    for (const DeviceRecord &d : m_store->devices()) {
        out << q(d.displayName()) << ',' << d.ip << ',' << d.mac << ','
            << DeviceStore::typeKey(d.effectiveType()) << ',' << q(d.vendor) << ',' << q(d.model)
            << ',' << (d.online ? '1' : '0') << ',' << (d.proxyEnabled ? '1' : '0') << ','
            << d.totalDown << ',' << d.totalUp << ','
            << (d.firstSeen.isValid() ? d.firstSeen.toString(Qt::ISODate) : QString()) << '\n';
    }
    f.close();
    emit csvExported(path);
}

void DevicesController::pollConnections()
{
    if (!m_clash || m_connInFlight)
        return;
    m_connInFlight = true;
    m_clash->fetchConnections([this](QJsonArray arr) {
        m_connInFlight = false;
        QVariantList list;
        list.reserve(arr.size());
        for (const QJsonValue &v : arr) {
            const QJsonObject c = v.toObject();
            const QJsonObject meta = c.value("metadata").toObject();
            QVariantMap m;
            QString host = meta.value("host").toString();
            if (host.isEmpty())
                host = meta.value("destinationIP").toString();
            QString type = meta.value("type").toString();
            if (type.isEmpty())
                type = meta.value("network").toString();
            const QJsonArray chains = c.value("chains").toArray();
            m["host"] = host;
            m["type"] = type;
            m["chain"] = chains.isEmpty() ? QStringLiteral("-") : chains.first().toString();
            m["id"] = c.value("id").toString();
            m["start"] = c.value("start").toString(); // 详情连接列表按它把新连接排在最上面
            m["download"] = static_cast<qlonglong>(c.value("download").toInteger());
            m["upload"] = static_cast<qlonglong>(c.value("upload").toInteger());
            m["sourceIP"] = meta.value("sourceIP").toString();
            m["inboundUser"] = meta.value("inboundUser").toString();
            m["offline"] = false;
            list.append(m);
        }
        m_connModel.setRaw(list); // 详情连接列表（按选中设备 sourceIP 过滤）
        aggregate(list);
    });
}

void DevicesController::aggregate(const QVariantList &conns)
{
    // dt：距上一拍毫秒。首拍或过久用 1000ms 兜底。
    qint64 dt = m_clock.isValid() ? m_clock.restart() : 1000;
    if (dt <= 0)
        dt = 1000;

    // 每连接 id → 上次累计字节，逐连接取增量（>=0），避免连接关闭导致的和值回退。
    QHash<QString, QPair<qint64, qint64>> &connBytes = m_connBytes; // id → (down,up)（跨拍保留）
    QSet<QString> seen;
    QHash<QString, QPair<qint64, qint64>> devDelta; // mac → (dDown,dUp) 本拍
    QHash<QString, int> devConn;                    // mac → 活动连接数

    // ip → mac、以及 dev-<mac> 用户名 → mac 两张映射（本拍）。
    // 后者是透明网关代理设备**能看到流量速率的关键**:被代理设备的流量是
    // 设备 → 用户态栈 → SOCKS(127.0.0.1:coast-gateway) → mihomo,故 mihomo 侧连接的 sourceIP
    // 恒为 127.0.0.1(SOCKS 在本机),按 sourceIP 归类全落空;真实设备身份在 inboundUser
    // (= dev-<去冒号mac>,见 ConfigBuilder 的 per-user listener + DeviceStore::socksUser)。
    QHash<QString, QString> ipToMac;
    QHash<QString, QString> userToMac;
    QString selfMac; // 本机（第一条 isSelf 记录）——回环来源的连接归到它名下
    for (const DeviceRecord &d : m_store->devices()) {
        if (!d.ip.isEmpty())
            ipToMac.insert(d.ip, d.mac);
        const QString u = DeviceStore::socksUser(d.mac);
        if (!u.isEmpty())
            userToMac.insert(u, d.mac);
        if (d.isSelf && selfMac.isEmpty())
            selfMac = d.mac;
    }

    const bool dbg = qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG");
    int dbgByIp = 0, dbgByUser = 0, dbgByLocal = 0, dbgUnattr = 0;
    for (const QVariant &v : conns) {
        const QVariantMap m = v.toMap();
        const QString ip = m.value("sourceIP").toString();
        const QString inUser = m.value("inboundUser").toString();
        QString mac = ipToMac.value(ip);
        if (!mac.isEmpty()) {
            ++dbgByIp;
        } else { // 网关代理:sourceIP=127.0.0.1 归不到设备 → 靠 inboundUser 归属
            mac = userToMac.value(inUser);
            if (!mac.isEmpty())
                ++dbgByUser;
        }
        // 源地址是本机自己的某个网卡（回环 / TUN 的 198.18.0.1 / …）、又不是网关代理进来的
        // → 这是本机自己发出的流量，记到「本机」那台设备名下。以前这类全部落进「未归属」丢掉，
        // 于是设备列表里本机那一行的速率/今日用量恒为 0——全机器最忙的一台反而永远显示没流量。
        if (mac.isEmpty() && DeviceStore::isLocalMachineIp(ip)
            && !inUser.startsWith(QStringLiteral("dev-"))) {
            mac = selfMac;
            if (!mac.isEmpty())
                ++dbgByLocal;
        }
        if (mac.isEmpty()) {
            ++dbgUnattr;
            if (dbg && dbgUnattr <= 3) // 归不上时把原始线索打出来，否则只能看到一个「未归=N」
                std::fprintf(stderr, "[DEV] 未归属样本 sourceIP=%s inboundUser=%s selfMac=%s\n",
                             ip.toLatin1().constData(), inUser.toLatin1().constData(),
                             selfMac.toLatin1().constData()),
                    std::fflush(stderr);
            continue;
        }
        const QString id = m.value("id").toString();
        const qint64 down = m.value("download").toLongLong();
        const qint64 up = m.value("upload").toLongLong();
        seen.insert(id);
        // 「最后访问的地址」：本拍才第一次见到这个连接 id = 设备刚发起的连接。用「新建」而不是
        // 「字节最多」来定义最后访问——后者会被一条挂着的大下载长期霸屏，看不到设备在访问什么。
        // （不额外解析 metadata.start：新 id 就是最新，省掉每秒几百次 RFC3339 解析。）
        if (!connBytes.contains(id))
            m_store->setLastHost(mac, m.value("host").toString());
        const QPair<qint64, qint64> last = connBytes.value(id, {0, 0});
        const qint64 dd = down >= last.first ? down - last.first : down;   // 新连接=全量；回退=当前值
        const qint64 du = up >= last.second ? up - last.second : up;
        connBytes.insert(id, {down, up});
        auto &acc = devDelta[mac];
        acc.first += dd;
        acc.second += du;
        devConn[mac] += 1;
        // （Top 域名不再在这里攒：以前那份 QHash 只活在内存里，重启就清零。改由 HistoryStore
        //   统一记账——连接断了落库、在途的实时合并，见 refreshTopDomains。）
    }
    // 清理本拍未出现的连接 id（已关闭，字节已计入）。
    for (auto it = connBytes.begin(); it != connBytes.end();) {
        if (!seen.contains(it.key()))
            it = connBytes.erase(it);
        else
            ++it;
    }

    // 应用到台账：累加会话/累计，算速率；并汇总总速率。曾有速率、本拍无流量的设备归零一次。
    qint64 totalUp = 0, totalDown = 0;
    QSet<QString> active;
    for (auto it = devDelta.constBegin(); it != devDelta.constEnd(); ++it) {
        const QString mac = it.key();
        const qint64 dDown = it.value().first;
        const qint64 dUp = it.value().second;
        active.insert(mac);
        Prev &p = m_prev[mac];
        p.down += dDown;
        p.up += dUp;
        const qint64 rateDown = dt > 0 ? dDown * 1000 / dt : 0;
        const qint64 rateUp = dt > 0 ? dUp * 1000 / dt : 0;
        if ((rateDown > 0 || rateUp > 0) && qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG"))
            std::fprintf(stderr, "[DEV] %s rate down=%lld up=%lld conn=%d\n",
                         mac.toLatin1().constData(), static_cast<long long>(rateDown),
                         static_cast<long long>(rateUp), devConn.value(mac)),
                std::fflush(stderr);
        m_store->applyTraffic(mac, p.up, p.down, rateUp, rateDown, devConn.value(mac));
        totalUp += rateUp;
        totalDown += rateDown;
    }
    // 归零：上拍活动、本拍不活动的设备速率清 0（会话/累计保留）。
    for (auto it = m_prev.begin(); it != m_prev.end(); ++it) {
        if (!active.contains(it.key())) {
            m_store->applyTraffic(it.key(), it.value().up, it.value().down, 0, 0, 0);
        }
    }

    // 有连接时才打印(避免空闲每秒刷屏):按 sourceIP / inboundUser 各归属了多少。
    if (dbg && !conns.isEmpty())
        std::fprintf(stderr, "[DEV] conns=%d 归属:byIp=%d byUser=%d byLocal=%d 未归=%d\n",
                     int(conns.size()), dbgByIp, dbgByUser, dbgByLocal, dbgUnattr),
            std::fflush(stderr);

    m_totalRateUp = totalUp;
    m_totalRateDown = totalDown;

    // 流量是每秒高频更新：统一在这里刷新一次模型/详情/概览（applyTraffic 本身不发信号）。
    refreshModel();
    rebuildSelected();
    emit overviewChanged();
}

// ———————————————————————— 灰度：进程内出站（10b/10c）————————————————————————
namespace {
QString readFileText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    const QString s = QString::fromUtf8(f.readAll());
    f.close();
    return s;
}
// 从 full.yaml 文本里抽出 `rules:` 段的整行规则（去掉「  - 」列表前缀）。best-effort：喂给 RuleEngine
// 备用；本单元 Rule 模式回退核心，不靠它做进程内路由。拿不到就返回空。
QStringList extractRuleLines(const QString &yaml)
{
    QStringList out;
    const QStringList lines = yaml.split('\n');
    bool inRules = false;
    static const QRegularExpression rulesKey(QStringLiteral("^rules\\s*:"));
    for (QString line : lines) {
        line.remove('\r');
        if (!inRules) {
            if (rulesKey.match(line).hasMatch())
                inRules = true;
            continue;
        }
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#'))
            continue;
        // ★ 段结束的判据必须是「**顶层新键**」= 列首 且 不是列表项。
        //   之前写成「列首即结束」，而 ConfigBuilder 生成的 full.yaml 恰恰把 rules 的列表项写在
        //   **列首**（`- IP-CIDR,10.0.0.0/8,DIRECT,no-resolve`，无缩进、无引号）→ 第一条就 break、
        //   一条规则都取不到 → RuleEngine 表为空 → Rule 模式永远命中不了（真机诊断实测：target 恒空）。
        //   种子 default.yaml 用的是另一种写法（缩进 + 带引号，target 含 emoji）——两种都得吃下。
        if (!line.startsWith(' ') && !line.startsWith('\t') && !trimmed.startsWith('-'))
            break;
        if (trimmed.startsWith('-'))
            out << trimmed.mid(1).trimmed(); // 去掉列表项的 '-'；外层引号由 RuleEngine::parseRule 剥
    }
    return out;
}
} // namespace

// —————————— 用户手选的落盘（核心缺席时的权威来源）——————————
// 存成 configDir/selection.json：{"selected":"…","mode":"…"}。
// 与已有的 groupmap.json 分开放：那份是「核心报回来的组→叶子映射」，这份是「用户点了什么」，
// 来源与可信度不同，混在一起以后必然分不清谁覆盖谁。
static QString selectionPath(const QString &configDir)
{
    return QDir(configDir).filePath(QStringLiteral("selection.json"));
}

void DevicesController::noteUserSelection(const QString &group, const QString &leaf)
{
    if (leaf.isEmpty())
        return;
    // ① 组→叶子：并进已有的缓存（核心不在时 rebuild 会重放它）
    if (!group.isEmpty()) {
        QHash<QString, QString> map = loadGroupMapCache();
        map.insert(group, leaf);
        saveGroupMapCache(map);
    }
    // ② 全局选中节点：Global 模式直接用它；Rule 模式下也是「用户最后点的那个」
    QFile f(selectionPath(m_configDir));
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) {
        o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
    }
    o.insert(QStringLiteral("selected"), leaf);
    QFile w(selectionPath(m_configDir));
    if (w.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        w.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        w.close();
    }
    rebuildCoastCoreConfig(); // 立刻生效，不等下一次轮询
}

void DevicesController::noteUserMode(const QString &mode)
{
    if (mode.isEmpty())
        return;
    QFile f(selectionPath(m_configDir));
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) {
        o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
    }
    o.insert(QStringLiteral("mode"), mode);
    QFile w(selectionPath(m_configDir));
    if (w.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        w.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        w.close();
    }
    rebuildCoastCoreConfig();
}

void DevicesController::rebuildCoastCoreConfig()
{
    if (!m_pcfgStore)
        return;
    // 节点表取自 full.yaml：其 proxies 的节点名与核心/选中节点一致（subscribe.yaml 的裸名少了
    // 「 - 订阅名」后缀，对不上 selected）。full.yaml 缺失时退回 subscribe.yaml（名可能对不上 →
    // Global 找不到节点即回退 mihomo，安全）。
    QString proxiesYaml = readFileText(QDir(m_configDir).filePath(QStringLiteral("full.yaml")));
    if (proxiesYaml.trimmed().isEmpty())
        proxiesYaml = readFileText(QDir(m_configDir).filePath(QStringLiteral("subscribe.yaml")));

    // 模式 + 选中：从 ClashService 的当前快照读（无网络往返）。拿不到时按 Rule / 空处理 → 回退，安全。
    QString modeStr = m_clash ? m_clash->mode() : QString();
    QString selected = m_clash ? m_clash->selectedNode() : QString();
    // ★ 核心缺席时回放用户手选：REST 快照给不出值就读 selection.json。
    //   少了这一步，核心一停「模式」退回 Rule、「选中节点」变空 —— 用户刚点的节点悄悄失效，
    //   而 UI 上还显示着它选中，是那种「看着对、实际没生效」的坑。有核心时不走这里，行为不变。
    if (modeStr.isEmpty() || selected.isEmpty()) {
        QFile sf(selectionPath(m_configDir));
        if (sf.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(sf.readAll()).object();
            sf.close();
            if (modeStr.isEmpty())
                modeStr = o.value(QStringLiteral("mode")).toString();
            if (selected.isEmpty())
                selected = o.value(QStringLiteral("selected")).toString();
        }
    }
    ProxyConfig::Mode mode = ProxyConfig::Mode::Rule;
    if (modeStr.compare(QStringLiteral("Global"), Qt::CaseInsensitive) == 0)
        mode = ProxyConfig::Mode::Global;
    else if (modeStr.compare(QStringLiteral("Direct"), Qt::CaseInsensitive) == 0)
        mode = ProxyConfig::Mode::Direct;

    std::shared_ptr<const ProxyConfig> base = coastcore::buildProxyConfig(proxiesYaml, selected, mode);
    // 把「策略组 → 叶子节点」映射一并装进快照：Rule 模式的规则 target 绝大多数是**组名**
    //（🎯 全球直连 / 🚀 节点选择 …），没这张表就 nodeByName 必失配、整类回退核心。
    QHash<QString, QString> groupLeaf = m_clash ? m_clash->groupLeafMap() : QHash<QString, QString>();
    // ★ 核心不可用时的兜底：从我们**自己生成**的 full.yaml 里解析组结构。
    //   这是数据面最后一条隐藏的 mihomo 依赖 —— 这张表平时来自核心 REST（**权威**：含用户手选、
    //   由 store-selected 持久化），核心一停就空，于是 Rule 模式解析不出目标、整类回退到一个
    //   已经不在的核心。解析出来的是「默认选择」（每组取第一个成员），不含手选，所以**只在
    //   权威表拿不到时才用**：有核心时永远以核心为准，行为不变。
    if (!groupLeaf.isEmpty()) {
        // 核心给的是**权威**表（含用户手选）。落盘存一份：核心下次不在时可以重放它，
        // 而不是退回「每组取第一个」的默认值——那会把用户选的节点悄悄换掉。
        saveGroupMapCache(groupLeaf);
    } else {
        // 顺序有讲究：先用**上次核心给过的**（含手选），再退到从 full.yaml 解析的默认结构。
        groupLeaf = loadGroupMapCache();
        const char *src = "缓存(含手选)";
        if (groupLeaf.isEmpty()) {
            groupLeaf = coastcore::parseProxyGroupsLeaf(proxiesYaml);
            src = "full.yaml 解析(默认选择)";
        }
        if (!groupLeaf.isEmpty() && qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG"))
            std::fprintf(stderr, "[CCCFG] 核心的组映射为空，改用%s：%d 组\n", src,
                         int(groupLeaf.size())), std::fflush(stderr);
    }
    std::shared_ptr<const ProxyConfig> cfg =
        base ? std::make_shared<const ProxyConfig>(base->nodes(), base->selected(), base->mode(),
                                                   groupLeaf)
             : base;
    m_pcfgStore->reload(cfg); // 原子换手：在途连接留旧快照，新连接吃新快照（无停顿热更新）

    // 规则喂给引擎：Rule 模式的进程内路由**就靠它**（matchEx 判不了时回退核心，见 LanGateway 的 router）。
    if (m_ruleEngine)
        m_ruleEngine->setRulesFromLines(extractRuleLines(proxiesYaml));

    m_ccMode = modeStr;
    m_ccSelected = selected;
    m_ccGroupFp = groupFingerprint(groupLeaf);
}

// 「策略组 → 叶子」映射的指纹：排序后拼串，用来判断这张表有没有变（每秒轮询都比一次，必须便宜）。
QString DevicesController::groupFingerprint(const QHash<QString, QString> &m)
{
    QStringList parts;
    parts.reserve(m.size());
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        parts << (it.key() + QLatin1Char('') + it.value());
    parts.sort();
    return parts.join(QLatin1Char(''));
}

void DevicesController::refreshCoastCore()
{
    if (!m_coastCore)
        return; // 关着 → 什么都不动（零行为变化）
    // 热更新只需 rebuildCoastCoreConfig() —— 它内部 m_pcfgStore->reload() 是原子换手：已装在网关上的
    // CoreDialerFactory 的 router 每建一条新连接都现读 store->current()，因此换掉快照即刻对新连接生效，
    // 在途连接留旧快照，全程无停顿。这里**不**重新 setCoastCore（那会整体换掉出站工厂、多此一举，且
    // 工厂析构会 delete 其回退工厂——虽然 NetStack 的 ACCEPT 回调把 createTcp/connectTo 同步做完、
    // 换工厂只在事件循环间隙发生故实际安全，但直接 reload 更简、无此顾虑）。store 对象贯穿 app 生命
    // 周期不变，装一次即可。
    rebuildCoastCoreConfig();
}

void DevicesController::persistCoastCore(bool on)
{
    // 只改 config.yaml 的 coastcore 键、保留其余内容（复刻 SettingsController::persistConfigScalar）。
    const QString path = QDir(m_configDir).filePath(QStringLiteral("config.yaml"));
    QString yaml = readFileText(path);
    const QString line = QStringLiteral("coastcore: %1").arg(on ? QStringLiteral("true")
                                                               : QStringLiteral("false"));
    static const QRegularExpression re(QStringLiteral("(?m)^coastcore:.*$"));
    if (re.match(yaml).hasMatch()) {
        yaml.replace(re, line);
    } else {
        if (!yaml.isEmpty() && !yaml.endsWith('\n'))
            yaml += '\n';
        yaml += line + '\n';
    }
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        out.write(yaml.toUtf8());
        out.close();
    }
}

// 本机入站开关：写 config.yaml 的 coastcore_inbound（>0=开，0=关），并即时起/停（含端口换位）。
// 只有 coastcore 本身开着时才真正监听 —— 关着时没有可用的出站配置快照，监听了也只能全回退。
void DevicesController::setLocalInboundEnabled(bool on)
{
    if (m_switching)
        return; // 换位进行中（等端口时会泵事件循环），不接受再入
    const int want = on ? m_mixedPort : 0; // 端口由 `port:` 推导，不再写死
    if (want == m_inboundPort && (!on || (m_localInbound && m_localInbound->isListening())))
        return;
    m_inboundPort = want;
    if (m_coastCore) {
        if (on) {
            startLocalInbound();
        } else if (!stopLocalInbound()) {
            // 拒绝停止（核心不可用，端口交出去没人接）→ 整件事作废，状态回到「开着」。
            m_inboundPort = m_mixedPort;
            emit localInboundEnabledChanged();
            return;
        }
    }
    // 持久化**结果**而不是意图：切换失败时 startLocalInbound 已把 m_inboundPort 归零（或
    // stopLocalInbound 的回抢把它复位），照实写回 —— 配置、开关、真实监听三者永远一致。
    persistLocalInbound(m_inboundPort);
    emit localInboundEnabledChanged();
}

// ———————————— 设置页总开关：「全部切换到进程内」 ————————————
// 开 = coastcore（网关数据面）+ 本机入站（端口换位）一起开；strict 不动 —— 回退核心是这次
// 切换的安全网（订阅里 grpc/salamander/tuic 那些进程内不支持的节点仍要靠它）。
// 关 = 全部回落 mihomo：入站停、端口归还（核心回 mixedPort）、网关数据面也回核心。
// 读值 allInProcess() 反映**真实状态**（入站确实在听），失败时 QML 开关自然回弹。
bool DevicesController::allInProcess() const
{
    return m_coastCore && m_localInbound && m_localInbound->isListening();
}

void DevicesController::setAllInProcess(bool on)
{
    if (m_switching || on == allInProcess()) {
        emit allInProcessChanged(); // 把开关顶回真实状态（切换中/已是目标态时点了也不动作）
        return;
    }
    m_switchError.clear();
    if (on) {
        const bool coastCoreBefore = m_coastCore;
        if (m_inboundPort != m_mixedPort) {
            m_inboundPort = m_mixedPort;
            emit localInboundEnabledChanged();
        }
        if (!m_coastCore)
            setCoastCoreEnabled(true); // 会顺带 startLocalInbound（m_inboundPort 已就位）
        else
            startLocalInbound();
        // 持久化结果：成功 = mixedPort，失败 = 0（startLocalInbound 已把运行态归零）。
        persistLocalInbound(m_inboundPort);
        // ★ 「别留半截状态」：入站没起来（端口被占）就把总开关整体回滚，包括这次顺带打开的
        //   coastcore —— 否则用户看到开关弹回去，网关那条数据面却已经悄悄换了实现。
        if (!allInProcess() && !coastCoreBefore && m_coastCore)
            setCoastCoreEnabled(false);
    } else {
        if (m_inboundPort > 0 || m_localInbound) {
            setLocalInboundEnabled(false); // 停入站 + 端口归还核心（含确认与回抢）
            if (m_localInbound) {          // 被拒（核心不可用）→ 整体不关，保持进程内继续服务
                emit allInProcessChanged();
                return;
            }
        }
        if (m_coastCore)
            setCoastCoreEnabled(false); // 网关数据面回落 mihomo
    }
    emit allInProcessChanged();
}

void DevicesController::persistLocalInbound(int port)
{
    // 与 persistCoastCoreStrict 同一套「只改这一个键、保留其余内容」的写法。
    const QString path = QDir(m_configDir).filePath(QStringLiteral("config.yaml"));
    QString yaml = readFileText(path);
    const QString line = QStringLiteral("coastcore_inbound: %1").arg(port);
    static const QRegularExpression re(QStringLiteral("(?m)^coastcore_inbound:.*$"));
    if (re.match(yaml).hasMatch()) {
        yaml.replace(re, line);
    } else {
        if (!yaml.isEmpty() && !yaml.endsWith(QLatin1Char('\n')))
            yaml += QLatin1Char('\n');
        yaml += line + QLatin1Char('\n');
    }
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        out.write(yaml.toUtf8());
        out.close();
    }
}

void DevicesController::persistCoastCoreStrict(bool on)
{
    // 只改 config.yaml 的 coastcore_strict 键、保留其余内容（复刻 SettingsController::persistConfigScalar）。
    const QString path = QDir(m_configDir).filePath(QStringLiteral("config.yaml"));
    QString yaml = readFileText(path);
    const QString line = QStringLiteral("coastcore_strict: %1").arg(on ? QStringLiteral("true")
                                                               : QStringLiteral("false"));
    static const QRegularExpression re(QStringLiteral("(?m)^coastcore_strict:.*$"));
    if (re.match(yaml).hasMatch()) {
        yaml.replace(re, line);
    } else {
        if (!yaml.isEmpty() && !yaml.endsWith('\n'))
            yaml += '\n';
        yaml += line + '\n';
    }
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        out.write(yaml.toUtf8());
        out.close();
    }
}

void DevicesController::setCoastCoreEnabled(bool on)
{
    if (on == m_coastCore || m_switching)
        return;
    // 关闭时**先**把端口交还核心：交不回去就整体不关。顺序反了就会出现「开关显示关、
    // 7890 还在我们手里、而快照/网关已经切回 mihomo」的半截状态。
    if (!on && !stopLocalInbound()) {
        emit coastCoreEnabledChanged(); // 顶回真实状态（仍是开）
        emit allInProcessChanged();
        return;
    }
    m_coastCore = on;
    persistCoastCore(on);
    rebuildCoastCoreConfig(); // 先把快照建好，再切开关，避免开的那一刻拿到空快照
    if (m_gateway)
        m_gateway->setCoastCore(on, m_coastStrict, m_pcfgStore, m_ruleEngine, m_dnsResolver);
    // 本机入站跟着总开关走：关掉 coastcore 就该退回「本机全走 mihomo」，否则开关名不副实。
    // （关的那一路已在函数开头停过入站，这里只管开。）
    if (on)
        startLocalInbound();
    emit coastCoreEnabledChanged();
    emit allInProcessChanged();
}

void DevicesController::setCoastCoreStrict(bool on)
{
    if (on == m_coastStrict)
        return;
    m_coastStrict = on;
    persistCoastCoreStrict(on);
    // 热切换：只有进程内内核本身开着时才需要把新意图推给网关（关着时下次开启会带上）。
    if (m_gateway && m_coastCore)
        m_gateway->setCoastCore(true, m_coastStrict, m_pcfgStore, m_ruleEngine, m_dnsResolver);
    emit coastCoreStrictChanged();
}
