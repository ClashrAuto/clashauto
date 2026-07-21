#include "QmlBridge.h"

#include "../AppConfig.h"
#include "../ClashService.h"
#include "../CoreController.h"
#include "../SubscriptionStore.h"
#include "Version.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QVariantMap>
#include <QWindow>

#if defined(Q_OS_MACOS)
#include "../MacWindow.h" // configureMacTitleBar / enableMacBlur（纯 C++ 接口，实现在 MacWindow.mm）
#endif
#if defined(Q_OS_WIN)
#include "../WinWindow.h" // setWindowsCaptionColor（DWM 标题栏染色，实现在 WinWindow.cpp）
#endif

QmlBridge::QmlBridge(AppConfig *config, CoreController *core, ClashService *clash,
                     SubscriptionStore *subs, QObject *parent)
    : QObject(parent), m_core(core), m_clash(clash), m_subs(subs)
{
    if (config) {
        const QString t = config->theme;
        m_initialDark = !(t.compare("light", Qt::CaseInsensitive) == 0
                          || t.compare("white", Qt::CaseInsensitive) == 0);
    }

    // —— 流量卡 ——
    connect(m_clash, &ClashService::trafficUpdated, this, [this](qint64 up, qint64 down) {
        m_upBytes = up;
        m_downBytes = down;
        m_upText = speedText(up);
        m_downText = speedText(down);
        emit trafficChanged();
    });
    connect(m_clash, &ClashService::connectionsUpdated, this, [this](int count, qint64 total) {
        m_connectionsCount = count;
        m_totalDownText = speedText(total);
        emit connectionsChanged();
    });

    // 切换加载态：转圈帧推进（120ms）与 6s 兜底（严格对齐旧项目 m_spinnerTimer + beginNodeSwitch 内的兜底）
    m_spinnerTimer = new QTimer(this);
    m_spinnerTimer->setInterval(120);
    connect(m_spinnerTimer, &QTimer::timeout, this, [this] {
        m_spinnerFrame = (m_spinnerFrame + 1) % 4;
        emit spinnerChanged(); // QML 里目标按钮文字绑定 spinnerGlyph，随之刷新
    });
    m_failsafeTimer = new QTimer(this);
    m_failsafeTimer->setSingleShot(true);
    m_failsafeTimer->setInterval(6000);
    connect(m_failsafeTimer, &QTimer::timeout, this, [this] {
        if (m_switching) {
            endNodeSwitch();        // 6s 未确认（PUT 失败/网络异常）：强制解除，避免永久卡加载态
            m_nodeModel.resync();   // 用最新已知数据重渲染活动节点/按钮（对齐 endNodeSwitch→syncNodeRows）
        }
    });

    // —— 节点列表 —— 三分支严格对齐旧项目 MainWindow::nodesUpdated（615-661）
    connect(m_clash, &ClashService::nodesUpdated, this,
            [this](const QVector<NodeInfo> &nodes, const QString &selected) {
        // 先更新活动节点（对齐旧项目 635 在切换确认判断 637 之前）
        if (selected != m_selectedNode) {
            m_selectedNode = selected;
            emit nodesChanged();
        }
        // 分支1：切换在途且核心报告的活动节点已不同于切换前 → 切换已确认，结束加载态并整表 reconcile
        if (m_switching && !m_switchFrom.isEmpty() && selected != m_switchFrom) {
            endNodeSwitch();
            m_nodeModel.setNodes(nodes); // 一次 reconcile：置顶新活动节点、复位按钮文字（不闪现）
            return;
        }
        // 分支2：切换在途但尚未确认 → 只原地刷药丸（delay/speed），不重排/不改 active/不动按钮，
        //          以免扰乱转圈与「其余禁用」态（对齐旧项目 updateNodeBadges）
        if (m_switching) {
            m_nodeModel.updateBadges(nodes);
            return;
        }
        // 分支3：非切换态 → 常规增量对账（setChanged→增删，anyChange→原地重排+改字段）
        m_nodeModel.setNodes(nodes);
    });
    connect(m_clash, &ClashService::proxyGroupsUpdated, this,
            [this](const QStringList &groups, const QString &selectedGroup) {
        m_groups = groups;
        m_selectedGroup = selectedGroup;
        emit groupsChanged();
    });
    connect(m_clash, &ClashService::speedTestRunning, this, [this](bool running) {
        m_speedTesting = running;
        emit speedTestingChanged();
    });

    // —— 状态灯 ——
    // ClashService::statusUpdated 每秒发的 tun/proxy 是写死值，不可信；与 Widgets 版一致，
    // 一律以 CoreController 的真值刷新（把每秒轮询当作「自愈复位」）。
    connect(m_clash, &ClashService::statusUpdated, this, [this](bool, bool, bool) {
        refreshStatusFromCore();
    });
    connect(m_core, &CoreController::statusChanged, this, [this](bool, bool, bool) {
        refreshStatusFromCore();
    });

    // —— 日志（页脚 + 后续 LogsPage）——
    auto pushLog = [this](const QString &message) {
        m_lastLog = message;
        emit logAppended(message);
    };
    connect(m_core, &CoreController::logUpdated, this, pushLog);
    connect(m_clash, &ClashService::logUpdated, this, pushLog);
}

QString QmlBridge::version() const { return QString::fromUtf8(APP_VERSION); }

void QmlBridge::refreshStatusFromCore()
{
    if (!m_core)
        return;
    const bool running = m_core->isRunning();
    const bool tun = running && m_core->isTunEnabled();
    const bool proxy = running && m_core->isProxyEnabled();
    if (running == m_coreRunning && tun == m_tunEnabled && proxy == m_proxyEnabled)
        return;
    m_coreRunning = running;
    m_tunEnabled = tun;
    m_proxyEnabled = proxy;
    emit statusChanged();
}

void QmlBridge::toggleCore()
{
    if (m_core)
        m_core->toggleCore();
}

void QmlBridge::toggleProxy()
{
    if (m_core)
        m_core->toggleProxy();
}

void QmlBridge::toggleTun()
{
    if (m_core)
        m_core->toggleTun();
}

void QmlBridge::setMode(const QString &display)
{
    if (m_clash)
        m_clash->setMode(display); // ClashService 内部把中文/英文都映射成 Rule/Global/Direct
    if (display != m_mode) {
        m_mode = display;
        emit modeChanged();
    }
}

void QmlBridge::selectNode(const QString &rawName)
{
    if (m_clash)
        m_clash->selectNode(rawName);
}

QString QmlBridge::spinnerGlyph() const
{
    static const char *frames[] = {"\xE2\x97\x90", "\xE2\x97\x93", "\xE2\x97\x91", "\xE2\x97\x92"}; // ◐◓◑◒
    return QString::fromUtf8(frames[m_spinnerFrame % 4]);
}

void QmlBridge::beginNodeSwitch(const QString &target)
{
    if (m_switching)
        return; // 防重入：已有切换在途，忽略（对齐旧项目 beginNodeSwitch 顶部的 if 返回）
    m_switching = true;
    m_switchTarget = target;      // 该节点按钮转圈
    m_switchFrom = m_selectedNode; // 记录切换前的活动节点，用于确认「已切换」
    m_spinnerFrame = 0;
    m_spinnerTimer->start();       // 目标按钮开始转圈
    m_failsafeTimer->start();      // 6s 兜底
    emit spinnerChanged();
    emit switchingChanged();       // QML：目标按钮显示转圈、其余按钮禁用
}

void QmlBridge::endNodeSwitch()
{
    if (!m_switching)
        return;
    m_switching = false;
    m_switchTarget.clear();
    m_switchFrom.clear();
    m_spinnerTimer->stop();
    m_failsafeTimer->stop();
    emit switchingChanged(); // QML：按钮恢复「应用/禁用」、全部可点。列表 reconcile 由调用方完成
}

void QmlBridge::disableNode(const QString &rawName, const QString &rawNow)
{
    // 复刻旧项目 MainWindow::disableNodeByName：禁用「实际正在使用的叶子」。组行 now 非空时禁其 now。
    if (!m_subs || !m_core)
        return;
    const QString liveName = rawNow.isEmpty() ? rawName : rawNow;
    // 实时节点名 = 「订阅节点名 - 订阅名」(或带 [speedtest] 后缀，见 ConfigBuilder)；反查订阅节点后 use:false。
    const QVector<SubscriptionSummary> subs = m_subs->load();
    for (int s = 0; s < subs.size(); ++s) {
        const QVector<SubscriptionNodeSummary> nodes = m_subs->nodes(s);
        for (int n = 0; n < nodes.size(); ++n) {
            const QString base = nodes[n].name + QStringLiteral(" - ") + subs[s].name;
            if (liveName == base || liveName == base + QStringLiteral("[speedtest]")) {
                if (m_subs->setNodeEnabled(s, n, false)) {
                    m_core->rebuildConfig();  // 重新生成 full.yaml 并热重载，节点从池中移除
                    if (m_clash)
                        m_clash->refreshNodes(); // 立即向核心重新拉取列表（禁用的节点随即消失）
                }
                return;
            }
        }
    }
}

void QmlBridge::selectGroup(const QString &group)
{
    if (m_clash)
        m_clash->setSelectedGroup(group);
}

void QmlBridge::refreshNodes()
{
    if (m_clash)
        m_clash->refreshNodes();
}

void QmlBridge::runSpeedTest()
{
    if (!m_clash || m_speedTesting)
        return;
    m_clash->refreshNodes();
    m_clash->testDelays(/*thenSpeed=*/true);
}

void QmlBridge::setNodeFilter(const QString &filter)
{
    m_nodeModel.setFilter(filter);
}

void QmlBridge::clearConnections()
{
    if (m_clash)
        m_clash->clearConnections();
}

void QmlBridge::refreshConnections()
{
    if (!m_clash)
        return;
    // 异步拉取（复用 ClashService::fetchConnections）；映射成 QML 友好的 {type,host,chain,下载,上传,id,offline}。
    m_clash->fetchConnections([this](QJsonArray arr) {
        QVariantList list;
        for (const QJsonValue &v : arr) {
            const QJsonObject c = v.toObject();
            const QJsonObject meta = c.value("metadata").toObject();
            QString host = meta.value("host").toString();
            if (host.isEmpty())
                host = meta.value("destinationIP").toString();
            QString type = meta.value("type").toString();
            if (type.isEmpty())
                type = meta.value("network").toString();
            const QJsonArray chains = c.value("chains").toArray();
            const QString chain0 = chains.isEmpty() ? QStringLiteral("-") : chains.first().toString();
            QVariantMap m;
            m["type"] = type;
            m["host"] = host;
            m["chain"] = chain0;
            m["download"] = static_cast<qlonglong>(c.value("download").toInteger()); // 原始字节，QML 按旧格式(无空格)自行格式化
            m["upload"] = static_cast<qlonglong>(c.value("upload").toInteger());
            m["id"] = c.value("id").toString();
            m["offline"] = c.value("offline").toBool();
            list.append(m);
        }
        m_connModel.setRaw(list); // 增量套用（保 ListView 滚动位置）
    });
}

void QmlBridge::closeConnectionById(const QString &id)
{
    if (!m_clash)
        return;
    m_clash->closeConnection(id);
    refreshConnections();
}

void QmlBridge::applyMacGlass(QWindow *window, bool dark)
{
#if defined(Q_OS_MACOS)
    if (!window)
        return;
    const WId wid = window->winId();
    configureMacTitleBar(wid);
    enableMacBlur(wid, dark);
#else
    Q_UNUSED(window);
    Q_UNUSED(dark);
#endif
}

void QmlBridge::applyWindowsTitleBar(QWindow *window, const QColor &bg, bool dark)
{
#if defined(Q_OS_WIN)
    if (!window)
        return;
    setWindowsCaptionColor(window->winId(), bg, dark);
#else
    Q_UNUSED(window);
    Q_UNUSED(bg);
    Q_UNUSED(dark);
#endif
}

QString QmlBridge::speedText(qint64 value)
{
    double number = static_cast<double>(qMax<qint64>(0, value));
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (number >= 1024.0 && unit + 1 < 5) {
        number /= 1024.0;
        ++unit;
    }
    return QString::number(number, 'f', 2) + " " + units[unit];
}
