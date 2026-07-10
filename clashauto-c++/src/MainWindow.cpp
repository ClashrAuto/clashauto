#include "MainWindow.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPen>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QDateTime>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int TitleHeight = 28;
constexpr int SidebarWidth = 120;
constexpr int FooterHeight = 38;
constexpr int Radius = 5;
constexpr int MainWidth = 900;
constexpr int MainHeight = 510;

// UI 尺寸对齐旧项目实测值（见 docs/legacy-ui-spec.md）
constexpr int MenuSpacing = 5;    // 菜单项 margin-bottom
constexpr int MetricGutter = 10;  // 流量卡网格 gutter（旧值 16）
constexpr int DelayBadgeW = 120;  // 节点速度/延迟列宽（旧值 104）

// 日志时间线左侧「圆点 + 竖轴线」——复刻 el-timeline 视觉
class TimelineRail : public QWidget
{
public:
    TimelineRail(const QColor &dot, const QColor &axis, QWidget *parent = nullptr)
        : QWidget(parent), m_dot(dot), m_axis(axis)
    {
        setFixedWidth(14);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int x = 6;
        p.setPen(QPen(m_axis, 2));
        p.drawLine(x, 0, x, height());
        p.setPen(Qt::NoPen);
        p.setBrush(m_dot);
        p.drawEllipse(QPoint(x, 9), 4, 4);
    }

private:
    QColor m_dot;
    QColor m_axis;
};
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    const AppConfig config = AppConfigLoader::load();
    QDir().mkpath(config.userDir + "/logs");
    m_userDir = config.userDir;
    m_logFilePath = config.userDir + "/logs/qt-main.log";
    m_core = new CoreController(config, this);
    m_tray = new TrayController(this, this);
    m_subscriptions = new SubscriptionStore(config, this);
    m_closeToTray = config.closeToTray;

    // 退出时停止核心并还原系统代理（关闭到托盘或从托盘退出都会走到这里）
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_core) {
            m_core->stopCore();
        }
    });

    setWindowTitle("Clash Auto");
    setWindowIcon(QIcon(":/assets/icon.ico"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground, true);
    resize(MainWidth, MainHeight);
    setMinimumSize(MainWidth, MainHeight);

    auto *root = new QFrame(this);
    root->setObjectName("root");
    auto *outer = new QVBoxLayout(root);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    outer->addWidget(buildTitleBar());

    auto *body = new QFrame(root);
    body->setObjectName("body");
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(buildSidebar());

    auto *right = new QFrame(body);
    right->setObjectName("rightPane");
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(10, 10, 10, 0);
    rightLayout->setSpacing(0);

    m_pages = new QStackedWidget(right);
    m_pages->addWidget(buildStatusPage());
    m_pages->addWidget(buildSubscriptionsPage());
    m_pages->addWidget(buildSettingsPage());
    m_pages->addWidget(buildLogsPage());
    m_pages->addWidget(buildAboutPage());

    rightLayout->addWidget(m_pages, 1);
    bodyLayout->addWidget(right, 1);
    outer->addWidget(body, 1);
    outer->addWidget(buildFooter());
    setCentralWidget(root);

    applyTheme(config.theme);
    setCurrentPage(0);

    connect(&m_service, &ClashService::trafficUpdated, this, [this](qint64 up, qint64 down) {
        m_upValue->setText(speedText(up));
        m_downValue->setText(speedText(down));
        m_upChart->addSample(up);
        m_downChart->addSample(down);
        m_tray->setTraffic(up, down);
    });
    connect(&m_service, &ClashService::connectionsUpdated, this, [this](int count, qint64 total) {
        m_processValue->setText(QString::number(count));
        m_totalDownValue->setText(speedText(total));
    });
    connect(&m_service, &ClashService::nodesUpdated, this, [this](const QVector<NodeInfo> &nodes, const QString &selected) {
        m_currentNodes = nodes;
        m_selectedNode = selected;
        populateNodeList();
    });
    connect(&m_service, &ClashService::proxyGroupsUpdated, this, [this](const QStringList &groups, const QString &selectedGroup) {
        if (!m_nodeGroupSelector) {
            return;
        }
        const QSignalBlocker blocker(m_nodeGroupSelector);
        m_nodeGroupSelector->clear();
        m_nodeGroupSelector->addItems(groups);
        const int index = m_nodeGroupSelector->findText(selectedGroup);
        if (index >= 0) {
            m_nodeGroupSelector->setCurrentIndex(index);
        }
    });
    connect(&m_service, &ClashService::logUpdated, this, [this](const QString &message) {
        appendLog(message);
    });
    auto applyStatus = [this](bool tun, bool proxy, bool core) {
        m_tunDot->setProperty("on", tun);
        m_proxyDot->setProperty("on", proxy);
        m_coreDot->setProperty("on", core);
        for (QLabel *dot : {m_tunDot, m_proxyDot, m_coreDot}) {
            dot->style()->unpolish(dot);
            dot->style()->polish(dot);
            auto *anim = dot->findChild<QPropertyAnimation *>();
            auto *effect = qobject_cast<QGraphicsOpacityEffect *>(dot->graphicsEffect());
            if (anim) {
                if (dot->property("on").toBool()) {
                    if (anim->state() != QAbstractAnimation::Running) {
                        anim->start();
                    }
                } else {
                    anim->stop();
                    if (effect) {
                        effect->setOpacity(1.0);
                    }
                }
            }
        }
        if (m_logo) {
            // 旧项目：TUN 开启=红(global)，核心运行=黄(proxy)，否则白
            m_logo->setProperty("state", tun ? "tun" : (core ? "proxy" : "off"));
            m_logo->style()->unpolish(m_logo);
            m_logo->style()->polish(m_logo);
        }
        m_tray->setStatus(tun, proxy, core);
    };
    connect(&m_service, &ClashService::statusUpdated, this, applyStatus);
    connect(m_core, &CoreController::statusChanged, this, applyStatus);
    connect(m_core, &CoreController::logUpdated, this, [this](const QString &message) {
        appendLog(message);
        appendTimeline(m_clashTimeline, m_clashScroll, message);
    });
    connect(m_tray, &TrayController::toggleCoreRequested, m_core, &CoreController::toggleCore);
    connect(m_tray, &TrayController::toggleProxyRequested, m_core, &CoreController::toggleProxy);
    connect(m_tray, &TrayController::toggleTunRequested, m_core, &CoreController::toggleTun);
    connect(m_subscriptions, &SubscriptionStore::subscriptionUpdated, this, [this](int, bool ok, const QString &message) {
        appendLog(message);
        reloadSubscriptions();
        if (ok) {
            m_core->rebuildConfig();
        }
    });

    m_service.start();
}

QWidget *MainWindow::buildTitleBar()
{
    auto *bar = new QFrame(this);
    bar->setObjectName("titleBar");
    bar->setFixedHeight(TitleHeight);

    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 0, 0, 0);
    layout->setSpacing(0);

    auto *title = new QLabel("Clash Auto", bar);
    title->setObjectName("windowTitle");
    layout->addWidget(title, 1);

    auto addButton = [&](const QString &text, const char *name, auto slot) {
        auto *button = new QPushButton(text, bar);
        button->setObjectName(name);
        button->setFixedSize(42, TitleHeight);
        connect(button, &QPushButton::clicked, this, slot);
        layout->addWidget(button);
    };

    addButton(QString::fromUtf8("－"), "titleButton", [this] { showMinimized(); });
    addButton(QString::fromUtf8("❐"), "titleButton", [this] { isMaximized() ? showNormal() : showMaximized(); });
    addButton(QString::fromUtf8("✕"), "closeButton", [this] { close(); });
    return bar;
}

QWidget *MainWindow::buildSidebar()
{
    auto *side = new QFrame(this);
    side->setObjectName("sidebar");
    side->setFixedWidth(SidebarWidth);
    auto *layout = new QVBoxLayout(side);
    layout->setContentsMargins(5, 16, 0, 10);
    layout->setSpacing(0);

    auto *logoWrap = new QFrame(side);
    logoWrap->setObjectName("logoWrap");
    logoWrap->setFixedHeight(118);
    auto *logoLayout = new QVBoxLayout(logoWrap);
    logoLayout->setContentsMargins(0, 0, 5, 0);
    m_logo = new QLabel(QString::fromUtf8("☁"), logoWrap);
    m_logo->setObjectName("logo");
    m_logo->setProperty("state", "off");
    m_logo->setAlignment(Qt::AlignCenter);
    logoLayout->addWidget(m_logo);
    layout->addWidget(logoWrap);

    const QStringList menu = {
        QString::fromUtf8("状态"),
        QString::fromUtf8("订阅"),
        QString::fromUtf8("设置"),
        QString::fromUtf8("日志"),
        QString::fromUtf8("关于")
    };
    auto *menuLayout = new QVBoxLayout();
    menuLayout->setContentsMargins(0, 0, 0, 0);
    menuLayout->setSpacing(MenuSpacing);
    for (int i = 0; i < menu.size(); ++i) {
        menuLayout->addWidget(createMenuButton(menu[i], i));
    }
    layout->addLayout(menuLayout);
    layout->addStretch();

    auto *version = new QLabel("Ver: 3.0.2", side);
    version->setObjectName("version");
    version->setAlignment(Qt::AlignCenter);
    version->setFixedHeight(24);
    layout->addWidget(version);
    return side;
}

QWidget *MainWindow::buildStatusPage()
{
    auto *page = new QFrame(this);
    page->setObjectName("page");
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(10);

    auto *left = new QFrame(page);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    auto *metricsBox = new QFrame(left);
    metricsBox->setObjectName("metricsBox");
    auto *metrics = new QGridLayout(metricsBox);
    metrics->setContentsMargins(0, 0, 0, 0);
    metrics->setHorizontalSpacing(MetricGutter);
    metrics->setVerticalSpacing(MetricGutter);
    metrics->addWidget(createMetricCard(QString::fromUtf8("☁"), QString::fromUtf8("上传"), &m_upValue, "up"), 0, 0);
    metrics->addWidget(createMetricCard(QString::fromUtf8("☁"), QString::fromUtf8("下载"), &m_downValue, "down"), 0, 1);
    metrics->addWidget(createMetricCard(QString::fromUtf8("⌁"), QString::fromUtf8("进程数"), &m_processValue, "process"), 1, 0);
    metrics->addWidget(createMetricCard(QString::fromUtf8("▾"), QString::fromUtf8("总下载"), &m_totalDownValue, "download"), 1, 1);
    leftLayout->addWidget(metricsBox);

    auto *bandTitle = new QLabel("实时带宽", left);
    bandTitle->setObjectName("sectionTitle");
    leftLayout->addWidget(bandTitle);
    m_upChart = new TrafficChart("上传", QColor(177, 74, 74), left);
    m_downChart = new TrafficChart("下载", QColor(91, 180, 75), left);
    leftLayout->addWidget(m_upChart, 1);
    leftLayout->addWidget(m_downChart, 1);

    auto *right = new QFrame(page);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(8);

    auto *nodeHeader = new QFrame(right);
    auto *nodeHeaderLayout = new QHBoxLayout(nodeHeader);
    nodeHeaderLayout->setContentsMargins(0, 0, 0, 0);
    m_nodeTitle = new QLabel(QString::fromUtf8("节点 <span style='font-size:9px'>(0)</span>"), nodeHeader);
    m_nodeTitle->setObjectName("sectionTitle");
    nodeHeaderLayout->addWidget(m_nodeTitle);
    m_nodeSearch = new QLineEdit(nodeHeader);
    m_nodeSearch->setPlaceholderText(QString::fromUtf8("搜索"));
    m_nodeSearch->setMinimumWidth(80);
    nodeHeaderLayout->addWidget(m_nodeSearch, 1); // 自适应：占满标题栏剩余宽度
    m_nodeGroupSelector = new QComboBox(nodeHeader);
    m_nodeGroupSelector->addItem("GLOBAL");
    m_nodeGroupSelector->setFixedWidth(130);
    nodeHeaderLayout->addWidget(m_nodeGroupSelector);
    auto *refresh = new QPushButton("R", nodeHeader);
    refresh->setObjectName("iconButton");
    refresh->setFixedWidth(30);
    nodeHeaderLayout->addWidget(refresh);
    auto *speedTest = new QPushButton(QString::fromUtf8("⚡"), nodeHeader);
    speedTest->setObjectName("iconButton");
    speedTest->setFixedWidth(30);
    speedTest->setToolTip(QString::fromUtf8("测速(测试当前分组延迟)"));
    nodeHeaderLayout->addWidget(speedTest);
    rightLayout->addWidget(nodeHeader);
    connect(m_nodeSearch, &QLineEdit::textChanged, this, [this] { populateNodeList(); });
    connect(m_nodeGroupSelector, &QComboBox::currentTextChanged, &m_service, &ClashService::setSelectedGroup);
    connect(refresh, &QPushButton::clicked, &m_service, &ClashService::refreshNodes);
    connect(speedTest, &QPushButton::clicked, this, [this] {
        appendLog(QString::fromUtf8("开始测速..."));
        m_service.testDelays();
    });

    auto *scroll = new QScrollArea(right);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nodeList = new QFrame(scroll);
    auto *nodeLayout = new QVBoxLayout(m_nodeList);
    nodeLayout->setContentsMargins(0, 0, 0, 0);
    nodeLayout->setSpacing(10);
    scroll->setWidget(m_nodeList);
    rightLayout->addWidget(scroll, 1);

    layout->addWidget(left, 1);
    layout->addWidget(right, 1);
    return page;
}

QWidget *MainWindow::buildSubscriptionsPage()
{
    auto *page = new QFrame(this);
    page->setObjectName("page");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_subscriptionList = new QFrame(scroll);
    auto *grid = new QGridLayout(m_subscriptionList);
    grid->setContentsMargins(0, 0, 5, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    scroll->setWidget(m_subscriptionList);
    layout->addWidget(scroll, 1);

    auto *bottom = new QHBoxLayout();
    bottom->setContentsMargins(0, 0, 0, 0);
    auto *applyButton = new QPushButton(QString::fromUtf8("应用配置"), page);
    applyButton->setObjectName("primaryButton");
    applyButton->setFixedSize(96, 30);
    auto *updateAll = new QPushButton(QString::fromUtf8("更新全部"), page);
    updateAll->setObjectName("primaryButton");
    updateAll->setFixedSize(96, 30);
    bottom->addWidget(applyButton);
    bottom->addStretch();
    bottom->addWidget(updateAll);
    layout->addLayout(bottom);

    connect(applyButton, &QPushButton::clicked, m_core, &CoreController::rebuildConfig);
    connect(updateAll, &QPushButton::clicked, this, [this] {
        const QVector<SubscriptionSummary> subs = m_subscriptions->load();
        for (int i = 0; i < subs.size(); ++i) {
            m_subscriptions->updateSubscription(i);
        }
        appendLog(QString::fromUtf8("正在更新全部订阅..."));
    });

    reloadSubscriptions();
    return page;
}

QFrame *MainWindow::createSubscriptionCard(const SubscriptionSummary &subscription, int index)
{
    auto *card = new QFrame(this);
    card->setObjectName("subCard");
    card->setProperty("on", subscription.use);
    card->setMinimumHeight(103);
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(4);

    const QString title = subscription.name.isEmpty() ? subscription.url : subscription.name;
    auto *titleLabel = new QLabel(QString("[%1] %2").arg(subscription.type.isEmpty() ? "sub" : subscription.type, title), card);
    titleLabel->setObjectName("subCardTitle");
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titleLabel->setToolTip(subscription.url);
    v->addWidget(titleLabel);

    auto *meta = new QLabel(QString::fromUtf8("%1 / %2 节点").arg(subscription.enabledNodeCount).arg(subscription.nodeCount), card);
    meta->setObjectName("subCardMeta");
    v->addWidget(meta);
    v->addStretch();

    auto *btns = new QHBoxLayout();
    btns->setContentsMargins(0, 0, 0, 0);
    btns->setSpacing(3);
    auto addCircle = [&](const QString &glyph, const QString &tip, bool on, bool danger, auto slot) {
        auto *b = new QPushButton(glyph, card);
        b->setObjectName("circleButton");
        b->setToolTip(tip);
        if (on) {
            b->setProperty("on", true);
        }
        if (danger) {
            b->setProperty("danger", true);
        }
        connect(b, &QPushButton::clicked, this, slot);
        btns->addWidget(b);
    };

    addCircle(QString::fromUtf8("✓"), QString::fromUtf8("启用/停用"), subscription.use, false, [this, index, subscription] {
        if (m_subscriptions->setSubscriptionEnabled(index, !subscription.use)) {
            reloadSubscriptions();
            m_core->rebuildConfig();
        }
    });
    addCircle(QString::fromUtf8("⚡"), QString::fromUtf8("测速"), false, false, [this, index] {
        const QVector<SubscriptionNodeSummary> nodeList = m_subscriptions->nodes(index);
        QStringList names;
        for (const SubscriptionNodeSummary &n : nodeList) {
            if (!n.name.isEmpty()) {
                names << n.name;
            }
        }
        if (names.isEmpty()) {
            appendLog(QString::fromUtf8("该订阅暂无节点，请先更新"));
            return;
        }
        appendLog(QString::fromUtf8("正在测速该订阅 %1 个节点...").arg(names.size()));
        m_service.testNodeDelays(names);
    });
    addCircle(QString::fromUtf8("👁"), QString::fromUtf8("查看节点"), false, false, [this, index] {
        showSubscriptionNodes(index);
    });
    addCircle(QString::fromUtf8("⚙"), QString::fromUtf8("编辑"), false, false, [this, index, subscription] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QString::fromUtf8("编辑订阅"), QString::fromUtf8("名称:"), QLineEdit::Normal, subscription.name, &ok);
        if (!ok) {
            return;
        }
        const QString url = QInputDialog::getText(this, QString::fromUtf8("编辑订阅"), "URL:", QLineEdit::Normal, subscription.url, &ok);
        if (!ok || url.trimmed().isEmpty()) {
            return;
        }
        const QStringList types = {"sub", "clash"};
        const QString type = QInputDialog::getItem(this, QString::fromUtf8("编辑订阅"), QString::fromUtf8("类型:"), types,
                                                   subscription.type.compare("clash", Qt::CaseInsensitive) == 0 ? 1 : 0, false, &ok);
        if (!ok) {
            return;
        }
        if (m_subscriptions->editSubscription(index, name.trimmed(), url.trimmed(), type)) {
            reloadSubscriptions();
            m_core->rebuildConfig();
            appendLog(QString::fromUtf8("订阅已更新"));
        }
    });
    addCircle(QString::fromUtf8("⟳"), QString::fromUtf8("更新"), false, false, [this, index] {
        appendLog(QString::fromUtf8("正在更新订阅..."));
        m_subscriptions->updateSubscription(index);
    });
    addCircle(QString::fromUtf8("✕"), QString::fromUtf8("删除"), false, true, [this, index, subscription] {
        const QString label = subscription.name.isEmpty() ? subscription.url : subscription.name;
        if (QMessageBox::question(this, QString::fromUtf8("删除订阅"),
                                  QString::fromUtf8("确定删除订阅 [%1] %2 ?").arg(subscription.type.isEmpty() ? "sub" : subscription.type, label))
            != QMessageBox::Yes) {
            return;
        }
        if (m_subscriptions->removeSubscription(index)) {
            reloadSubscriptions();
            m_core->rebuildConfig();
            appendLog(QString::fromUtf8("已删除订阅"));
        }
    });
    btns->addStretch();
    v->addLayout(btns);
    return card;
}

QWidget *MainWindow::buildSettingsPage()
{
    const AppConfig config = AppConfigLoader::load();

    // 与保存逻辑共享的控件
    auto *host = new QComboBox();
    host->setEditable(true);
    host->addItems({"127.0.0.1", "localhost"});
    host->setCurrentText(config.host);
    auto *uiPort = new QLineEdit(QString::number(config.uiPort));
    uiPort->setFixedWidth(200);
    auto *mixedPort = new QLineEdit(QString::number(config.mixedPort));
    mixedPort->setFixedWidth(200);
    auto *allowUse = new QCheckBox();
    allowUse->setChecked(config.allowRuleEnabled);
    auto *allowRule = new QLineEdit(config.allowRule.isEmpty() ? "\\[0\\.[0-9]+\\]" : config.allowRule);
    auto *blockUse = new QCheckBox();
    blockUse->setChecked(config.noAllowRuleEnabled);
    auto *blockRule = new QLineEdit(config.noAllowRule.isEmpty() ? "CN|^CN|CN_" : config.noAllowRule);
    auto *nodeOnly = new QCheckBox();
    nodeOnly->setChecked(config.nodeOnlyAvailable);
    auto *webProxy = new QCheckBox();
    webProxy->setChecked(config.webProxy);
    auto *tun = new QCheckBox();
    tun->setChecked(config.tun);
    auto *clearConnections = new QCheckBox();
    clearConnections->setChecked(config.clearConnections);
    auto *increment = new QCheckBox();
    increment->setChecked(config.increment);
    auto *closeToTray = new QCheckBox();
    closeToTray->setChecked(config.closeToTray);
    auto *autoStart = new QCheckBox();
    autoStart->setChecked(config.autoStart);
    auto *theme = new QComboBox();
    theme->addItems({QString::fromUtf8("黑色"), QString::fromUtf8("白色")});
    theme->setFixedWidth(200);
    theme->setCurrentIndex(config.theme.compare("light", Qt::CaseInsensitive) == 0 ? 1 : 0);
    auto *language = new QComboBox();
    language->addItems({"zh-CN", "en-US"});
    language->setFixedWidth(200);
    language->setCurrentText(config.language);

    auto *tabs = new QTabWidget(this);
    tabs->setObjectName("settingsTabs");

    auto row = [](const QString &label, QWidget *w) -> QWidget * {
        auto *r = new QWidget();
        auto *h = new QHBoxLayout(r);
        h->setContentsMargins(0, 0, 0, 0);
        auto *l = new QLabel(label, r);
        l->setMinimumWidth(140);
        h->addWidget(l);
        h->addWidget(w);
        h->addStretch();
        return r;
    };

    // Tab 1: 远程
    auto *remote = new QWidget(tabs);
    auto *remoteLayout = new QVBoxLayout(remote);
    remoteLayout->setContentsMargins(10, 16, 10, 10);
    remoteLayout->setSpacing(8);
    auto *remoteTitle = new QLabel(QString::fromUtf8("远程 API 地址"), remote);
    remoteTitle->setObjectName("settingGroupTitle");
    remoteLayout->addWidget(remoteTitle);
    host->setMinimumWidth(300);
    remoteLayout->addWidget(row(QString::fromUtf8("Host"), host));
    remoteLayout->addStretch();
    tabs->addTab(remote, QString::fromUtf8("远程"));

    // Tab 2: 过滤
    auto *filter = new QWidget(tabs);
    auto *filterLayout = new QVBoxLayout(filter);
    filterLayout->setContentsMargins(10, 16, 10, 10);
    filterLayout->setSpacing(8);
    allowRule->setMinimumWidth(300);
    blockRule->setMinimumWidth(300);
    filterLayout->addWidget(row(QString::fromUtf8("启用允许规则"), allowUse));
    filterLayout->addWidget(row(QString::fromUtf8("允许规则(正则)"), allowRule));
    filterLayout->addWidget(row(QString::fromUtf8("启用排除规则"), blockUse));
    filterLayout->addWidget(row(QString::fromUtf8("排除规则(正则)"), blockRule));
    filterLayout->addStretch();
    tabs->addTab(filter, QString::fromUtf8("过滤"));

    // Tab 3 / 4: 区域 / 规则（真数据 CRUD，持久化到 userDir/rules.json）
    tabs->addTab(buildRuleTableTab("area"), QString::fromUtf8("区域"));
    tabs->addTab(buildRuleTableTab("rule"), QString::fromUtf8("规则"));

    // Tab 5: 系统（分组 + 虚线分隔）
    auto *systemScroll = new QScrollArea(tabs);
    systemScroll->setWidgetResizable(true);
    systemScroll->setFrameShape(QFrame::NoFrame);
    auto *sysBody = new QWidget(systemScroll);
    auto *sysLayout = new QVBoxLayout(sysBody);
    sysLayout->setContentsMargins(10, 16, 10, 10);
    sysLayout->setSpacing(8);
    auto addGroup = [&](const QString &title) {
        auto *h = new QLabel(title, sysBody);
        h->setObjectName("settingGroupTitle");
        sysLayout->addWidget(h);
    };
    auto addDivider = [&]() {
        auto *d = new QFrame(sysBody);
        d->setObjectName("settingDivider");
        d->setFixedHeight(12);
        sysLayout->addWidget(d);
    };
    addGroup(QString::fromUtf8("界面"));
    sysLayout->addWidget(row(QString::fromUtf8("主题"), theme));
    sysLayout->addWidget(row(QString::fromUtf8("语言"), language));
    addDivider();
    addGroup(QString::fromUtf8("节点"));
    sysLayout->addWidget(row(QString::fromUtf8("仅可用节点"), nodeOnly));
    sysLayout->addWidget(row(QString::fromUtf8("增量更新"), increment));
    addDivider();
    addGroup(QString::fromUtf8("内核"));
    sysLayout->addWidget(row(QString::fromUtf8("API 端口"), uiPort));
    sysLayout->addWidget(row(QString::fromUtf8("混合端口"), mixedPort));
    sysLayout->addWidget(row(QString::fromUtf8("TUN"), tun));
    sysLayout->addWidget(row(QString::fromUtf8("系统代理"), webProxy));
    sysLayout->addWidget(row(QString::fromUtf8("切换时清理连接"), clearConnections));
    addDivider();
    addGroup(QString::fromUtf8("托盘 / 启动"));
    sysLayout->addWidget(row(QString::fromUtf8("关闭到托盘"), closeToTray));
    sysLayout->addWidget(row(QString::fromUtf8("开机自启"), autoStart));
    sysLayout->addStretch();
    systemScroll->setWidget(sysBody);
    tabs->addTab(systemScroll, QString::fromUtf8("系统"));

    // 页面 + 顶部右侧保存（对应旧项目右上角「应用」）
    auto *page = new QFrame(this);
    page->setObjectName("page");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(4);
    auto *topBar = new QHBoxLayout();
    topBar->setContentsMargins(0, 0, 0, 0);
    topBar->addStretch();
    auto *save = new QPushButton(QString::fromUtf8("应用"), page);
    save->setObjectName("primaryButton");
    save->setFixedSize(82, 28);
    topBar->addWidget(save);
    layout->addLayout(topBar);
    layout->addWidget(tabs, 1);

    connect(save, &QPushButton::clicked, this, [=] {
        const QString path = QDir(config.userDir).filePath("config.yaml");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            appendLog(QString("Save settings failed: %1").arg(file.errorString()));
            return;
        }
        const bool light = theme->currentIndex() == 1;
        QTextStream out(&file);
        out << "host: " << host->currentText().trimmed() << "\n";
        out << "ui: " << uiPort->text().trimmed() << "\n";
        out << "port: " << mixedPort->text().trimmed() << "\n";
        out << "web: " << (webProxy->isChecked() ? "true" : "false") << "\n";
        out << "use: " << (tun->isChecked() ? "true" : "false") << "\n";
        out << "node: " << (nodeOnly->isChecked() ? "true" : "false") << "\n";
        out << "clearConnections: " << (clearConnections->isChecked() ? "true" : "false") << "\n";
        out << "increment: " << (increment->isChecked() ? "true" : "false") << "\n";
        out << "mini: " << (closeToTray->isChecked() ? "true" : "false") << "\n";
        out << "sys: " << (autoStart->isChecked() ? "true" : "false") << "\n";
        out << "theme: " << (light ? "light" : "black") << "\n";
        out << "language: " << language->currentText() << "\n";
        out << "use_rule:\n";
        out << "  allow: " << allowRule->text() << "\n";
        out << "  noallow: " << blockRule->text() << "\n";
        out << "  allowUse: " << (allowUse->isChecked() ? "true" : "false") << "\n";
        out << "  noallowUse: " << (blockUse->isChecked() ? "true" : "false") << "\n";
        appendLog(QString("Settings saved: %1").arg(path));
        m_closeToTray = closeToTray->isChecked();
        applyAutoStart(autoStart->isChecked());
        applyTheme(light ? "light" : "black");
        m_core->rebuildConfig();
    });

    return page;
}

QString MainWindow::rulesFilePath() const
{
    return QDir(m_userDir).filePath("rules.json");
}

QJsonArray MainWindow::loadRuleSection(const QString &section) const
{
    QFile file(rulesFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    return root.value(section).toArray();
}

bool MainWindow::saveRuleSection(const QString &section, const QJsonArray &array)
{
    QFile file(rulesFilePath());
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }
    root[section] = array;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

QWidget *MainWindow::buildRuleTableTab(const QString &section)
{
    const bool isArea = section == "area";
    const QStringList headers = isArea
        ? QStringList{QString::fromUtf8("名称"), QString::fromUtf8("类型"), QString::fromUtf8("节点"), QString::fromUtf8("操作")}
        : QStringList{QString::fromUtf8("类型"), QString::fromUtf8("节点"), QString::fromUtf8("值"), QString::fromUtf8("操作")};
    const int stretchCol = 2;

    auto *w = new QWidget();
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(10, 10, 10, 10);
    l->setSpacing(8);

    auto *addBtn = new QPushButton(QString::fromUtf8("＋ 添加"), w);
    addBtn->setObjectName("primaryButton");
    addBtn->setFixedSize(96, 30);
    connect(addBtn, &QPushButton::clicked, this, [this, section] { openRuleEditor(section, -1); });
    auto *bar = new QHBoxLayout();
    bar->setContentsMargins(0, 0, 0, 0);
    bar->addWidget(addBtn);
    bar->addStretch();
    l->addLayout(bar);

    auto *table = new QTableWidget(0, headers.size(), w);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int c = 0; c < headers.size(); ++c) {
        if (c == stretchCol) {
            table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Stretch);
        } else if (c == headers.size() - 1) {
            table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Fixed);
            table->setColumnWidth(c, 100);
        } else {
            table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Fixed);
            table->setColumnWidth(c, 180);
        }
    }
    l->addWidget(table, 1);

    if (isArea) {
        m_areaTable = table;
    } else {
        m_ruleTable = table;
    }
    reloadRuleTable(section);
    return w;
}

void MainWindow::reloadRuleTable(const QString &section)
{
    QTableWidget *table = section == "area" ? m_areaTable : m_ruleTable;
    if (!table) {
        return;
    }
    const QStringList keys = section == "area"
        ? QStringList{"name", "type", "rule"}
        : QStringList{"type", "node", "value"};
    const QJsonArray array = loadRuleSection(section);

    table->setRowCount(0);
    for (int i = 0; i < array.size(); ++i) {
        const QJsonObject obj = array[i].toObject();
        const int row = table->rowCount();
        table->insertRow(row);
        for (int c = 0; c < keys.size(); ++c) {
            table->setItem(row, c, new QTableWidgetItem(obj.value(keys[c]).toString()));
        }

        auto *actions = new QWidget();
        auto *ah = new QHBoxLayout(actions);
        ah->setContentsMargins(0, 0, 0, 0);
        ah->setSpacing(4);
        auto *edit = new QPushButton(QString::fromUtf8("✎"), actions);
        edit->setObjectName("iconButton");
        edit->setFixedWidth(28);
        auto *del = new QPushButton(QString::fromUtf8("✕"), actions);
        del->setObjectName("iconButton");
        del->setFixedWidth(28);
        ah->addStretch();
        ah->addWidget(edit);
        ah->addWidget(del);
        ah->addStretch();
        connect(edit, &QPushButton::clicked, this, [this, section, i] { openRuleEditor(section, i); });
        connect(del, &QPushButton::clicked, this, [this, section, i] {
            QJsonArray current = loadRuleSection(section);
            if (i >= 0 && i < current.size()) {
                current.removeAt(i);
                saveRuleSection(section, current);
                reloadRuleTable(section);
                if (m_core) {
                    m_core->rebuildConfig();
                }
                appendLog(QString::fromUtf8("已删除一条%1").arg(section == "area" ? QString::fromUtf8("区域") : QString::fromUtf8("规则")));
            }
        });
        table->setCellWidget(row, keys.size(), actions);
    }
}

void MainWindow::openRuleEditor(const QString &section, int editIndex)
{
    const bool isArea = section == "area";
    const QStringList keys = isArea ? QStringList{"name", "type", "rule"} : QStringList{"type", "node", "value"};
    const QStringList labels = isArea
        ? QStringList{QString::fromUtf8("名称"), QString::fromUtf8("类型 (select/url-test/fallback)"), QString::fromUtf8("节点匹配 (正则)")}
        : QStringList{QString::fromUtf8("类型 (DOMAIN-SUFFIX/IP-CIDR/...)"), QString::fromUtf8("节点/策略"), QString::fromUtf8("值")};

    const QJsonArray array = loadRuleSection(section);
    const QJsonObject cur = (editIndex >= 0 && editIndex < array.size()) ? array[editIndex].toObject() : QJsonObject{};

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(isArea ? QString::fromUtf8("区域分组") : QString::fromUtf8("自定义规则"));
    dialog->setStyleSheet(styleSheet());
    dialog->resize(380, 260);
    auto *dl = new QVBoxLayout(dialog);
    dl->setContentsMargins(12, 12, 12, 12);
    dl->setSpacing(6);

    QVector<QLineEdit *> edits;
    for (int i = 0; i < keys.size(); ++i) {
        auto *lab = new QLabel(labels[i], dialog);
        auto *ed = new QLineEdit(cur.value(keys[i]).toString(), dialog);
        dl->addWidget(lab);
        dl->addWidget(ed);
        edits << ed;
    }
    dl->addStretch();

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto *cancel = new QPushButton(QString::fromUtf8("取消"), dialog);
    cancel->setObjectName("nodeButton");
    cancel->setFixedSize(72, 30);
    auto *ok = new QPushButton(QString::fromUtf8("确定"), dialog);
    ok->setObjectName("primaryButton");
    ok->setFixedSize(72, 30);
    btnRow->addWidget(cancel);
    btnRow->addWidget(ok);
    dl->addLayout(btnRow);

    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(ok, &QPushButton::clicked, dialog, [this, dialog, section, editIndex, keys, edits] {
        QJsonObject obj;
        for (int i = 0; i < keys.size(); ++i) {
            obj[keys[i]] = edits[i]->text().trimmed();
        }
        QJsonArray current = loadRuleSection(section);
        if (editIndex >= 0 && editIndex < current.size()) {
            current[editIndex] = obj;
        } else {
            current.append(obj);
        }
        saveRuleSection(section, current);
        reloadRuleTable(section);
        if (m_core) {
            m_core->rebuildConfig();
        }
        appendLog(QString::fromUtf8("已保存一条%1").arg(section == "area" ? QString::fromUtf8("区域") : QString::fromUtf8("规则")));
        dialog->accept();
    });
    dialog->show();
}

QWidget *MainWindow::buildLogsPage()
{
    auto *page = new QFrame(this);
    page->setObjectName("page");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->addStretch();
    auto *openDir = new QPushButton(QString::fromUtf8("打开日志目录"), page);
    openDir->setObjectName("iconButton");
    header->addWidget(openDir);
    layout->addLayout(header);

    auto *tabs = new QTabWidget(page);
    tabs->setObjectName("logsTabs");

    auto makeTimelineTab = [](QScrollArea *&scrollOut, QVBoxLayout *&layoutOut) -> QWidget * {
        auto *scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *body = new QFrame();
        body->setObjectName("timelineBody");
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(6, 6, 6, 6);
        v->setSpacing(0);
        v->addStretch();
        scroll->setWidget(body);
        scrollOut = scroll;
        layoutOut = v;
        return scroll;
    };

    tabs->addTab(makeTimelineTab(m_logScroll, m_logTimeline), QString::fromUtf8("主日志"));
    tabs->addTab(makeTimelineTab(m_clashScroll, m_clashTimeline), "Clash");

    layout->addWidget(tabs, 1);

    connect(openDir, &QPushButton::clicked, this, [this] {
        const QString dir = m_logFilePath.isEmpty() ? QDir::homePath() : QFileInfo(m_logFilePath).absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    return page;
}

QWidget *MainWindow::buildAboutPage()
{
    const AppConfig config = AppConfigLoader::load();
    QString version = "3.0.2";
    QFile versionFile(QDir(config.sourceRoot).filePath("version"));
    if (versionFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString text = QString::fromUtf8(versionFile.readAll()).trimmed();
        if (!text.isEmpty()) {
            version = text;
        }
    }

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QFrame(scroll);
    page->setObjectName("page");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 4, 4, 10);
    layout->setSpacing(0);

    auto addTitle = [&](const QString &text) {
        auto *l = new QLabel(text, page);
        l->setStyleSheet("font-size:14px;");
        l->setFixedHeight(40);
        layout->addWidget(l);
    };
    auto addValue = [&](const QString &text, const char *objName) {
        auto *l = new QLabel(text, page);
        l->setObjectName(objName);
        l->setFixedHeight(40);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(l);
    };
    auto addLink = [&](const QString &title, const QString &url) {
        addTitle(title);
        auto *l = new QLabel(page);
        l->setFixedHeight(40);
        l->setTextFormat(Qt::RichText);
        l->setText(QString("<a href=\"%1\" style=\"color:red;text-decoration:none;\">%1</a>").arg(url));
        l->setOpenExternalLinks(true);
        layout->addWidget(l);
    };

    auto *checkUpdate = new QPushButton(QString::fromUtf8("检查更新"), page);
    checkUpdate->setObjectName("primaryButton");
    checkUpdate->setFixedSize(96, 30);
    connect(checkUpdate, &QPushButton::clicked, this, &MainWindow::showUpdateDialog);
    layout->addWidget(checkUpdate);

    addTitle(QString::fromUtf8("Clash Auto:"));
    addValue(version, "versionValue");
    addLink(QString::fromUtf8("用户讨论和 bug 提交:"), "https://t.me/clashr_auto");
    addLink(QString::fromUtf8("项目地址:"), "https://github.com/clashrauto/clashr-auto-desktop");
    addLink("Read Me:", "https://clashr-auto.gitbook.io");
    addTitle("Exe path:");
    addValue(QCoreApplication::applicationDirPath(), "nodeName");
    addTitle("Root path:");
    addValue(config.sourceRoot, "nodeName");
    layout->addStretch();

    scroll->setWidget(page);
    return scroll;
}

QWidget *MainWindow::buildFooter()
{
    auto *footer = new QFrame(this);
    footer->setObjectName("footer");
    footer->setFixedHeight(FooterHeight);
    auto *layout = new QHBoxLayout(footer);
    layout->setContentsMargins(SidebarWidth, 0, 5, 0);
    layout->setSpacing(5);

    m_usersLabel = new QLabel(QString::fromUtf8("◉ -"), footer);
    m_usersLabel->setObjectName("usersBadge");
    auto *footerArrow = new QLabel(QString::fromUtf8("›"), footer);
    footerArrow->setObjectName("footerArrow");
    m_logLabel = new QLabel("Ready", footer);
    m_logLabel->setObjectName("footerLog");
    layout->addWidget(m_usersLabel);
    layout->addWidget(footerArrow);
    layout->addWidget(m_logLabel, 1);

    auto addSwitch = [&](const QString &text, QLabel **dot, auto slot) {
        auto *button = new QPushButton(text, footer);
        button->setObjectName("switchButton");
        *dot = createSwitchDot(false);
        auto *buttonLayout = new QHBoxLayout(button);
        buttonLayout->setContentsMargins(6, 0, 8, 0);
        buttonLayout->setSpacing(5);
        buttonLayout->addWidget(*dot);
        buttonLayout->addStretch();
        connect(button, &QPushButton::clicked, m_core, slot);
        layout->addWidget(button);
    };

    addSwitch("增强", &m_tunDot, &CoreController::toggleTun);
    addSwitch("网页", &m_proxyDot, &CoreController::toggleProxy);
    addSwitch("核心", &m_coreDot, &CoreController::toggleCore);

    auto *mode = new QComboBox(footer);
    mode->addItems({"全部", "规则", "直连"});
    mode->setFixedWidth(120);
    connect(mode, &QComboBox::currentTextChanged, &m_service, &ClashService::setMode);
    layout->addWidget(mode);
    return footer;
}

QFrame *MainWindow::createMetricCard(const QString &icon, const QString &title, QLabel **valueLabel, const QString &className)
{
    auto *card = new QFrame(this);
    card->setObjectName("metricCard");
    card->setProperty("kind", className);
    card->setMinimumHeight(70);
    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(14, 18, 14, 18);
    layout->setSpacing(16);

    auto *iconLabel = new QLabel(icon, card);
    iconLabel->setObjectName("metricIcon");
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setFixedWidth(50);
    layout->addWidget(iconLabel);

    auto *texts = new QVBoxLayout();
    texts->setSpacing(2);
    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName("metricTitle");
    *valueLabel = new QLabel("0.00 B", card);
    (*valueLabel)->setObjectName("metricValue");
    texts->addWidget(titleLabel);
    texts->addWidget(*valueLabel);
    layout->addLayout(texts, 1);
    return card;
}

QPushButton *MainWindow::createMenuButton(const QString &text, int page)
{
    auto *button = new QPushButton(text, this);
    button->setObjectName("menuButton");
    button->setCheckable(true);
    button->setFixedSize(115, 40);
    connect(button, &QPushButton::clicked, this, [this, page] { setCurrentPage(page); });
    m_menuButtons.push_back(button);
    return button;
}

QFrame *MainWindow::createNodeRow(const NodeInfo &node)
{
    auto *row = new QFrame(this);
    row->setObjectName("nodeRow");
    row->setProperty("active", node.active);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(8);

    auto *name = new QLabel(node.active ? QString("[%1] %2").arg(node.name, node.now.isEmpty() ? node.name : node.now) : node.name, row);
    name->setObjectName("nodeName");
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // 自适应：名称随列宽伸缩，不撑宽整行
    layout->addWidget(name, 1);

    // 速度/延迟：内容大小的药丸，右对齐在 120px 列内（对应旧项目 span width:120px + 内层 padding 徽标）
    auto *badgeWrap = new QWidget(row);
    badgeWrap->setFixedWidth(DelayBadgeW);
    auto *badgeLayout = new QHBoxLayout(badgeWrap);
    badgeLayout->setContentsMargins(0, 0, 0, 0);
    badgeLayout->addStretch();
    auto *delay = new QLabel(QString("%1/%2").arg(speedText(node.speed), node.delay == 0 ? "-" : QString::number(node.delay)), badgeWrap);
    delay->setObjectName("delayBadge");
    delay->setStyleSheet(QString("background:%1;").arg(delayColor(node.delay).name(QColor::HexArgb)));
    delay->setAlignment(Qt::AlignCenter);
    delay->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); // 背景贴合文字，不被行高拉伸
    badgeLayout->addWidget(delay, 0, Qt::AlignVCenter);
    layout->addWidget(badgeWrap);

    auto *apply = new QPushButton(node.active ? "禁用" : "应用", row);
    apply->setObjectName("nodeButton");
    apply->setFixedSize(82, 38);
    connect(apply, &QPushButton::clicked, this, [this, node] {
        if (node.active) {
            m_service.clearConnections();
            return;
        }
        m_service.selectNode(node.name);
    });
    layout->addWidget(apply);
    return row;
}

void MainWindow::showSubscriptionNodes(int subscriptionIndex)
{
    auto *dialog = new QDialog(this);
    dialog->setWindowTitle("Subscription nodes");
    dialog->resize(620, 420);
    auto *dialogLayout = new QVBoxLayout(dialog);
    dialogLayout->setContentsMargins(10, 10, 10, 10);
    dialogLayout->setSpacing(10);

    auto *scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *container = new QFrame(scroll);
    auto *list = new QVBoxLayout(container);
    list->setContentsMargins(0, 0, 0, 0);
    list->setSpacing(8);

    const QVector<SubscriptionNodeSummary> nodeList = m_subscriptions->nodes(subscriptionIndex);
    if (nodeList.isEmpty()) {
        auto *empty = new QLabel("No nodes. Click Update first.", container);
        empty->setObjectName("nodeName");
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(80);
        list->addWidget(empty);
    } else {
        for (int i = 0; i < nodeList.size(); ++i) {
            const SubscriptionNodeSummary node = nodeList[i];
            auto *row = new QFrame(container);
            row->setObjectName("nodeRow");
            row->setProperty("active", node.use);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(8, 0, 0, 0);
            auto *label = new QLabel(QString("%1\n%2:%3").arg(node.name, node.server, node.port), row);
            label->setObjectName("nodeName");
            rowLayout->addWidget(label, 1);
            auto *toggle = new QPushButton(node.use ? "Disable" : "Use", row);
            toggle->setObjectName("nodeButton");
            toggle->setFixedSize(82, 38);
            connect(toggle, &QPushButton::clicked, dialog, [this, dialog, subscriptionIndex, i, node] {
                if (m_subscriptions->setNodeEnabled(subscriptionIndex, i, !node.use)) {
                    reloadSubscriptions();
                    m_core->rebuildConfig();
                    dialog->close();
                    showSubscriptionNodes(subscriptionIndex);
                }
            });
            rowLayout->addWidget(toggle);
            list->addWidget(row);
        }
    }

    list->addStretch();
    scroll->setWidget(container);
    dialogLayout->addWidget(scroll, 1);

    auto *close = new QPushButton("Close", dialog);
    close->setObjectName("primaryButton");
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    dialogLayout->addWidget(close, 0, Qt::AlignRight);
    dialog->setStyleSheet(styleSheet());
    dialog->show();
}

void MainWindow::showUpdateDialog()
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QString::fromUtf8("Clash Auto 更新"));
    dialog->setStyleSheet(styleSheet());
    dialog->resize(600, 660);
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto *title = new QLabel(QString::fromUtf8("Clash Auto 更新"), dialog);
    title->setObjectName("sectionTitle");
    root->addWidget(title);

    auto *tabs = new QTabWidget(dialog);
    tabs->setTabPosition(QTabWidget::West);
    tabs->setMinimumHeight(450);
    root->addWidget(tabs, 1);

    struct Refs {
        QLabel *ver;
        QTextEdit *body;
        QListWidget *assets;
    };
    auto makeTab = [&](const QString &name) -> Refs {
        auto *page = new QWidget();
        auto *v = new QVBoxLayout(page);
        v->setContentsMargins(10, 10, 10, 10);
        v->setSpacing(4);
        auto *ver = new QLabel("VERSION: -", page);
        ver->setObjectName("sectionTitle");
        auto *h1 = new QLabel(QString::fromUtf8("更新说明"), page);
        h1->setObjectName("subCardMeta");
        auto *body = new QTextEdit(page);
        body->setReadOnly(true);
        body->setMinimumHeight(200);
        auto *h2 = new QLabel(QString::fromUtf8("下载资源（双击下载到「下载」目录）"), page);
        h2->setObjectName("subCardMeta");
        auto *assets = new QListWidget(page);
        assets->setMinimumHeight(130);
        v->addWidget(ver);
        v->addWidget(h1);
        v->addWidget(body, 1);
        v->addWidget(h2);
        v->addWidget(assets);
        tabs->addTab(page, name);
        return {ver, body, assets};
    };
    const Refs rel = makeTab(QString::fromUtf8("正式版"));
    const Refs beta = makeTab(QString::fromUtf8("测试版"));

    auto *progress = new QProgressBar(dialog);
    progress->setFixedHeight(26);
    progress->setValue(0);
    progress->hide();
    root->addWidget(progress);

    auto *bottom = new QHBoxLayout();
    auto *noTip = new QCheckBox(QString::fromUtf8("不再提示"), dialog);
    bottom->addWidget(noTip);
    bottom->addStretch();
    auto *closeBtn = new QPushButton(QString::fromUtf8("关闭"), dialog);
    closeBtn->setObjectName("nodeButton");
    closeBtn->setFixedSize(80, 30);
    auto *openPage = new QPushButton(QString::fromUtf8("打开下载页"), dialog);
    openPage->setObjectName("primaryButton");
    openPage->setFixedSize(110, 30);
    bottom->addWidget(closeBtn);
    bottom->addWidget(openPage);
    root->addLayout(bottom);

    connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(openPage, &QPushButton::clicked, dialog, [] {
        QDesktopServices::openUrl(QUrl("https://github.com/ClashrAuto/Clashr-Auto-Desktop/releases"));
    });
    // 拉取 GitHub releases（失败时优雅降级，仅显示提示）
    auto *nam = new QNetworkAccessManager(dialog);

    // 双击资源即在应用内下载到「下载」目录，带进度条；完成后打开所在文件夹
    auto downloadAsset = [this, dialog, nam, progress](QListWidgetItem *item) {
        const QString url = item->data(Qt::UserRole).toString();
        const QString name = item->text();
        if (url.isEmpty()) {
            return;
        }
        QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (dir.isEmpty()) {
            dir = QDir::homePath();
        }
        const QString savePath = QDir(dir).filePath(name);

        const QUrl assetUrl(url);
        QNetworkRequest req(assetUrl);
        req.setRawHeader("User-Agent", "clashauto-cpp");
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = nam->get(req);

        progress->setValue(0);
        progress->show();
        appendLog(QString::fromUtf8("开始下载: %1").arg(name));

        connect(reply, &QNetworkReply::downloadProgress, dialog, [progress](qint64 received, qint64 total) {
            if (total > 0) {
                progress->setValue(static_cast<int>(received * 100 / total));
            }
        });
        connect(reply, &QNetworkReply::finished, dialog, [this, reply, progress, savePath, name] {
            const bool ok = reply->error() == QNetworkReply::NoError;
            const QByteArray data = reply->readAll();
            const QString err = reply->errorString();
            reply->deleteLater();
            if (!ok) {
                progress->hide();
                appendLog(QString::fromUtf8("下载失败: %1 (%2)").arg(name, err));
                return;
            }
            QFile out(savePath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                progress->hide();
                appendLog(QString::fromUtf8("保存失败: %1").arg(savePath));
                return;
            }
            out.write(data);
            out.close();
            progress->setValue(100);
            appendLog(QString::fromUtf8("下载完成: %1").arg(savePath));
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(savePath).absolutePath()));
        });
    };
    connect(rel.assets, &QListWidget::itemActivated, dialog, downloadAsset);
    connect(beta.assets, &QListWidget::itemActivated, dialog, downloadAsset);
    QNetworkRequest req(QUrl("https://api.github.com/repos/ClashrAuto/Clashr-Auto-Desktop/releases"));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "clashauto-cpp");
    rel.body->setPlainText(QString::fromUtf8("正在获取..."));
    beta.body->setPlainText(QString::fromUtf8("正在获取..."));
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, dialog, [reply, rel, beta] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (!ok) {
            rel.body->setPlainText(QString::fromUtf8("获取失败: ") + reply->errorString());
            beta.body->setPlainText(QString());
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(body).array();
        auto fill = [](const QJsonObject &r, const Refs &refs) {
            refs.ver->setText("VERSION: " + r.value("tag_name").toString());
            refs.body->setPlainText(r.value("body").toString());
            refs.assets->clear();
            const QJsonArray assets = r.value("assets").toArray();
            for (const QJsonValue &av : assets) {
                const QJsonObject a = av.toObject();
                auto *item = new QListWidgetItem(a.value("name").toString());
                item->setData(Qt::UserRole, a.value("browser_download_url").toString());
                refs.assets->addItem(item);
            }
        };
        bool haveRel = false;
        bool haveBeta = false;
        for (const QJsonValue &value : arr) {
            const QJsonObject r = value.toObject();
            if (!r.value("prerelease").toBool() && !haveRel) {
                fill(r, rel);
                haveRel = true;
            } else if (r.value("prerelease").toBool() && !haveBeta) {
                fill(r, beta);
                haveBeta = true;
            }
            if (haveRel && haveBeta) {
                break;
            }
        }
        if (!haveRel) {
            rel.body->setPlainText(QString::fromUtf8("暂无正式版"));
        }
        if (!haveBeta) {
            beta.body->setPlainText(QString::fromUtf8("暂无测试版"));
        }
    });

    dialog->show();
}

void MainWindow::appendLog(const QString &message)
{
    const QString line = QString("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
                             .arg(message);
    if (m_logLabel) {
        m_logLabel->setText(message);
    }
    appendTimeline(m_logTimeline, m_logScroll, message);
    if (!m_logFilePath.isEmpty()) {
        QFile file(m_logFilePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            file.write(line.toUtf8());
            file.write("\n");
        }
    }
}

void MainWindow::appendTimeline(QVBoxLayout *layout, QScrollArea *scroll, const QString &message)
{
    if (!layout) {
        return;
    }
    QColor dot(72, 152, 248); // info 蓝
    const QString low = message.toLower();
    if (low.contains("fail") || low.contains("error") || message.contains(QString::fromUtf8("失败"))) {
        dot = QColor(245, 108, 108); // 红
    } else if (low.contains("warn")) {
        dot = QColor(230, 162, 60); // 橙
    } else if (low.contains("finished") || low.contains("success") || low.contains(" ok") || message.contains(QString::fromUtf8("已")) || message.contains(QString::fromUtf8("完成"))) {
        dot = QColor(103, 194, 58); // 绿
    }
    const QColor axis = m_theme == "white" ? QColor(0xcc, 0xcc, 0xcc) : QColor(0x33, 0x33, 0x33);

    auto *entry = new QFrame();
    auto *h = new QHBoxLayout(entry);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    h->addWidget(new TimelineRail(dot, axis));

    auto *texts = new QVBoxLayout();
    texts->setContentsMargins(0, 4, 0, 10);
    texts->setSpacing(2);
    auto *ts = new QLabel(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    ts->setObjectName("timelineTime");
    auto *msg = new QLabel(message);
    msg->setObjectName("timelineMsg");
    msg->setWordWrap(true);
    texts->addWidget(ts);
    texts->addWidget(msg);
    h->addLayout(texts, 1);

    // el-timeline :reverse=true —— 最新在顶部
    layout->insertWidget(0, entry);
    if (scroll) {
        QScrollBar *bar = scroll->verticalScrollBar();
        QTimer::singleShot(0, scroll, [bar] {
            if (bar) {
                bar->setValue(bar->minimum());
            }
        });
    }
}

QLabel *MainWindow::createSwitchDot(bool enabled)
{
    auto *dot = new QLabel(this);
    dot->setObjectName("switchDot");
    dot->setProperty("on", enabled);
    dot->setFixedSize(16, 16);

    // 旧项目 .quan.on 的 1s 呼吸动画（QSS 不支持 @keyframes，用透明度动画近似）
    auto *effect = new QGraphicsOpacityEffect(dot);
    effect->setOpacity(1.0);
    dot->setGraphicsEffect(effect);
    auto *anim = new QPropertyAnimation(effect, "opacity", dot);
    anim->setDuration(1000);
    anim->setStartValue(1.0);
    anim->setKeyValueAt(0.5, 0.35);
    anim->setEndValue(1.0);
    anim->setLoopCount(-1);
    return dot;
}

QString MainWindow::speedText(qint64 value) const
{
    double number = static_cast<double>(qMax<qint64>(0, value));
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (number >= 1024.0 && unit + 1 < units.size()) {
        number /= 1024.0;
        ++unit;
    }
    return QString::number(number, 'f', 2) + " " + units[unit];
}

QColor MainWindow::delayColor(int delay) const
{
    if (delay <= 0) {
        return QColor(255, 255, 255, 50);
    }
    const int red = qMin(255, delay * 65535 / 1200 / 255);
    const int green = qMax(0, 255 - red);
    return QColor(red, green, 0, 230);
}

void MainWindow::setCurrentPage(int page)
{
    m_pages->setCurrentIndex(page);
    for (int i = 0; i < m_menuButtons.size(); ++i) {
        m_menuButtons[i]->setChecked(i == page);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 关闭到托盘：隐藏窗口、核心继续运行；真正退出走托盘「退出程序」
    if (m_closeToTray && QSystemTrayIcon::isSystemTrayAvailable()) {
        hide();
        event->ignore();
        if (!m_trayHintShown && m_tray) {
            m_tray->notify(QString::fromUtf8("Clash Auto"),
                           QString::fromUtf8("已最小化到托盘，右键托盘图标可退出"));
            m_trayHintShown = true;
        }
        return;
    }
    event->accept();
    qApp->quit();
}

void MainWindow::applyAutoStart(bool enabled)
{
#if defined(Q_OS_WIN)
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    if (enabled) {
        const QString path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        reg.setValue("ClashAuto", QString("\"%1\"").arg(path));
    } else {
        reg.remove("ClashAuto");
    }
    appendLog(enabled ? QString::fromUtf8("已设置开机自启") : QString::fromUtf8("已取消开机自启"));
#else
    Q_UNUSED(enabled);
    appendLog(QString::fromUtf8("开机自启当前仅支持 Windows"));
#endif
}

void MainWindow::reloadSubscriptions()
{
    if (!m_subscriptionList) {
        return;
    }
    auto *grid = qobject_cast<QGridLayout *>(m_subscriptionList->layout());
    if (!grid) {
        return;
    }
    while (QLayoutItem *item = grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const int columns = 3;

    auto *addCard = new QPushButton("+", m_subscriptionList);
    addCard->setObjectName("addCard");
    addCard->setMinimumHeight(103);
    connect(addCard, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString url = QInputDialog::getText(this, QString::fromUtf8("添加订阅"), QString::fromUtf8("URL 或本地路径:"), QLineEdit::Normal, "", &ok);
        if (!ok || url.trimmed().isEmpty()) {
            return;
        }
        const QString name = QInputDialog::getText(this, QString::fromUtf8("添加订阅"), QString::fromUtf8("名称:"), QLineEdit::Normal, url, &ok);
        if (!ok) {
            return;
        }
        const QStringList types = {"sub", "clash"};
        const QString type = QInputDialog::getItem(this, QString::fromUtf8("添加订阅"), QString::fromUtf8("类型:"), types, 0, false, &ok);
        if (!ok) {
            return;
        }
        if (m_subscriptions->addSubscription(name.trimmed(), url.trimmed(), type)) {
            reloadSubscriptions();
            appendLog(QString::fromUtf8("已添加订阅"));
        }
    });
    grid->addWidget(addCard, 0, 0);

    const QVector<SubscriptionSummary> subscriptions = m_subscriptions->load();
    int cell = 1;
    for (int i = 0; i < subscriptions.size(); ++i) {
        grid->addWidget(createSubscriptionCard(subscriptions[i], i), cell / columns, cell % columns);
        ++cell;
    }

    for (int c = 0; c < columns; ++c) {
        grid->setColumnStretch(c, 1);
    }
    grid->setRowStretch(cell / columns + 1, 1);
}

void MainWindow::populateNodeList()
{
    if (!m_nodeList) {
        return;
    }
    auto *layout = qobject_cast<QVBoxLayout *>(m_nodeList->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QString filter = m_nodeSearch ? m_nodeSearch->text().trimmed() : QString();
    int shown = 0;
    for (const NodeInfo &node : std::as_const(m_currentNodes)) {
        if (!filter.isEmpty() && !node.name.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        layout->addWidget(createNodeRow(node));
        ++shown;
    }

    if (shown == 0) {
        auto *empty = new QLabel(filter.isEmpty() ? "No nodes" : "No matched nodes", m_nodeList);
        empty->setObjectName("nodeName");
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(80);
        layout->addWidget(empty);
    }
    layout->addStretch();

    if (m_nodeTitle) {
        m_nodeTitle->setText(QString::fromUtf8("节点 <span style='font-size:9px'>(%1/%2)</span>")
                                 .arg(shown)
                                 .arg(m_currentNodes.size()));
    }
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && event->pos().y() <= TitleHeight) {
        m_dragging = true;
        m_dragStart = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
    }
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && !isMaximized()) {
        move(event->globalPosition().toPoint() - m_dragStart);
        event->accept();
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
}

QString MainWindow::appStyle() const
{
    return R"(
        #root { background:#242425; border-radius:10px; }
        #titleBar { background:#222; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#ccc; font-size:12px; padding-left:0; }
        #titleButton, #closeButton { color:#ccc; background:#222; border:0; border-radius:0; }
        #titleButton:hover { background:#333; }
        #closeButton:hover { background:red; color:white; }
        #body, #rightPane, #page { background:rgba(0,0,0,0); }
        #sidebar { background:#303032; }
        #logo { color:#ffff00; background:transparent; min-width:80px; max-width:80px; min-height:80px; max-height:80px; font-size:70px; }
        #logo[state="tun"] { color:#ff0000; }
        #logo[state="proxy"] { color:#ffff00; }
        #logo[state="off"] { color:#ffffff; }
        #menuButton { color:#fff; background:transparent; border:0; border-left:3px solid transparent; border-radius:5px 0 0 5px; text-align:left; padding-left:32px; font-size:14px; }
        #menuButton:hover { background:rgb(62,62,62); padding-left:36px; }
        #menuButton:checked { background:#000; color:#ccc; border-left:3px solid #4898f8; }
        #version { color:#666; font-size:12px; }
        #metricCard { background:#222; border:0; border-radius:4px; min-height:70px; }
        #metricIcon { font-size:30px; color:#aaa; }
        #metricTitle { color:#bfbfbf; font-size:12px; }
        #metricValue { color:#bfbfbf; font-size:20px; }
        #metricCard[kind="up"] QLabel { color:rgb(168,67,67); }
        #metricCard[kind="down"] QLabel { color:rgb(77,161,62); }
        #metricCard[kind="process"] QLabel { color:rgb(70,110,168); }
        #metricCard[kind="download"] QLabel { color:rgb(72,165,167); }
        #sectionTitle { color:#eee; font-size:18px; font-weight:400; }
        QScrollArea { background:transparent; border:0; }
        QScrollBar:vertical { background:#242425; width:10px; margin:0; }
        QScrollBar::handle:vertical { background:#555; border-radius:5px; min-height:24px; }
        QLineEdit, QComboBox, QTextEdit, QSpinBox { color:#fff; background:#444; border:1px solid #333; border-radius:3px; min-height:28px; padding:0 8px; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border:1px solid #4898f8; }
        QTabWidget::pane { border:0; }
        QTabBar::tab { color:#999; background:transparent; padding:8px 16px; }
        QTabBar::tab:selected { color:#fff; border-bottom:2px solid #4898f8; }
        QCheckBox, QLabel { color:#ccc; }
        #iconButton { color:#4898f8; background:transparent; border:0; font-size:18px; }
        #nodeRow, #plainCard { background:#252525; border:0; border-left:3px solid transparent; border-radius:0; min-height:56px; }
        #nodeRow[active="true"] { background:rgba(72,151,248,0.69); border-left:3px solid #83bdff; }
        #nodeName { color:#ccc; font-size:12px; }
        #delayBadge { color:#222; border-radius:5px; padding:3px 5px; font-size:12px; }
        #nodeButton { color:#ccc; background:transparent; border:0; border-left:1px solid #000; border-radius:0; font-size:12px; }
        #nodeButton:hover { background:#333; }
        #primaryButton { color:white; background:#4898f8; border:0; border-radius:4px; min-height:30px; }
        #footer { background:#303032; border-bottom-left-radius:10px; border-bottom-right-radius:10px; }
        #usersBadge { color:#fff; background:#000; border-radius:3px; padding:3px 5px; font-size:12px; }
        #footerArrow { color:#666; font-size:14px; }
        #footerLog { color:#ccc; font-size:12px; }
        #switchButton { color:#eee; background:#000; border:0; border-radius:3px; min-height:28px; padding-left:22px; padding-right:8px; font-size:12px; }
        #switchDot { background:rgba(102,102,102,0.15); border:4px solid rgba(102,102,102,0.15); border-radius:8px; }
        #switchDot[on="true"] { background:#4898f8; border-color:rgba(72,152,248,0.30); }
        #timelineBody { background:#111; }
        #timelineTime { color:#888; font-size:11px; }
        #timelineMsg { color:#ccc; font-size:13px; }
        #aboutTitle { color:#eee; font-size:28px; }
        #linkLabel { color:red; font-size:14px; }
        #linkLabel:hover { text-decoration:underline; }
        #versionValue { color:green; font-size:14px; font-weight:bold; }
        #settingGroupTitle { color:#666; font-size:24px; font-weight:800; }
        #settingDivider { background:transparent; border:0; border-bottom:1px dashed #333; }
        #circleButton { border-radius:14px; min-width:28px; max-width:28px; min-height:28px; max-height:28px; color:#ccc; background:#444; border:0; }
        #circleButton:hover { background:#333; }
        #circleButton[on="true"] { background:#4898f8; color:white; }
        #circleButton[danger="true"]:hover { background:red; color:white; }
        #subCard { background:#222; border:0; border-left:3px solid transparent; border-radius:5px; }
        #subCard[on="true"] { border-left:3px solid #4898f8; }
        #subCardTitle { color:#eee; font-size:16px; }
        #subCardMeta { color:#999; font-size:12px; }
        #addCard { background:#222; border:2px dashed #444; border-radius:5px; color:#4898f8; font-size:48px; }
    )";
}

QString MainWindow::lightStyle() const
{
    return R"(
        #root { background:#ffffff; border-radius:10px; }
        #titleBar { background:#eee; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#333; font-size:12px; }
        #titleButton, #closeButton { color:#333; background:#eee; border:0; border-radius:0; }
        #titleButton:hover { background:#fff; }
        #closeButton:hover { background:red; color:white; }
        #body, #rightPane, #page { background:rgba(0,0,0,0); }
        #sidebar { background:#fafafa; }
        #logo { color:#ffff00; background:transparent; min-width:80px; max-width:80px; min-height:80px; max-height:80px; font-size:70px; }
        #logo[state="tun"] { color:#ff0000; }
        #logo[state="proxy"] { color:#ffff00; }
        #logo[state="off"] { color:#333; }
        #menuButton { color:#333; background:transparent; border:0; border-left:3px solid transparent; border-radius:5px 0 0 5px; text-align:left; padding-left:32px; font-size:14px; }
        #menuButton:hover { background:rgb(210,210,210); padding-left:36px; }
        #menuButton:checked { background:#fff; color:#333; border-left:3px solid #4898f8; }
        #version { color:#666; font-size:12px; }
        #metricCard { background:#eee; border:0; border-radius:4px; min-height:70px; }
        #metricIcon { font-size:30px; color:#888; }
        #metricTitle { color:#3d3d3d; font-size:12px; }
        #metricValue { color:#3d3d3d; font-size:20px; }
        #metricCard[kind="up"] QLabel { color:rgb(168,67,67); }
        #metricCard[kind="down"] QLabel { color:rgb(77,161,62); }
        #metricCard[kind="process"] QLabel { color:rgb(70,110,168); }
        #metricCard[kind="download"] QLabel { color:rgb(72,165,167); }
        #sectionTitle { color:#111; font-size:18px; font-weight:400; }
        QScrollArea { background:transparent; border:0; }
        QScrollBar:vertical { background:#fff; width:10px; margin:0; }
        QScrollBar::handle:vertical { background:#ccc; border-radius:5px; min-height:24px; }
        QLineEdit, QComboBox, QTextEdit, QSpinBox { color:#333; background:#eaeaea; border:1px solid #ccc; border-radius:3px; min-height:28px; padding:0 8px; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border:1px solid #4898f8; }
        QTabWidget::pane { border:0; }
        QTabBar::tab { color:#999; background:transparent; padding:8px 16px; }
        QTabBar::tab:selected { color:#333; border-bottom:2px solid #4898f8; }
        QCheckBox, QLabel { color:#333; }
        #iconButton { color:#4898f8; background:transparent; border:0; font-size:18px; }
        #nodeRow, #plainCard { background:#eee; border:0; border-left:3px solid transparent; border-radius:0; min-height:56px; }
        #nodeRow[active="true"] { background:rgba(72,151,248,0.69); border-left:3px solid #1f6fd2; }
        #nodeName { color:#333; font-size:12px; }
        #delayBadge { color:#333; border-radius:5px; padding:3px 5px; font-size:12px; }
        #nodeButton { color:#333; background:transparent; border:0; border-left:1px solid #fff; border-radius:0; font-size:12px; }
        #nodeButton:hover { background:#c1c1c1; }
        #primaryButton { color:white; background:#4898f8; border:0; border-radius:4px; min-height:30px; }
        #footer { background:#fafafa; border-bottom-left-radius:10px; border-bottom-right-radius:10px; }
        #usersBadge { color:#333; background:#fff; border-radius:3px; padding:3px 5px; font-size:12px; }
        #footerArrow { color:#e8e8e8; font-size:14px; }
        #footerLog { color:#333; font-size:12px; }
        #switchButton { color:#333; background:#eee; border:0; border-radius:3px; min-height:28px; padding-left:22px; padding-right:8px; font-size:12px; }
        #switchDot { background:rgba(255,255,255,0.15); border:4px solid rgba(255,255,255,0.15); border-radius:8px; }
        #switchDot[on="true"] { background:#4898f8; border-color:rgba(72,152,248,0.30); }
        #timelineBody { background:#fff; }
        #timelineTime { color:#999; font-size:11px; }
        #timelineMsg { color:#333; font-size:13px; }
        #aboutTitle { color:#111; font-size:28px; }
        #linkLabel { color:red; font-size:14px; }
        #linkLabel:hover { text-decoration:underline; }
        #versionValue { color:green; font-size:14px; font-weight:bold; }
        #settingGroupTitle { color:#ccc; font-size:24px; font-weight:800; }
        #settingDivider { background:transparent; border:0; border-bottom:1px dashed #ccc; }
        #circleButton { border-radius:14px; min-width:28px; max-width:28px; min-height:28px; max-height:28px; color:#333; background:#ccc; border:0; }
        #circleButton:hover { background:#c1c1c1; }
        #circleButton[on="true"] { background:#4898f8; color:white; }
        #circleButton[danger="true"]:hover { background:red; color:white; }
        #subCard { background:#eee; border:0; border-left:3px solid transparent; border-radius:5px; }
        #subCard[on="true"] { border-left:3px solid #4898f8; }
        #subCardTitle { color:#333; font-size:16px; }
        #subCardMeta { color:#999; font-size:12px; }
        #addCard { background:#eee; border:2px dashed #ccc; border-radius:5px; color:#4898f8; font-size:48px; }
    )";
}

void MainWindow::applyTheme(const QString &theme)
{
    const bool light = theme.compare("light", Qt::CaseInsensitive) == 0
                       || theme.compare("white", Qt::CaseInsensitive) == 0;
    m_theme = light ? "white" : "black";
    setStyleSheet(light ? lightStyle() : appStyle());
}
