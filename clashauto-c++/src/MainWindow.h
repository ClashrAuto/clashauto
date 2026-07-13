#pragma once

#include "ClashService.h"
#include "CoreController.h"
#include "SubscriptionStore.h"
#include "TrafficChart.h"
#include "TrayController.h"

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
class QLineEdit;
class QScrollArea;
class QTabWidget;
class QVBoxLayout;
class QTableWidget;
class QJsonArray;
class QCloseEvent;
class QTimer;

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
    void checkForUpdate(bool silent); // 拉取最新 release 与本地版本比较；silent=启动自动检查
    void showConnectionsDialog();
    void appendLog(const QString &message);
    void appendTimeline(QVBoxLayout *layout, QScrollArea *scroll, const QString &message);
    QWidget *createSwitchDot(bool enabled);
    void reloadSubscriptions();
    void populateNodeList();
    void updateNodeBadges(); // 仅延迟/速度变化时原地更新药丸，避免整表重建导致的闪烁/清空
    void syncNodeRows();     // 集合不变时：原地更新药丸/名称/按钮态并按新次序重排现有行（不销毁 → 不闪现）
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
#if defined(Q_OS_WIN)
    static bool isProcessElevated();     // 当前进程是否已提权（管理员）
    void relaunchElevatedForTun();       // 以管理员身份重启自身并带 --tun-elevated
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
    QLabel *m_usersLabel = nullptr;
    QLabel *m_versionLabel = nullptr; // 关于页版本行；发现新版本时高亮提示
    QWidget *m_tunDot = nullptr; // BreathingDot（自绘呼吸圆点）
    QWidget *m_proxyDot = nullptr;
    QWidget *m_coreDot = nullptr;
    QFrame *m_subscriptionList = nullptr;
    QTableWidget *m_areaTable = nullptr;
    QTableWidget *m_ruleTable = nullptr;
    QTabWidget *m_settingsTabs = nullptr;      // 设置页 tab（过滤/区域/规则/系统）
    QScrollArea *m_sysScroll = nullptr;        // 系统 tab 的滚动区（用于滚到「更新内核」）
    QPushButton *m_coreUpdateBtn = nullptr;    // 「更新内核」按钮（无内核时高亮引导）
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
    bool m_closeToTray = true;
    bool m_trayHintShown = false;
    bool m_nodeSwitchNote = true;
    bool m_nodeInitialized = false;
    bool m_autoTheme = false;
    int m_autoUpdateMinutes = 0;
    int m_runMinutes = 0;
    QTimer *m_autoUpdateTimer = nullptr;
    QPoint m_dragStart;
};
