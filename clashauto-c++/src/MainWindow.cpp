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
#include <QGuiApplication>
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
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QTimer>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QDateTime>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <utility>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM
#endif

// Version.h 由 CMake 从 src/Version.h.in 生成（CI 用 major.minor.<提交数>）
#if __has_include("Version.h")
#include "Version.h"
#endif
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

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
    // 简单轮转：超 2MB 滚动为 .old（保留一代），防止无限增长
    if (QFileInfo(m_logFilePath).size() > 2 * 1024 * 1024) {
        QFile::remove(m_logFilePath + ".old");
        QFile::rename(m_logFilePath, m_logFilePath + ".old");
    }
    m_core = new CoreController(config, this);
    m_tray = new TrayController(this, this);
    m_subscriptions = new SubscriptionStore(config, this);
    registerUrlScheme();
    m_service.setClearConnectionsOnSwitch(config.clearConnections);
    m_closeToTray = config.closeToTray;
    m_nodeSwitchNote = config.nodeSwitchNote;
    m_autoTheme = config.autoTheme;

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
    // 恢复上次窗口位置/大小（对应旧项目 config.bounds）
    const QByteArray savedGeometry = QSettings().value("window/geometry").toByteArray();
    if (!savedGeometry.isEmpty()) {
        restoreGeometry(savedGeometry);
    }

    auto *root = new QFrame(this);
    root->setObjectName("root");
    auto *outer = new QVBoxLayout(root);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    outer->addWidget(buildTitleBar());

    // 侧栏跨整个高度到窗口最底部；页脚放到右侧列（页面下方）
    auto *body = new QFrame(root);
    body->setObjectName("body");
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(buildSidebar());

    auto *rightColumn = new QFrame(body);
    rightColumn->setObjectName("rightColumn");
    auto *rightColLayout = new QVBoxLayout(rightColumn);
    rightColLayout->setContentsMargins(0, 0, 0, 0);
    rightColLayout->setSpacing(0);

    auto *right = new QFrame(rightColumn);
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
    rightColLayout->addWidget(right, 1);
    rightColLayout->addWidget(buildFooter());
    bodyLayout->addWidget(rightColumn, 1);
    outer->addWidget(body, 1);
    setCentralWidget(root);

    applyTheme(config.theme);
    setCurrentPage(0);

    // 订阅自动定时更新（对应旧项目 updateTime）：每分钟一跳，按各订阅自己的周期取模；
    // 订阅未设周期（updateTime==0）时沿用设置里的全局默认 m_autoUpdateMinutes。
    m_autoUpdateMinutes = config.autoUpdateMinutes;
    m_autoUpdateTimer = new QTimer(this);
    connect(m_autoUpdateTimer, &QTimer::timeout, this, [this] {
        ++m_runMinutes;
        const QVector<SubscriptionSummary> subs = m_subscriptions->load();
        int updated = 0;
        for (int i = 0; i < subs.size(); ++i) {
            const int period = subs[i].updateTime > 0 ? subs[i].updateTime : m_autoUpdateMinutes;
            if (period > 0 && m_runMinutes % period == 0) {
                m_subscriptions->updateSubscription(i);
                ++updated;
            }
        }
        if (updated > 0) {
            appendLog(QString::fromUtf8("订阅自动更新已触发（%1 个）").arg(updated));
        }
    });
    m_autoUpdateTimer->start(60 * 1000);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // 跟随系统深浅色（config.autoTheme）：启动应用 + 监听系统切换
    auto applySystemScheme = [this] {
        applyTheme(QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
                       ? QStringLiteral("black")
                       : QStringLiteral("light"));
    };
    if (m_autoTheme) {
        applySystemScheme();
    }
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this, applySystemScheme](Qt::ColorScheme) {
                if (m_autoTheme) {
                    applySystemScheme();
                }
            });
#endif

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
        // 节点切换通知（对应旧项目 config.note）；跳过首次填充，避免启动即误报
        if (m_nodeSwitchNote && m_nodeInitialized && m_tray && !selected.isEmpty() && selected != m_selectedNode) {
            m_tray->notify(QString::fromUtf8("节点切换"), QString::fromUtf8("已切换到 %1").arg(selected));
        }
        m_nodeInitialized = true;
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
    layout->setContentsMargins(5, 16, 0, 5); // 版本号紧贴侧栏底部 5px
    layout->setSpacing(0);

    auto *logoWrap = new QFrame(side);
    logoWrap->setObjectName("logoWrap");
    logoWrap->setFixedHeight(118);
    auto *logoLayout = new QVBoxLayout(logoWrap);
    logoLayout->setContentsMargins(0, 0, 5, 0);
    m_logo = new QLabel(QChar(0xE600), logoWrap); // iconfont icon-wangluo（网络）
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

    auto *version = new QLabel("Ver: " APP_VERSION, side);
    version->setObjectName("version");
    version->setAlignment(Qt::AlignCenter);
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
    metricsBox->setFixedHeight(200); // 旧项目：左上流量卡区固定 200px（每卡 ~95px），防止被折线图挤扁
    auto *metrics = new QGridLayout(metricsBox);
    metrics->setContentsMargins(0, 0, 0, 0);
    metrics->setHorizontalSpacing(MetricGutter);
    metrics->setVerticalSpacing(MetricGutter);
    metrics->addWidget(createMetricCard(QChar(0xE6CC), QString::fromUtf8("上传"), &m_upValue, "up"), 0, 0);
    metrics->addWidget(createMetricCard(QChar(0xE6CD), QString::fromUtf8("下载"), &m_downValue, "down"), 0, 1);
    // 进程数卡：右上角「查看全部连接 / 关闭全部」两个图标（复刻旧项目 .look 角标）
    auto *processCard = createMetricCard(QChar(0xE6BC), QString::fromUtf8("进程数"), &m_processValue, "process");
    auto *cornerWrap = new QWidget(processCard);
    auto *cornerLayout = new QHBoxLayout(cornerWrap);
    cornerLayout->setContentsMargins(0, 0, 0, 0);
    cornerLayout->setSpacing(0);
    auto *viewConns = new QPushButton(QString::fromUtf8("◉"), cornerWrap);
    viewConns->setObjectName("cardCorner");
    viewConns->setFixedSize(26, 22);
    viewConns->setToolTip(QString::fromUtf8("查看全部连接"));
    auto *closeConns = new QPushButton(QString::fromUtf8("✕"), cornerWrap);
    closeConns->setObjectName("cardCornerDanger");
    closeConns->setFixedSize(26, 22);
    closeConns->setToolTip(QString::fromUtf8("关闭全部连接"));
    cornerLayout->addWidget(viewConns);
    cornerLayout->addWidget(closeConns);
    connect(viewConns, &QPushButton::clicked, this, &MainWindow::showConnectionsDialog);
    connect(closeConns, &QPushButton::clicked, this, [this] {
        m_service.clearConnections();
        appendLog(QString::fromUtf8("已清理全部连接"));
    });
    auto *processGrid = new QGridLayout();
    processGrid->setContentsMargins(0, 0, 0, 0);
    processGrid->addWidget(processCard, 0, 0);
    processGrid->addWidget(cornerWrap, 0, 0, Qt::AlignTop | Qt::AlignRight);
    metrics->addLayout(processGrid, 1, 0);
    metrics->addWidget(createMetricCard(QChar(0xE7A3), QString::fromUtf8("总下载"), &m_totalDownValue, "download"), 1, 1);
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
    // 旧项目节点栏：搜索图标(el-icon-search)紧邻标题，点开展开输入框；测速/帮助在最右
    auto *searchIcon = new QPushButton(QString::fromUtf8("🔍"), nodeHeader); // 放大镜（U+1F50D 需 UTF-8，超出 BMP）
    searchIcon->setObjectName("iconButton");
    searchIcon->setFixedWidth(28);
    searchIcon->setToolTip(QString::fromUtf8("搜索"));
    nodeHeaderLayout->addWidget(searchIcon);
    m_nodeSearch = new QLineEdit(nodeHeader);
    m_nodeSearch->setPlaceholderText(QString::fromUtf8("搜索节点"));
    m_nodeSearch->setFixedWidth(180);
    m_nodeSearch->hide(); // 默认隐藏，点放大镜展开
    nodeHeaderLayout->addWidget(m_nodeSearch);
    nodeHeaderLayout->addStretch();
    auto *speedTest = new QPushButton(QChar(0x21BB), nodeHeader); // ↻ 测速/刷新
    speedTest->setObjectName("iconButton");
    speedTest->setFixedWidth(30);
    speedTest->setToolTip(QString::fromUtf8("测速"));
    nodeHeaderLayout->addWidget(speedTest);
    auto *helpBtn = new QPushButton(QChar(0x003F), nodeHeader); // ? 帮助
    helpBtn->setObjectName("iconButton");
    helpBtn->setFixedWidth(30);
    helpBtn->setToolTip(QString::fromUtf8("帮助"));
    nodeHeaderLayout->addWidget(helpBtn);
    rightLayout->addWidget(nodeHeader);
    connect(searchIcon, &QPushButton::clicked, this, [this] {
        m_nodeSearch->setVisible(!m_nodeSearch->isVisible());
        if (m_nodeSearch->isVisible()) {
            m_nodeSearch->setFocus();
        } else {
            m_nodeSearch->clear();
        }
    });
    connect(m_nodeSearch, &QLineEdit::textChanged, this, [this] { populateNodeList(); });
    connect(speedTest, &QPushButton::clicked, this, [this] {
        appendLog(QString::fromUtf8("开始测速..."));
        m_service.refreshNodes();
        m_service.testDelays();
    });
    connect(helpBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl("https://clashr-auto.gitbook.io"));
    });

    auto *scroll = new QScrollArea(right);
    scroll->setObjectName("nodeScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nodeList = new QFrame(scroll);
    m_nodeList->setObjectName("nodeListBody");
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
    scroll->setObjectName("subScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_subscriptionList = new QFrame(scroll);
    m_subscriptionList->setObjectName("subListBody");
    auto *grid = new QGridLayout(m_subscriptionList);
    grid->setContentsMargins(0, 0, 5, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    scroll->setWidget(m_subscriptionList);
    layout->addWidget(scroll, 1);

    auto *bottom = new QHBoxLayout();
    bottom->setContentsMargins(0, 0, 0, 0);
    auto *applyButton = new QPushButton(QString::fromUtf8("应用"), page);
    applyButton->setObjectName("primaryButton");
    applyButton->setFixedSize(96, 30);
    auto *updateAll = new QPushButton(QString::fromUtf8("更新所有订阅"), page);
    updateAll->setObjectName("primaryButton");
    updateAll->setFixedSize(120, 30);
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

    const QString period = subscription.updateTime > 0
                               ? QString::fromUtf8(" · 每 %1 分钟").arg(subscription.updateTime)
                               : QString();
    auto *meta = new QLabel(QString::fromUtf8("%1 / %2 节点%3")
                                .arg(QString::number(subscription.enabledNodeCount),
                                     QString::number(subscription.nodeCount), period), card);
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
        const int updateTime = QInputDialog::getInt(this, QString::fromUtf8("编辑订阅"),
                                                    QString::fromUtf8("自动更新周期(分钟, 0=用全局默认):"),
                                                    subscription.updateTime, 0, 1440, 5, &ok);
        if (!ok) {
            return;
        }
        if (m_subscriptions->editSubscription(index, name.trimmed(), url.trimmed(), type)) {
            m_subscriptions->setSubscriptionUpdateTime(index, updateTime);
            reloadSubscriptions();
            m_core->rebuildConfig();
            appendLog(QString::fromUtf8("订阅已更新（自动更新周期 %1 分钟）").arg(updateTime));
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
    auto *nodeNote = new QCheckBox();
    nodeNote->setChecked(config.nodeSwitchNote);
    auto *autoTheme = new QCheckBox();
    autoTheme->setChecked(config.autoTheme);
    auto *autoUpdateSpin = new QSpinBox();
    autoUpdateSpin->setRange(0, 1440);
    autoUpdateSpin->setSuffix(QString::fromUtf8(" 分钟"));
    autoUpdateSpin->setValue(config.autoUpdateMinutes);
    autoUpdateSpin->setFixedWidth(200);
    auto *geoipBtn = new QPushButton(QString::fromUtf8("更新 GeoIP"));
    geoipBtn->setObjectName("nodeButton");
    geoipBtn->setFixedSize(110, 30);
    connect(geoipBtn, &QPushButton::clicked, this, [this, geoipBtn] {
        geoipBtn->setEnabled(false);
        geoipBtn->setText(QString::fromUtf8("下载中..."));
        const AppConfig cfg = AppConfigLoader::load();
        auto *nam = new QNetworkAccessManager(this);
        const QUrl mmdbUrl(QStringLiteral(
            "https://github.com/MetaCubeX/meta-rules-dat/releases/latest/download/country.mmdb"));
        QNetworkRequest req(mmdbUrl);
        req.setRawHeader("User-Agent", "clashauto-cpp");
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = nam->get(req);
        connect(reply, &QNetworkReply::downloadProgress, geoipBtn, [geoipBtn](qint64 r, qint64 t) {
            if (t > 0) {
                geoipBtn->setText(QString::fromUtf8("下载中 %1%").arg(int(r * 100 / t)));
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, nam, geoipBtn, cfg] {
            const bool ok = reply->error() == QNetworkReply::NoError;
            const QByteArray data = reply->readAll();
            const QString err = reply->errorString();
            reply->deleteLater();
            nam->deleteLater();
            geoipBtn->setEnabled(true);
            geoipBtn->setText(QString::fromUtf8("更新 GeoIP"));
            if (!ok || data.isEmpty()) {
                appendLog(QString::fromUtf8("GeoIP 更新失败: %1").arg(err));
                return;
            }
            int saved = 0;
            const QStringList targets = {QDir(cfg.userDir).filePath("Country.mmdb"),
                                         QDir(cfg.sourceRoot).filePath("config/Country.mmdb")};
            for (const QString &path : targets) {
                QDir().mkpath(QFileInfo(path).absolutePath());
                QFile out(path);
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    out.write(data);
                    out.close();
                    ++saved;
                }
            }
            appendLog(QString::fromUtf8("GeoIP 已更新（%1 处，%2 KB），重启核心生效")
                          .arg(QString::number(saved), QString::number(data.size() / 1024)));
        });
    });
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
    systemScroll->setObjectName("sysScroll");
    systemScroll->setWidgetResizable(true);
    systemScroll->setFrameShape(QFrame::NoFrame);
    auto *sysBody = new QWidget(systemScroll);
    sysBody->setObjectName("sysBody");
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
    sysLayout->addWidget(row(QString::fromUtf8("跟随系统深浅色"), autoTheme));
    sysLayout->addWidget(row(QString::fromUtf8("语言"), language));
    addDivider();
    addGroup(QString::fromUtf8("节点"));
    sysLayout->addWidget(row(QString::fromUtf8("仅可用节点"), nodeOnly));
    sysLayout->addWidget(row(QString::fromUtf8("增量更新"), increment));
    sysLayout->addWidget(row(QString::fromUtf8("切换通知"), nodeNote));
    sysLayout->addWidget(row(QString::fromUtf8("订阅自动更新(默认)"), autoUpdateSpin));
    addDivider();
    addGroup(QString::fromUtf8("内核"));
    sysLayout->addWidget(row(QString::fromUtf8("API 端口"), uiPort));
    sysLayout->addWidget(row(QString::fromUtf8("混合端口"), mixedPort));
    sysLayout->addWidget(row(QString::fromUtf8("TUN"), tun));
    sysLayout->addWidget(row(QString::fromUtf8("系统代理"), webProxy));
    sysLayout->addWidget(row(QString::fromUtf8("切换时清理连接"), clearConnections));
    sysLayout->addWidget(row(QString::fromUtf8("GeoIP 数据"), geoipBtn));
    addDivider();
    addGroup(QString::fromUtf8("托盘 / 启动"));
    sysLayout->addWidget(row(QString::fromUtf8("关闭到托盘"), closeToTray));
    sysLayout->addWidget(row(QString::fromUtf8("开机自启"), autoStart));
    addDivider();
    addGroup(QString::fromUtf8("其他"));
    // 旧项目系统页的工具：打开配置目录 / 导出节点 / 打开面板
    auto *openCfgBtn = new QPushButton(QString::fromUtf8("打开配置目录"));
    openCfgBtn->setObjectName("nodeButton");
    openCfgBtn->setFixedSize(120, 30);
    connect(openCfgBtn, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_userDir));
    });
    auto *exportBtn = new QPushButton(QString::fromUtf8("导出节点"));
    exportBtn->setObjectName("nodeButton");
    exportBtn->setFixedSize(120, 30);
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        if (m_currentNodes.isEmpty()) {
            appendLog(QString::fromUtf8("当前无节点可导出（请先在状态页加载节点）"));
            return;
        }
        QJsonArray arr;
        for (const NodeInfo &n : m_currentNodes) {
            arr.append(QJsonObject{{"name", n.name}, {"delay", n.delay}});
        }
        QJsonObject exp;
        exp.insert("selected", m_selectedNode);
        exp.insert("nodes", arr);
        QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (dir.isEmpty()) {
            dir = QDir::homePath();
        }
        const QString path = QDir(dir).filePath("clashauto-nodes.json");
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(exp).toJson(QJsonDocument::Indented));
            file.close();
            appendLog(QString::fromUtf8("已导出 %1 个节点: %2").arg(QString::number(m_currentNodes.size()), path));
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        } else {
            appendLog(QString::fromUtf8("导出失败: %1").arg(path));
        }
    });
    auto *openDashBtn = new QPushButton(QString::fromUtf8("打开面板"));
    openDashBtn->setObjectName("nodeButton");
    openDashBtn->setFixedSize(120, 30);
    connect(openDashBtn, &QPushButton::clicked, this, [host = config.host, port = config.uiPort] {
        QDesktopServices::openUrl(
            QUrl(QString("https://yacd.metacubex.one/?hostname=%1&port=%2").arg(host).arg(port)));
    });
    sysLayout->addWidget(row(QString::fromUtf8("配置目录"), openCfgBtn));
    sysLayout->addWidget(row(QString::fromUtf8("节点导出"), exportBtn));
    sysLayout->addWidget(row(QString::fromUtf8("Clash 面板"), openDashBtn));
    sysLayout->addStretch();
    systemScroll->setWidget(sysBody);
    tabs->addTab(systemScroll, QString::fromUtf8("系统"));

    // 页面：「应用」按钮放到 tab 栏右上角同一行（对应旧项目 top:5 right:5 绝对定位）
    auto *page = new QFrame(this);
    page->setObjectName("page");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(4);
    auto *save = new QPushButton(QString::fromUtf8("应用"));
    save->setObjectName("primaryButton");
    save->setFixedSize(82, 28);
    auto *saveCorner = new QWidget(tabs); // 加右边距，避免按钮贴边被裁
    auto *saveCornerLayout = new QHBoxLayout(saveCorner);
    saveCornerLayout->setContentsMargins(0, 0, 8, 0);
    saveCornerLayout->addWidget(save);
    tabs->setCornerWidget(saveCorner, Qt::TopRightCorner);
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
        out << "note: " << (nodeNote->isChecked() ? "true" : "false") << "\n";
        out << "autoUpdate: " << autoUpdateSpin->value() << "\n";
        out << "theme: " << (light ? "light" : "black") << "\n";
        out << "autoTheme: " << (autoTheme->isChecked() ? "true" : "false") << "\n";
        out << "language: " << language->currentText() << "\n";
        out << "use_rule:\n";
        out << "  allow: " << allowRule->text() << "\n";
        out << "  noallow: " << blockRule->text() << "\n";
        out << "  allowUse: " << (allowUse->isChecked() ? "true" : "false") << "\n";
        out << "  noallowUse: " << (blockUse->isChecked() ? "true" : "false") << "\n";
        appendLog(QString("Settings saved: %1").arg(path));
        m_closeToTray = closeToTray->isChecked();
        m_nodeSwitchNote = nodeNote->isChecked();
        m_autoTheme = autoTheme->isChecked();
        m_service.setClearConnectionsOnSwitch(clearConnections->isChecked());
        applyAutoStart(autoStart->isChecked());
        applyAutoUpdate(autoUpdateSpin->value());
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (m_autoTheme) {
            applyTheme(QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
                           ? QStringLiteral("black")
                           : QStringLiteral("light"));
        } else
#endif
        {
            applyTheme(light ? "light" : "black");
        }
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

    auto *tabs = new QTabWidget(page);
    tabs->setObjectName("logsTabs");
    // 「打开日志目录」放到 tab 栏右上角同一行（对应旧项目 top:5 right:5）
    auto *openDir = new QPushButton(QString::fromUtf8("打开日志目录"), tabs);
    openDir->setObjectName("iconButton");
    tabs->setCornerWidget(openDir, Qt::TopRightCorner);

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

    tabs->addTab(makeTimelineTab(m_logScroll, m_logTimeline), QString::fromUtf8("主要"));
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
    const QString version = QString::fromUtf8(APP_VERSION);

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

    // 复刻旧项目 about：版本信息 / 版本(绿粗，点击查更新) / Clash 核心 / 链接 / 路径
    addTitle(QString::fromUtf8("版本信息:"));
    auto *verLabel = new QLabel(page);
    verLabel->setFixedHeight(40);
    verLabel->setTextFormat(Qt::RichText);
    verLabel->setText(QString("Clash Auto: <a href='update' style='color:green;font-weight:bold;text-decoration:none;'>%1</a>").arg(version));
    verLabel->setToolTip(QString::fromUtf8("点击检查更新"));
    connect(verLabel, &QLabel::linkActivated, this, [this](const QString &) { showUpdateDialog(); });
    layout->addWidget(verLabel);
    addTitle("Clash:");
    addLink(QString::fromUtf8("用户讨论和bug提交:"), "https://t.me/clashr_auto");
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
    layout->setContentsMargins(10, 0, 5, 0); // 页脚已在右侧列内，无需再为侧栏留 120 左边距
    layout->setSpacing(5);

    m_usersLabel = new QLabel(QString::fromUtf8("◉ -"), footer);
    m_usersLabel->setObjectName("usersBadge");
    auto *footerArrow = new QLabel(QChar(0xE625), footer); // iconfont icon-arrow-2（方向-向右-粗）
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
    mode->addItems({QString::fromUtf8("规则"), QString::fromUtf8("全局"), QString::fromUtf8("直连")});
    mode->setFixedWidth(120); // 默认「规则」(Rule)，与旧项目一致
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
    // 旧项目 el-card__body 8px 10px（真机验证：垂直 28 会在 ~65px 高的卡里把两行文字挤重叠）
    layout->setContentsMargins(10, 8, 10, 8);
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
    layout->setContentsMargins(2, 0, 0, 0); // + 3px 透明左条 = 名称左内边距 5（对齐旧项目 padding-left:5）
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

    auto *footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    auto bulk = [this, dialog, subscriptionIndex](bool enabled) {
        if (m_subscriptions->setAllNodesEnabled(subscriptionIndex, enabled)) {
            reloadSubscriptions();
            m_core->rebuildConfig();
            dialog->close();
            showSubscriptionNodes(subscriptionIndex);
        }
    };
    auto *selectAll = new QPushButton(QString::fromUtf8("全选"), dialog);
    selectAll->setObjectName("nodeButton");
    selectAll->setFixedSize(82, 30);
    connect(selectAll, &QPushButton::clicked, dialog, [bulk] { bulk(true); });
    auto *selectNone = new QPushButton(QString::fromUtf8("全不选"), dialog);
    selectNone->setObjectName("nodeButton");
    selectNone->setFixedSize(82, 30);
    connect(selectNone, &QPushButton::clicked, dialog, [bulk] { bulk(false); });
    auto *close = new QPushButton("Close", dialog);
    close->setObjectName("primaryButton");
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    footer->addWidget(selectAll);
    footer->addWidget(selectNone);
    footer->addStretch();
    footer->addWidget(close);
    dialogLayout->addLayout(footer);
    dialog->setStyleSheet(styleSheet());
    dialog->show();
}

void MainWindow::showConnectionsDialog()
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QString::fromUtf8("连接"));
    dialog->setStyleSheet(styleSheet());
    dialog->resize(760, 480);
    auto *v = new QVBoxLayout(dialog);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(8);

    const QStringList headers = {QString::fromUtf8("主机"), QString::fromUtf8("网络"),
                                 QString::fromUtf8("代理链"), QString::fromUtf8("上传"),
                                 QString::fromUtf8("下载"), QString::fromUtf8("规则")};
    auto *table = new QTableWidget(0, headers.size(), dialog);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);

    auto *search = new QLineEdit(dialog);
    search->setPlaceholderText(QString::fromUtf8("按主机过滤"));
    v->addWidget(search);
    v->addWidget(table, 1);

    auto *countLabel = new QLabel(QString::fromUtf8("正在获取..."), dialog);
    countLabel->setObjectName("subCardMeta");

    // 按主机名过滤（隐藏不匹配行）；每次刷新后需重新应用
    auto applyFilter = [table, search] {
        const QString f = search->text().trimmed();
        for (int r = 0; r < table->rowCount(); ++r) {
            QTableWidgetItem *hostItem = table->item(r, 0);
            const QString host = hostItem ? hostItem->text() : QString();
            table->setRowHidden(r, !f.isEmpty() && !host.contains(f, Qt::CaseInsensitive));
        }
    };
    connect(search, &QLineEdit::textChanged, dialog, [applyFilter] { applyFilter(); });

    // 用 QPointer 守护对话框：异步回调返回时若已关闭则直接丢弃，避免悬空访问
    QPointer<QDialog> guard = dialog;
    auto reload = [this, guard, table, countLabel, applyFilter] {
        m_service.fetchConnections([this, guard, table, countLabel, applyFilter](QJsonArray conns) {
            if (!guard) {
                return;
            }
            table->setRowCount(0);
            for (const QJsonValue &cv : conns) {
                const QJsonObject c = cv.toObject();
                const QJsonObject meta = c.value("metadata").toObject();
                QString host = meta.value("host").toString();
                if (host.isEmpty()) {
                    host = meta.value("destinationIP").toString();
                }
                const QString port = meta.value("destinationPort").toString();
                if (!port.isEmpty()) {
                    host += ":" + port;
                }
                QStringList chains;
                for (const QJsonValue &ch : c.value("chains").toArray()) {
                    chains << ch.toString();
                }
                const int row = table->rowCount();
                table->insertRow(row);
                auto *hostItem = new QTableWidgetItem(host);
                hostItem->setData(Qt::UserRole, c.value("id").toString());
                table->setItem(row, 0, hostItem);
                table->setItem(row, 1, new QTableWidgetItem(meta.value("network").toString()));
                table->setItem(row, 2, new QTableWidgetItem(chains.join(" / ")));
                table->setItem(row, 3, new QTableWidgetItem(speedText(c.value("upload").toInteger())));
                table->setItem(row, 4, new QTableWidgetItem(speedText(c.value("download").toInteger())));
                table->setItem(row, 5, new QTableWidgetItem(c.value("rule").toString()));
            }
            countLabel->setText(QString::fromUtf8("共 %1 条连接").arg(table->rowCount()));
            applyFilter();
        });
    };

    auto *bar = new QHBoxLayout();
    bar->addWidget(countLabel);
    bar->addStretch();
    auto *refreshBtn = new QPushButton(QString::fromUtf8("刷新"), dialog);
    refreshBtn->setObjectName("nodeButton");
    refreshBtn->setFixedSize(72, 30);
    auto *closeSelBtn = new QPushButton(QString::fromUtf8("关闭选中"), dialog);
    closeSelBtn->setObjectName("nodeButton");
    closeSelBtn->setFixedSize(84, 30);
    auto *closeAllBtn = new QPushButton(QString::fromUtf8("关闭全部"), dialog);
    closeAllBtn->setObjectName("primaryButton");
    closeAllBtn->setFixedSize(84, 30);
    bar->addWidget(refreshBtn);
    bar->addWidget(closeSelBtn);
    bar->addWidget(closeAllBtn);
    v->addLayout(bar);

    connect(refreshBtn, &QPushButton::clicked, dialog, [reload] { reload(); });
    connect(closeSelBtn, &QPushButton::clicked, dialog, [this, table, reload, dialog] {
        QList<int> rows;
        for (QTableWidgetItem *item : table->selectedItems()) {
            if (!rows.contains(item->row())) {
                rows << item->row();
            }
        }
        for (int row : rows) {
            const QString id = table->item(row, 0)->data(Qt::UserRole).toString();
            if (!id.isEmpty()) {
                m_service.closeConnection(id);
            }
        }
        QTimer::singleShot(300, dialog, [reload] { reload(); });
    });
    connect(closeAllBtn, &QPushButton::clicked, dialog, [this, reload, dialog] {
        m_service.clearConnections();
        QTimer::singleShot(300, dialog, [reload] { reload(); });
    });

    // 实时刷新（1.5s）；有选中行时跳过，避免打断「关闭选中」
    auto *autoTimer = new QTimer(dialog);
    autoTimer->setInterval(1500);
    connect(autoTimer, &QTimer::timeout, dialog, [table, reload] {
        if (table->selectedItems().isEmpty()) {
            reload();
        }
    });
    autoTimer->start();

    reload();
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
    QSettings().setValue("window/geometry", saveGeometry());
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

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    applyAcrylic(); // 窗口显示后应用亚克力（部分系统需窗口已实体化）
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#if defined(Q_OS_WIN)
    // 无边框窗口的原生缩放：命中窗口边缘时返回 HT* 让系统接管拉伸（含正确光标）
    MSG *msg = static_cast<MSG *>(message);
    if (msg && msg->message == WM_NCHITTEST) {
        const qreal dpr = devicePixelRatioF();
        const QPoint gp(qRound(GET_X_LPARAM(msg->lParam) / dpr), qRound(GET_Y_LPARAM(msg->lParam) / dpr));
        const QPoint p = mapFromGlobal(gp);
        const int b = 6; // 边缘感应宽度
        const int w = width();
        const int h = height();
        const bool left = p.x() >= 0 && p.x() < b;
        const bool right = p.x() <= w && p.x() > w - b;
        const bool top = p.y() >= 0 && p.y() < b;
        const bool bottom = p.y() <= h && p.y() > h - b;
        if (top && left) { *result = HTTOPLEFT; return true; }
        if (top && right) { *result = HTTOPRIGHT; return true; }
        if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
        if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
        if (left) { *result = HTLEFT; return true; }
        if (right) { *result = HTRIGHT; return true; }
        if (top) { *result = HTTOP; return true; }
        if (bottom) { *result = HTBOTTOM; return true; }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
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

void MainWindow::applyAutoUpdate(int minutes)
{
    // 全局默认周期（供未单独设置的订阅使用）；主定时器始终以 1 分钟节拍运行
    m_autoUpdateMinutes = minutes;
}

void MainWindow::registerUrlScheme()
{
#if defined(Q_OS_WIN)
    // 注册 clash-auto:// 协议到 HKCU（无需管理员）；每次启动幂等刷新指向当前 exe
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString base = "HKCU\\Software\\Classes\\clash-auto";
    const QString cmdVal = QLatin1Char('"') + exe + QLatin1String("\" \"%1\"");
    QProcess::execute("reg", {"add", base, "/ve", "/d", "URL:Clash Auto Protocol", "/f"});
    // 空数据用 /t REG_SZ 不带 /d，避免 QProcess 传空串参数的 Windows 怪癖
    QProcess::execute("reg", {"add", base, "/v", "URL Protocol", "/t", "REG_SZ", "/f"});
    QProcess::execute("reg", {"add", base + "\\shell\\open\\command", "/ve", "/d", cmdVal, "/f"});
#endif
}

void MainWindow::handleProtocolUrl(const QString &raw)
{
    // 支持 clash-auto://import?url=...&name=...&type=... 及 .../install-config?url=...
    const QUrl u(raw.trimmed());
    if (u.scheme().compare("clash-auto", Qt::CaseInsensitive) != 0) {
        return;
    }
    const QUrlQuery q(u);
    const QString subUrl = q.queryItemValue("url", QUrl::FullyDecoded).trimmed();
    if (subUrl.isEmpty()) {
        appendLog(QString::fromUtf8("协议链接缺少 url 参数: %1").arg(raw));
        return;
    }
    const QString name = q.queryItemValue("name", QUrl::FullyDecoded).trimmed();
    QString type = q.queryItemValue("type", QUrl::FullyDecoded).trimmed();
    if (type.isEmpty()) {
        type = "sub";
    }
    if (m_subscriptions->addSubscription(name, subUrl, type)) {
        const int index = m_subscriptions->load().size() - 1;
        reloadSubscriptions();
        setCurrentPage(1); // 订阅页
        const QString label = name.isEmpty() ? subUrl : name;
        appendLog(QString::fromUtf8("已从链接添加订阅: %1，正在获取节点...").arg(label));
        if (m_tray) {
            m_tray->notify(QString::fromUtf8("Clash Auto"),
                           QString::fromUtf8("已导入订阅 %1").arg(label));
        }
        if (index >= 0) {
            m_subscriptions->updateSubscription(index);
        }
    } else {
        appendLog(QString::fromUtf8("从链接添加订阅失败: %1").arg(raw));
    }
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
    // 标题栏 + 侧栏空白处都可拖拽窗口（菜单按钮自行消费点击，不会触发拖拽）
    if (event->button() == Qt::LeftButton
        && (event->pos().y() <= TitleHeight || event->pos().x() < SidebarWidth)) {
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
        #root { background:transparent; border-radius:10px; }
        #titleBar { background:#222; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#ccc; font-size:12px; padding-left:0; }
        #titleButton, #closeButton { color:#ccc; background:#222; border:0; border-radius:0; }
        #titleButton:hover { background:#333; }
        #closeButton:hover { background:red; color:white; }
        #body, #page { background:rgba(0,0,0,0); }
        #rightPane { background:#242425; }
        #sidebar { background:#222; border-bottom-left-radius:10px; }
        #logo { color:#ffff00; background:transparent; min-width:80px; max-width:80px; min-height:80px; max-height:80px; font-size:70px; font-family:'iconfont'; }
        #logo[state="tun"] { color:#ff0000; }
        #logo[state="proxy"] { color:#ffff00; }
        #logo[state="off"] { color:#ffffff; }
        #menuButton { color:#fff; background:transparent; border:0; border-left:3px solid transparent; border-radius:5px 0 0 5px; text-align:left; padding-left:32px; font-size:14px; }
        #menuButton:hover { background:rgb(62,62,62); padding-left:36px; }
        #menuButton:checked { background:#000; color:#ccc; border-left:3px solid #4898f8; }
        #version { color:#666; font-size:12px; }
        #metricCard { background:#222; border:0; border-radius:4px; min-height:70px; }
        #metricIcon { font-size:30px; color:#aaa; font-family:'iconfont'; }
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
        QComboBox { padding-right:24px; }
        QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:center right; width:24px; border:0; background:transparent; }
        QComboBox::down-arrow { image:url(:/assets/chevron-dark.svg); width:12px; height:12px; }
        QComboBox::down-arrow:on { top:1px; }
        QTabWidget::pane { border:0; }
        QTabBar::tab { color:#999; background:transparent; padding:8px 16px; }
        QTabBar::tab:selected { color:#fff; border-bottom:2px solid #4898f8; }
        QCheckBox, QLabel { color:#ccc; }
        #iconButton { color:#4898f8; background:transparent; border:0; font-size:18px; }
        #cardCorner { color:#fff; background:rgba(0,0,0,0.30); border:0; border-radius:0 0 0 5px; font-size:12px; }
        #cardCornerDanger { color:#fff; background:rgba(255,0,0,0.30); border:0; border-radius:0 5px 0 0; font-size:12px; }
        #nodeScroll, #nodeScroll > QWidget > QWidget, #nodeListBody { background:#242425; }
        #subScroll, #subScroll > QWidget > QWidget, #subListBody { background:#242425; }
        #sysScroll, #sysScroll > QWidget > QWidget, #sysBody { background:#242425; }
        #nodeRow, #plainCard { background:#252525; border:0; border-left:3px solid transparent; border-radius:4px; min-height:40px; }
        #nodeRow[active="true"] { background:rgba(72,151,248,0.69); border-left:3px solid #83bdff; }
        #nodeName { color:#ccc; font-size:12px; }
        #delayBadge { color:#222; border-radius:5px; padding:3px 5px; font-size:12px; }
        #nodeButton { color:#ccc; background:transparent; border:0; border-left:1px solid #000; border-radius:0; font-size:12px; }
        #nodeButton:hover { background:#333; }
        #primaryButton { color:white; background:#4898f8; border:0; border-radius:4px; min-height:30px; }
        #footer { background:#303032; border-bottom-right-radius:10px; }
        #usersBadge { color:#fff; background:#000; border-radius:3px; padding:3px 5px; font-size:12px; }
        #footerArrow { color:#666; font-size:14px; font-family:'iconfont'; }
        #footerLog { color:#ccc; font-size:12px; }
        #switchButton { color:#eee; background:#000; border:0; border-radius:3px; min-height:28px; padding-left:22px; padding-right:8px; font-size:12px; }
        #switchDot { background:#666; border:4px solid rgba(102,102,102,0.15); border-radius:8px; }
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
        #addCard { background:#222; border:0; border-radius:5px; color:#888; font-size:80px; }
        QDialog { background:#333; }
        QTableWidget { background:#333; color:#fff; border:1px solid #464646; gridline-color:#464646; }
        QTableWidget::item { padding:4px; }
        QTableWidget::item:selected { background:#222; color:#fff; }
        QHeaderView::section { background:#333; color:#fff; border:1px solid #464646; padding:4px 6px; }
        QTableCornerButton::section { background:#333; border:1px solid #464646; }
        QComboBox QAbstractItemView { color:#eee; background:#333; border:1px solid #000; border-radius:3px; padding:2px; selection-background-color:#555; selection-color:#eee; outline:0; }
        QComboBox QAbstractItemView::item { min-height:26px; padding:0 6px; }
        QProgressBar { background:#444; border:0; border-radius:5px; color:#fff; text-align:center; font-size:12px; }
        QProgressBar::chunk { background:#4898f8; border-radius:5px; }
        QSpinBox::up-button, QSpinBox::down-button { background:#333; border:1px solid #000; width:16px; }
        QListWidget { background:#222; color:#ccc; border:1px solid #333; border-radius:5px; font-size:12px; }
        QListWidget::item { padding:5px; border-radius:5px; }
        QListWidget::item:selected { background:#4898f8; color:white; }
        #footer QComboBox { background:#111; border:0; font-size:12px; }
    )";
}

QString MainWindow::lightStyle() const
{
    return R"(
        #root { background:transparent; border-radius:10px; }
        #titleBar { background:#eee; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#333; font-size:12px; }
        #titleButton, #closeButton { color:#333; background:#eee; border:0; border-radius:0; }
        #titleButton:hover { background:#fff; }
        #closeButton:hover { background:red; color:white; }
        #body, #page { background:rgba(0,0,0,0); }
        #rightPane { background:#ffffff; }
        #sidebar { background:#eee; border-bottom-left-radius:10px; }
        #logo { color:#ffff00; background:transparent; min-width:80px; max-width:80px; min-height:80px; max-height:80px; font-size:70px; font-family:'iconfont'; }
        #logo[state="tun"] { color:#ff0000; }
        #logo[state="proxy"] { color:#ffff00; }
        #logo[state="off"] { color:#333; }
        #menuButton { color:#333; background:transparent; border:0; border-left:3px solid transparent; border-radius:5px 0 0 5px; text-align:left; padding-left:32px; font-size:14px; }
        #menuButton:hover { background:rgb(210,210,210); padding-left:36px; }
        #menuButton:checked { background:#fff; color:#333; border-left:3px solid #4898f8; }
        #version { color:#666; font-size:12px; }
        #metricCard { background:#eee; border:0; border-radius:4px; min-height:70px; }
        #metricIcon { font-size:30px; color:#888; font-family:'iconfont'; }
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
        QComboBox { padding-right:24px; }
        QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:center right; width:24px; border:0; background:transparent; }
        QComboBox::down-arrow { image:url(:/assets/chevron-light.svg); width:12px; height:12px; }
        QComboBox::down-arrow:on { top:1px; }
        QTabWidget::pane { border:0; }
        QTabBar::tab { color:#999; background:transparent; padding:8px 16px; }
        QTabBar::tab:selected { color:#333; border-bottom:2px solid #4898f8; }
        QCheckBox, QLabel { color:#333; }
        #iconButton { color:#4898f8; background:transparent; border:0; font-size:18px; }
        #cardCorner { color:#fff; background:rgba(0,0,0,0.30); border:0; border-radius:0 0 0 5px; font-size:12px; }
        #cardCornerDanger { color:#fff; background:rgba(255,0,0,0.30); border:0; border-radius:0 5px 0 0; font-size:12px; }
        #nodeScroll, #nodeScroll > QWidget > QWidget, #nodeListBody { background:#ffffff; }
        #subScroll, #subScroll > QWidget > QWidget, #subListBody { background:#ffffff; }
        #sysScroll, #sysScroll > QWidget > QWidget, #sysBody { background:#ffffff; }
        #nodeRow, #plainCard { background:#eee; border:0; border-left:3px solid transparent; border-radius:4px; min-height:40px; }
        #nodeRow[active="true"] { background:rgba(72,151,248,0.69); border-left:3px solid #1f6fd2; }
        #nodeName { color:#333; font-size:12px; }
        #delayBadge { color:#333; border-radius:5px; padding:3px 5px; font-size:12px; }
        #nodeButton { color:#333; background:transparent; border:0; border-left:1px solid #fff; border-radius:0; font-size:12px; }
        #nodeButton:hover { background:#c1c1c1; }
        #primaryButton { color:white; background:#4898f8; border:0; border-radius:4px; min-height:30px; }
        #footer { background:#fafafa; border-bottom-right-radius:10px; }
        #usersBadge { color:#333; background:#fff; border-radius:3px; padding:3px 5px; font-size:12px; }
        #footerArrow { color:#e8e8e8; font-size:14px; font-family:'iconfont'; }
        #footerLog { color:#333; font-size:12px; }
        #switchButton { color:#333; background:#eee; border:0; border-radius:3px; min-height:28px; padding-left:22px; padding-right:8px; font-size:12px; }
        #switchDot { background:#fff; border:4px solid rgba(255,255,255,0.15); border-radius:8px; }
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
        #addCard { background:#eee; border:0; border-radius:5px; color:#333; font-size:80px; }
        QDialog { background:#fff; }
        QTableWidget { background:#fff; color:#333; border:1px solid #ebeef5; gridline-color:#ebeef5; }
        QTableWidget::item { padding:4px; }
        QTableWidget::item:selected { background:#f5f8ff; color:#333; }
        QHeaderView::section { background:#fff; color:#333; border:1px solid #ebeef5; padding:4px 6px; }
        QTableCornerButton::section { background:#fff; border:1px solid #ebeef5; }
        QComboBox QAbstractItemView { color:#606266; background:#fff; border:1px solid #e4e7ed; border-radius:3px; padding:2px; selection-background-color:#f5f7fa; selection-color:#4898f8; outline:0; }
        QComboBox QAbstractItemView::item { min-height:26px; padding:0 6px; }
        QProgressBar { background:#eaeaea; border:0; border-radius:5px; color:#333; text-align:center; font-size:12px; }
        QProgressBar::chunk { background:#4898f8; border-radius:5px; }
        QSpinBox::up-button, QSpinBox::down-button { background:#ccc; border:1px solid #eaeaea; width:16px; }
        QListWidget { background:#eee; color:#333; border:1px solid #ccc; border-radius:5px; font-size:12px; }
        QListWidget::item { padding:5px; border-radius:5px; }
        QListWidget::item:selected { background:#4898f8; color:white; }
        #footer QComboBox { background:#eee; border:0; font-size:12px; }
    )";
}

void MainWindow::applyTheme(const QString &theme)
{
    const bool light = theme.compare("light", Qt::CaseInsensitive) == 0
                       || theme.compare("white", Qt::CaseInsensitive) == 0;
    m_theme = light ? "white" : "black";
    setStyleSheet(light ? lightStyle() : appStyle());
    applyAcrylic(); // 主题变化时重新着色亚克力
}

void MainWindow::applyAcrylic()
{
#if defined(Q_OS_WIN)
    // 整窗亚克力毛玻璃：调用未文档化的 user32!SetWindowCompositionAttribute
    enum { ACCENT_ENABLE_ACRYLICBLURBEHIND = 4, WCA_ACCENT_POLICY = 19 };
    struct AccentPolicy { int AccentState; int AccentFlags; unsigned int GradientColor; int AnimationId; };
    struct WinCompAttrData { int Attrib; void *pvData; size_t cbData; };
    using SetWCA = BOOL(WINAPI *)(HWND, WinCompAttrData *);

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    auto setWCA = reinterpret_cast<SetWCA>(GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!setWCA) {
        return;
    }
    // GradientColor 为 ABGR：标题色 + 0.9 不透明（0xE6）。侧栏本身透明，由亚克力提供 0.9 标题色 + 模糊
    const unsigned int tint = (m_theme == "white") ? 0xE6EEEEEE : 0xE6222222;
    AccentPolicy accent{ ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, tint, 0 };
    WinCompAttrData data{ WCA_ACCENT_POLICY, &accent, sizeof(accent) };
    setWCA(reinterpret_cast<HWND>(winId()), &data);
#endif
}
