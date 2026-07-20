#pragma once

#include "ClashService.h"
#include "CoreController.h"
#include "SubscriptionStore.h"
#include "TrafficChart.h"
#include "TrayController.h"

#include <QFile>
#include <QFrame>
#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>

class QTextEdit;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QScrollArea;
class QTabWidget;
class QVBoxLayout;
class QTableWidget;
class QJsonArray;
class QCloseEvent;
class QTimer;
class QNetworkAccessManager;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // 处理 clash-auto:// 协议链接（导入订阅）；由 main 在启动或二次实例转发时调用
    void handleProtocolUrl(const QString &url);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    QWidget *buildTitleBar();
    QWidget *buildSidebar();
    QWidget *buildStatusPage();
    QWidget *buildSubscriptionsPage();
    QWidget *buildSettingsPage();
    QWidget *buildLogsPage();
    QWidget *buildAboutPage();
    QWidget *buildFooter();
    QFrame *createMetricCard(const QString &icon, const QString &title, QLabel **valueLabel, const QString &className);
    QPushButton *createMenuButton(const QString &text, int page);
    QFrame *createNodeRow(const NodeInfo &node);
    void disableNodeByName(const QString &liveName); // 把状态列表的实时节点名映射回订阅节点并 use:false + 重建
    void showSubscriptionNodes(int subscriptionIndex);
    void showUpdateDialog();
    void setMirrorEnabled(bool on); // 「国内加速 / 国内代理下载」共用偏好：持久化到 config.mirror 并同步两处勾选框
    void persistConfigBool(const QString &key, bool value); // 只改 config.yaml 的单个布尔键、保留其余内容
    void checkForUpdate(bool silent, int retriesLeft = 2); // 拉取最新 release 与本地版本比较；silent=启动自动检查；失败时静默重试
    // 让下载（检查更新/更新包/内核/mmdb 等 GitHub 资源）在核心运行时经混合端口走代理——
    // 墙内直连 GitHub 常不通，走代理更可靠。核心没跑则保持直连。
    // 注意：勾选「国内加速/国内代理下载」时，更新包/内核的下载改为直连 ghfast.top 镜像
    // （不经此代理）；API 查询与 .sha256 边车校验仍走本函数配置的通道。
    void applyDownloadProxy(QNetworkAccessManager *nam) const;
    // 用官方 <文件>.sha256 边车校验已下载文件完整性：流式算 SHA-256，与边车内的十六进制摘要
    // 去空白后不区分大小写比较。防镜像/中间人投递被篡改的安装包或内核。
    bool verifySha256(const QString &filePath, const QString &expectedHexLower) const;
    void showConnectionsDialog();
    void appendLog(const QString &message);
    void appendTimeline(QVBoxLayout *layout, QScrollArea *scroll, const QString &message);
    QWidget *createSwitchDot(bool enabled);
    void reloadSubscriptions();
    void populateNodeList();
    void updateNodeBadges(); // 仅延迟/速度变化时原地更新药丸，避免整表重建导致的闪烁/清空
    void syncNodeRows();     // 集合不变时：原地更新药丸/名称/按钮态并按新次序重排现有行（不销毁 → 不闪现）
    bool nodeListTested() const;                           // 当前列表是否已测出结果（有任一 delay>0）
    bool nodeHidden(const NodeInfo &node, bool tested) const; // 「仅可用节点」是否应隐藏该节点
    void beginNodeSwitch(const QString &target); // 点应用/禁用后进入加载态（目标转圈、其余禁用）
    void endNodeSwitch();                         // 切换确认或超时后解除加载态
    QString speedText(qint64 value) const;
    QColor delayColor(int delay) const;
    QString appStyle() const;
    QString lightStyle() const;
    void applyTheme(const QString &theme);
    void applyTitleBarColor(); // 通过 DWM 给系统原生标题栏着色
    QFrame *createSubscriptionCard(const SubscriptionSummary &subscription, int index);
    QWidget *buildRuleTableTab(const QString &section);
    void reloadRuleTable(const QString &section);
    void openRuleEditor(const QString &section, int editIndex);
    void openAreaEditor(int editIndex); // 新增/编辑 default.yaml 里的代理组
    QString defaultConfigPath() const; // userDir/default.yaml：规则/分组的读写源
    QStringList proxyGroupNames() const; // 供规则「节点」下拉选择
    QJsonArray loadRuleSection(const QString &section) const;
    bool saveRuleSection(const QString &section, const QJsonArray &array);
    void setCurrentPage(int page);
    void applyAutoStart(bool enabled);
    void applyAutoUpdate(int minutes);
    void registerUrlScheme();
    void updateMihomoCore(QPushButton *btn, bool useMirror); // 从 GitHub 下载最新 mihomo 内核并替换（useMirror=国内加速）
    QString extractCoreBinary(const QString &archivePath, const QString &tmpDir); // 解压 zip/gz 得到内核二进制
    void promptDownloadCore();  // 未检测到内核时弹窗提示，确认后跳转设置下载
    void goToCoreDownload();    // 跳到「设置 → 系统」并滚动/高亮「更新内核」按钮
    void onToggleTunRequested(); // 增强/TUN 开关入口：开启且非管理员时先提权（对齐旧项目按需提权）
    void onToggleProxyRequested(); // 网页(系统代理)开关入口：切换并把状态落盘（重启/更新后恢复）
#if defined(Q_OS_WIN)
    static bool isProcessElevated();     // 当前进程是否已提权（管理员）
    bool relaunchElevatedForTun();       // 以管理员身份重启自身并带 --tun-elevated；true=已重启（本实例将退出）
    void launchSilentUpdateAndRestart(const QString &installerPath); // 一键更新收尾：静默安装并自动重启
#endif
#if defined(Q_OS_MACOS)
    void refreshMacHelperStatus();       // 刷新设置页「免密助手」状态文字
    void installMacHelper();             // 注册特权 helper（未批准则引导系统设置并轮询）
    void uninstallMacHelper();           // 注销特权 helper
    void startMacHelperApprovalWatch();  // 引导批准后轮询，状态变为已启用时提示
#endif

    ClashService m_service;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_logo = nullptr;
    QString m_theme = "black";
    QFrame *m_nodeList = nullptr;
    QLabel *m_nodeTitle = nullptr;
    QComboBox *m_nodeGroupSelector = nullptr;
    QLineEdit *m_nodeSearch = nullptr;
    QLabel *m_upValue = nullptr;
    QLabel *m_downValue = nullptr;
    QLabel *m_processValue = nullptr;
    QLabel *m_totalDownValue = nullptr;
    QLabel *m_logLabel = nullptr;
    QVBoxLayout *m_logTimeline = nullptr;
    QVBoxLayout *m_clashTimeline = nullptr;
    QScrollArea *m_logScroll = nullptr;
    QScrollArea *m_clashScroll = nullptr;
    QString m_logFilePath;
    QFile m_logFile; // 常开的日志文件句柄：避免每条日志 open/append/close（VM 上文件打开慢）
    QLabel *m_usersLabel = nullptr;
    QLabel *m_versionLabel = nullptr; // 关于页版本行；发现新版本时高亮提示
    QLabel *m_sidebarVersionLabel = nullptr; // 侧栏底部版本行；发现新版本时变红提示（点击检查/更新）
    QString m_proxyHost = QStringLiteral("127.0.0.1"); // 下载走代理用的主机（对齐 config.host）
    int m_proxyMixedPort = 7890;                        // 下载走代理用的混合端口（对齐 config.mixedPort）
    QWidget *m_tunDot = nullptr; // BreathingDot（自绘呼吸圆点）
    QWidget *m_proxyDot = nullptr;
    QWidget *m_coreDot = nullptr;
    QFrame *m_subscriptionList = nullptr;
    QTableWidget *m_areaTable = nullptr;
    QTableWidget *m_ruleTable = nullptr;
    QTabWidget *m_settingsTabs = nullptr;      // 设置页 tab（过滤/区域/规则/系统）
    QScrollArea *m_sysScroll = nullptr;        // 系统 tab 的滚动区（用于滚到「更新内核」）
    QPushButton *m_coreUpdateBtn = nullptr;    // 「更新内核」按钮（无内核时高亮引导）
    QCheckBox *m_cnAccelCheck = nullptr;       // 设置页「国内加速」勾选框（供 setMirrorEnabled 同步，避免与更新弹窗不同步）
    QCheckBox *m_webProxyCheck = nullptr;      // 设置页「系统代理」勾选框（底部「网页」开关切换时同步，避免「应用」用旧值写回）
#if defined(Q_OS_MACOS)
    QLabel *m_helperStatusLabel = nullptr;     // 设置页「免密助手」状态文字
    QTimer *m_helperApprovalTimer = nullptr;   // 引导批准后轮询状态
    int m_helperApprovalTicks = 0;
#endif
    bool m_mirror = false;                     // 下载走国内镜像（对应 config.mirror）
    bool m_coreMissingPrompted = false;        // 避免重复弹「未检测到内核」窗
    QLineEdit *m_ruleFilter = nullptr;   // 规则搜索过滤
    QLabel *m_ruleCountLabel = nullptr;  // 规则条数/显示上限提示
    QLabel *m_areaCountLabel = nullptr;
    QString m_userDir;
    TrafficChart *m_upChart = nullptr;
    TrafficChart *m_downChart = nullptr;
    CoreController *m_core = nullptr;
    TrayController *m_tray = nullptr;
    SubscriptionStore *m_subscriptions = nullptr;
    QVector<QPushButton *> m_menuButtons;
    QVector<NodeInfo> m_currentNodes;
    QScrollArea *m_nodeScroll = nullptr;              // 节点列表滚动区（刷新时保留滚动位置）
    QHash<QString, QPointer<QLabel>> m_delayBadges;   // 节点名→延迟药丸，供原地更新（不清空重建）
    // 以下三张表配合 syncNodeRows 做「原地重排 + 原地改内容」：延迟/速度变化时只移动现有行、改文字，
    // 绝不销毁重建，从而既能按新次序排序、又不会闪现（对齐旧项目 Vue 的响应式原地更新）。
    QHash<QString, QPointer<QFrame>> m_nodeRows;       // 节点名→整行
    QHash<QString, QPointer<QPushButton>> m_nodeButtons; // 节点名→应用/禁用按钮
    QHash<QString, QPointer<QLabel>> m_nodeNameLabels; // 节点名→名称标签（实为 ElidingLabel，组行的「→子节点」会变）
    QString m_selectedNode;
    // 切换加载态（对齐旧项目 disableLoading）：点应用/禁用后，目标按钮转圈、其余按钮禁用，
    // 直到轮询确认「当前节点==目标」或超时才解除（防重入、给用户明确反馈）
    bool m_nodeSwitching = false;
    bool m_speedTesting = false;                       // 下载测速进行中：测速按钮禁用+显示⏳，忽略重复点击
    QString m_nodeSwitchTarget;                       // 被点击的节点名（该按钮转圈）
    QString m_nodeSwitchFrom;                          // 点击前的活动节点；当前活动 != 它即视为切换完成
    QPointer<QPushButton> m_spinnerButton;            // 正在转圈的目标按钮
    QTimer *m_spinnerTimer = nullptr;                 // 转圈动画计时器
    int m_spinnerFrame = 0;
    bool m_dragging = false;
    bool m_inSizeMove = false;        // Windows 交互式拖动/缩放进行中（WM_ENTERSIZEMOVE~EXITSIZEMOVE）
    QSize m_sizeMoveEntrySize;        // 进入拖动/缩放时的客户区尺寸：尺寸没变=纯移动，擦底可整个跳过
    bool m_nodeResyncPending = false; // 拖动/缩放期间挂起的节点列表刷新，结束后补一次
    bool m_closeToTray = true;
    bool m_trayHintShown = false;
    bool m_nodeSwitchNote = true;
    bool m_nodeOnlyAvailable = true;                   // 「仅可用节点」：开启时状态页隐藏 delay<=0（未测/超时）的节点
    bool m_nodeInitialized = false;
    bool m_autoTheme = false;
    int m_autoUpdateMinutes = 0;
    int m_runMinutes = 0;
    QTimer *m_autoUpdateTimer = nullptr;
    QTimer *m_nodeSearchTimer = nullptr;  // 状态页搜索框去抖（250ms）：避免每个按键都整表重建
    QTimer *m_subRebuildTimer = nullptr;  // 订阅更新后 rebuildConfig 去抖（500ms）：合并批量更新的多次热重载
    QPoint m_dragStart;
};
