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

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

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
    QLabel *createSwitchDot(bool enabled);
    void reloadSubscriptions();
    void populateNodeList();
    QString speedText(qint64 value) const;
    QColor delayColor(int delay) const;
    QString appStyle() const;
    QString lightStyle() const;
    void applyTheme(const QString &theme);
    QFrame *createSubscriptionCard(const SubscriptionSummary &subscription, int index);
    QWidget *buildRuleTableTab(const QString &section);
    void reloadRuleTable(const QString &section);
    void openRuleEditor(const QString &section, int editIndex);
    QString rulesFilePath() const;
    QJsonArray loadRuleSection(const QString &section) const;
    bool saveRuleSection(const QString &section, const QJsonArray &array);
    void setCurrentPage(int page);
    void applyAutoStart(bool enabled);

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
    QLabel *m_tunDot = nullptr;
    QLabel *m_proxyDot = nullptr;
    QLabel *m_coreDot = nullptr;
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
    QPoint m_dragStart;
};
