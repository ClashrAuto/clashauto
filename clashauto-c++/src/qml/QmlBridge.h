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

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class AppConfig;
class ClashService;
class CoreController;
class SubscriptionStore;
class QTimer;
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
    // —— 节点切换加载态（对齐旧项目 beginNodeSwitch/endNodeSwitch + 转圈动画）——
    Q_PROPERTY(bool switching READ switching NOTIFY switchingChanged)       // 是否有切换/禁用在途
    Q_PROPERTY(QString switchTarget READ switchTarget NOTIFY switchingChanged) // 正在切换的目标节点原名
    Q_PROPERTY(QString spinnerGlyph READ spinnerGlyph NOTIFY spinnerChanged)   // 当前转圈帧 ◐◓◑◒
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
    bool switching() const { return m_switching; }
    QString switchTarget() const { return m_switchTarget; }
    QString spinnerGlyph() const;
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
    // 禁用当前正在使用的节点：把它从订阅池摘除并重建配置（对齐旧项目 disableNodeByName）。
    // rawNow 为组行实际使用的叶子（node.now）；空则用 rawName 本身。
    Q_INVOKABLE void disableNode(const QString &rawName, const QString &rawNow);
    // 进入切换加载态：目标按钮转圈、其余按钮禁用（对齐旧项目 beginNodeSwitch）。
    // 由 QML 的 apply()/disable() 在调用 selectNode/disableNode 之前调用；已在途则忽略（防重入）。
    Q_INVOKABLE void beginNodeSwitch(const QString &target);
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

    // Windows 原生标题栏染色：DWM 把标题栏背景染成 bg（窗口壳色），与应用背景融为一体。
    // 非 Windows 上是安全 no-op。旧 Windows(非 Win11)上 DWM 忽略 → 保持系统默认标题栏。
    Q_INVOKABLE void applyWindowsTitleBar(QWindow *window, const QColor &bg, bool dark);

    // 启动自动拉起核心（main_qml.cpp 延时调用）：有内核且未在跑就起核心。Windows 上若上次退出时
    // 增强(TUN)开着（config use:true）而当前非提权，先按需提权重启，让提权实例带 TUN 冷启动。
    Q_INVOKABLE void autoStartCore();

#if defined(Q_OS_WIN)
    // 增强(TUN) 的「按需提权」：当前进程是否已以管理员身份运行；否则建不了 wintun 虚拟网卡。
    static bool isProcessElevated();
    // 以管理员身份重启自身并带 --tun-elevated：先落盘 use:true、硬杀本(非提权)核心并退出，
    // 让提权实例接管。true = 已成功发起重启（本实例即将退出）。取消/失败时回滚 use: 并返回 false。
    bool relaunchElevatedForTun();
#endif

signals:
    void statusChanged();
    void trafficChanged();
    void connectionsChanged();
    void nodesChanged();
    void groupsChanged();
    void speedTestingChanged();
    void switchingChanged();
    void spinnerChanged();
    void modeChanged();
    void logAppended(const QString &line);

private:
    static QString speedText(qint64 value);
    void refreshStatusFromCore(); // 以 CoreController 为准刷新三盏灯
    void endNodeSwitch();          // 结束切换加载态：停转圈、清态（对齐旧项目 endNodeSwitch）
    void pushLog(const QString &message); // 写页脚日志：更新 lastLog 并广播（同构造里的 pushLog）
    // 轻量落盘：只改 config.yaml 的单个键、保留其余内容（复刻 MainWindow/SettingsController::persistConfigBool）。
    // 增强(TUN) 每次切换即把 use: 落盘，重启/一键更新/提权重启后据此恢复。
    void persistConfigBool(const QString &key, bool value);

    QString m_userDir; // 用户可写配置目录（config.yaml 所在），用于 persistConfigBool
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
    // —— 切换加载态 ——
    bool m_switching = false;
    QString m_switchTarget;   // 正在切换/禁用的目标节点原名（转圈落在它的按钮上）
    QString m_switchFrom;     // 点击切换前的活动节点；核心报告的 selected 不再等于它 = 切换已确认
    int m_spinnerFrame = 0;
    QTimer *m_spinnerTimer = nullptr;  // 120ms 推进转圈帧
    QTimer *m_failsafeTimer = nullptr; // 6s 兜底：未确认则强制结束，避免永久卡加载态
    QString m_mode = QString::fromUtf8("规则");
    QString m_lastLog = QStringLiteral("Ready");
    bool m_initialDark = true;
};
