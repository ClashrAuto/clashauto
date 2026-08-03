#pragma once

#include <QHash>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>

class QJsonDocument;
class QJsonObject;
class QNetworkReply;
class QNetworkRequest;

struct NodeInfo {
    QString name;
    QString now;
    int delay = 0;
    qint64 speed = 0;
    bool active = false;

    bool operator==(const NodeInfo &o) const
    {
        return name == o.name && now == o.now && delay == o.delay
               && speed == o.speed && active == o.active;
    }
    bool operator!=(const NodeInfo &o) const { return !(*this == o); }
};

class ClashService final : public QObject
{
    Q_OBJECT

public:
    explicit ClashService(QObject *parent = nullptr);

    void start();

    // 界面有没有人在看（由 QmlBridge 按窗口显隐推过来）。
    //
    // 只挡**纯给界面看**的那一条：`/proxies` 每秒一次，返回的是全部节点/策略组的大 JSON
    // （几十上百个节点），是这里最贵的一条轮询。`/traffic`（托盘菜单要）与 `/connections`
    // （历史记录的唯一数据源，停了会在历史上留一段空白）照旧跑。
    // 恢复可见时立刻补一次，不让用户对着上一次的旧列表发呆。
    void setUiActive(bool active);
    /// 用户是不是正停在**节点页**。决定 `/proxies` 的轮询频率。
    ///
    /// ★ `/proxies` 的响应**很大**：本机 70 个节点时实测 **51 KB**，而这条定时器原本
    ///   **恒 1 秒**拉一次并整份 `QJsonDocument::fromJson`。而这份数据**只有节点页在看** ——
    ///   状态页、设备页、日志页都不需要节点状态每秒刷新。
    ///   所以按「在不在看节点页」分档：看着就 1s（那是它该有的实时手感），
    ///   没看着就 5s（只为切回去时不至于是陈旧数据）。
    ///   与 `setUiActive` 的分工：那个管「窗口开没开」，这个管「开着时在看哪一页」。
    ///   Swift 线的 `ClashService.nodesVisible` 是同一设计，两条线保持对齐。
    void setNodesVisible(bool visible);
    void setEndpoint(const QString &host, int port); // REST API 地址（对齐 config.uiPort，默认 9191）
    void setMixedPort(int port); // 混合代理端口（对齐 config.mixedPort，默认 7890）——下载测速经此端口走代理
    void setSecret(const QString &secret); // external-controller 的 secret；非空时所有发往核心的请求带 Authorization: Bearer
    void setMode(const QString &mode);
    void setSelectedGroup(const QString &group);
    void setClearConnectionsOnSwitch(bool enabled);
    void selectNode(const QString &name);
    void clearConnections();
    void fetchConnections(std::function<void(QJsonArray)> callback);
    void closeConnection(const QString &id);
    void refreshNodes();
    void testDelays(bool thenSpeed = false); // thenSpeed=true：延迟测完后，对有效延迟节点自动跑下载测速
    void testNodeDelays(const QStringList &names, bool thenSpeed = false);

signals:
    void trafficUpdated(qint64 up, qint64 down);
    void connectionsUpdated(int count, qint64 downloadTotal);
    // 每次 /connections 轮询拿到的**完整**连接数组（HistoryStore 据此做「连接消失=已断开」判定）。
    // 复用同一次请求，不额外发包；轮询周期即 m_connectionsTimer 的 2s。
    void connectionsSnapshot(QJsonArray connections);
    void nodesUpdated(QVector<NodeInfo> nodes, QString selected);
    void proxyGroupsUpdated(QStringList groups, QString selectedGroup);
    void logUpdated(QString message);
    void statusUpdated(bool tun, bool proxy, bool core);
    void speedTestRunning(bool running); // 下载测速开始/结束——供 UI 切换测速按钮的加载态

private:
    // /traffic 是「流式」接口（mihomo 每秒推一条，连接常开、永不 finished）——绝不能像普通接口
    // 那样每秒 GET 一次，否则每次泄漏一个连接，几秒内耗尽 QNetworkAccessManager 的 6 连接/主机池，
    // 之后所有请求（含 selectNode 的 PUT）永久排队 → 列表不刷新、点应用无反应。改为常开单流读取。
    void startTrafficStream();
    void ensureTrafficStream(); // 流断了（核心重启/端口变更）就重连
    void pollConnections();
    void pollNodes();
    void applyAuth(QNetworkRequest &req) const; // secret 非空时给发往核心的请求加 Authorization: Bearer 头（统一入口，避免遗漏）
    void sendGet(const QUrl &url, std::function<void(const QJsonDocument &)> onJson, std::function<void()> onFinished = {});
    void sendJsonRequest(const QUrl &url, const QByteArray &method, const QJsonObject &payload, std::function<void(bool, QString)> onDone);
    void sendDelete(const QUrl &url, std::function<void(bool, QString)> onDone);

    // —— 下载测速（不依赖核心补丁）——
    // 延迟测完后，对有效延迟节点逐个测下载速度：由于核心 REST 无「按名测速」接口，改为在主选择组里
    // 临时选中目标节点、经混合端口下载测速文件、按字节/耗时算速度；已建立的下载连接会「钉」在拨号时选中的
    // 出站上（切组只影响新连接），因此把「选组+建连」串行化后，多个下载可真正并发（默认 5）。测速期间轮询
    // 上报的活动节点强制显示为原节点、结束后恢复选择，避免列表因活动节点变化而反复重建。
    void startSpeedTestForValidNodes(); // 拉一次 /proxies，筛出有效延迟节点后开测
    void startSpeedTest(const QStringList &names);
    void pumpSpeedTest();               // 调度器：在 !选组中 && 并发<5 && 队列非空 时启动下一个
    void beginSpeedDownload(const QString &name);

    QNetworkAccessManager m_network;
    QNetworkAccessManager m_delayNetwork;    // 延迟测速专用：与轮询/切换分开连接池，避免 63 个测速请求
                                             // 占满 6 连接/主机导致 pollNodes/selectNode 排队卡住
    QNetworkAccessManager m_speedNetwork;    // 下载测速专用连接池：调核心的 /proxies/<name>/speed，不挤占轮询/切换
    QNetworkReply *m_trafficReply = nullptr; // 常开的 /traffic 流；nullptr 表示未连/已断
    QTimer m_trafficTimer;                    // 看门狗：定期确保 /traffic 流还活着，断了就重连
    QTimer m_connectionsTimer;
    QTimer m_nodesTimer;
    bool m_connectionsInFlight = false; // /connections 轮询在途：上一拍未回则跳过，防止慢响应下请求自我堆积
    bool m_nodesInFlight = false;        // /proxies 轮询在途：同上
    bool m_uiActive = true;              // 见 setUiActive
    bool m_nodesVisible = false;         // 见 setNodesVisible
    /// 当前该用的 /proxies 轮询间隔（毫秒）。看着节点页 1s，否则 5s。
    int nodesPollMs() const { return m_nodesVisible ? 1000 : 5000; }
    bool m_started = false;              // start() 调过没有——没起来时别自作主张启定时器
    QString m_host = "127.0.0.1";
    int m_port = 9191; // 默认对齐 AppConfig::uiPort / default.yaml；实际由 setEndpoint 按配置设定
    int m_mixedPort = 7890; // 默认对齐 AppConfig::mixedPort；由 setMixedPort 按配置设定
    QString m_secret;       // external-controller secret；空 = 不加鉴权头（兼容未设 secret 的老核心）
    QString m_selectedGroup; // 空 = 未定；首轮 pollNodes 选主组（按模式：Rule→🚀 节点选择, Global→GLOBAL）
    QString m_mode = "Rule";  // 当前代理模式，决定主选择组（对齐旧项目 getProxies）
    QString m_selectedNode;
    bool m_clearOnSwitch = true;
    bool m_autoTested = false; // 核心起来后自动测一次延迟（异步，非阻塞）；核心掉线后重置以便重测

    // —— 下载测速运行态 ——
    QHash<QString, qint64> m_measuredSpeeds; // 节点名 → 实测下载速度(字节/秒)；pollNodes 注入到 NodeInfo.speed
    QStringList m_speedQueue;                // 待测节点队列
    int m_speedActive = 0;                   // 进行中的下载数（并发上限 5）
    bool m_speedTesting = false;             // 整轮测速进行中：供 UI 切换按钮加载态
};
