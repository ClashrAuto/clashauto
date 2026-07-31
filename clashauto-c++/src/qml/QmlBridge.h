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
class DevicesController;
class DeviceStore;
class LocalTunService;
class HistoryStore;
class SubscriptionStore;
class QJsonArray;
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
    // —— 状态页：本次会话的流量构成 + 两份连接速览（都由 observeConnections 每拍算出）——
    // 直连/代理是**本会话累计**（按连接 id 取增量攒的），不是核心那个 downloadTotal：
    // 后者只有一个总数，答不了「有多少走了代理」。
    Q_PROPERTY(double directBytes READ directBytes NOTIFY trafficStatsChanged)
    Q_PROPERTY(double proxyBytes READ proxyBytes NOTIFY trafficStatsChanged)
    Q_PROPERTY(QString directText READ directText NOTIFY trafficStatsChanged)
    Q_PROPERTY(QString proxyText READ proxyText NOTIFY trafficStatsChanged)
    Q_PROPERTY(QString totalText READ totalText NOTIFY trafficStatsChanged)
    // 最近建立的 5 条连接 {host, chain, device, direct, bytes}
    Q_PROPERTY(QVariantList recentConnections READ recentConnections NOTIFY trafficStatsChanged)
    // 本会话用量最大的 5 个目标 {host, device, direct, bytes}
    Q_PROPERTY(QVariantList topConnections READ topConnections NOTIFY trafficStatsChanged)
    // —— 状态页「今日流量」卡（数据来自历史库，跨重启保留）——
    Q_PROPERTY(QVariantList todayHourly READ todayHourly NOTIFY todayTrafficChanged)  // 24 个小时桶的字节数
    Q_PROPERTY(QVariantList todayTop READ todayTop NOTIFY todayTrafficChanged)        // Top5 {label, bytes}
    Q_PROPERTY(double todayTotal READ todayTotal NOTIFY todayTrafficChanged)          // 今日合计（当前口径）
    // 口径：只算走代理的流量；维度：0=进程 1=设备 2=域名。都由卡片上的控件写入。
    Q_PROPERTY(bool trafficProxyOnly READ trafficProxyOnly WRITE setTrafficProxyOnly NOTIFY todayTrafficChanged)
    Q_PROPERTY(int trafficDimension READ trafficDimension WRITE setTrafficDimension NOTIFY todayTrafficChanged)
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
    // 关闭到托盘（config.mini）：两处用途——(1) 启动时开→静默启动仅托盘、不显示窗口，关→正常显示
    // 窗口（见 Main.qml Component.onCompleted）；(2) ✕ 时开→隐藏窗口而非退出（Win/Linux 用；mac 恒隐藏
    // 不看此值，见 Main.qml onClosing）。随设置页「应用」更新，故非 CONSTANT（(2) 即时生效；(1) 下次启动生效）。
    Q_PROPERTY(bool closeToTray READ closeToTray NOTIFY closeToTrayChanged)

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
    double directBytes() const { return static_cast<double>(m_directBytes); }
    double proxyBytes() const { return static_cast<double>(m_proxyBytes); }
    QString directText() const { return speedText(m_directBytes); }
    QString proxyText() const { return speedText(m_proxyBytes); }
    QString totalText() const { return speedText(m_directBytes + m_proxyBytes); }
    QVariantList recentConnections() const { return m_recentConns; }
    QVariantList topConnections() const { return m_topConns; }
    QVariantList todayHourly() const { return m_todayHourly; }
    QVariantList todayTop() const { return m_todayTop; }
    double todayTotal() const { return static_cast<double>(m_todayTotal); }
    bool trafficProxyOnly() const { return m_trafficProxyOnly; }
    void setTrafficProxyOnly(bool on);
    int trafficDimension() const { return m_trafficDimension; }
    void setTrafficDimension(int dim);
    ConnectionsModel *connectionsModel() { return &m_connModel; }

    // 设备台账后置注入（main 里 bridge 先于 DeviceStore 构造）：只用来把连接归到设备名下，
    // 没注入也能正常跑，连接速览里的「设备」一列会退回显示来源 IP。
    void setDeviceStore(DeviceStore *store) { m_deviceStore = store; }
    // 历史库后置注入（同上）：今日流量卡的数据源。没注入时那张卡显示空。
    void setHistoryStore(HistoryStore *history) { m_history = history; }
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
    bool closeToTray() const { return m_closeToTray; }

    // —— UI 动作 → 后端 slot ——
    Q_INVOKABLE void toggleCore();
    Q_INVOKABLE void toggleProxy();
    Q_INVOKABLE void toggleTun();

    // 「增强」按钮改走**进程内 TUN** 的接线口。传 nullptr / 不调 = 维持原样（用 mihomo 的 TUN）。
    //
    // ★ 按钮语义**不变**：还是「开 TUN」。区别只在 coastcore 打开时由我们自己的
    //   LocalTunService 建 TUN，而不是让 mihomo 去建。coastcore 关着时一行行为都不变。
    // ★ 出站配置与分流实现取自 DevicesController 的**同一份** store/rules —— 复制一份就会
    //   开始漂移（网关和 TUN 用两套规则，用户改了设置只有一边生效）。
    void setInProcessTunSources(DevicesController *devices);

    // 核心缺席时用进程内配置填节点列表（详见 .cpp）。核心在时不调用。
    void syncNodesFromInProcess();
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
    Q_INVOKABLE void refreshConnections();               // 拉取当前连接并与「已见连接」增量合并（断开的标 offline 保留）
    Q_INVOKABLE void resetConnections();                 // 清空「已见连接」历史（连接窗每次打开时调用，避免上次会话残留）
    Q_INVOKABLE void closeConnectionById(const QString &id); // 断开单个连接，随后刷新列表
    // 状态页显隐：只有它可见时才每 10s 重算今日流量（那是几条 GROUP BY，没人看时白烧 CPU）。
    Q_INVOKABLE void setStatusActive(bool active);

    // macOS 毛玻璃：把 QML 窗口交给原生层做「透明标题栏 + 整窗 NSVisualEffectView」。
    // 非 macOS 上是安全 no-op。dark 决定玻璃深浅（跟随应用主题）。
    Q_INVOKABLE void applyMacGlass(QWindow *window, bool dark);

    // Windows 原生标题栏染色：DWM 把标题栏背景染成 bg（窗口壳色），与应用背景融为一体。
    // 非 Windows 上是安全 no-op。旧 Windows(非 Win11)上 DWM 忽略 → 保持系统默认标题栏。
    Q_INVOKABLE void applyWindowsTitleBar(QWindow *window, const QColor &bg, bool dark);

    // macOS：窗口可见时显示 Dock 图标（Regular），隐藏（✕ 关闭）时移除 Dock 图标（Accessory），
    // 即「关闭窗口不留 Dock」。由 Main.qml onVisibleChanged 驱动。非 macOS 上是安全 no-op。
    Q_INVOKABLE void setMacDockVisible(bool visible);

    // 启动自动拉起核心（main_qml.cpp 延时调用）：有内核且未在跑就起核心。Windows 上若上次退出时
    // 增强(TUN)开着（config use:true）而当前非提权，先按需提权重启，让提权实例带 TUN 冷启动。
    Q_INVOKABLE void autoStartCore();

    // —— 跟随系统深浅色（设置页「跟随系统深浅色」= config.autoTheme）——
    // 当前系统是否为暗色外观（Qt 6.5+ 用 QStyleHints::colorScheme；低版本回退启动主题）。
    Q_INVOKABLE bool systemDark() const;
    // 设置页「应用」后调用：更新「跟随系统」开关的实时状态；开启时立刻按当前系统外观切主题
    // （通过 systemThemeChanged 通知 QML 设 Theme.dark）。对齐 Widgets 版保存后立即 applyTheme。
    Q_INVOKABLE void setAutoTheme(bool on);
    // 设置页「应用」后调用：更新「关闭到托盘」实时状态，供下次 ✕ 关闭时 Main.qml onClosing 判定。
    Q_INVOKABLE void setCloseToTray(bool on);
    // 设置页「应用」后调用：更新「切换通知」(config.note) 实时状态。若从「关」手动切到「开」，
    // 发 reinitNotificationsRequested 让托盘重注册系统通知（尝试恢复此前失效/被清的注册；仅手动触发）。
    Q_INVOKABLE void setNodeSwitchNote(bool on);

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
    void trafficStatsChanged();
    void todayTrafficChanged();
    void nodesChanged();
    void groupsChanged();
    void speedTestingChanged();
    void switchingChanged();
    void spinnerChanged();
    void modeChanged();
    void logAppended(const QString &line);
    // 系统深浅色变化（仅在「跟随系统」开启时发出）：dark=true 暗色。Main.qml 据此设 Theme.dark。
    void systemThemeChanged(bool dark);
    void closeToTrayChanged();
    // 请求发一条系统托盘通知（节点切换等）：main_qml 连到 TrayController::notify。
    void notifyRequested(const QString &title, const QString &message);
    // 请求重注册系统通知（重显托盘图标）：仅在用户把「切换通知」从关手动切到开时发。
    void reinitNotificationsRequested();

private slots:
    // 每 2s 的 /connections 快照（与历史库共用同一次请求，不额外发包）：按连接 id 取增量，
    // 攒出「直连 / 代理」两桶累计流量、按目标 host 的累计用量，以及最近建立的那几条连接。
    // 声明成 slot（而非普通私有函数）是为了能被 invokeMethod 按名字调到：这块纯算术没有 UI，
    // 只能靠喂两拍假快照、把四个输出打出来验（COAST_CONNSTATS_SELFTEST=1，见 main_qml.cpp）。
    void observeConnections(const QJsonArray &conns);

private:
    static QString speedText(qint64 value);
    // sourceIP / inboundUser → 设备显示名（台账没注入或归不到设备时返回来源 IP）。
    QString deviceNameFor(const QString &sourceIp, const QString &inboundUser) const;
    // —— 进程内控制面（InprocTelemetry）与 mihomo REST 的合并（见 .cpp 各调用点的注释）——
    // 发布流量卡/带宽图：显示值 = REST 速率 + 进程内速率（两个集合不相交，相加即全量）。
    void applyTrafficDisplay();
    // 发布连接数/累计下载：同上口径合并。
    void applyConnStats();
    // 把 REST 快照与进程内快照**合成一份**后喂给 observeConnections + HistoryStore::observe。
    // 两个消费者都把「本拍缺席」当「连接已断开」，所以绝不能把两个来源分两次喂 ——
    // 那会让每一拍都误判另一半已断开（历史库狂写、增量记账清底）。
    void feedMergedSnapshot(const QJsonArray &restConns);
    // 重算今日流量卡（小时柱 + 当前维度的 Top5）。切口径/切 tab 时立刻调，平时 10s 一次。
    void refreshTodayTraffic();
    void refreshStatusFromCore(); // 以 CoreController 为准刷新三盏灯
    void endNodeSwitch();          // 结束切换加载态：停转圈、清态（对齐旧项目 endNodeSwitch）
    void pushLog(const QString &message); // 写页脚日志：更新 lastLog 并广播（同构造里的 pushLog）
    // 轻量落盘：只改 config.yaml 的单个键、保留其余内容（复刻 MainWindow/SettingsController::persistConfigBool）。
    // 增强(TUN) 每次切换即把 use: 落盘，重启/一键更新/提权重启后据此恢复。
    void persistConfigBool(const QString &key, bool value);

    QString m_userDir; // 用户可写配置目录（config.yaml 所在），用于 persistConfigBool
    bool m_autoTheme = false; // 是否跟随系统深浅色（config.autoTheme）；控制是否响应系统外观变化
    bool m_closeToTray = false; // 关闭到托盘（config.mini）：启动静默 + ✕ 隐藏；默认关（实际值由 config 覆盖）
    bool m_nodeSwitchNote = true; // 切换节点是否发系统通知（config.note）；设置页实时更新，仅它为真时发通知
    // 「进程内增强(TUN)」：coastcore 打开时由它建 TUN，而不是让 mihomo 建。
    // 懒创建（第一次真的要开才 new），关掉后保留对象以便复用。
    DevicesController *m_devices = nullptr;   // 出站配置/分流的**唯一**来源，见 setInProcessTunSources
    LocalTunService *m_localTun = nullptr;
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
    // —— 进程内控制面与 REST 的合并痕迹 ——
    // REST 侧最近一拍的原始值（合并显示时的加数之一；进程内份额是另一个加数）。
    qint64 m_restUp = 0;
    qint64 m_restDown = 0;
    int m_restConnCount = 0;
    qint64 m_restDownTotal = 0;
    // REST 各通道最近一次来数据的时刻（毫秒时间戳）：静默超阈值 = 核心不在/接口断，
    // 由 m_inprocTimer 接管发布节拍（核心在时定时器不发布，行为与从前一致）。
    qint64 m_lastRestTrafficMs = 0;
    qint64 m_lastRestConnMs = 0;
    qint64 m_lastRestSnapshotMs = 0;
    // 进程内累计字节的上一秒读数与差分出的速率（B/s）。
    quint64 m_inprocPrevUp = 0;
    quint64 m_inprocPrevDown = 0;
    qint64 m_inprocUpRate = 0;
    qint64 m_inprocDownRate = 0;
    QTimer *m_inprocTimer = nullptr; // 1s：差分进程内速率；REST 静默时接管发布
    int m_inprocTick = 0;            // 快照兜底每 2 拍一次（对齐 REST 的 2s 周期）
    ConnectionsModel m_connModel;
    // 连接管理页：累积「见过的连接」（按 id）。每次 poll 里没出现的不丢弃，而是标 offline 保留，
    // 这样 Offline 过滤才有内容（对齐旧项目 connections.vue 的 loadConnections）。连接窗打开时清空。
    QList<QVariantMap> m_seenConns;

    // —— 状态页的流量构成（observeConnections 维护）——
    DeviceStore *m_deviceStore = nullptr; // 只读台账，用于把连接归到设备名下；可为空
    struct ConnBytes { qint64 down = 0, up = 0; };
    QHash<QString, ConnBytes> m_connBytes; // 连接 id → 上一拍的累计值，用来取增量（连接消失即清）
    struct HostStat {
        qint64 bytes = 0;
        QString device;
        bool direct = false;
    };
    QHash<QString, HostStat> m_hostBytes;  // 目标 host → 本会话累计用量（「用得最多的 5 个」）
    qint64 m_directBytes = 0;
    qint64 m_proxyBytes = 0;
    QVariantList m_recentConns;
    QVariantList m_topConns;

    // —— 今日流量卡（数据来自历史库；只在状态页可见时每 10s 重算，见 setStatusActive）——
    HistoryStore *m_history = nullptr;
    QTimer *m_todayTimer = nullptr;
    QVariantList m_todayHourly;
    QVariantList m_todayTop;
    qint64 m_todayTotal = 0;
    bool m_trafficProxyOnly = false;
    int m_trafficDimension = 0; // 0=进程 1=设备 2=域名
    // m_hostBytes 是会话内只增的：超过这个数就只留用量最大的一半，免得挂机一整天涨到几万条。
    static constexpr int kMaxHostStats = 512;
    QString m_selectedNode;
    bool m_nodeInitialized = false; // 首次节点填充跳过切换通知，避免启动即误报（对齐 MainWindow m_nodeInitialized）
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
