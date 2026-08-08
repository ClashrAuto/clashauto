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
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class AppConfig;
class ClashService;
class CoreController;
class DeviceStore;
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
    // 内核二进制找不到时的路径（空 = 正常）。★ 这条**不能**只写进日志：内核缺失时整个 app 什么
    //   都干不了，而日志页是要用户自己翻的。CoreController 早就为此发了 coreMissing 信号，只是
    //   一直没有任何接收方——于是开发构建/首启下载失败的用户看到的只是一个不亮的状态灯。
    Q_PROPERTY(QString coreMissingPath READ coreMissingPath NOTIFY coreMissingChanged)
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
    // 「启动到托盘」（config.mini）：开 → 启动时静默、只留托盘不显示窗口；关 → 正常显示窗口
    // （见 Main.qml Component.onCompleted）。设置页里那颗开关的文案就是「启动到托盘」。
    // ★ 它**不管 ✕**：✕ 在所有平台上恒隐藏窗口（见 Main.qml onClosing）。这里原来的注释还写着
    //   「✕ 时开→隐藏窗口而非退出（Win/Linux 用）」，那是早就改掉的旧语义。
    Q_PROPERTY(bool closeToTray READ closeToTray NOTIFY closeToTrayChanged)

public:
    QmlBridge(AppConfig *config, CoreController *core, ClashService *clash,
              SubscriptionStore *subs, QObject *parent = nullptr);

    bool coreRunning() const { return m_coreRunning; }
    QString coreMissingPath() const { return m_coreMissingPath; }
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
    /// 节点页显隐 → `/proxies` 的轮询频率分档（节点页 1s，其他页 5s）。
    /// 与 setStatusActive 同一模式：页面自己在 onVisibleChanged 里报告。
    /// 理由见 ClashService::setNodesVisible —— /proxies 本机实测 51 KB，
    /// 恒 1s 拉是纯浪费，而这份数据只有节点页在看。
    Q_INVOKABLE void setNodesActive(bool active);

    /// 系统托盘此刻可不可用。
    ///
    /// ★ ✕ 恒隐藏窗口、界面里又没有任何退出入口 —— 于是**退出只有托盘菜单一条路**。
    ///   而托盘并不是到处都有：GNOME 40+ 默认就没有系统托盘（要装 AppIndicator 扩展），
    ///   那里托盘图标根本不出现。两件事凑一起的后果是：用户点了 ✕，窗口藏了、再也叫不回来，
    ///   程序还在后台占着系统代理，**只能去杀进程**。所以托盘不可用时 ✕ 必须真退（见 Main.qml）。
    ///
    ///   **每次现查而不是启动时存一份**：X11 上面板可能比应用起得晚，启动那一刻的答案会过期。
    Q_INVOKABLE bool trayAvailable() const;

    /// 读一个「置位即生效」的环境变量。给 UI 调试钩子用（如 `COAST_OPEN_UPDATE`）——
    /// QML 里读不到环境变量，而这类钩子（启动即打开某个只能点出来的窗）是无头截图核对
    /// 版式的唯一入口。正式运行不设就是零影响。
    Q_INVOKABLE bool envFlag(const QString &name) const;

    /// mac：窗口顶部被系统标题栏占掉的高度。QML 用**负的上边距**把内容顶进那条带子里
    /// （理由见 `MacWindow.h` 的 `macTitleBarInset`）。非 mac 恒 0。
    Q_INVOKABLE int macTitleBarInset(QWindow *window) const;

    // 界面此刻有没有一个窗口真的被人看着（主窗或连接/详情/更新那几个附属窗）。
    //
    // ★ 点 ✕ 走的是「只隐藏不销毁」（Main.qml 的 onClosing + hide()），QML 场景整棵都还在，
    //   于是每秒的节点轮询、连接聚合、模型更新照旧全跑一遍，算完没有任何人看。实测（接真核心、
    //   核心不由本进程启动）：**窗口开着 6.5%，收进托盘还是 6.5%** —— 收起来一点不省。
    //
    //   页面级的 setStatusActive / DevicesController::setActive 挡不住这件事：QML 里
    //   `Item.visible` 只反映自身与祖先项，**窗口隐藏不会把它变成 false**，所以页面一直
    //   以为自己是「活的」。这里补的是窗口这一层。
    //
    // 停的是纯喂界面的那些；`/traffic`（托盘菜单要）、`/connections` → 历史落库、
    // ARP 巡检与设备在线态热更新（新设备提醒、代理自愈都靠它）一律不停。
    bool uiVisible() const { return m_uiVisible; }

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
    /// 窗口显隐变了。main_qml 把它接到 ClashService::setUiActive 与
    /// DevicesController::setUiVisible —— 这里只负责「看得见没有」，各家自己决定停什么。
    void uiVisibleChanged(bool visible);
    void statusChanged();
    void coreMissingChanged();
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

private slots:
    void syncUiVisible();

private:
    static QString speedText(qint64 value);
    // sourceIP / inboundUser → 设备显示名（台账没注入或归不到设备时返回来源 IP）。
    QString deviceNameFor(const QString &sourceIp, const QString &inboundUser) const;
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
    CoreController *m_core = nullptr;
    ClashService *m_clash = nullptr;
    SubscriptionStore *m_subs = nullptr;
    NodeListModel m_nodeModel;

    bool m_coreRunning = false;
    QString m_coreMissingPath; // 见 coreMissingPath 属性；内核起来后清空
    bool m_proxyEnabled = false;
    bool m_tunEnabled = false;
    QString m_upText = QStringLiteral("0.00 B");
    QString m_downText = QStringLiteral("0.00 B");
    qint64 m_upBytes = 0;
    qint64 m_downBytes = 0;
    int m_connectionsCount = 0;
    QString m_totalDownText = QStringLiteral("0.00 B");
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
    // 1s 轮询窗口显隐。**不用信号**：Qt 没有「任意顶层窗口显隐」的全局信号，而附属窗散在
    // 各个页面的 QML 里（连接窗在状态页、详情窗在设备页、更新窗在 Main），逐个去挂
    // onVisibleChanged 既容易漏、又把这件事摊到四个文件里。每秒遍历几个 QWindow 的代价
    // 可以忽略（与 Swift 端把可见性搭在 1Hz 采样循环上同一取舍）。
    QTimer *m_visibilityTimer = nullptr;
    bool m_uiVisible = true;
    bool m_statusActive = false;   // 状态页自己报的可见性（与窗口那层取「与」）
    bool m_nodesActive = false;  // 见 setNodesActive
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
    // 页脚那行「最新日志」。**广播要节流**：Main.qml 里有个 Text 绑在 lastLog 上，每变一次
    // 就是一次文字重排 + 整窗重绘（Qt Quick 没有局部重绘）。核心 log-level=info、网关满负载时
    // 每秒几百行日志 —— 而每秒变几百次的文字根本没人读得了，纯粹是把整窗按在地上重画。
    // 真机实测（树莓派网关，239 上 20 并发短连接持续加压，各采样 25s）：
    //     日志入模型不设门 + 页脚不节流   App 27.5% / 28.2%   （核心自己才 21%，App 比它还贵）
    //     只给日志模型设可见性门          App 10.4% / 10.4%   ← 剩下的就是这行页脚文字
    //     再加页脚节流（本节流）          App  4.4% /  5.0%
    //     参照：把核心 log-level 调成 warning（几乎没有日志）时是 3.3% —— 已经贴到地板
    //   跨平台复核（Windows 真实图形会话、窗口可见、20 并发经混合口加压，各 30s）：
    //     空载 App 1.6% / 带载 App 1.0%（同期核心 66.7%）—— 负载不再抬高 App，与 Linux 侧一致。
    //   顺带（同一轮实测）：内存不随负载增长。树莓派网关连打 4 轮各 60s 的连接风暴再静置，
    //     App RSS 恒在 159.1~159.5 MB、核心 64.8~68.6 MB；Windows 工作集 229→239 MB 后不再涨。
    //     历史库 4.0 MB / 33979 行，保留期清理正常。没有泄漏。
    // 节流用**前沿**触发：距上次广播超过 kLogEmitMs 就立刻发（第一条日志不会迟到），
    // 否则安排一次尾部广播，把窗口内的若干条合并成一次。
    QString m_lastLog = QStringLiteral("Ready");
    static constexpr int kLogEmitMs = 200; // 页脚最多每 200ms 更新一次
    QElapsedTimer m_lastLogEmitAt;
    bool m_logEmitScheduled = false;
    bool m_initialDark = true;
};
