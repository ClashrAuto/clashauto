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

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QWidget *buildTitleBar();
    QWidget *buildSidebar();
    QWidget *buildStatusPage();
    QWidget *buildSubscriptionsPage();
    QWidget *buildSubscriptionsPageLegacy();
    QWidget *buildSettingsPage();
    QWidget *buildLogsPage();
    QWidget *buildVipPage();
    QWidget *buildAboutPage();
    QWidget *buildFooter();
    QFrame *createMetricCard(const QString &icon, const QString &title, QLabel **valueLabel, const QString &className);
    QPushButton *createMenuButton(const QString &text, int page);
    QFrame *createNodeRow(const NodeInfo &node);
    QFrame *createSubscriptionRow(const SubscriptionSummary &subscription, int index);
    void showSubscriptionNodes(int subscriptionIndex);
    void appendLog(const QString &message);
    QLabel *createSwitchDot(bool enabled);
    void reloadSubscriptions();
    void populateNodeList();
    QString speedText(qint64 value) const;
    QColor delayColor(int delay) const;
    QString appStyle() const;
    void setCurrentPage(int page);

    ClashService m_service;
    QStackedWidget *m_pages = nullptr;
    QFrame *m_nodeList = nullptr;
    QLabel *m_nodeTitle = nullptr;
    QComboBox *m_nodeGroupSelector = nullptr;
    QLineEdit *m_nodeSearch = nullptr;
    QLabel *m_upValue = nullptr;
    QLabel *m_downValue = nullptr;
    QLabel *m_processValue = nullptr;
    QLabel *m_totalDownValue = nullptr;
    QLabel *m_logLabel = nullptr;
    QTextEdit *m_logView = nullptr;
    QString m_logFilePath;
    QLabel *m_usersLabel = nullptr;
    QLabel *m_tunDot = nullptr;
    QLabel *m_proxyDot = nullptr;
    QLabel *m_coreDot = nullptr;
    QFrame *m_subscriptionList = nullptr;
    TrafficChart *m_upChart = nullptr;
    TrafficChart *m_downChart = nullptr;
    CoreController *m_core = nullptr;
    TrayController *m_tray = nullptr;
    SubscriptionStore *m_subscriptions = nullptr;
    QVector<QPushButton *> m_menuButtons;
    QVector<NodeInfo> m_currentNodes;
    QString m_selectedNode;
    bool m_dragging = false;
    QPoint m_dragStart;
};
