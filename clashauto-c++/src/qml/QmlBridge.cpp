#include "QmlBridge.h"

#include "../AppConfig.h"
#include "../ClashService.h"
#include "../CoreController.h"
#include "../SubscriptionStore.h"
#include "Version.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <QWindow>

#if defined(Q_OS_MACOS)
#include "../MacWindow.h" // configureMacTitleBar / enableMacBlur（纯 C++ 接口，实现在 MacWindow.mm）
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

    // —— 节点列表 ——
    connect(m_clash, &ClashService::nodesUpdated, this,
            [this](const QVector<NodeInfo> &nodes, const QString &selected) {
        m_nodeModel.setNodes(nodes);
        if (selected != m_selectedNode) {
            m_selectedNode = selected;
        }
        emit nodesChanged();
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
        m_connections = list;
        emit connectionsListChanged();
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
