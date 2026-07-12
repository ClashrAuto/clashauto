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
#include <QDirIterator>
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
#include <algorithm>
#include <functional>
#include <memory>
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
#include <QVariantAnimation>
#include <QRegularExpression>
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
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
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
#include <dwmapi.h>   // DwmSetWindowAttribute（系统标题栏着色）
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

// 底部开关（增强/网页/核心）的「呼吸圆点」——自绘并开启抗锯齿，
// 每帧按当前透明度重绘，避免 QGraphicsOpacityEffect 在高分屏下产生毛边。
class BreathingDot : public QWidget
{
public:
    explicit BreathingDot(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(16, 16);
        setAttribute(Qt::WA_TransparentForMouseEvents); // 点击穿透到开关按钮
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(1000);
        m_anim->setStartValue(1.0);
        m_anim->setKeyValueAt(0.5, 0.35);
        m_anim->setEndValue(1.0);
        m_anim->setLoopCount(-1);
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_opacity = v.toReal();
            update();
        });
    }

    void setLight(bool light)
    {
        m_light = light;
        update();
    }

    void setOn(bool on)
    {
        m_on = on;
        if (on) {
            if (m_anim->state() != QAbstractAnimation::Running)
                m_anim->start();
        } else {
            m_anim->stop();
            m_opacity = 1.0;
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const QColor core = m_on ? QColor(0x48, 0x98, 0xf8)
                                 : (m_light ? QColor(0xff, 0xff, 0xff) : QColor(0x66, 0x66, 0x66));
        QColor halo = core;
        halo.setAlphaF(m_on ? 0.30 : 0.15);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setOpacity(m_opacity);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);                                // 外圈光晕（对应旧 4px 半透明边框）
        p.drawEllipse(QRectF(0.5, 0.5, 15.0, 15.0));
        p.setBrush(core);                                // 内核实心圆点（8px）
        p.drawEllipse(QRectF(width() / 2.0 - 4.0, height() / 2.0 - 4.0, 8.0, 8.0));
    }

private:
    QVariantAnimation *m_anim = nullptr;
    qreal m_opacity = 1.0;
    bool m_on = false;
    bool m_light = false;
};

// ---- default.yaml 规则/分组解析辅助（无 YAML 库，按文本处理，见 CLAUDE.md 约定）----

// YAML 双引号标量反转义：\UXXXXXXXX / \uXXXX / \n \t \" \\ \/（default.yaml 里 emoji 被转义成 \U....）
QString yamlUnescapeDq(const QString &in)
{
    QString out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (c == QLatin1Char('\\') && i + 1 < in.size()) {
            const QChar n = in.at(i + 1);
            if (n == QLatin1Char('U') && i + 9 < in.size()) {
                bool ok = false;
                const char32_t cp = in.mid(i + 2, 8).toUInt(&ok, 16);
                if (ok) { out += QString::fromUcs4(&cp, 1); i += 9; continue; }
            } else if (n == QLatin1Char('u') && i + 5 < in.size()) {
                bool ok = false;
                const uint cp = in.mid(i + 2, 4).toUInt(&ok, 16);
                if (ok) { out += QChar(cp); i += 5; continue; }
            } else if (n == QLatin1Char('n')) { out += QLatin1Char('\n'); ++i; continue; }
            else if (n == QLatin1Char('t')) { out += QLatin1Char('\t'); ++i; continue; }
            else if (n == QLatin1Char('"')) { out += QLatin1Char('"'); ++i; continue; }
            else if (n == QLatin1Char('\\')) { out += QLatin1Char('\\'); ++i; continue; }
            else if (n == QLatin1Char('/')) { out += QLatin1Char('/'); ++i; continue; }
        }
        out += c;
    }
    return out;
}

// 取列表项 "  - xxx" 中 "- " 之后的原始文本（保留引号/转义，供原样回写）
QString yamlListItemRaw(const QString &line)
{
    const int dash = line.indexOf(QLatin1String("- "));
    return dash < 0 ? QString() : line.mid(dash + 2).trimmed();
}

// 把（可能带引号的）标量还原成明文，供显示与逗号分割
QString yamlScalarText(const QString &raw)
{
    if (raw.size() >= 2 && raw.startsWith(QLatin1Char('"')) && raw.endsWith(QLatin1Char('"'))) {
        return yamlUnescapeDq(raw.mid(1, raw.size() - 2));
    }
    if (raw.size() >= 2 && raw.startsWith(QLatin1Char('\'')) && raw.endsWith(QLatin1Char('\''))) {
        return raw.mid(1, raw.size() - 2).replace(QLatin1String("''"), QLatin1String("'"));
    }
    return raw;
}

// 双引号转义（回写新增/编辑的规则）
QString yamlEscapeDq(const QString &in)
{
    QString out = in;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return out;
}

// 取 "    type: select" 这类行的值（去掉 key: 前缀与引号）
QString yamlInlineValue(const QString &line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    if (colon < 0) return QString();
    return yamlScalarText(line.mid(colon + 1).trimmed());
}
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
    // 使用系统原生标题栏（不再无边框）；标题栏配色由 applyTitleBarColor() 通过 DWM 设置
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

    // 顶部自绘标题栏已移除，改用系统原生标题栏
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
        // 仅当节点数据真正变化时才重建列表——轮询大多返回相同数据，
        // 每次都重建会让状态列表刷新时闪烁。
        const bool changed = (nodes != m_currentNodes);
        m_currentNodes = nodes;
        // 节点切换通知（对应旧项目 config.note）；跳过首次填充，避免启动即误报
        if (m_nodeSwitchNote && m_nodeInitialized && m_tray && !selected.isEmpty() && selected != m_selectedNode) {
            m_tray->notify(QString::fromUtf8("节点切换"), QString::fromUtf8("已切换到 %1").arg(selected));
        }
        m_nodeInitialized = true;
        m_selectedNode = selected;
        if (changed) {
            populateNodeList();
        }
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
        static_cast<BreathingDot *>(m_tunDot)->setOn(tun);
        static_cast<BreathingDot *>(m_proxyDot)->setOn(proxy);
        static_cast<BreathingDot *>(m_coreDot)->setOn(core);
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
    logoLayout->addWidget(m_logo, 0, Qt::AlignHCenter); // 地球图标在侧栏中水平居中
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
    // 两列固定各占 50%，卡片宽度不再随数值文本长度变化（消除刷新时的宽度抖动）
    metrics->setColumnStretch(0, 1);
    metrics->setColumnStretch(1, 1);
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
    // 允许/排除规则改为可编辑下拉，预置项对齐旧项目 use_rule.allows / noallows
    auto *allowRule = new QComboBox();
    allowRule->setEditable(true);
    allowRule->addItems({QStringLiteral("\\[0\\.[0-9]+\\]")});
    allowRule->setCurrentText(config.allowRule.isEmpty() ? QStringLiteral("\\[0\\.[0-9]+\\]") : config.allowRule);
    auto *blockUse = new QCheckBox();
    blockUse->setChecked(config.noAllowRuleEnabled);
    auto *blockRule = new QComboBox();
    blockRule->setEditable(true);
    blockRule->addItems({QStringLiteral("\\[[0-9]+\\]"), QStringLiteral("[0-9\\[\\]]+"),
                         QStringLiteral("[\\[0-9\\]]+$"), QStringLiteral("[0-9]{1}\\]$"),
                         QStringLiteral("CN"), QStringLiteral("^CN"), QStringLiteral("CN_"), QStringLiteral(" ")});
    blockRule->setCurrentText(config.noAllowRule.isEmpty() ? QStringLiteral("CN|^CN|CN_") : config.noAllowRule);
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

    // 对齐旧项目：设置页仅 4 个 tab（过滤/区域/规则/系统）；原「远程」的 Host 并入系统 tab 内核分组
    // Tab 1: 过滤
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

    // Tab 3 / 4: 区域 / 规则（读写 userDir/default.yaml；ConfigBuilder 再生成 full.yaml 给核心）
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
    // 分组与项目严格对齐旧项目 setting.vue 系统 tab（system/node/note/increase/ui/core/other）
    addGroup(QString::fromUtf8("系统"));
    sysLayout->addWidget(row(QString::fromUtf8("开机自启"), autoStart));
    sysLayout->addWidget(row(QString::fromUtf8("关闭到托盘"), closeToTray));
    addDivider();
    addGroup(QString::fromUtf8("节点"));
    sysLayout->addWidget(row(QString::fromUtf8("仅可用节点"), nodeOnly));
    sysLayout->addWidget(row(QString::fromUtf8("增量更新"), increment));
    addDivider();
    addGroup(QString::fromUtf8("通知"));
    sysLayout->addWidget(row(QString::fromUtf8("切换通知"), nodeNote));
    addDivider();
    addGroup(QString::fromUtf8("增强"));
    sysLayout->addWidget(row(QString::fromUtf8("系统代理"), webProxy));
    sysLayout->addWidget(row(QString::fromUtf8("GeoIP 数据"), geoipBtn));
    addDivider();
    addGroup(QString::fromUtf8("界面"));
    sysLayout->addWidget(row(QString::fromUtf8("主题"), theme));
    sysLayout->addWidget(row(QString::fromUtf8("跟随系统深浅色"), autoTheme));
    addDivider();
    addGroup(QString::fromUtf8("内核"));
    host->setMinimumWidth(200);
    sysLayout->addWidget(row(QString::fromUtf8("Host"), host));       // 原「远程」tab 的 Host 并入此处
    sysLayout->addWidget(row(QString::fromUtf8("端口"), mixedPort));  // http(s)&socks 混合端口
    sysLayout->addWidget(row(QString::fromUtf8("切换时清理连接"), clearConnections));
    addDivider();
    addGroup(QString::fromUtf8("其他"));
    sysLayout->addWidget(row(QString::fromUtf8("语言"), language));
    // 以下项已按旧项目从界面移除（API 端口固定、TUN 由底部开关控制、订阅周期用默认），
    // 但控件仍存活以便「应用」时把原值写回，避免重置。
    uiPort->setParent(sysBody);
    uiPort->hide();
    tun->setParent(sysBody);
    tun->hide();
    autoUpdateSpin->setParent(sysBody);
    autoUpdateSpin->hide();
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
        out << "  allow: " << allowRule->currentText() << "\n";
        out << "  noallow: " << blockRule->currentText() << "\n";
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

QString MainWindow::defaultConfigPath() const
{
    // 规则/分组的持久化源 = userDir/default.yaml（对齐旧项目 def）；缺失时从 bundle 复制
    const QString userDef = QDir(m_userDir).filePath("default.yaml");
    if (!QFile::exists(userDef)) {
        const AppConfig cfg = AppConfigLoader::load();
        const QString bundled = QDir(cfg.sourceRoot).filePath("config/default.yaml");
        if (QFile::exists(bundled)) {
            QDir().mkpath(m_userDir);
            QFile::copy(bundled, userDef);
        }
    }
    return userDef;
}

QJsonArray MainWindow::loadRuleSection(const QString &section) const
{
    QFile f(defaultConfigPath());
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QString yaml = QString::fromUtf8(f.readAll());
    f.close();
    const QStringList lines = yaml.split(QLatin1Char('\n'));
    QJsonArray arr;

    if (section == QLatin1String("rule")) {
        // 解析 rules: 块，每条 "  - TYPE,VALUE,NODE[,opts]"（MATCH 只有 2 段）；raw 原样保留以便回写
        bool inRules = false;
        for (const QString &line : lines) {
            if (!inRules) {
                if (line.startsWith(QLatin1String("rules:"))) {
                    inRules = true;
                }
                continue;
            }
            if (line.startsWith(QLatin1String("  - "))) {
                const QString raw = yamlListItemRaw(line);
                const QStringList p = yamlScalarText(raw).split(QLatin1Char(','));
                QJsonObject o;
                o["raw"] = raw;
                if (p.value(0) == QLatin1String("MATCH")) {
                    o["type"] = QStringLiteral("MATCH");
                    o["node"] = p.value(1);
                    o["value"] = QString();
                } else {
                    o["type"] = p.value(0);
                    o["value"] = p.value(1);
                    o["node"] = p.value(2);
                }
                arr.append(o);
            } else if (!line.trimmed().isEmpty() && !line.startsWith(QLatin1Char(' '))) {
                break; // 下一个顶层键
            }
        }
        return arr;
    }

    if (section == QLatin1String("area")) {
        // 解析 proxy-groups：每组从 "  - name:" 到下一个 "  - " 或块结束；raw 保留整组文本
        bool inGroups = false;
        QJsonObject cur;
        QStringList curRaw;
        QStringList curProxies;
        auto flush = [&]() {
            if (!curRaw.isEmpty()) {
                cur["raw"] = curRaw.join(QLatin1Char('\n'));
                if (cur.value("rule").toString().isEmpty()) {
                    cur["rule"] = curProxies.join(QLatin1String(", "));
                }
                arr.append(cur);
            }
            cur = QJsonObject();
            curRaw.clear();
            curProxies.clear();
        };
        for (const QString &line : lines) {
            if (!inGroups) {
                if (line.startsWith(QLatin1String("proxy-groups:"))) {
                    inGroups = true;
                }
                continue;
            }
            if (!line.trimmed().isEmpty() && !line.startsWith(QLatin1Char(' '))) {
                flush();
                break; // 到 rules: 等下一个顶层键
            }
            if (line.startsWith(QLatin1String("  - "))) {
                flush();
                curRaw << line;
                const QString afterDash = yamlListItemRaw(line);
                if (afterDash.startsWith(QLatin1String("name:"))) {
                    cur["name"] = yamlInlineValue(afterDash);
                }
            } else {
                curRaw << line;
                const QString t = line.trimmed();
                if (t.startsWith(QLatin1String("name:"))) {
                    cur["name"] = yamlInlineValue(t);
                } else if (t.startsWith(QLatin1String("type:"))) {
                    cur["type"] = yamlInlineValue(t);
                } else if (t.startsWith(QLatin1String("rule:"))) {
                    cur["rule"] = yamlInlineValue(t);
                } else if (t.startsWith(QLatin1String("- "))) {
                    curProxies << yamlScalarText(yamlListItemRaw(t));
                }
            }
        }
        flush();
        return arr;
    }
    return {};
}

bool MainWindow::saveRuleSection(const QString &section, const QJsonArray &array)
{
    // 目前仅 rule 支持回写（区域为只读展示：mihomo 无分组 rule 字段，且改动嵌套组易产生悬空引用）
    if (section != QLatin1String("rule")) {
        return false;
    }
    const QString path = defaultConfigPath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QString yaml = QString::fromUtf8(f.readAll());
    f.close();

    // 构建新的 rules 块：未改动项用原始 raw（保留转义/选项），新增/编辑项按 3 段序列化
    QString block = QStringLiteral("rules:\n");
    for (const QJsonValue &v : array) {
        const QJsonObject o = v.toObject();
        const QString raw = o.value("raw").toString();
        if (!raw.isEmpty()) {
            block += QStringLiteral("  - ") + raw + QLatin1Char('\n');
        } else {
            const QString type = o.value("type").toString();
            const QString node = o.value("node").toString();
            const QString value = o.value("value").toString();
            const QString s = (type == QLatin1String("MATCH"))
                                  ? type + QLatin1Char(',') + node
                                  : type + QLatin1Char(',') + value + QLatin1Char(',') + node;
            block += QStringLiteral("  - \"") + yamlEscapeDq(s) + QStringLiteral("\"\n");
        }
    }

    // 定位并替换旧 rules 块（rules: 行起点 → 下一个顶层键 / EOF）
    int start = -1;
    if (yaml.startsWith(QLatin1String("rules:"))) {
        start = 0;
    } else {
        const int p = yaml.indexOf(QLatin1String("\nrules:"));
        if (p >= 0) {
            start = p + 1;
        }
    }
    if (start < 0) {
        if (!yaml.endsWith(QLatin1Char('\n'))) {
            yaml += QLatin1Char('\n');
        }
        yaml += block;
    } else {
        int end = yaml.size();
        int pos = yaml.indexOf(QLatin1Char('\n'), start); // rules: 行末
        while (pos >= 0) {
            const int lineStart = pos + 1;
            if (lineStart >= yaml.size()) {
                end = yaml.size();
                break;
            }
            const int nextNl = yaml.indexOf(QLatin1Char('\n'), lineStart);
            const QString ln = yaml.mid(lineStart, (nextNl < 0 ? yaml.size() : nextNl) - lineStart);
            if (ln.startsWith(QLatin1Char(' ')) || ln.trimmed().isEmpty()) {
                if (nextNl < 0) {
                    end = yaml.size();
                    break;
                }
                pos = nextNl;
            } else {
                end = lineStart; // 下一个顶层键起点
                break;
            }
        }
        yaml.replace(start, end - start, block);
    }

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    f.write(yaml.toUtf8());
    f.close();
    return true;
}

QStringList MainWindow::proxyGroupNames() const
{
    QStringList names;
    const QJsonArray groups = loadRuleSection("area");
    for (const QJsonValue &v : groups) {
        const QString n = v.toObject().value("name").toString();
        if (!n.isEmpty()) {
            names << n;
        }
    }
    return names;
}

QWidget *MainWindow::buildRuleTableTab(const QString &section)
{
    const bool isArea = section == "area";
    // 区域为只读展示（无「操作」列）；规则支持增删改
    const QStringList headers = isArea
        ? QStringList{QString::fromUtf8("名称"), QString::fromUtf8("类型"), QString::fromUtf8("节点/成员")}
        : QStringList{QString::fromUtf8("类型"), QString::fromUtf8("节点"), QString::fromUtf8("值"), QString::fromUtf8("操作")};
    const int keyCount = isArea ? 3 : 3;
    const int colCount = isArea ? 3 : 4;

    auto *w = new QWidget();
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(10, 10, 10, 10);
    l->setSpacing(8);

    auto *bar = new QHBoxLayout();
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(8);
    if (!isArea) {
        auto *addBtn = new QPushButton(QString::fromUtf8("＋ 添加"), w);
        addBtn->setObjectName("primaryButton");
        addBtn->setFixedSize(96, 30);
        connect(addBtn, &QPushButton::clicked, this, [this, section] { openRuleEditor(section, -1); });
        bar->addWidget(addBtn);
        auto *filter = new QLineEdit(w);
        filter->setPlaceholderText(QString::fromUtf8("搜索规则（类型/节点/值）"));
        filter->setFixedWidth(220);
        connect(filter, &QLineEdit::textChanged, this, [this] { reloadRuleTable("rule"); });
        m_ruleFilter = filter;
        bar->addWidget(filter);
    } else {
        auto *note = new QLabel(QString::fromUtf8("代理组来自 default.yaml（只读）"), w);
        note->setObjectName("subCardMeta");
        bar->addWidget(note);
    }
    auto *count = new QLabel(w);
    count->setObjectName("subCardMeta");
    if (isArea) {
        m_areaCountLabel = count;
    } else {
        m_ruleCountLabel = count;
    }
    bar->addStretch();
    bar->addWidget(count);
    l->addLayout(bar);

    auto *table = new QTableWidget(0, colCount, w);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int c = 0; c < colCount; ++c) {
        if (c == keyCount - 1) { // 最后一个数据列拉伸（值 / 节点成员）
            table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Stretch);
        } else if (!isArea && c == colCount - 1) { // 规则的操作列
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
    const bool isArea = section == "area";
    QTableWidget *table = isArea ? m_areaTable : m_ruleTable;
    if (!table) {
        return;
    }
    const QStringList keys = isArea
        ? QStringList{"name", "type", "rule"}
        : QStringList{"type", "node", "value"};
    const QJsonArray array = loadRuleSection(section);
    const QString filter = (!isArea && m_ruleFilter) ? m_ruleFilter->text().trimmed() : QString();
    const int cap = 400; // 规则可达上千条，超出用搜索缩小范围，避免一次性建太多行

    table->setUpdatesEnabled(false);
    table->setRowCount(0);
    int shown = 0;
    for (int i = 0; i < array.size(); ++i) {
        const QJsonObject obj = array[i].toObject();
        if (!filter.isEmpty()) {
            bool match = false;
            for (const QString &k : keys) {
                if (obj.value(k).toString().contains(filter, Qt::CaseInsensitive)) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                continue;
            }
        }
        if (shown >= cap) {
            break;
        }
        const int row = table->rowCount();
        table->insertRow(row);
        for (int c = 0; c < keys.size(); ++c) {
            table->setItem(row, c, new QTableWidgetItem(obj.value(keys[c]).toString()));
        }

        if (!isArea) {
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
            const int fullIndex = i; // 过滤/截断下仍指向完整数组的真实下标
            connect(edit, &QPushButton::clicked, this, [this, section, fullIndex] { openRuleEditor(section, fullIndex); });
            connect(del, &QPushButton::clicked, this, [this, section, fullIndex] {
                QJsonArray current = loadRuleSection(section);
                if (fullIndex >= 0 && fullIndex < current.size()) {
                    current.removeAt(fullIndex);
                    saveRuleSection(section, current);
                    reloadRuleTable(section);
                    if (m_core) {
                        m_core->rebuildConfig();
                    }
                    appendLog(QString::fromUtf8("已删除一条规则并重建配置"));
                }
            });
            table->setCellWidget(row, keys.size(), actions);
        }
        ++shown;
    }
    table->setUpdatesEnabled(true);

    QLabel *count = isArea ? m_areaCountLabel : m_ruleCountLabel;
    if (count) {
        QString txt = QString::fromUtf8("共 %1 条").arg(array.size());
        if (shown < array.size()) {
            txt += QString::fromUtf8("，显示前 %1 条").arg(shown);
        }
        count->setText(txt);
    }
}

void MainWindow::openRuleEditor(const QString &section, int editIndex)
{
    // 区域只读，此编辑器仅用于「规则」的增/改
    const QJsonArray array = loadRuleSection(section);
    const QJsonObject cur = (editIndex >= 0 && editIndex < array.size()) ? array[editIndex].toObject() : QJsonObject{};

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(editIndex >= 0 ? QString::fromUtf8("编辑规则") : QString::fromUtf8("新增规则"));
    dialog->setStyleSheet(styleSheet());
    dialog->resize(400, 250);
    auto *dl = new QVBoxLayout(dialog);
    dl->setContentsMargins(12, 12, 12, 12);
    dl->setSpacing(6);

    dl->addWidget(new QLabel(QString::fromUtf8("类型 (DOMAIN-SUFFIX / IP-CIDR / MATCH ...)"), dialog));
    auto *typeEd = new QLineEdit(cur.value("type").toString(), dialog);
    typeEd->setPlaceholderText("DOMAIN-SUFFIX");
    dl->addWidget(typeEd);

    dl->addWidget(new QLabel(QString::fromUtf8("值 (域名 / IP 段；MATCH 留空)"), dialog));
    auto *valueEd = new QLineEdit(cur.value("value").toString(), dialog);
    dl->addWidget(valueEd);

    dl->addWidget(new QLabel(QString::fromUtf8("节点/策略组"), dialog));
    auto *nodeBox = new QComboBox(dialog);
    nodeBox->setEditable(true);
    nodeBox->addItems(proxyGroupNames());
    nodeBox->setCurrentText(cur.value("node").toString());
    dl->addWidget(nodeBox);
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
    connect(ok, &QPushButton::clicked, dialog, [this, dialog, section, editIndex, typeEd, valueEd, nodeBox] {
        const QString type = typeEd->text().trimmed();
        const QString node = nodeBox->currentText().trimmed();
        if (type.isEmpty() || node.isEmpty()) {
            return; // 类型/节点必填
        }
        QJsonObject obj; // 不含 raw → 回写按 3 段（或 MATCH 2 段）重新序列化
        obj["type"] = type;
        obj["node"] = node;
        obj["value"] = valueEd->text().trimmed();
        QJsonArray current = loadRuleSection(section);
        if (editIndex >= 0 && editIndex < current.size()) {
            current[editIndex] = obj;
        } else {
            current.prepend(obj); // 新增前插（对齐旧项目 def.rules.unshift）
        }
        saveRuleSection(section, current);
        reloadRuleTable(section);
        if (m_core) {
            m_core->rebuildConfig();
        }
        appendLog(QString::fromUtf8("已保存一条规则并重建配置"));
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

    auto addSwitch = [&](const QString &text, QWidget **dot, auto slot) {
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
    // 数值宽度不参与卡片最小宽度计算，避免长短数值互相挤压导致抖动
    (*valueLabel)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    // 对齐旧项目 connections.vue：卡片列表（非表格），顶部 Online/Offline 切换 + 搜索，
    // 每张卡片显示 [type] host + 代理链/下载/上传 徽标 + 删除按钮；断开的连接保留为灰色「离线」。
    const bool light = (m_theme == "white");
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QString::fromUtf8("连接"));
    dialog->setStyleSheet(styleSheet());
    dialog->resize(720, 480);
    auto *v = new QVBoxLayout(dialog);
    v->setContentsMargins(5, 5, 5, 5);
    v->setSpacing(5);

    // --- 顶部工具栏：Online/Offline 切换按钮组 + Search 输入 ---
    auto *form = new QHBoxLayout();
    form->setContentsMargins(5, 0, 5, 0);
    form->setSpacing(0);
    auto *onlineBtn = new QPushButton(QString::fromUtf8("Online (0)"), dialog);
    auto *offlineBtn = new QPushButton(QString::fromUtf8("Offline (0)"), dialog);
    for (QPushButton *b : {onlineBtn, offlineBtn}) {
        b->setCheckable(true);
        b->setChecked(true);
        b->setCursor(Qt::PointingHandCursor);
    }
    onlineBtn->setStyleSheet("QPushButton{background:#909399;color:#fff;border:0;min-height:26px;padding:0 12px;font-size:12px;border-radius:3px 0 0 3px;} QPushButton:checked{background:#4898f8;}");
    offlineBtn->setStyleSheet("QPushButton{background:#909399;color:#fff;border:0;min-height:26px;padding:0 12px;font-size:12px;border-radius:0 3px 3px 0;} QPushButton:checked{background:#4898f8;}");
    form->addWidget(onlineBtn);
    form->addWidget(offlineBtn);
    form->addSpacing(10);

    auto *prepend = new QLabel(QString::fromUtf8("Search"), dialog);
    prepend->setAlignment(Qt::AlignCenter);
    prepend->setStyleSheet(light
        ? "background:#eaeaea;color:#333;border:1px solid #ccc;border-right:0;border-radius:3px 0 0 3px;padding:0 10px;min-height:26px;"
        : "background:#444;color:#fff;border:1px solid #333;border-right:0;border-radius:3px 0 0 3px;padding:0 10px;min-height:26px;");
    auto *search = new QLineEdit(dialog);
    search->setStyleSheet(light
        ? "background:#eaeaea;color:#333;border:1px solid #ccc;border-radius:0 3px 3px 0;min-height:26px;padding:0 8px;"
        : "background:#444;color:#fff;border:1px solid #333;border-radius:0 3px 3px 0;min-height:26px;padding:0 8px;");
    form->addWidget(prepend);
    form->addWidget(search, 1);
    v->addLayout(form);

    // --- 可滚动卡片列表（复用 subScroll/subListBody 的主题背景）---
    auto *scroll = new QScrollArea(dialog);
    scroll->setObjectName("subScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *listBody = new QFrame(scroll);
    listBody->setObjectName("subListBody");
    auto *listLayout = new QVBoxLayout(listBody);
    listLayout->setContentsMargins(5, 5, 5, 5);
    listLayout->setSpacing(8);
    scroll->setWidget(listBody);
    v->addWidget(scroll, 1);

    QPointer<QDialog> guard = dialog;
    // 累积的连接列表（保留离线项，模拟旧项目按 id 合并 + 离线标记 + 下载增量排序）
    auto acc = std::make_shared<QList<QJsonObject>>();
    auto reloadFn = std::make_shared<std::function<void()>>();

    // 速度/流量格式（旧项目 connections 无空格：1.50KB）
    auto spd = [](qint64 value) -> QString {
        double n = value < 0 ? 0 : static_cast<double>(value);
        const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        int i = 0;
        while (n >= 1024.0 && i < 4) { n /= 1024.0; ++i; }
        return QString::number(n, 'f', 2) + units[i];
    };
    // 小徽标：iconfont 图标 + 文本，固定配色（与旧项目一致，不随主题变化）
    auto makeBadge = [](QChar glyph, const QString &text, const QString &bg, const QString &fg) -> QWidget * {
        auto *badge = new QFrame();
        badge->setStyleSheet(QString("QFrame{background:%1;border-radius:5px;}").arg(bg));
        auto *h = new QHBoxLayout(badge);
        h->setContentsMargins(6, 3, 6, 3);
        h->setSpacing(4);
        auto *ico = new QLabel(glyph, badge);
        ico->setStyleSheet(QString("font-family:'iconfont';color:%1;font-size:12px;background:transparent;").arg(fg));
        auto *t = new QLabel(text, badge);
        t->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;").arg(fg));
        h->addWidget(ico);
        h->addWidget(t);
        return badge;
    };
    // 单张连接卡片
    auto makeCard = [this, dialog, light, spd, makeBadge](const QJsonObject &c) -> QFrame * {
        const QJsonObject meta = c.value("metadata").toObject();
        const bool offline = c.value("offline").toBool();
        QString host = meta.value("host").toString();
        if (host.isEmpty()) {
            host = meta.value("destinationIP").toString();
        }
        QString type = meta.value("type").toString();
        if (type.isEmpty()) {
            type = meta.value("network").toString();
        }
        QStringList chains;
        for (const QJsonValue &ch : c.value("chains").toArray()) {
            chains << ch.toString();
        }
        const QString chain0 = chains.isEmpty() ? QStringLiteral("-") : chains.first();
        const qint64 dl = c.value("download").toInteger();
        const qint64 ul = c.value("upload").toInteger();

        auto *card = new QFrame();
        card->setObjectName("subCard");
        auto *h = new QHBoxLayout(card);
        h->setContentsMargins(10, 6, 10, 6);
        h->setSpacing(10);

        auto *dot = new QLabel(QString::fromUtf8("●"), card);
        dot->setStyleSheet(offline ? "color:#999;font-size:10px;background:transparent;"
                                   : "color:#67c23a;font-size:10px;background:transparent;");
        h->addWidget(dot);

        auto *title = new QLabel(QString("[%1] %2").arg(type, host), card);
        title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        title->setStyleSheet(offline
            ? "color:#999;font-size:14px;background:transparent;"
            : (light ? "color:#333;font-size:14px;background:transparent;"
                     : "color:#eee;font-size:14px;background:transparent;"));
        h->addWidget(title, 1);

        // 代理链（深底白字）/ 下载（绿底 #333）/ 上传（红底 #333）——配色对齐旧项目
        h->addWidget(makeBadge(QChar(0xE6BC), chain0, "rgba(0,0,0,0.35)", "#fff"));
        h->addWidget(makeBadge(QChar(0xE6CD), dl ? spd(dl) : QStringLiteral("-"), "rgba(0,255,0,0.5)", "#333"));
        h->addWidget(makeBadge(QChar(0xE6CC), ul ? spd(ul) : QStringLiteral("-"), "rgba(255,0,0,0.5)", "#333"));

        auto *del = new QPushButton(QString::fromUtf8("✕"), card);
        del->setFixedSize(30, 30);
        del->setEnabled(!offline);
        del->setCursor(Qt::PointingHandCursor);
        del->setStyleSheet(light
            ? "QPushButton{background:#eee;color:#f56c6c;border:1px solid #ddd;border-radius:3px;font-size:12px;} QPushButton:hover{background:#f56c6c;color:#fff;} QPushButton:disabled{color:#ccc;background:#f5f5f5;}"
            : "QPushButton{background:#333;color:#f56c6c;border:0;border-radius:3px;font-size:12px;} QPushButton:hover{background:#f56c6c;color:#fff;} QPushButton:disabled{color:#666;background:#2a2a2a;}");
        const QString id = c.value("id").toString();
        // 删除后靠 1s 自动轮询刷新列表（对齐旧项目 closeProcess）
        connect(del, &QPushButton::clicked, dialog, [this, id] {
            if (!id.isEmpty()) {
                m_service.closeConnection(id);
            }
        });
        h->addWidget(del);
        return card;
    };

    // 根据切换/搜索状态重建卡片列表，并更新按钮上的计数
    auto buildList = [guard, acc, listBody, listLayout, onlineBtn, offlineBtn, search, makeCard]() {
        if (!guard) {
            return;
        }
        listBody->setUpdatesEnabled(false);
        while (QLayoutItem *item = listLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        const bool showOnline = onlineBtn->isChecked();
        const bool showOffline = offlineBtn->isChecked();
        const QString f = search->text().trimmed();
        int onlineCount = 0, offlineCount = 0;
        for (const QJsonObject &c : std::as_const(*acc)) {
            const bool off = c.value("offline").toBool();
            if (off) { ++offlineCount; } else { ++onlineCount; }
            if (off && !showOffline) { continue; }
            if (!off && !showOnline) { continue; }
            const QJsonObject meta = c.value("metadata").toObject();
            QString host = meta.value("host").toString();
            if (host.isEmpty()) {
                host = meta.value("destinationIP").toString();
            }
            if (!f.isEmpty() && !host.contains(f, Qt::CaseInsensitive)) {
                continue;
            }
            listLayout->addWidget(makeCard(c));
        }
        listLayout->addStretch();
        listBody->setUpdatesEnabled(true);
        onlineBtn->setText(QString::fromUtf8("Online (%1)").arg(onlineCount));
        offlineBtn->setText(QString::fromUtf8("Offline (%1)").arg(offlineCount));
    };

    // 拉取连接：按 id 合并进 acc（新增/更新在线，缺失标记离线），按下载增量降序
    *reloadFn = [this, guard, acc, buildList]() {
        m_service.fetchConnections([guard, acc, buildList](QJsonArray conns) {
            if (!guard) {
                return;
            }
            for (QJsonObject &o : *acc) {
                o.insert("offline", true);
            }
            for (const QJsonValue &cv : conns) {
                QJsonObject c = cv.toObject();
                const QString id = c.value("id").toString();
                int idx = -1;
                for (int i = 0; i < acc->size(); ++i) {
                    if (acc->at(i).value("id").toString() == id) { idx = i; break; }
                }
                if (idx >= 0) {
                    const double prevDl = acc->at(idx).value("download").toDouble();
                    c.insert("sort", c.value("download").toDouble() - prevDl);
                    c.insert("offline", false);
                    (*acc)[idx] = c;
                } else {
                    c.insert("sort", 0);
                    c.insert("offline", false);
                    acc->append(c);
                }
            }
            std::sort(acc->begin(), acc->end(), [](const QJsonObject &a, const QJsonObject &b) {
                return a.value("sort").toDouble() > b.value("sort").toDouble();
            });
            buildList();
        });
    };

    // 切换按钮：不允许两个都关（对齐旧项目 setOnline/setOffline）
    connect(onlineBtn, &QPushButton::clicked, dialog, [onlineBtn, offlineBtn, buildList] {
        if (!onlineBtn->isChecked() && !offlineBtn->isChecked()) {
            offlineBtn->setChecked(true);
        }
        buildList();
    });
    connect(offlineBtn, &QPushButton::clicked, dialog, [onlineBtn, offlineBtn, buildList] {
        if (!offlineBtn->isChecked() && !onlineBtn->isChecked()) {
            onlineBtn->setChecked(true);
        }
        buildList();
    });
    connect(search, &QLineEdit::textChanged, dialog, [buildList] { buildList(); });

    // 实时刷新（对齐旧项目 1s 轮询）
    auto *autoTimer = new QTimer(dialog);
    autoTimer->setInterval(1000);
    connect(autoTimer, &QTimer::timeout, dialog, [reloadFn] { (*reloadFn)(); });
    autoTimer->start();

    (*reloadFn)();
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

QWidget *MainWindow::createSwitchDot(bool enabled)
{
    // 自绘抗锯齿呼吸圆点（旧项目 .quan.on 的 1s 呼吸动画）
    auto *dot = new BreathingDot(this);
    dot->setLight(m_theme == "white");
    dot->setOn(enabled);
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
    applyTitleBarColor(); // 窗口显示后给系统标题栏着色（需窗口已实体化）
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    // 系统标题栏/边框由系统负责缩放与移动，这里不再拦截 WM_NCHITTEST
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

QString MainWindow::extractCoreBinary(const QString &archivePath, const QString &tmpDir)
{
    const QString outDir = QDir(tmpDir).filePath("extract");
    QDir(outDir).removeRecursively();
    QDir().mkpath(outDir);
#if defined(Q_OS_WIN)
    // .zip：优先用系统自带 tar.exe（Win10 1803+/Win11），失败回退 PowerShell Expand-Archive
    int code = QProcess::execute("tar", {"-xf", archivePath, "-C", outDir});
    if (code != 0) {
        code = QProcess::execute("powershell", {"-NoProfile", "-Command",
            QString("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force").arg(archivePath, outDir)});
    }
    if (code != 0) {
        return QString();
    }
    QDirIterator it(outDir, {"*.exe"}, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
#else
    // .gz：单成员 gzip，用 gzip -dc 解到目标文件
    const QString outBin = QDir(outDir).filePath("mihomo");
    QProcess gz;
    gz.setStandardOutputFile(outBin);
    gz.start("gzip", {"-dc", archivePath});
    if (!gz.waitForFinished(30000) || gz.exitCode() != 0) {
        return QString();
    }
    return QFileInfo(outBin).size() > 0 ? outBin : QString();
#endif
}

void MainWindow::updateMihomoCore(QPushButton *btn)
{
    btn->setEnabled(false);
    btn->setText(QString::fromUtf8("检查中..."));
    const AppConfig cfg = AppConfigLoader::load();
    auto restore = [btn] {
        btn->setEnabled(true);
        btn->setText(QString::fromUtf8("更新内核"));
    };

    auto *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl("https://api.github.com/repos/MetaCubeX/mihomo/releases/latest"));
    req.setRawHeader("User-Agent", "clashauto-cpp");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, btn, cfg, restore] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray body = reply->readAll();
        const QString err = reply->errorString();
        reply->deleteLater();
        if (!ok) {
            nam->deleteLater();
            restore();
            appendLog(QString::fromUtf8("内核更新失败: 查询版本出错 %1").arg(err));
            return;
        }
        const QJsonObject rel = QJsonDocument::fromJson(body).object();
        const QString tag = rel.value("tag_name").toString();

#if defined(Q_OS_WIN)
        const QString os = QStringLiteral("windows");
        const QString ext = QStringLiteral(".zip");
#elif defined(Q_OS_MACOS)
        const QString os = QStringLiteral("darwin");
        const QString ext = QStringLiteral(".gz");
#else
        const QString os = QStringLiteral("linux");
        const QString ext = QStringLiteral(".gz");
#endif
        const bool isArm = QSysInfo::currentCpuArchitecture().contains("arm");
        const QString arch = isArm ? QStringLiteral("arm64") : QStringLiteral("amd64");
        const QJsonArray assets = rel.value("assets").toArray();
        // amd64 优先用 compatible 构建：默认/微架构版针对较新 CPU 指令集，在虚拟机或老 CPU
        // （如 QEMU Virtual CPU）上启动即 0xC0000005 崩溃；compatible 版兼容所有 amd64。
        // arm64 无微架构分级，直接用标准资源名。按优先级精确匹配（不能 startsWith，避免命中 v1/v2/v3/goNNN 变体）。
        QStringList candidates;
        if (!isArm) {
            candidates << QString("mihomo-%1-amd64-compatible-%2%3").arg(os, tag, ext);
        }
        candidates << QString("mihomo-%1-%2-%3%4").arg(os, arch, tag, ext);
        QString url, assetName;
        for (const QString &want : candidates) {
            for (const QJsonValue &av : assets) {
                if (av.toObject().value("name").toString() == want) {
                    url = av.toObject().value("browser_download_url").toString();
                    assetName = want;
                    break;
                }
            }
            if (!url.isEmpty()) {
                break;
            }
        }
        if (url.isEmpty()) {
            // 兜底：amd64 取 compatible-vX.Y…，arm64 取基线 vX.Y…（排除 v1/v2/v3 微架构与 goNNN 变体）
            const QString pat = isArm ? QString("^mihomo-%1-arm64-v\\d+\\.\\d").arg(os)
                                      : QString("^mihomo-%1-amd64-compatible-v\\d+\\.\\d").arg(os);
            const QRegularExpression re(pat);
            for (const QJsonValue &av : assets) {
                const QString n = av.toObject().value("name").toString();
                if (re.match(n).hasMatch() && n.endsWith(ext)) {
                    url = av.toObject().value("browser_download_url").toString();
                    assetName = n;
                    break;
                }
            }
        }
        if (url.isEmpty()) {
            nam->deleteLater();
            restore();
            appendLog(QString::fromUtf8("内核更新失败: 未找到匹配的 mihomo 资源 (%1/%2)").arg(os, arch));
            return;
        }

        appendLog(QString::fromUtf8("下载 mihomo %1 ...").arg(tag));
        btn->setText(QString::fromUtf8("下载中..."));
        QNetworkRequest dreq{QUrl(url)};
        dreq.setRawHeader("User-Agent", "clashauto-cpp");
        dreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *dl = nam->get(dreq);
        connect(dl, &QNetworkReply::downloadProgress, btn, [btn](qint64 r, qint64 t) {
            if (t > 0) {
                btn->setText(QString::fromUtf8("下载中 %1%").arg(int(r * 100 / t)));
            }
        });
        connect(dl, &QNetworkReply::finished, this, [this, dl, nam, btn, cfg, tag, assetName, restore] {
            const bool ok2 = dl->error() == QNetworkReply::NoError;
            const QByteArray data = dl->readAll();
            const QString err2 = dl->errorString();
            dl->deleteLater();
            nam->deleteLater();
            if (!ok2 || data.isEmpty()) {
                restore();
                appendLog(QString::fromUtf8("内核下载失败: %1").arg(err2));
                return;
            }
            const QString tmpDir = QDir(cfg.userDir).filePath("core-update");
            QDir().mkpath(tmpDir);
            const QString archivePath = QDir(tmpDir).filePath(assetName);
            QFile f(archivePath);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                restore();
                appendLog(QString::fromUtf8("内核更新失败: 无法写入临时文件"));
                return;
            }
            f.write(data);
            f.close();

            btn->setText(QString::fromUtf8("安装中..."));
            const QString extracted = extractCoreBinary(archivePath, tmpDir);
            if (extracted.isEmpty()) {
                QDir(tmpDir).removeRecursively();
                restore();
                appendLog(QString::fromUtf8("内核更新失败: 解压失败（缺少 tar/gzip 或压缩包异常）"));
                return;
            }

            // 停核心 → 替换二进制（保留原文件名）→ 恢复运行
            const QString target = cfg.clashExecutable();
            const bool wasRunning = m_core && m_core->isRunning();
            if (wasRunning) {
                m_core->stopCore();
            }
            QDir().mkpath(QFileInfo(target).absolutePath());
            bool replaced = false;
            for (int attempt = 0; attempt < 8 && !replaced; ++attempt) {
                QFile::remove(target);
                if (QFile::copy(extracted, target)) {
                    replaced = true;
                } else {
                    QThread::msleep(150); // Windows 下核心退出后句柄释放可能有短暂延迟
                }
            }
#if !defined(Q_OS_WIN)
            if (replaced) {
                QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                                  | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                                  | QFileDevice::ReadOther | QFileDevice::ExeOther);
            }
#endif
            QDir(tmpDir).removeRecursively();
            if (wasRunning) {
                m_core->startCore();
            }
            restore();
            if (replaced) {
                appendLog(QString::fromUtf8("内核已更新到 mihomo %1%2").arg(tag, wasRunning ? QString::fromUtf8("，已重启核心") : QString()));
            } else {
                appendLog(QString::fromUtf8("内核更新失败: 无法替换文件（可能被占用）"));
            }
        });
    });
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
    // 重建期间关闭重绘，整批完成后一次性刷新，避免列表清空→重填的闪烁
    m_nodeList->setUpdatesEnabled(false);
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
    m_nodeList->setUpdatesEnabled(true);

    if (m_nodeTitle) {
        m_nodeTitle->setText(QString::fromUtf8("节点 <span style='font-size:9px'>(%1/%2)</span>")
                                 .arg(shown)
                                 .arg(m_currentNodes.size()));
    }
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    // 系统标题栏之外，允许按住侧栏空白处拖拽移动窗口
    // （菜单按钮自行消费点击，不会触发拖拽；用 pos() 与 move() 配对，避免带框窗口跳变）
    if (event->button() == Qt::LeftButton && event->pos().x() < SidebarWidth) {
        m_dragging = true;
        m_dragStart = event->globalPosition().toPoint() - pos();
        event->accept();
        return;
    }
    QMainWindow::mousePressEvent(event);
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
        #root { background:#242425; }
        #titleBar { background:#222; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#ccc; font-size:12px; padding-left:0; }
        #titleButton, #closeButton { color:#ccc; background:#222; border:0; border-radius:0; }
        #titleButton:hover { background:#333; }
        #closeButton:hover { background:red; color:white; }
        #body, #page { background:rgba(0,0,0,0); }
        #rightPane { background:#242425; }
        #sidebar { background:#222; }
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
        #metricValue { color:#bfbfbf; font-size:18px; }
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
        #footer { background:#303032; }
        #usersBadge { color:#fff; background:#000; border-radius:3px; padding:3px 5px; font-size:12px; }
        #footerArrow { color:#666; font-size:14px; font-family:'iconfont'; }
        #footerLog { color:#ccc; font-size:12px; }
        #switchButton { color:#eee; background:#000; border:0; border-radius:3px; min-height:28px; padding-left:32px; padding-right:8px; font-size:12px; }
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
        #root { background:#ffffff; }
        #titleBar { background:#eee; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#333; font-size:12px; }
        #titleButton, #closeButton { color:#333; background:#eee; border:0; border-radius:0; }
        #titleButton:hover { background:#fff; }
        #closeButton:hover { background:red; color:white; }
        #body, #page { background:rgba(0,0,0,0); }
        #rightPane { background:#ffffff; }
        #sidebar { background:#eee; }
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
        #metricValue { color:#3d3d3d; font-size:18px; }
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
        #footer { background:#fafafa; }
        #usersBadge { color:#333; background:#fff; border-radius:3px; padding:3px 5px; font-size:12px; }
        #footerArrow { color:#e8e8e8; font-size:14px; font-family:'iconfont'; }
        #footerLog { color:#333; font-size:12px; }
        #switchButton { color:#333; background:#eee; border:0; border-radius:3px; min-height:28px; padding-left:32px; padding-right:8px; font-size:12px; }
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
    // 呼吸圆点是自绘的，随主题切换刷新其颜色（关/浅色白、关/深色灰、开=蓝）
    for (QWidget *dot : {m_tunDot, m_proxyDot, m_coreDot}) {
        if (dot)
            static_cast<BreathingDot *>(dot)->setLight(light);
    }
    applyTitleBarColor(); // 主题变化时重新给系统标题栏着色
}

void MainWindow::applyTitleBarColor()
{
#if defined(Q_OS_WIN)
    // 通过 DWM 让系统原生标题栏与自绘标题栏同色（Windows 11 22000+ 支持）。
    // 属性值直接用字面量，避免旧 SDK 头文件未定义 DWMWA_CAPTION_COLOR/TEXT_COLOR。
    constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    constexpr DWORD DWMWA_CAPTION_COLOR = 35;
    constexpr DWORD DWMWA_TEXT_COLOR = 36;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const bool light = (m_theme == "white");
    // 深色标题（#222）需要浅色的最小化/关闭按钮图标 -> 开启沉浸式深色模式
    const BOOL dark = light ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // 标题栏背景色，与自绘标题栏一致：浅色 #eee / 深色 #222（COLORREF = 0x00BBGGRR）
    const COLORREF caption = light ? RGB(0xEE, 0xEE, 0xEE) : RGB(0x22, 0x22, 0x22);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    // 标题文字颜色
    const COLORREF text = light ? RGB(0x33, 0x33, 0x33) : RGB(0xEE, 0xEE, 0xEE);
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
#endif
}
