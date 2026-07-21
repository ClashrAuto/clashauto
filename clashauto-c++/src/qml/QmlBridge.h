#pragma once

// QmlBridge —— QML 与既有 C++ 后端之间唯一的「门面/胶水」对象。
// 设计原则（见 qml/ARCHITECTURE.md）：后端类（CoreController/ClashService/SubscriptionStore/
// TrayController）原样复用、不改；本类只做三件事：
//   1) 把后端的信号「翻译」成 QML 友好的 Q_PROPERTY（NOTIFY 驱动 UI 更新）；
//   2) 把 UI 动作转成对后端 slot 的调用（Q_INVOKABLE）；
//   3) 提供一个 NodeListModel 给 StatusPage 的 ListView。
// 后端对象的生命周期由 main_qml.cpp 拥有；本类只持有裸指针、不接管所有权。
#include "ConnectionsModel.h"
#include "NodeListModel.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class AppConfig;
class ClashService;
class CoreController;
class SubscriptionStore;
class QWindow;

class QmlBridge final : public QObject
{
    Q_OBJECT
    // —— 状态灯（真值以 CoreController 为准，见 .cpp 注释）——
    Q_PROPERTY(bool coreRunning READ coreRunning NOTIFY statusChanged)
    Q_PROPERTY(bool proxyEnabled READ proxyEnabled NOTIFY statusChanged)
    Q_PROPERTY(bool tunEnabled READ tunEnabled NOTIFY statusChanged)
    // —— 流量 / 连接 指标（StatusPage 流量卡）——
    Q_PROPERTY(QString upText READ upText NOTIFY trafficChanged)
    Q_PROPERTY(QString downText READ downText NOTIFY trafficChanged)
    Q_PROPERTY(double upBytes READ upBytes NOTIFY trafficChanged)   // 原始字节/秒（供带宽图）
    Q_PROPERTY(double downBytes READ downBytes NOTIFY trafficChanged)
    Q_PROPERTY(int connectionsCount READ connectionsCount NOTIFY connectionsChanged)
    Q_PROPERTY(QString totalDownText READ totalDownText NOTIFY connectionsChanged)
    // 连接列表模型（进程卡「查看全部连接」弹窗按需刷新）：增量更新，保滚动位置。
    Q_PROPERTY(ConnectionsModel *connectionsModel READ connectionsModel CONSTANT)
    // —— 节点 / 组 ——
    Q_PROPERTY(NodeListModel *nodeModel READ nodeModel CONSTANT)
    Q_PROPERTY(QString selectedNode READ selectedNode NOTIFY nodesChanged)
    Q_PROPERTY(QStringList groups READ groups NOTIFY groupsChanged)
    Q_PROPERTY(QString selectedGroup READ selectedGroup NOTIFY groupsChanged)
    Q_PROPERTY(bool speedTesting READ speedTesting NOTIFY speedTestingChanged)
    // —— 页脚 / 页眉 ——
    Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(QString lastLog READ lastLog NOTIFY logAppended)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool initialDark READ initialDark CONSTANT)

public:
    QmlBridge(AppConfig *config, CoreController *core, ClashService *clash,
              SubscriptionStore *subs, QObject *parent = nullptr);

    bool coreRunning() const { return m_coreRunning; }
    bool proxyEnabled() const { return m_proxyEnabled; }
    bool tunEnabled() const { return m_tunEnabled; }
    QString upText() const { return m_upText; }
    QString downText() const { return m_downText; }
    double upBytes() const { return static_cast<double>(m_upBytes); }
    double downBytes() const { return static_cast<double>(m_downBytes); }
    int connectionsCount() const { return m_connectionsCount; }
    QString totalDownText() const { return m_totalDownText; }
    ConnectionsModel *connectionsModel() { return &m_connModel; }
    NodeListModel *nodeModel() { return &m_nodeModel; }
    QString selectedNode() const { return m_selectedNode; }
    QStringList groups() const { return m_groups; }
    QString selectedGroup() const { return m_selectedGroup; }
    bool speedTesting() const { return m_speedTesting; }
    QString mode() const { return m_mode; }
    QString lastLog() const { return m_lastLog; }
    QString version() const;
    bool initialDark() const { return m_initialDark; }

    // —— UI 动作 → 后端 slot ——
    Q_INVOKABLE void toggleCore();
    Q_INVOKABLE void toggleProxy();
    Q_INVOKABLE void toggleTun();
    Q_INVOKABLE void setMode(const QString &display); // 传中文「规则/全局/直连」即可
    Q_INVOKABLE void selectNode(const QString &rawName);
    Q_INVOKABLE void selectGroup(const QString &group);
    Q_INVOKABLE void refreshNodes();
    Q_INVOKABLE void runSpeedTest();
    Q_INVOKABLE void setNodeFilter(const QString &filter);
    Q_INVOKABLE void clearConnections();
    Q_INVOKABLE void refreshConnections();               // 拉取当前连接列表（异步，完成后 m_connModel.setRaw）
    Q_INVOKABLE void closeConnectionById(const QString &id); // 断开单个连接，随后刷新列表

    // macOS 毛玻璃：把 QML 窗口交给原生层做「透明标题栏 + 整窗 NSVisualEffectView」。
    // 非 macOS 上是安全 no-op。dark 决定玻璃深浅（跟随应用主题）。
    Q_INVOKABLE void applyMacGlass(QWindow *window, bool dark);

signals:
    void statusChanged();
    void trafficChanged();
    void connectionsChanged();
    void nodesChanged();
    void groupsChanged();
    void speedTestingChanged();
    void modeChanged();
    void logAppended(const QString &line);

private:
    static QString speedText(qint64 value);
    void refreshStatusFromCore(); // 以 CoreController 为准刷新三盏灯

    CoreController *m_core = nullptr;
    ClashService *m_clash = nullptr;
    SubscriptionStore *m_subs = nullptr;
    NodeListModel m_nodeModel;

    bool m_coreRunning = false;
    bool m_proxyEnabled = false;
    bool m_tunEnabled = false;
    QString m_upText = QStringLiteral("0.00 B");
    QString m_downText = QStringLiteral("0.00 B");
    qint64 m_upBytes = 0;
    qint64 m_downBytes = 0;
    int m_connectionsCount = 0;
    QString m_totalDownText = QStringLiteral("0.00 B");
    ConnectionsModel m_connModel;
    QString m_selectedNode;
    QStringList m_groups;
    QString m_selectedGroup;
    bool m_speedTesting = false;
    QString m_mode = QString::fromUtf8("规则");
    QString m_lastLog = QStringLiteral("Ready");
    bool m_initialDark = true;
};
