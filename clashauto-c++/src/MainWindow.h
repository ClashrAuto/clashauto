#pragma once

#include "ClashService.h"
#include "CoreController.h"
#include "SubscriptionStore.h"
#include "TrafficChart.h"
#include "TrayController.h"

#include <QFrame>
#include <QLabel>
#include <QMainWindow>
#include <QPoint>
#include <QPushButton>
#include <QStackedWidget>

class QTextEdit;
class QComboBox;
class QLineEdit;
class QScrollArea;
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
    void showSubscriptionNodes(int subscriptionIndex);
    void showUpdateDialog();
    void showConnectionsDialog();
    void appendLog(const QString &message);
    void appendTimeline(QVBoxLayout *layout, QScrollArea *scroll, const QString &message);
    QWidget *createSwitchDot(bool enabled);
    void reloadSubscriptions();
    void populateNodeList();
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
    QString rulesFilePath() const;
    QJsonArray loadRuleSection(const QString &section) const;
    bool saveRuleSection(const QString &section, const QJsonArray &array);
    void setCurrentPage(int page);
    void applyAutoStart(bool enabled);
    void applyAutoUpdate(int minutes);
    void registerUrlScheme();
    void updateMihomoCore(QPushButton *btn); // 从 GitHub 下载最新 mihomo 内核并替换
    QString extractCoreBinary(const QString &archivePath, const QString &tmpDir); // 解压 zip/gz 得到内核二进制

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
    QWidget *m_tunDot = nullptr; // BreathingDot（自绘呼吸圆点）
    QWidget *m_proxyDot = nullptr;
    QWidget *m_coreDot = nullptr;
    QFrame *m_subscriptionList = nullptr;
    QTableWidget *m_areaTable = nullptr;
    QTableWidget *m_ruleTable = nullptr;
    QString m_userDir;
    TrafficChart *m_upChart = nullptr;
    TrafficChart *m_downChart = nullptr;
    CoreController *m_core = nullptr;
    TrayController *m_tray = nullptr;
    SubscriptionStore *m_subscriptions = nullptr;
    QVector<QPushButton *> m_menuButtons;
    QVector<NodeInfo> m_currentNodes;
    QString m_selectedNode;
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
