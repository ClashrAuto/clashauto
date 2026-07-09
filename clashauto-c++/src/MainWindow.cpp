#include "MainWindow.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QDateTime>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int TitleHeight = 28;
constexpr int SidebarWidth = 120;
constexpr int FooterHeight = 38;
constexpr int Radius = 5;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    const AppConfig config = AppConfigLoader::load();
    QDir().mkpath(config.userDir + "/logs");
    m_logFilePath = config.userDir + "/logs/qt-main.log";
    m_core = new CoreController(config, this);
    m_tray = new TrayController(this, this);
    m_subscriptions = new SubscriptionStore(config, this);

    setWindowTitle("Clash Auto");
    setWindowIcon(QIcon(":/assets/icon.ico"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground, true);
    resize(980, 640);
    setMinimumSize(760, 480);

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
    m_pages->addWidget(buildVipPage());
    m_pages->addWidget(buildAboutPage());

    rightLayout->addWidget(m_pages, 1);
    bodyLayout->addWidget(right, 1);
    outer->addWidget(body, 1);
    outer->addWidget(buildFooter());
    setCentralWidget(root);

    setStyleSheet(appStyle());
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
        }
        m_tray->setStatus(tun, proxy, core);
    };
    connect(&m_service, &ClashService::statusUpdated, this, applyStatus);
    connect(m_core, &CoreController::statusChanged, this, applyStatus);
    connect(m_core, &CoreController::logUpdated, this, [this](const QString &message) {
        appendLog(message);
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

    addButton("-", "titleButton", [this] { showMinimized(); });
    addButton("[]", "titleButton", [this] { isMaximized() ? showNormal() : showMaximized(); });
    addButton("X", "closeButton", [this] { close(); });
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
    logoWrap->setFixedHeight(96);
    auto *logoLayout = new QVBoxLayout(logoWrap);
    logoLayout->setContentsMargins(0, 0, 5, 0);
    auto *logo = new QLabel("CA", logoWrap);
    logo->setObjectName("logo");
    logo->setAlignment(Qt::AlignCenter);
    logoLayout->addWidget(logo);
    layout->addWidget(logoWrap);

    const QStringList menu = {"Status", "Sub", "Settings", "Logs", "VIP", "About"};
    for (int i = 0; i < menu.size(); ++i) {
        layout->addWidget(createMenuButton(menu[i], i));
    }
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

    auto *metrics = new QGridLayout();
    metrics->setContentsMargins(0, 0, 0, 0);
    metrics->setHorizontalSpacing(10);
    metrics->setVerticalSpacing(10);
    metrics->addWidget(createMetricCard("UP", "Upload", &m_upValue, "up"), 0, 0);
    metrics->addWidget(createMetricCard("DN", "Download", &m_downValue, "down"), 0, 1);
    metrics->addWidget(createMetricCard("PR", "Processes", &m_processValue, "process"), 1, 0);
    metrics->addWidget(createMetricCard("TD", "Total Down", &m_totalDownValue, "download"), 1, 1);
    leftLayout->addLayout(metrics);

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
    m_nodeTitle = new QLabel("Nodes <span style='font-size:9px'>(0)</span>", nodeHeader);
    m_nodeTitle->setObjectName("sectionTitle");
    nodeHeaderLayout->addWidget(m_nodeTitle);
    m_nodeSearch = new QLineEdit(nodeHeader);
    m_nodeSearch->setPlaceholderText("Search nodes");
    m_nodeSearch->setFixedWidth(180);
    nodeHeaderLayout->addWidget(m_nodeSearch);
    m_nodeGroupSelector = new QComboBox(nodeHeader);
    m_nodeGroupSelector->addItem("GLOBAL");
    m_nodeGroupSelector->setFixedWidth(130);
    nodeHeaderLayout->addWidget(m_nodeGroupSelector);
    auto *refresh = new QPushButton("R", nodeHeader);
    refresh->setObjectName("iconButton");
    refresh->setFixedWidth(30);
    nodeHeaderLayout->addWidget(refresh);
    rightLayout->addWidget(nodeHeader);
    connect(m_nodeSearch, &QLineEdit::textChanged, this, [this] { populateNodeList(); });
    connect(m_nodeGroupSelector, &QComboBox::currentTextChanged, &m_service, &ClashService::setSelectedGroup);
    connect(refresh, &QPushButton::clicked, &m_service, &ClashService::refreshNodes);

    auto *scroll = new QScrollArea(right);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
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
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 10);

    auto *actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    auto *addButton = new QPushButton("Add", page);
    addButton->setObjectName("primaryButton");
    addButton->setFixedWidth(82);
    auto *reloadButton = new QPushButton("Reload", page);
    reloadButton->setObjectName("primaryButton");
    reloadButton->setFixedWidth(82);
    auto *applyButton = new QPushButton("Apply", page);
    applyButton->setObjectName("primaryButton");
    applyButton->setFixedWidth(82);
    actions->addWidget(addButton);
    actions->addWidget(reloadButton);
    actions->addStretch();
    actions->addWidget(applyButton);
    layout->addLayout(actions);

    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_subscriptionList = new QFrame(scroll);
    auto *listLayout = new QVBoxLayout(m_subscriptionList);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(10);
    scroll->setWidget(m_subscriptionList);
    layout->addWidget(scroll, 1);

    connect(reloadButton, &QPushButton::clicked, this, &MainWindow::reloadSubscriptions);
    connect(applyButton, &QPushButton::clicked, m_core, &CoreController::rebuildConfig);
    connect(addButton, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString url = QInputDialog::getText(this, "Add subscription", "URL or local path:", QLineEdit::Normal, "", &ok);
        if (!ok || url.trimmed().isEmpty()) {
            return;
        }
        const QString name = QInputDialog::getText(this, "Add subscription", "Name:", QLineEdit::Normal, url, &ok);
        if (!ok) {
            return;
        }
        const QStringList types = {"sub", "clash"};
        const QString type = QInputDialog::getItem(this, "Add subscription", "Type:", types, 0, false, &ok);
        if (!ok) {
            return;
        }
        if (m_subscriptions->addSubscription(name.trimmed(), url.trimmed(), type)) {
            reloadSubscriptions();
            appendLog("Subscription added");
        }
    });

    reloadSubscriptions();
    return page;
}

QWidget *MainWindow::buildSubscriptionsPageLegacy()
{
    auto *page = new QFrame(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 10);
    auto *button = new QPushButton("添加订阅", page);
    button->setObjectName("primaryButton");
    button->setFixedWidth(96);
    layout->addWidget(button, 0, Qt::AlignLeft);
    for (const QString &name : {"默认订阅", "Clash 订阅链接"}) {
        NodeInfo node;
        node.name = name;
        node.delay = 0;
        layout->addWidget(createNodeRow(node));
    }
    layout->addStretch();
    return page;
}

QWidget *MainWindow::buildSettingsPage()
{
    const AppConfig config = AppConfigLoader::load();
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName("settingsTabs");

    auto *uiPage = new QWidget(tabs);
    auto *form = new QFormLayout(uiPage);
    form->setContentsMargins(10, 16, 10, 10);
    auto *host = new QComboBox(uiPage);
    host->setEditable(true);
    host->addItems({"127.0.0.1", "localhost"});
    host->setCurrentText(config.host);
    auto *uiPort = new QLineEdit(QString::number(config.uiPort), uiPage);
    auto *mixedPort = new QLineEdit(QString::number(config.mixedPort), uiPage);
    form->addRow("Clash API host", host);
    form->addRow("Clash API port", uiPort);
    form->addRow("Mixed port", mixedPort);
    tabs->addTab(uiPage, "UI");

    auto *systemPage = new QWidget(tabs);
    auto *system = new QFormLayout(systemPage);
    system->setContentsMargins(10, 16, 10, 10);
    auto *nodeOnly = new QCheckBox(systemPage);
    nodeOnly->setChecked(config.nodeOnlyAvailable);
    auto *webProxy = new QCheckBox(systemPage);
    webProxy->setChecked(config.webProxy);
    auto *tun = new QCheckBox(systemPage);
    tun->setChecked(config.tun);
    auto *clearConnections = new QCheckBox(systemPage);
    clearConnections->setChecked(config.clearConnections);
    auto *increment = new QCheckBox(systemPage);
    increment->setChecked(config.increment);
    system->addRow("Only available nodes", nodeOnly);
    system->addRow("Web proxy", webProxy);
    system->addRow("TUN", tun);
    system->addRow("Clear connections", clearConnections);
    system->addRow("Increment update", increment);
    auto *theme = new QComboBox(systemPage);
    theme->addItems({"Dark", "Light"});
    theme->setCurrentText(config.theme.compare("light", Qt::CaseInsensitive) == 0 ? "Light" : "Dark");
    system->addRow("Theme", theme);
    tabs->addTab(systemPage, "System");

    auto *filterPage = new QWidget(tabs);
    auto *filter = new QFormLayout(filterPage);
    filter->setContentsMargins(10, 16, 10, 10);
    auto *allowUse = new QCheckBox(filterPage);
    allowUse->setChecked(config.allowRuleEnabled);
    auto *allowRule = new QLineEdit(config.allowRule.isEmpty() ? "\\[0\\.[0-9]+\\]" : config.allowRule, filterPage);
    auto *blockUse = new QCheckBox(filterPage);
    blockUse->setChecked(config.noAllowRuleEnabled);
    auto *blockRule = new QLineEdit(config.noAllowRule.isEmpty() ? "CN|^CN|CN_" : config.noAllowRule, filterPage);
    filter->addRow("Enable allow rule", allowUse);
    filter->addRow("Allow rule", allowRule);
    filter->addRow("Enable block rule", blockUse);
    filter->addRow("Block rule", blockRule);
    tabs->addTab(filterPage, "Node filter");

    auto *page = new QFrame(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->addWidget(tabs, 1);
    auto *save = new QPushButton("Save settings", page);
    save->setObjectName("primaryButton");
    save->setFixedWidth(120);
    layout->addWidget(save, 0, Qt::AlignRight);
    connect(save, &QPushButton::clicked, this, [=] {
        const QString path = QDir(config.userDir).filePath("config.yaml");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            appendLog(QString("Save settings failed: %1").arg(file.errorString()));
            return;
        }
        QTextStream out(&file);
        out << "host: " << host->currentText().trimmed() << "\n";
        out << "ui: " << uiPort->text().trimmed() << "\n";
        out << "port: " << mixedPort->text().trimmed() << "\n";
        out << "web: " << (webProxy->isChecked() ? "true" : "false") << "\n";
        out << "use: " << (tun->isChecked() ? "true" : "false") << "\n";
        out << "node: " << (nodeOnly->isChecked() ? "true" : "false") << "\n";
        out << "clearConnections: " << (clearConnections->isChecked() ? "true" : "false") << "\n";
        out << "increment: " << (increment->isChecked() ? "true" : "false") << "\n";
        out << "theme: " << (theme->currentText() == "Light" ? "light" : "black") << "\n";
        out << "language: " << config.language << "\n";
        out << "use_rule:\n";
        out << "  allow: " << allowRule->text() << "\n";
        out << "  noallow: " << blockRule->text() << "\n";
        out << "  allowUse: " << (allowUse->isChecked() ? "true" : "false") << "\n";
        out << "  noallowUse: " << (blockUse->isChecked() ? "true" : "false") << "\n";
        appendLog(QString("Settings saved: %1").arg(path));
        m_core->rebuildConfig();
    });

    return page;
}

QWidget *MainWindow::buildLogsPage()
{
    m_logView = new QTextEdit(this);
    m_logView->setObjectName("logView");
    m_logView->setReadOnly(true);
    m_logView->setPlainText("Main log\nWaiting for core output...");
    return m_logView;
}

QWidget *MainWindow::buildVipPage()
{
    auto *page = new QFrame(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 10);
    for (const QString &text : {"Points: -", "Member: unbound", "Device ID: ********", "Share link"}) {
        auto *card = new QFrame(page);
        card->setObjectName("plainCard");
        auto *cardLayout = new QHBoxLayout(card);
        cardLayout->addWidget(new QLabel(text, card));
        layout->addWidget(card);
    }
    layout->addStretch();
    return page;
}

QWidget *MainWindow::buildAboutPage()
{
    auto *page = new QFrame(this);
    auto *layout = new QVBoxLayout(page);
    layout->setAlignment(Qt::AlignCenter);
    auto *logo = new QLabel("Clash Auto", page);
    logo->setObjectName("aboutTitle");
    logo->setAlignment(Qt::AlignCenter);
    auto *desc = new QLabel("C++ / Qt rebuild of Clashr-Auto UI", page);
    desc->setAlignment(Qt::AlignCenter);
    layout->addWidget(logo);
    layout->addWidget(desc);
    return page;
}

QWidget *MainWindow::buildFooter()
{
    auto *footer = new QFrame(this);
    footer->setObjectName("footer");
    footer->setFixedHeight(FooterHeight);
    auto *layout = new QHBoxLayout(footer);
    layout->setContentsMargins(SidebarWidth, 0, 5, 0);
    layout->setSpacing(5);

    m_usersLabel = new QLabel("-", footer);
    m_usersLabel->setObjectName("usersBadge");
    m_logLabel = new QLabel("Ready", footer);
    m_logLabel->setObjectName("footerLog");
    layout->addWidget(m_usersLabel);
    layout->addWidget(new QLabel("|", footer));
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
    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(10);

    auto *iconLabel = new QLabel(icon, card);
    iconLabel->setObjectName("metricIcon");
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setFixedWidth(34);
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
    layout->addWidget(name, 1);

    auto *delay = new QLabel(QString("%1/%2").arg(speedText(node.speed), node.delay == 0 ? "-" : QString::number(node.delay)), row);
    delay->setObjectName("delayBadge");
    delay->setStyleSheet(QString("background:%1;").arg(delayColor(node.delay).name(QColor::HexArgb)));
    delay->setAlignment(Qt::AlignCenter);
    delay->setFixedWidth(104);
    layout->addWidget(delay);

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

QFrame *MainWindow::createSubscriptionRow(const SubscriptionSummary &subscription, int index)
{
    auto *row = new QFrame(this);
    row->setObjectName("nodeRow");
    row->setProperty("active", subscription.use);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(8);

    const QString title = subscription.name.isEmpty() ? subscription.url : subscription.name;
    auto *text = new QLabel(QString("[%1] %2\n%3 / %4 nodes")
                                .arg(subscription.type.isEmpty() ? "sub" : subscription.type)
                                .arg(title)
                                .arg(subscription.enabledNodeCount)
                                .arg(subscription.nodeCount),
                            row);
    text->setObjectName("nodeName");
    layout->addWidget(text, 1);

    auto *url = new QLabel(subscription.url, row);
    url->setObjectName("nodeName");
    url->setFixedWidth(240);
    url->setToolTip(subscription.url);
    layout->addWidget(url);

    auto *nodes = new QPushButton("Nodes", row);
    nodes->setObjectName("nodeButton");
    nodes->setFixedSize(82, 38);
    connect(nodes, &QPushButton::clicked, this, [this, index] {
        showSubscriptionNodes(index);
    });
    layout->addWidget(nodes);

    auto *update = new QPushButton("Update", row);
    update->setObjectName("nodeButton");
    update->setFixedSize(82, 38);
    connect(update, &QPushButton::clicked, this, [this, index] {
        appendLog("Updating subscription...");
        m_subscriptions->updateSubscription(index);
    });
    layout->addWidget(update);

    auto *toggle = new QPushButton(subscription.use ? "Disable" : "Use", row);
    toggle->setObjectName("nodeButton");
    toggle->setFixedSize(82, 38);
    connect(toggle, &QPushButton::clicked, this, [this, index, subscription] {
        if (m_subscriptions->setSubscriptionEnabled(index, !subscription.use)) {
            reloadSubscriptions();
            m_core->rebuildConfig();
        }
    });
    layout->addWidget(toggle);
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

void MainWindow::appendLog(const QString &message)
{
    const QString line = QString("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
                             .arg(message);
    if (m_logLabel) {
        m_logLabel->setText(message);
    }
    if (m_logView) {
        m_logView->append(line);
    }
    if (!m_logFilePath.isEmpty()) {
        QFile file(m_logFilePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            file.write(line.toUtf8());
            file.write("\n");
        }
    }
}

QLabel *MainWindow::createSwitchDot(bool enabled)
{
    auto *dot = new QLabel(this);
    dot->setObjectName("switchDot");
    dot->setProperty("on", enabled);
    dot->setFixedSize(16, 16);
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

void MainWindow::reloadSubscriptions()
{
    if (!m_subscriptionList) {
        return;
    }
    auto *layout = qobject_cast<QVBoxLayout *>(m_subscriptionList->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QVector<SubscriptionSummary> subscriptions = m_subscriptions->load();
    if (subscriptions.isEmpty()) {
        auto *empty = new QLabel("No subscriptions. Click Add to create one.", m_subscriptionList);
        empty->setObjectName("nodeName");
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(80);
        layout->addWidget(empty);
    } else {
        for (int i = 0; i < subscriptions.size(); ++i) {
            layout->addWidget(createSubscriptionRow(subscriptions[i], i));
        }
    }
    layout->addStretch();
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
        m_nodeTitle->setText(QString("Nodes <span style='font-size:9px'>(%1/%2)</span>")
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
        #root { background:#242425; border-radius:5px; }
        #titleBar { background:#222; border-top-left-radius:5px; border-top-right-radius:5px; }
        #windowTitle { color:#ccc; font-size:12px; padding-left:0; }
        #titleButton, #closeButton { color:#ccc; background:#222; border:0; border-radius:0; }
        #titleButton:hover { background:#333; }
        #closeButton:hover { background:red; color:white; }
        #body, #rightPane, #page { background:rgba(0,0,0,0); }
        #sidebar { background:#303032; }
        #logo { color:#ffff00; font-size:70px; text-shadow:0 0 5px #baba36; }
        #menuButton { color:#fff; background:transparent; border:0; border-radius:5px 0 0 5px; text-align:left; padding-left:35px; font-size:14px; }
        #menuButton:hover { background:rgb(62,62,62); }
        #menuButton:checked { background:#000; color:#ccc; }
        #version { color:#666; font-size:12px; }
        #metricCard { background:#222; border:0; border-radius:4px; min-height:70px; }
        #metricIcon { font-size:30px; color:#aaa; }
        #metricTitle { color:#bfbfbf; font-size:12px; }
        #metricValue { color:#bfbfbf; font-size:20px; }
        #sectionTitle { color:#eee; font-size:18px; font-weight:400; }
        QScrollArea { background:transparent; border:0; }
        QScrollBar:vertical { background:#242425; width:10px; margin:0; }
        QScrollBar::handle:vertical { background:#555; border-radius:5px; min-height:24px; }
        QLineEdit, QComboBox, QTextEdit { color:#fff; background:#444; border:1px solid #333; border-radius:3px; min-height:28px; padding:0 8px; }
        QTabWidget::pane { border:0; }
        QTabBar::tab { color:#999; background:transparent; padding:8px 16px; }
        QTabBar::tab:selected { color:#fff; border-bottom:2px solid #4898f8; }
        QCheckBox, QLabel { color:#ccc; }
        #iconButton { color:#4898f8; background:transparent; border:0; font-size:18px; }
        #nodeRow, #plainCard { background:#252525; border:0; border-radius:4px; min-height:38px; }
        #nodeRow[active="true"] { background:rgba(72,151,248,0.69); }
        #nodeName { color:#ccc; font-size:12px; }
        #delayBadge { color:#222; border-radius:5px; padding:3px 5px; font-size:12px; }
        #nodeButton { color:#ccc; background:transparent; border:0; border-left:1px solid #000; border-radius:0; font-size:12px; }
        #nodeButton:hover { background:#333; }
        #primaryButton { color:white; background:#4898f8; border:0; border-radius:4px; min-height:30px; }
        #footer { background:#303032; border-bottom-left-radius:5px; border-bottom-right-radius:5px; }
        #usersBadge { color:#fff; background:#000; border-radius:3px; padding:3px 6px; font-size:12px; }
        #footerLog { color:#ccc; font-size:12px; }
        #switchButton { color:#eee; background:#000; border:0; border-radius:3px; min-height:28px; padding-left:22px; padding-right:8px; font-size:12px; }
        #switchDot { background:rgba(102,102,102,0.15); border:4px solid rgba(102,102,102,0.15); border-radius:8px; }
        #switchDot[on="true"] { background:#4898f8; border-color:rgba(72,152,248,0.30); }
        #logView { background:#111; color:#ccc; border:0; }
        #aboutTitle { color:#eee; font-size:28px; }
    )";
}
