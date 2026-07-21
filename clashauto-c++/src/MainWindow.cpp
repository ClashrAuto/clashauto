#include "MainWindow.h"
#if defined(Q_OS_MACOS)
#include "MacHelperClient.h" // 开启增强(TUN)时确保特权 helper 已就绪并以 root 起核心
#include "MacWindow.h"       // 系统标题栏透明 + 内容铺满整窗 + 整窗毛玻璃（NSVisualEffectView）
#endif

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
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
#include <QFontMetrics>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
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
#include <QSet>
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
#include <QTabBar> // macOS：给各 QTabBar 装事件过滤器，Show/Hide/Move 后恢复毛玻璃（见 eventFilter）
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
#include <QTextStream>
#include <QDateTime>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWindow>

#include <functional>
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
#include <shellapi.h> // ShellExecuteEx（runas 提权重启）
#include <tlhelp32.h> // CreateToolhelp32Snapshot（枚举运行中的进程，供 PROCESS 规则值下拉）
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
// 最小尺寸留出缩小余量：旧项目 minWidth 锁死 900，窄屏（小笔记本/虚拟机）上窗口怎么拖都缩不小。
// 640 高于各页面布局最小宽度（约束最紧的是设置「过滤」tab：140 标签 + 300 正则下拉 ≈ 610）。
constexpr int MainMinWidth = 640;
constexpr int MainMinHeight = 430;

// UI 尺寸对齐旧项目实测值（见 docs/legacy-ui-spec.md）
constexpr int MenuSpacing = 5;    // 菜单项 margin-bottom
constexpr int MetricGutter = 10;  // 流量卡网格 gutter（旧值 16）

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

// 自适应宽度的名称标签：随列宽伸缩，超出可用宽度时右侧用「…」省略；
// 完整名称放进 tooltip 便于悬停查看（节点名很长时——如订阅名是整条 URL）。
// 省略在 paintEvent 里现算，绝不 setText：QLabel::setText 会 updateGeometry 使整个节点列表的
// 布局缓存失效并全表重算 sizeHint，窗口每拖一步所有长名行都触发一次——正是缩放卡顿的主因。
class ElidingLabel : public QLabel
{
public:
    explicit ElidingLabel(QWidget *parent = nullptr) : QLabel(parent) {}
    void setFullText(const QString &text)
    {
        if (m_full == text) {
            return;
        }
        m_full = text;
        setToolTip(text);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(PadLeft, 0, 0, 0);
        p.setPen(palette().color(foregroundRole())); // QSS #nodeName 的 color 经 polish 落在调色板上
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter,
                   fontMetrics().elidedText(m_full, Qt::ElideRight, r.width()));
    }

private:
    // 自绘不再走 QLabel 的样式表文本渲染，QSS #nodeName 的 padding-left:5 在此手动对齐
    static constexpr int PadLeft = 5;
    QString m_full;
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
                                 : (m_light ? QColor(0xc0, 0xc0, 0xc0) : QColor(0x66, 0x66, 0x66));
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

#if defined(Q_OS_MACOS)
namespace {
// 悬浮滚动条（仅 macOS，经 installOverlayScrollBar 安装）：把滚动区自带的竖滚动条设为
// AlwaysOff（不再参与布局、不占宽——它仍是滚动「模型」，滚轮/键盘/程序滚动都驱动它），
// 另建一根代理 QScrollBar 浮在滚动区右缘之上，与内部条双向同步值/范围。
// 外观不变：代理条仍被全局样式表的 QScrollBar 规则渲染（8px 透明轨道 + 圆角滑块）。
// 行为像 mac 原生 transient：默认隐藏，滚动（值变化或滚轮）时浮现，闲置 ~1.2s 自动隐藏；
// 内容未溢出时始终不显示。生命周期挂在滚动区上（parent），无需外部管理。
class OverlayScrollBar : public QObject
{
public:
    explicit OverlayScrollBar(QAbstractScrollArea *area)
        : QObject(area), m_area(area),
          m_bar(new QScrollBar(Qt::Vertical, area)),
          m_hideTimer(new QTimer(this))
    {
        QScrollBar *src = area->verticalScrollBar();
        area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        m_bar->hide(); // 默认收起，滚动时才浮现
        m_bar->setRange(src->minimum(), src->maximum());
        m_bar->setPageStep(src->pageStep());
        m_bar->setSingleStep(src->singleStep());
        m_bar->setValue(src->value());

        m_hideTimer->setSingleShot(true);
        m_hideTimer->setInterval(1200);

        connect(src, &QScrollBar::rangeChanged, this, [this, src](int min, int max) {
            m_bar->setRange(min, max);
            m_bar->setPageStep(src->pageStep());
            m_bar->setSingleStep(src->singleStep());
            if (max <= min)
                m_bar->hide(); // 内容不再溢出：立即收起
        });
        connect(src, &QScrollBar::valueChanged, this, [this](int value) {
            m_bar->setValue(value); // 同值 setValue 为空操作，双向同步不会递归
            flash();
        });
        connect(m_bar, &QScrollBar::valueChanged, src, &QScrollBar::setValue);
        connect(m_hideTimer, &QTimer::timeout, this, [this] {
            // 正拖滑块或悬停其上时不收起，续一轮计时
            if (m_bar->isSliderDown() || m_bar->underMouse())
                m_hideTimer->start();
            else
                m_bar->hide();
        });

        area->installEventFilter(this);             // Resize/Show 重新贴右缘
        area->viewport()->installEventFilter(this); // 滚轮即浮现（含已到顶/底、值不变时）
        reposition();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (watched == m_area && (type == QEvent::Resize || type == QEvent::Show))
            reposition();
        else if (watched == m_area->viewport() && type == QEvent::Wheel)
            flash();
        return QObject::eventFilter(watched, event);
    }

private:
    void reposition()
    {
        const int w = 8; // 与样式表 QScrollBar:vertical { width:8px } 一致
        m_bar->setGeometry(m_area->width() - w, 0, w, m_area->height());
        m_bar->raise();
    }
    void flash()
    {
        if (m_bar->maximum() <= m_bar->minimum())
            return; // 内容未溢出：不显示
        m_bar->show();
        m_bar->raise();
        m_hideTimer->start();
    }

    QAbstractScrollArea *m_area;
    QScrollBar *m_bar;
    QTimer *m_hideTimer;
};
} // namespace
#endif // Q_OS_MACOS

// 给滚动区装悬浮滚动条（仅 macOS 生效）：竖滚动条浮在内容右缘、不占布局宽度，列表行
// 距卡片右缘恒为 10px（长短表一致）；滚动时浮现、闲置自动隐藏，外观仍是 8px 细条样式。
// Win/Linux 不装：保持常驻、占宽的样式滚动条（无系统级悬浮滚动条惯例）。
static void installOverlayScrollBar(QAbstractScrollArea *area)
{
#if defined(Q_OS_MACOS)
    new OverlayScrollBar(area); // parent 挂在 area 上，随其销毁
#else
    Q_UNUSED(area);
#endif
}

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

// 同步执行子进程且不弹控制台窗口（GUI 子系统下需 CREATE_NO_WINDOW）
int runHidden(const QString &program, const QStringList &args, int timeoutMs = 30000)
{
    QProcess p;
#if defined(Q_OS_WIN)
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
        a->flags |= 0x08000000u; // CREATE_NO_WINDOW
    });
#endif
    p.start(program, args);
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        return -1;
    }
    return p.exitCode();
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
    // 常开句柄 + 每条 flush：之前每条日志都 open/append/close 一次文件，
    // mihomo 的连接日志逐条转发进来，在 QEMU 虚拟机上文件打开开销明显
    m_logFile.setFileName(m_logFilePath);
    m_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    m_core = new CoreController(config, this);
    m_tray = new TrayController(this, this);
    m_subscriptions = new SubscriptionStore(config, this);
    registerUrlScheme();
    m_service.setEndpoint(config.host, config.uiPort); // 让轮询/切换走配置里的 API 端口（默认 9191）
    m_service.setMixedPort(config.mixedPort);          // 下载测速经此混合端口走代理
    m_service.setSecret(config.secret);                // 核心开启鉴权时，轮询/切换请求带上 secret
    m_proxyHost = config.host;                         // 下载走代理用的主机/端口（applyDownloadProxy）
    m_proxyMixedPort = config.mixedPort;
    m_service.setClearConnectionsOnSwitch(config.clearConnections);
    // 切换加载态的转圈动画：只更新那个目标按钮的文字，不整表重建
    m_spinnerTimer = new QTimer(this);
    connect(m_spinnerTimer, &QTimer::timeout, this, [this] {
        static const char *frames[] = {"◐", "◓", "◑", "◒"}; // ◐◓◑◒
        m_spinnerFrame = (m_spinnerFrame + 1) % 4;
        if (m_spinnerButton) {
            m_spinnerButton->setText(QString::fromUtf8(frames[m_spinnerFrame]));
        }
    });
    m_closeToTray = config.closeToTray;
    m_nodeSwitchNote = config.nodeSwitchNote;
    m_nodeOnlyAvailable = config.nodeOnlyAvailable;
    m_mirror = config.mirror;
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
    setMinimumSize(MainMinWidth, MainMinHeight); // 默认仍开 900x510，但允许拖小
    // 恢复上次窗口位置/大小（对应旧项目 config.bounds）
    const QByteArray savedGeometry = QSettings().value("window/geometry").toByteArray();
    if (!savedGeometry.isEmpty()) {
        restoreGeometry(savedGeometry);
    }

    auto *root = new QFrame(this);
    root->setObjectName("root");
#if defined(Q_OS_MACOS)
    // 标题栏已做成透明 + FullSizeContentView（内容铺满整窗）。但 Qt 默认会把 macOS 安全区
    // （标题栏高度）当内边距加到控件上，导致内容仍被顶到标题栏下方、「没超过标题栏」。
    // 主窗口 + 中央控件都关掉这个属性，内容才真正铺到窗口顶端。
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    root->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    // 整窗毛玻璃（enableMacBlur 在窗底垫 NSVisualEffectView）：顶层窗口开半透明，
    // backing store 以全透明清底，凡样式表里是 transparent 的区域（root/侧栏/页脚）都露出
    // 玻璃；主内容 #rightPane 仍是不透明纯色卡片（见 appStyle/lightStyle + applyTheme 的
    // mac 覆写）。必须在原生窗创建前设置——下方 applyTheme → applyTitleBarColor 会调
    // winId() 触发创建。仅影响 macOS，Windows/Linux 不走此属性。
    setAttribute(Qt::WA_TranslucentBackground, true);
#endif
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
#if defined(Q_OS_MACOS)
    // 毛玻璃下让主内容（及页脚）离窗口上、右边缘各留 5px，透出玻璃、像浮起的圆角卡片。
    // 左边贴侧栏、下边贴窗口底，故只给 top/right = 5。
    rightColLayout->setContentsMargins(0, 5, 5, 0);
#else
    rightColLayout->setContentsMargins(0, 0, 0, 0);
#endif
    rightColLayout->setSpacing(0);

    auto *right = new QFrame(rightColumn);
    right->setObjectName("rightPane");
    auto *rightLayout = new QVBoxLayout(right);
    // 四周常规 10px 内边距。mac 上各列表滚动条是自绘悬浮式（OverlayScrollBar：8px 细条
    // 浮在内容右缘、不占宽、闲置自动隐藏），列表行右缘无论长短表都恰好离卡片右边 10px；
    // Win/Linux 保留常驻占宽的 8px 样式滚动条（溢出时行右缘距边 10+8=18px，维持旧观感）。
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

#if defined(Q_OS_MACOS)
    // 毛玻璃保活：QTabBar（设置页 settingsTabs / 日志页 logsTabs）每次 Show/Hide/Move 都会
    // 触发 Qt Cocoa 的 applyContentBorderThickness，无条件把 NSWindow.titlebarAppearsTransparent
    // 置回 NO——切到带 tab 的页面标题栏立刻变回不透明底、毛玻璃「碎掉」。应用侧无法拦截该
    // 调用（QTabBar 直连平台插件），只能在其后重新断言（见 eventFilter，排队到事件处理之后）。
    // 此时全部页面已构建完，一次性给主窗内所有 QTabBar 装过滤器；独立对话框是另外的
    // NSWindow，不影响主窗，无需处理。
    const auto tabBars = findChildren<QTabBar *>();
    for (QTabBar *tabBar : tabBars) {
        tabBar->installEventFilter(this);
    }
#endif

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
        // 节点「集合」(名字集，与顺序无关) 变了才需要整表重建（增删节点）；活动节点或延迟/速度/次序变都
        // 用 syncNodeRows 原地处理：移动现有行到新次序、改药丸/名称/按钮态，既完成排序又不闪现。
        // 测速/自动测延迟时每秒都在变延迟——正是这里做到「按新延迟重排且不清空列表」。
        auto nameSet = [](const QVector<NodeInfo> &v) {
            QStringList names;
            for (const NodeInfo &n : v) {
                names << n.name;
            }
            names.sort();
            return names;
        };
        const bool setChanged = nameSet(nodes) != nameSet(m_currentNodes);
        const bool anyChange = (nodes != m_currentNodes); // 含次序/延迟/速度/活动/now 任一变化
        m_currentNodes = nodes;
        // 节点切换通知（对应旧项目 config.note）；跳过首次填充，避免启动即误报
        if (m_nodeSwitchNote && m_nodeInitialized && m_tray && !selected.isEmpty() && selected != m_selectedNode) {
            m_tray->notify(QString::fromUtf8("节点切换"), QString::fromUtf8("已切换到 %1").arg(selected));
        }
        m_nodeInitialized = true;
        m_selectedNode = selected;
        // 加载态确认：当前活动节点已不同于点击前 → 切换/禁用完成，解除转圈+禁用（对齐旧项目 find() 轮询）
        if (m_nodeSwitching && !m_nodeSwitchFrom.isEmpty() && selected != m_nodeSwitchFrom) {
            endNodeSwitch(); // 内部原地渲染新的活动节点与可点按钮（不闪现）
            return;
        }
        if (m_nodeSwitching) {
            // 切换进行中（尚未确认）：只原地刷药丸，不重排/不动按钮，避免扰乱转圈与禁用态
            if (anyChange) {
                updateNodeBadges();
            }
            return;
        }
        if (m_inSizeMove) {
            // 窗口正被拖动/缩放（模态 sizing 循环里定时器/网络照常派发）：重排/重建都会全表
            // 重布局，会明显掉帧——先挂起，WM_EXITSIZEMOVE 时一次性补齐
            if (setChanged || anyChange) {
                m_nodeResyncPending = true;
            }
            return;
        }
        if (setChanged) {
            populateNodeList(); // 仅「增删节点」才整表重建（会重新登记各行）
        } else if (anyChange) {
            syncNodeRows();     // 集合不变：原地重排 + 原地改内容，不闪现
        }
    });
    connect(&m_service, &ClashService::proxyGroupsUpdated, this, [this](const QStringList &groups, const QString &selectedGroup) {
        if (!m_nodeGroupSelector) {
            return;
        }
        const QSignalBlocker blocker(m_nodeGroupSelector);
        // 组列表没变就不重建下拉，避免 2s 轮询时打断用户正在展开的下拉/选择
        QStringList current;
        for (int i = 0; i < m_nodeGroupSelector->count(); ++i) {
            current << m_nodeGroupSelector->itemText(i);
        }
        if (current != groups) {
            m_nodeGroupSelector->clear();
            m_nodeGroupSelector->addItems(groups);
        }
        const int index = m_nodeGroupSelector->findText(selectedGroup);
        if (index >= 0 && index != m_nodeGroupSelector->currentIndex()) {
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
    // ClashService 只从 REST /traffic 流推断「核心是否在跑」，并不知道 TUN/系统代理的真实开关，
    // 它每秒发来的是写死的 (tun=false, proxy=true)。若直接采信，会把刚开启的增强(TUN)灯每秒冲掉一次
    // ——这正是「点增强后标志不亮/提权重启后看着像没开增强」的根因。TUN/代理/核心的真值只在
    // CoreController（与旧项目一致：status 由 clash 控制器发，而非 API 轮询）。故这里忽略流里的
    // 布尔值，一律以 CoreController 为准，把每秒一次的轮询当作「自愈式复位」用正确来源刷新灯。
    connect(&m_service, &ClashService::statusUpdated, this, [this, applyStatus](bool, bool, bool) {
        const bool running = m_core->isRunning();
        applyStatus(running && m_core->isTunEnabled(), running && m_core->isProxyEnabled(), running);
    });
    connect(m_core, &CoreController::statusChanged, this, applyStatus);
    connect(m_core, &CoreController::logUpdated, this, [this](const QString &message) {
        appendLog(message);
        appendTimeline(m_clashTimeline, m_clashScroll, message);
    });
    // 未检测到内核（不再预装）→ 弹窗引导用户去设置下载
    connect(m_core, &CoreController::coreMissing, this, [this](const QString &) { promptDownloadCore(); });
    connect(m_tray, &TrayController::toggleCoreRequested, m_core, &CoreController::toggleCore);
    connect(m_tray, &TrayController::toggleProxyRequested, this, &MainWindow::onToggleProxyRequested);
    connect(m_tray, &TrayController::toggleTunRequested, this, &MainWindow::onToggleTunRequested);
    // 「更新所有订阅」会连续触发多次 subscriptionUpdated；把 rebuildConfig（生成 full.yaml + 热重载核心）
    // 经 500ms single-shot 去抖合并——500ms 内的多次变更只重建/热重载一次。
    m_subRebuildTimer = new QTimer(this);
    m_subRebuildTimer->setSingleShot(true);
    connect(m_subRebuildTimer, &QTimer::timeout, this, [this] {
        if (m_core) {
            m_core->rebuildConfig();
        }
    });
    connect(m_subscriptions, &SubscriptionStore::subscriptionUpdated, this,
            [this](int, bool ok, const QString &message, bool changed) {
        appendLog(message);
        reloadSubscriptions();
        // 内容没变就不重建 full.yaml、不热重载核心：自动更新每隔几分钟跑一次，
        // 订阅通常无变化，之前每次都无谓地重载核心（日志里 67 次/18 小时）
        if (ok && changed) {
            m_subRebuildTimer->start(500); // 去抖合并批量更新的多次重建
        }
    });

    m_service.start();

#if defined(Q_OS_WIN)
    const bool tunElevated = QCoreApplication::arguments().contains(QStringLiteral("--tun-elevated"));
    // 由「增强」提权重启而来：等旧实例硬杀核心、释放 9090/系统代理并退出后，带 TUN 冷启动核心。
    if (tunElevated) {
        QTimer::singleShot(1000, this, [this] {
            if (m_core && !m_core->isRunning()) {
                m_core->setTunEnabled(true);
                m_core->startCore();
                appendLog(QString::fromUtf8("已以管理员身份重启，正在开启增强(TUN)模式"));
            }
        });
    }
#else
    const bool tunElevated = false;
#endif

    // 启动即处理内核：有内核就自动拉起核心；首次没内核则直接跳「设置 → 系统」引导下载。
    // （提权开 TUN 的冷启动交给上面的 --tun-elevated 分支，避免这里先无 TUN 起核心把它顶掉。）
    if (m_core && !tunElevated) {
        QTimer::singleShot(600, this, [this] {
            if (!m_core) {
                return;
            }
            if (m_core->isCoreInstalled()) {
#if defined(Q_OS_WIN)
                // 恢复上次退出前的增强(TUN)状态（config 的 use:，每次切换即落盘；一键更新
                // 自动重启后也走这里）。非管理员进程建不了 TUN 网卡 → 先按需提权重启。
                if (m_core->isTunEnabled() && !isProcessElevated()) {
                    appendLog(QString::fromUtf8("正在恢复上次的增强(TUN)模式，需要管理员权限..."));
                    if (relaunchElevatedForTun()) {
                        return; // 提权实例接管，本实例即将退出
                    }
                    // 取消授权/提权失败：本次不带 TUN 启动（use: 已由 relaunch 回滚为 false，
                    // 避免以后每次启动都弹 UAC）
                    m_core->setTunEnabled(false);
                }
#endif
                if (!m_core->isRunning()) {
                    m_core->startCore(); // 有内核：启动时自动开启核心
                }
            } else {
                // 首次没内核：不弹是否前往的对话框，直接跳到设置页下载区并定位「更新内核」
                appendLog(QString::fromUtf8("未检测到 mihomo 内核，请在「设置 → 系统」下载"));
                goToCoreDownload();
            }
        });
    }

    // 更新检查时机：①启动后 3 秒自动检查一次（失败会自动重试）；②之后每 6 小时再查一次，
    // 长时间挂着也能发现新版；③手动点侧栏/关于页的版本号随时检查。
    // 有新版本则：托盘提示 + 侧栏与关于页版本号变红 + 弹更新窗（勾了「不再提示该版本」则不弹窗）。
    QTimer::singleShot(3000, this, [this] { checkForUpdate(true); });
    auto *updateTimer = new QTimer(this);
    connect(updateTimer, &QTimer::timeout, this, [this] { checkForUpdate(true); });
    updateTimer->start(6 * 60 * 60 * 1000); // 6 小时
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

    // 侧栏版本行：平时灰色显示当前版本、点击即检查更新；发现新版本时由 checkForUpdate 改成红色提示。
    // （之前这里是个临时局部量、从不更新，所以「有新版本」提示永远出不来——存成成员即可原地刷新。）
    auto *version = new QLabel(side);
    version->setObjectName("version");
    version->setAlignment(Qt::AlignCenter);
    version->setTextFormat(Qt::RichText);
    version->setText(QString("<a href='update' style='color:#666;text-decoration:none;'>Ver: %1</a>")
                         .arg(QString::fromUtf8(APP_VERSION)));
    version->setToolTip(QString::fromUtf8("点击检查更新"));
    connect(version, &QLabel::linkActivated, this, [this](const QString &) { checkForUpdate(false); });
    m_sidebarVersionLabel = version; // 供 checkForUpdate 发现新版本时改红提示
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
    // 搜索去抖：每个按键都整表重建（~2000 个 widget）会明显卡顿；改为 250ms 后才真正过滤重建。
    m_nodeSearchTimer = new QTimer(this);
    m_nodeSearchTimer->setSingleShot(true);
    connect(m_nodeSearchTimer, &QTimer::timeout, this, [this] { populateNodeList(); });
    connect(m_nodeSearch, &QLineEdit::textChanged, this, [this] { m_nodeSearchTimer->start(250); });
    connect(speedTest, &QPushButton::clicked, this, [this] {
        if (m_speedTesting) {
            return; // 测速进行中：忽略重复点击
        }
        appendLog(QString::fromUtf8("开始测速（先测延迟，再对有效节点测下载速度）..."));
        m_service.refreshNodes();
        m_service.testDelays(/*thenSpeed=*/true); // 延迟测完 → 有效延迟节点逐个测下载速度（并发 5）
    });
    // 测速进行中：按钮转为「⏳」并禁用，结束后复位为「↻」——给用户明确的进行/完成反馈
    connect(&m_service, &ClashService::speedTestRunning, this, [this, speedTest](bool running) {
        m_speedTesting = running;
        speedTest->setEnabled(!running);
        speedTest->setText(running ? QString::fromUtf8("⏳") : QString(QChar(0x21BB)));
        // 测速过程中每次 nodesUpdated 已用 syncNodeRows 原地按速度重排（不闪现），无需在此整表重建。
    });
    connect(helpBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl("https://clashr-auto.gitbook.io"));
    });

    auto *scroll = new QScrollArea(right);
    scroll->setObjectName("nodeScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nodeScroll = scroll; // 保存以便刷新时保留滚动位置
    m_nodeList = new QFrame(scroll);
    m_nodeList->setObjectName("nodeListBody");
    auto *nodeLayout = new QVBoxLayout(m_nodeList);
    nodeLayout->setContentsMargins(0, 0, 0, 0);
    nodeLayout->setSpacing(10);
    scroll->setWidget(m_nodeList);
    installOverlayScrollBar(scroll); // mac：悬浮滚动条，行右缘恒距卡片边 10px
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
    installOverlayScrollBar(scroll); // mac：悬浮滚动条
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
    // 即时生效并落盘：勾选/取消立刻重建状态列表并写盘，不必再点「应用」。
    // 修复「关掉了却还在过滤（不显示全部）」——此前过滤只在点「应用」时才更新 m_nodeOnlyAvailable。
    connect(nodeOnly, &QCheckBox::toggled, this, [this](bool on) {
        if (m_nodeOnlyAvailable == on) {
            return;
        }
        m_nodeOnlyAvailable = on;
        if (!m_nodeSwitching) {
            populateNodeList();
        }
        persistConfigBool(QStringLiteral("node"), on);
    });
    auto *webProxy = new QCheckBox();
    webProxy->setChecked(config.webProxy);
    m_webProxyCheck = webProxy; // 底部「网页」开关切换时同步勾选，保证「应用」写盘值 == 实际状态
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
        applyDownloadProxy(nam); // 核心在跑则经代理下载 mmdb
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
    m_settingsTabs = tabs; // 供「未检测到内核」引导跳到系统 tab

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
    m_sysScroll = systemScroll;
    systemScroll->setObjectName("sysScroll");
    systemScroll->setWidgetResizable(true);
    systemScroll->setFrameShape(QFrame::NoFrame);
    installOverlayScrollBar(systemScroll); // mac：悬浮滚动条
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
    // 从 GitHub 拉取最新 mihomo（amd64 取 compatible 版）替换内核——虚拟机/老 CPU 上必需
    auto *cnAccel = new QCheckBox(QString::fromUtf8("国内加速"));
    cnAccel->setToolTip(QString::fromUtf8("勾选后经国内镜像加速下载 GitHub 内核（网络不佳/被墙时使用）"));
    cnAccel->setChecked(config.mirror);        // 与更新弹窗「国内代理下载」共用 config.mirror
    m_cnAccelCheck = cnAccel;
    connect(cnAccel, &QCheckBox::toggled, this, &MainWindow::setMirrorEnabled); // 勾选即持久化，无需点「应用」
    auto *coreBtn = new QPushButton(QString::fromUtf8("更新内核"));
    coreBtn->setObjectName("nodeButton");
    coreBtn->setFixedSize(110, 30);
    m_coreUpdateBtn = coreBtn; // 无内核时的下载入口，供引导高亮
    coreBtn->setToolTip(QString::fromUtf8("从 GitHub 获取最新 mihomo 内核并替换（amd64 使用兼容版）"));
    connect(coreBtn, &QPushButton::clicked, this, [this, coreBtn, cnAccel] { updateMihomoCore(coreBtn, cnAccel->isChecked()); });
    auto *coreRow = new QWidget();
    auto *coreRowLayout = new QHBoxLayout(coreRow);
    coreRowLayout->setContentsMargins(0, 0, 0, 0);
    coreRowLayout->setSpacing(10);
    coreRowLayout->addWidget(cnAccel); // 复选框在「更新内核」按钮前
    coreRowLayout->addWidget(coreBtn);
    coreRowLayout->addStretch();
    sysLayout->addWidget(row(QString::fromUtf8("mihomo 内核"), coreRow));
#if defined(Q_OS_MACOS)
    // 免密助手（特权 helper）：安装并在系统设置批准后，代理/增强模式全程免密（对标 Surge）
    auto *helperStatus = new QLabel(sysBody);
    m_helperStatusLabel = helperStatus;
    auto *helperInstallBtn = new QPushButton(QString::fromUtf8("安装/启用"));
    helperInstallBtn->setObjectName("nodeButton");
    helperInstallBtn->setFixedSize(90, 30);
    helperInstallBtn->setToolTip(QString::fromUtf8("注册以 root 运行的特权助手；首次需在系统设置→登录项批准一次"));
    connect(helperInstallBtn, &QPushButton::clicked, this, &MainWindow::installMacHelper);
    auto *helperRemoveBtn = new QPushButton(QString::fromUtf8("卸载"));
    helperRemoveBtn->setObjectName("nodeButton");
    helperRemoveBtn->setFixedSize(70, 30);
    connect(helperRemoveBtn, &QPushButton::clicked, this, &MainWindow::uninstallMacHelper);
    auto *helperRow = new QWidget();
    auto *helperRowLayout = new QHBoxLayout(helperRow);
    helperRowLayout->setContentsMargins(0, 0, 0, 0);
    helperRowLayout->setSpacing(10);
    helperRowLayout->addWidget(helperStatus);
    helperRowLayout->addWidget(helperInstallBtn);
    helperRowLayout->addWidget(helperRemoveBtn);
    helperRowLayout->addStretch();
    sysLayout->addWidget(row(QString::fromUtf8("免密助手"), helperRow));
    refreshMacHelperStatus();
#endif
    addDivider();
    addGroup(QString::fromUtf8("界面"));
    sysLayout->addWidget(row(QString::fromUtf8("主题"), theme));
    sysLayout->addWidget(row(QString::fromUtf8("跟随系统深浅色"), autoTheme));
    addDivider();
    addGroup(QString::fromUtf8("内核"));
    host->setMinimumWidth(200);
    sysLayout->addWidget(row(QString::fromUtf8("Host"), host));       // 原「远程」tab 的 Host 并入此处
    sysLayout->addWidget(row(QString::fromUtf8("端口"), mixedPort));  // http(s)&socks 混合端口
    uiPort->setToolTip(QString::fromUtf8("mihomo REST API / external-controller 端口；默认 9191（避开原版 9090，可与原版同时运行）"));
    sysLayout->addWidget(row(QString::fromUtf8("API 端口"), uiPort)); // external-controller，改后会重启核心
    sysLayout->addWidget(row(QString::fromUtf8("切换时清理连接"), clearConnections));
    addDivider();
    addGroup(QString::fromUtf8("其他"));
    sysLayout->addWidget(row(QString::fromUtf8("语言"), language));
    // 以下项已按旧项目从界面移除（TUN 由底部开关控制、订阅周期用默认），
    // 但控件仍存活以便「应用」时把原值写回，避免重置。
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
        // use:(增强/TUN) 由底部开关独占管理、每次切换即落盘——这里写「当前实际状态」而非
        // 建页时的旧快照，否则「应用」会把切换后的增强状态悄悄改回去
        out << "use: " << (m_core && m_core->isTunEnabled() ? "true" : "false") << "\n";
        out << "node: " << (nodeOnly->isChecked() ? "true" : "false") << "\n";
        out << "clearConnections: " << (clearConnections->isChecked() ? "true" : "false") << "\n";
        out << "increment: " << (increment->isChecked() ? "true" : "false") << "\n";
        out << "mini: " << (closeToTray->isChecked() ? "true" : "false") << "\n";
        out << "sys: " << (autoStart->isChecked() ? "true" : "false") << "\n";
        out << "note: " << (nodeNote->isChecked() ? "true" : "false") << "\n";
        out << "mirror: " << (cnAccel->isChecked() ? "true" : "false") << "\n"; // 国内镜像下载偏好
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
        // 「系统代理」应用即生效，并与底部「网页」开关同一状态源（写盘值 == 实际状态）
        if (m_core && webProxy->isChecked() != m_core->isProxyEnabled()) {
            m_core->toggleProxy();
        }
        m_closeToTray = closeToTray->isChecked();
        m_nodeSwitchNote = nodeNote->isChecked();
        if (m_nodeOnlyAvailable != nodeOnly->isChecked()) {
            m_nodeOnlyAvailable = nodeOnly->isChecked();
            if (!m_nodeSwitching) {
                populateNodeList(); // 「仅可用节点」改变了可见集合，整表重建即时生效
            }
        }
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
        // API 端口变更：external-controller 无法热重载，必须重启核心才能 bind 新端口；
        // 同时把轮询/切换指向新端口。与 m_core 当前端口比对，避免重复保存时反复重启。
        const int newPort = uiPort->text().trimmed().toInt();
        const bool portChanged = (newPort > 0 && newPort != m_core->uiPort());
        if (portChanged) {
            m_core->setUiPort(newPort);
            m_service.setEndpoint(host->currentText().trimmed(), newPort);
        }
        const int newMixedPort = mixedPort->text().trimmed().toInt();
        if (newMixedPort > 0) {
            m_service.setMixedPort(newMixedPort); // 混合端口改了：下载测速也走新端口
            m_proxyMixedPort = newMixedPort;      // 下载走代理也用新端口
        }
        const QString newHost = host->currentText().trimmed();
        if (!newHost.isEmpty()) {
            m_proxyHost = newHost;
        }
        if (portChanged && m_core->isRunning()) {
            m_core->stopCore();
            m_core->startCore(); // 重新按新端口生成 full.yaml 并启动核心
            appendLog(QString::fromUtf8("API 端口已改为 %1，核心已重启").arg(newPort));
        } else {
            m_core->rebuildConfig();
            if (portChanged) {
                appendLog(QString::fromUtf8("API 端口已改为 %1（下次启动核心生效）").arg(newPort));
            }
        }
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
                cur["proxies"] = QJsonArray::fromStringList(curProxies);
                // 「节点/成员」列显示：有 rule 字段用 rule，否则用成员列表
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
                } else if (t.startsWith(QLatin1String("url:"))) {
                    cur["url"] = yamlInlineValue(t);
                } else if (t.startsWith(QLatin1String("interval:"))) {
                    cur["interval"] = yamlInlineValue(t);
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
    if (section != QLatin1String("rule") && section != QLatin1String("area")) {
        return false;
    }
    const QString path = defaultConfigPath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QString yaml = QString::fromUtf8(f.readAll());
    f.close();

    const QString key = section == QLatin1String("area") ? QStringLiteral("proxy-groups:")
                                                         : QStringLiteral("rules:");
    QString block = key + QLatin1Char('\n');
    // 未改动项用原始 raw（保留转义/结构）；新增/编辑项重新序列化
    for (const QJsonValue &v : array) {
        const QJsonObject o = v.toObject();
        const QString raw = o.value("raw").toString();
        if (section == QLatin1String("area")) {
            if (!raw.isEmpty()) {
                block += raw + QLatin1Char('\n'); // raw 为整组多行文本（以 "  - name:" 开头）
            } else {
                QString type = o.value("type").toString();
                if (type.isEmpty()) {
                    type = QStringLiteral("select");
                }
                block += QStringLiteral("  - name: \"") + yamlEscapeDq(o.value("name").toString()) + QStringLiteral("\"\n");
                block += QStringLiteral("    type: ") + type + QLatin1Char('\n');
                if (type == QLatin1String("url-test") || type == QLatin1String("fallback")
                    || type == QLatin1String("load-balance")) {
                    QString url = o.value("url").toString();
                    if (url.isEmpty()) {
                        url = QStringLiteral("http://www.gstatic.com/generate_204");
                    }
                    QString interval = o.value("interval").toString();
                    if (interval.isEmpty()) {
                        interval = QStringLiteral("300");
                    }
                    block += QStringLiteral("    url: ") + url + QLatin1Char('\n');
                    block += QStringLiteral("    interval: ") + interval + QLatin1Char('\n');
                }
                block += QStringLiteral("    proxies:\n");
                const QJsonArray proxies = o.value("proxies").toArray();
                if (proxies.isEmpty()) {
                    block += QStringLiteral("      - DIRECT\n"); // 空成员会导致校验失败，兜底 DIRECT
                } else {
                    for (const QJsonValue &pv : proxies) {
                        block += QStringLiteral("      - \"") + yamlEscapeDq(pv.toString()) + QStringLiteral("\"\n");
                    }
                }
            }
        } else { // rule
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
    }

    // 定位并替换旧块（<key> 行起点 → 下一个顶层键 / EOF）
    int start = -1;
    if (yaml.startsWith(key)) {
        start = 0;
    } else {
        const int p = yaml.indexOf(QLatin1Char('\n') + key);
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
    // 区域/规则都支持增/改（区域不删，避免删除被引用的组导致悬空引用）
    const QStringList headers = isArea
        ? QStringList{QString::fromUtf8("名称"), QString::fromUtf8("类型"), QString::fromUtf8("节点/成员"), QString::fromUtf8("操作")}
        : QStringList{QString::fromUtf8("类型"), QString::fromUtf8("节点"), QString::fromUtf8("值"), QString::fromUtf8("操作")};
    const int keyCount = 3;
    const int colCount = 4;

    auto *w = new QWidget();
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(10, 10, 10, 10);
    l->setSpacing(8);

    auto *bar = new QHBoxLayout();
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(8);
    auto *addBtn = new QPushButton(QString::fromUtf8("＋ 添加"), w);
    addBtn->setObjectName("primaryButton");
    addBtn->setFixedSize(96, 30);
    if (isArea) {
        connect(addBtn, &QPushButton::clicked, this, [this] { openAreaEditor(-1); });
    } else {
        connect(addBtn, &QPushButton::clicked, this, [this, section] { openRuleEditor(section, -1); });
    }
    bar->addWidget(addBtn);
    if (!isArea) {
        auto *filter = new QLineEdit(w);
        filter->setPlaceholderText(QString::fromUtf8("搜索规则（类型/节点/值）"));
        filter->setFixedWidth(220);
        connect(filter, &QLineEdit::textChanged, this, [this] { reloadRuleTable("rule"); });
        m_ruleFilter = filter;
        bar->addWidget(filter);
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
        } else if (c == colCount - 1) { // 操作列
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

        auto *actions = new QWidget();
        auto *ah = new QHBoxLayout(actions);
        ah->setContentsMargins(0, 0, 0, 0);
        ah->setSpacing(4);
        auto *edit = new QPushButton(QString::fromUtf8("✎"), actions);
        edit->setObjectName("iconButton");
        edit->setFixedWidth(28);
        ah->addStretch();
        ah->addWidget(edit);
        const int fullIndex = i; // 过滤/截断下仍指向完整数组的真实下标
        if (isArea) {
            connect(edit, &QPushButton::clicked, this, [this, fullIndex] { openAreaEditor(fullIndex); });
        } else {
            connect(edit, &QPushButton::clicked, this, [this, section, fullIndex] { openRuleEditor(section, fullIndex); });
            auto *del = new QPushButton(QString::fromUtf8("✕"), actions);
            del->setObjectName("iconButton");
            del->setFixedWidth(28);
            ah->addWidget(del);
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
        }
        ah->addStretch();
        table->setCellWidget(row, keys.size(), actions);
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

namespace {
// 枚举当前运行的进程，去重+排序后返回：wantPath=false 取进程名(chrome.exe)，true 取可执行完整路径。
// 供「规则」编辑器里 PROCESS-NAME / PROCESS-PATH 的「值」下拉。Windows 用 Toolhelp 快照（无需提权）；
// 其它平台退回 `ps`（CI 的 Linux 构建需能编译）。
QStringList processChoices(bool wantPath)
{
    QStringList list;
    QSet<QString> seen;
#if defined(Q_OS_WIN)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snap, &entry)) {
            do {
                QString value;
                if (!wantPath) {
                    value = QString::fromWCharArray(entry.szExeFile);
                } else {
                    // 完整路径需 OpenProcess；受保护/系统进程可能失败，失败则跳过该项。
                    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                    if (proc) {
                        wchar_t buf[MAX_PATH];
                        DWORD sz = MAX_PATH;
                        if (QueryFullProcessImageNameW(proc, 0, buf, &sz)) {
                            value = QString::fromWCharArray(buf, static_cast<int>(sz));
                        }
                        CloseHandle(proc);
                    }
                }
                if (!value.isEmpty() && !seen.contains(value)) {
                    seen.insert(value);
                    list << value;
                }
            } while (Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
    }
#else
    QProcess ps;
    ps.start(QStringLiteral("ps"), {QStringLiteral("-eo"), QStringLiteral("comm=")});
    if (ps.waitForFinished(3000)) {
        const QList<QByteArray> lines = ps.readAllStandardOutput().split('\n');
        for (const QByteArray &l : lines) {
            const QString p = QString::fromUtf8(l).trimmed();
            const QString value = wantPath ? p : QFileInfo(p).fileName();
            if (!value.isEmpty() && !seen.contains(value)) {
                seen.insert(value);
                list << value;
            }
        }
    }
#endif
    list.sort(Qt::CaseInsensitive);
    return list;
}
} // namespace

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

    dl->addWidget(new QLabel(QString::fromUtf8("类型"), dialog));
    auto *typeBox = new QComboBox(dialog);
    typeBox->setEditable(true); // 预置常用类型，也允许手输自定义类型
    typeBox->addItems({"DOMAIN-SUFFIX", "DOMAIN", "DOMAIN-KEYWORD", "DOMAIN-REGEX",
                       "IP-CIDR", "IP-CIDR6", "GEOIP", "GEOSITE", "IP-ASN",
                       "SRC-IP-CIDR", "SRC-PORT", "DST-PORT",
                       "PROCESS-NAME", "PROCESS-PATH", "RULE-SET", "MATCH"});
    typeBox->setCurrentText(cur.value("type").toString().isEmpty() ? QStringLiteral("DOMAIN-SUFFIX")
                                                                   : cur.value("type").toString());
    dl->addWidget(typeBox);

    dl->addWidget(new QLabel(QString::fromUtf8("值 (域名 / IP 段；进程规则从下拉选运行中的进程；MATCH 留空)"), dialog));
    auto *valueBox = new QComboBox(dialog);
    valueBox->setEditable(true); // 可下拉选、也可手输
    valueBox->setInsertPolicy(QComboBox::NoInsert);
    valueBox->setCurrentText(cur.value("value").toString());
    dl->addWidget(valueBox);

    // 类型为 PROCESS-NAME/PROCESS-PATH 时，把「值」下拉填成当前运行的进程名/路径；其它类型清空成普通可输入框。
    // 用 lastKind 记录已填充的类别，避免可编辑下拉每次按键都重新枚举进程（枚举有系统开销）。
    auto lastKind = std::make_shared<QString>();
    auto refreshValueChoices = [valueBox, lastKind](const QString &type) {
        QString kind;
        if (type.compare(QLatin1String("PROCESS-NAME"), Qt::CaseInsensitive) == 0) {
            kind = QStringLiteral("name");
        } else if (type.compare(QLatin1String("PROCESS-PATH"), Qt::CaseInsensitive) == 0) {
            kind = QStringLiteral("path");
        }
        if (kind == *lastKind) {
            return; // 类别未变，无需重新枚举
        }
        *lastKind = kind;
        const QString keep = valueBox->currentText(); // 保留用户已填/已存的值
        QSignalBlocker block(valueBox);
        valueBox->clear();
        if (kind == QLatin1String("name")) {
            valueBox->addItems(processChoices(false));
        } else if (kind == QLatin1String("path")) {
            valueBox->addItems(processChoices(true));
        }
        valueBox->setCurrentText(keep);
    };
    refreshValueChoices(typeBox->currentText()); // 编辑已有 PROCESS 规则时，打开即填充
    connect(typeBox, &QComboBox::currentTextChanged, dialog,
            [refreshValueChoices](const QString &t) { refreshValueChoices(t); });

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
    connect(ok, &QPushButton::clicked, dialog, [this, dialog, section, editIndex, typeBox, valueBox, nodeBox] {
        const QString type = typeBox->currentText().trimmed();
        const QString node = nodeBox->currentText().trimmed();
        if (type.isEmpty() || node.isEmpty()) {
            return; // 类型/节点必填
        }
        QJsonObject obj; // 不含 raw → 回写按 3 段（或 MATCH 2 段）重新序列化
        obj["type"] = type;
        obj["node"] = node;
        obj["value"] = valueBox->currentText().trimmed();
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

void MainWindow::openAreaEditor(int editIndex)
{
    const QJsonArray array = loadRuleSection("area");
    const bool isEdit = editIndex >= 0 && editIndex < array.size();
    const QJsonObject cur = isEdit ? array[editIndex].toObject() : QJsonObject{};

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(isEdit ? QString::fromUtf8("编辑区域分组") : QString::fromUtf8("新增区域分组"));
    dialog->setStyleSheet(styleSheet());
    dialog->resize(420, 380);
    auto *dl = new QVBoxLayout(dialog);
    dl->setContentsMargins(12, 12, 12, 12);
    dl->setSpacing(6);

    dl->addWidget(new QLabel(QString::fromUtf8("名称"), dialog));
    auto *nameEd = new QLineEdit(cur.value("name").toString(), dialog);
    if (isEdit) {
        nameEd->setReadOnly(true); // 重命名会破坏 rules/其它组的引用，编辑时锁定名称
        nameEd->setToolTip(QString::fromUtf8("编辑时不支持改名（会破坏引用）；如需改名请新增一个组"));
    }
    dl->addWidget(nameEd);

    dl->addWidget(new QLabel(QString::fromUtf8("类型"), dialog));
    auto *typeBox = new QComboBox(dialog);
    typeBox->addItems({"select", "url-test", "fallback", "load-balance", "relay"});
    typeBox->setCurrentText(cur.value("type").toString().isEmpty() ? QStringLiteral("select") : cur.value("type").toString());
    dl->addWidget(typeBox);

    dl->addWidget(new QLabel(QString::fromUtf8("成员（每行一个：DIRECT / REJECT / 其它组名 / 节点名）"), dialog));
    auto *proxiesEd = new QTextEdit(dialog);
    QStringList members;
    for (const QJsonValue &pv : cur.value("proxies").toArray()) {
        members << pv.toString();
    }
    proxiesEd->setPlainText(members.join(QLatin1Char('\n')));
    proxiesEd->setFixedHeight(120);
    dl->addWidget(proxiesEd);

    auto *urlRow = new QHBoxLayout();
    auto *urlEd = new QLineEdit(cur.value("url").toString(), dialog);
    urlEd->setPlaceholderText(QString::fromUtf8("url（url-test/fallback，可留空用默认）"));
    auto *intervalEd = new QLineEdit(cur.value("interval").toString(), dialog);
    intervalEd->setPlaceholderText(QString::fromUtf8("间隔秒"));
    intervalEd->setFixedWidth(90);
    urlRow->addWidget(urlEd);
    urlRow->addWidget(intervalEd);
    dl->addLayout(urlRow);
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
    connect(ok, &QPushButton::clicked, dialog, [this, dialog, editIndex, isEdit, nameEd, typeBox, proxiesEd, urlEd, intervalEd] {
        const QString name = nameEd->text().trimmed();
        if (name.isEmpty()) {
            return;
        }
        QJsonObject obj; // 不含 raw → 回写时重新序列化整组
        obj["name"] = name;
        obj["type"] = typeBox->currentText();
        obj["url"] = urlEd->text().trimmed();
        obj["interval"] = intervalEd->text().trimmed();
        QJsonArray proxies;
        const QStringList lines = proxiesEd->toPlainText().split(QLatin1Char('\n'));
        for (const QString &m : lines) {
            const QString t = m.trimmed();
            if (!t.isEmpty()) {
                proxies.append(t);
            }
        }
        obj["proxies"] = proxies;

        QJsonArray current = loadRuleSection("area");
        if (isEdit && editIndex >= 0 && editIndex < current.size()) {
            current[editIndex] = obj;
        } else {
            current.append(obj);
        }
        saveRuleSection("area", current);
        reloadRuleTable("area");
        if (m_core) {
            m_core->rebuildConfig();
        }
        appendLog(QString::fromUtf8("已保存区域分组并重建配置"));
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
        installOverlayScrollBar(scroll); // mac：悬浮滚动条（主要/Clash 两个时间线共用此构造）
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
    installOverlayScrollBar(scroll); // mac：悬浮滚动条
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
    connect(verLabel, &QLabel::linkActivated, this, [this](const QString &) { checkForUpdate(false); });
    m_versionLabel = verLabel; // 供发现新版本时改红提示
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
    // 右内边距 0：最右的模式下拉框贴到页脚框右缘；页脚框本身由右侧列留 5px（离窗口右边 5px）。
    layout->setContentsMargins(10, 0, 0, 0);
    layout->setSpacing(5);

    auto *footerArrow = new QLabel(QChar(0xE625), footer); // iconfont icon-arrow-2（方向-向右-粗）
    footerArrow->setObjectName("footerArrow");
    m_logLabel = new QLabel("Ready", footer);
    m_logLabel->setObjectName("footerLog");
    // 不参与最小宽度计算：长日志只在可用空间内截断，不把右侧开关挤出窗口、也不阻止窗口缩小
    m_logLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(footerArrow);
    layout->addWidget(m_logLabel, 1);

    auto addSwitch = [&](const QString &text, QWidget **dot, std::function<void()> onClick) {
        auto *button = new QPushButton(text, footer);
        button->setObjectName("switchButton");
        *dot = createSwitchDot(false);
        auto *buttonLayout = new QHBoxLayout(button);
        buttonLayout->setContentsMargins(6, 0, 8, 0);
        buttonLayout->setSpacing(5);
        buttonLayout->addWidget(*dot);
        buttonLayout->addStretch();
        connect(button, &QPushButton::clicked, this, [onClick] { onClick(); });
        layout->addWidget(button);
    };

    // 增强(TUN) 走 onToggleTunRequested：需要管理员权限，非提权时先弹 UAC 提权重启
    addSwitch("增强", &m_tunDot, [this] { onToggleTunRequested(); });
    addSwitch("网页", &m_proxyDot, [this] { onToggleProxyRequested(); });
    addSwitch("核心", &m_coreDot, [this] { if (m_core) m_core->toggleCore(); });

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

    // 组（now 非空，如自动分类国家组）附带显示当前使用的节点：「组名 → 使用中的节点」
    QString display = node.name;
    if (!node.now.isEmpty() && node.now != node.name) {
        display = QString::fromUtf8("%1 → %2").arg(node.name, node.now);
    }
    auto *name = new ElidingLabel(row);
    name->setObjectName("nodeName");
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // 自适应：名称随列宽伸缩，不撑宽整行
    name->setFullText(display); // 超出列宽用「…」省略，完整名进 tooltip
    layout->addWidget(name, 1);
    m_nodeNameLabels.insert(node.name, name); // 登记名称标签，供 syncNodeRows 原地更新「→子节点」

    // 速度/延迟药丸：内容大小、不再占固定 120px 列——名称(stretch)因此能一直伸到药丸左侧，
    // 消除名称与延迟标签间的空隙；名称列宽自适应，各行药丸右缘仍对齐（在「应用」按钮前）。
    auto *badgeWrap = new QWidget(row);
    auto *badgeLayout = new QHBoxLayout(badgeWrap);
    badgeLayout->setContentsMargins(0, 0, 0, 0);
    auto *delay = new QLabel(QString("%1/%2").arg(speedText(node.speed), node.delay == 0 ? "-" : QString::number(node.delay)), badgeWrap);
    delay->setObjectName("delayBadge");
    delay->setStyleSheet(QString("background:%1;").arg(delayColor(node.delay).name(QColor::HexArgb)));
    delay->setAlignment(Qt::AlignCenter);
    delay->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); // 背景贴合文字，不被行高拉伸
    badgeLayout->addWidget(delay, 0, Qt::AlignVCenter);
    layout->addWidget(badgeWrap);
    m_delayBadges.insert(node.name, delay); // 登记药丸，供 updateNodeBadges 原地刷新

    // 单个按钮：非活动项显示「应用」= 切换到该节点/分组；活动（正在使用）项显示「禁用」=
    //  禁用该节点（分组则禁其当前实际使用的节点 now）。只有正在使用的节点才出现「禁用」。
    auto *apply = new QPushButton(node.active ? QString::fromUtf8("禁用") : QString::fromUtf8("应用"), row);
    apply->setObjectName("nodeButton");
    apply->setFixedSize(82, 38);
    if (m_nodeSwitching) {
        apply->setEnabled(false); // 切换中：所有按钮禁用（对齐旧项目 disableLoading）
        if (!m_nodeSwitchTarget.isEmpty() && node.name == m_nodeSwitchTarget) {
            static const char *frames[] = {"◐", "◓", "◑", "◒"};
            apply->setText(QString::fromUtf8(frames[m_spinnerFrame % 4])); // 目标按钮转圈
            m_spinnerButton = apply;
        }
    }
    // 点击槽只捕获节点名，运行时再从 m_currentNodes 查当前活动态——这样 syncNodeRows 原地改按钮文字
    // （应用↔禁用）后，行不重建、槽不重连，点击仍按「最新」状态走对分支（避免捕获旧 node.active 而误判）。
    connect(apply, &QPushButton::clicked, this, [this, nodeName = node.name] {
        if (m_nodeSwitching) {
            return; // 防重入：切换进行中忽略点击
        }
        NodeInfo cur;
        bool found = false;
        for (const NodeInfo &n : std::as_const(m_currentNodes)) {
            if (n.name == nodeName) {
                cur = n;
                found = true;
                break;
            }
        }
        if (!found) {
            return;
        }
        beginNodeSwitch(nodeName); // 转圈落在被点节点的按钮上
        if (cur.active) {
            disableNodeByName(cur.now.isEmpty() ? nodeName : cur.now); // 禁用：禁其当前实际使用的节点
        } else {
            m_service.selectNode(nodeName); // 应用：切换到该节点
        }
    });
    layout->addWidget(apply);
    m_nodeButtons.insert(node.name, apply); // 登记按钮，供 syncNodeRows 原地改「应用/禁用」与启用态
    m_nodeRows.insert(node.name, row);      // 登记整行，供 syncNodeRows 原地重排（不销毁）
    return row;
}

void MainWindow::disableNodeByName(const QString &liveName)
{
    // 实时节点名 = 「订阅节点名 - 订阅名[speedtest]」（见 ConfigBuilder）；反查订阅节点后 use:false 并重建。
    const QVector<SubscriptionSummary> subs = m_subscriptions->load();
    for (int s = 0; s < subs.size(); ++s) {
        const QVector<SubscriptionNodeSummary> nodes = m_subscriptions->nodes(s);
        for (int n = 0; n < nodes.size(); ++n) {
            const QString base = nodes[n].name + QStringLiteral(" - ") + subs[s].name;
            if (liveName == base || liveName == base + QStringLiteral("[speedtest]")) {
                if (m_subscriptions->setNodeEnabled(s, n, false)) {
                    reloadSubscriptions();
                    m_core->rebuildConfig(); // 重新生成 full.yaml 并热重载，节点从池中移除
                    m_service.refreshNodes(); // 立即向核心重新拉取列表
                    appendLog(QString::fromUtf8("已禁用节点: %1").arg(liveName));
                } else {
                    appendLog(QString::fromUtf8("禁用节点失败: %1").arg(liveName));
                }
                return;
            }
        }
    }
    appendLog(QString::fromUtf8("未找到可禁用的订阅节点: %1（可能是 DIRECT/内置组）").arg(liveName));
}

void MainWindow::showSubscriptionNodes(int subscriptionIndex)
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose); // 反复勾选会「关旧框开新框」，不自动销毁会累积泄漏
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
    // P4：每张卡片的可更新控件句柄（按连接 id 索引）+ 上次可见连接的 id 集合。
    // 可见集合不变时只原地更新这些控件的数值，不 delete/重建整表（避免每秒重建 + 闪烁）。
    struct CardRefs {
        QPointer<QFrame> card;
        QPointer<QLabel> dot;
        QPointer<QLabel> title;
        QPointer<QLabel> chain;
        QPointer<QLabel> down;
        QPointer<QLabel> up;
        QPointer<QPushButton> del;
    };
    auto cards = std::make_shared<QHash<QString, CardRefs>>();
    auto prevIds = std::make_shared<QSet<QString>>();

    // 速度/流量格式（旧项目 connections 无空格：1.50KB）
    auto spd = [](qint64 value) -> QString {
        double n = value < 0 ? 0 : static_cast<double>(value);
        const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        int i = 0;
        while (n >= 1024.0 && i < 4) { n /= 1024.0; ++i; }
        return QString::number(n, 'f', 2) + units[i];
    };
    // 小徽标：iconfont 图标 + 文本，固定配色（与旧项目一致，不随主题变化）
    auto makeBadge = [](QChar glyph, const QString &text, const QString &bg, const QString &fg,
                        QLabel **outText = nullptr) -> QWidget * {
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
        if (outText) {
            *outText = t; // 供集合不变时原地更新药丸文本（下载/上传/代理链）
        }
        return badge;
    };
    // 单张连接卡片
    auto makeCard = [this, dialog, light, spd, makeBadge, cards](const QJsonObject &c) -> QFrame * {
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
        QLabel *chainText = nullptr, *dlText = nullptr, *ulText = nullptr;
        h->addWidget(makeBadge(QChar(0xE6BC), chain0, "rgba(0,0,0,0.35)", "#fff", &chainText));
        h->addWidget(makeBadge(QChar(0xE6CD), dl ? spd(dl) : QStringLiteral("-"), "rgba(0,255,0,0.5)", "#333", &dlText));
        h->addWidget(makeBadge(QChar(0xE6CC), ul ? spd(ul) : QStringLiteral("-"), "rgba(255,0,0,0.5)", "#333", &ulText));

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
        // 登记控件句柄，供集合不变时原地更新（del 的 clicked 捕获的是稳定的 id，无需重连）
        CardRefs refs;
        refs.card = card;
        refs.dot = dot;
        refs.title = title;
        refs.chain = chainText;
        refs.down = dlText;
        refs.up = ulText;
        refs.del = del;
        cards->insert(id, refs);
        return card;
    };

    // 集合不变时的原地更新：不销毁卡片，只刷新在线圆点/标题/代理链/上下行/删除按钮态。
    // 样式字符串需与 makeCard 保持一致。
    auto updateCard = [light, spd](const CardRefs &refs, const QJsonObject &c) {
        if (!refs.card) {
            return;
        }
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
        if (refs.dot) {
            refs.dot->setStyleSheet(offline ? "color:#999;font-size:10px;background:transparent;"
                                            : "color:#67c23a;font-size:10px;background:transparent;");
        }
        if (refs.title) {
            refs.title->setText(QString("[%1] %2").arg(type, host));
            refs.title->setStyleSheet(offline
                ? "color:#999;font-size:14px;background:transparent;"
                : (light ? "color:#333;font-size:14px;background:transparent;"
                         : "color:#eee;font-size:14px;background:transparent;"));
        }
        if (refs.chain) {
            refs.chain->setText(chain0);
        }
        if (refs.down) {
            refs.down->setText(dl ? spd(dl) : QStringLiteral("-"));
        }
        if (refs.up) {
            refs.up->setText(ul ? spd(ul) : QStringLiteral("-"));
        }
        if (refs.del) {
            refs.del->setEnabled(!offline);
        }
    };

    // 刷新卡片列表 + 计数。可见连接的 id 集合与上次相同时，只原地更新已有卡片的数值（不重排、不重建），
    // 避免每秒 delete+重建上千个 widget 的卡顿与闪烁；集合变化时才整表重建并重建句柄表。
    auto buildList = [guard, acc, listBody, listLayout, onlineBtn, offlineBtn, search,
                      makeCard, updateCard, cards, prevIds]() {
        if (!guard) {
            return;
        }
        const bool showOnline = onlineBtn->isChecked();
        const bool showOffline = offlineBtn->isChecked();
        const QString f = search->text().trimmed();

        // 第一遍：算出可见连接（保持 acc 的排序次序）、其 id 集合与计数——不做任何 widget 操作
        QList<QJsonObject> visible;
        QSet<QString> visibleIds;
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
            visible.append(c);
            visibleIds.insert(c.value("id").toString());
        }
        onlineBtn->setText(QString::fromUtf8("Online (%1)").arg(onlineCount));
        offlineBtn->setText(QString::fromUtf8("Offline (%1)").arg(offlineCount));

        // 集合不变：只原地更新已有卡片的数值（按 id 查表，与显示次序无关）
        if (visibleIds == *prevIds && !cards->isEmpty()) {
            for (const QJsonObject &c : std::as_const(visible)) {
                const auto it = cards->constFind(c.value("id").toString());
                if (it != cards->constEnd()) {
                    updateCard(*it, c);
                }
            }
            return;
        }

        // 集合变化：整表重建（并重建 id→控件 句柄表）
        listBody->setUpdatesEnabled(false);
        while (QLayoutItem *item = listLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        cards->clear();
        for (const QJsonObject &c : std::as_const(visible)) {
            listLayout->addWidget(makeCard(c));
        }
        listLayout->addStretch();
        listBody->setUpdatesEnabled(true);
        *prevIds = visibleIds;
    };

    // 拉取连接：按 id 合并进 acc（新增/更新在线，缺失标记离线），按下载增量降序。
    // 用 QHash<id,下标> 替代线性查找（O(在线×累积) → O(在线+累积)）；acc 设上限防离线连接无界堆积。
    *reloadFn = [this, guard, acc, buildList]() {
        m_service.fetchConnections([guard, acc, buildList](QJsonArray conns) {
            if (!guard) {
                return;
            }
            QHash<QString, int> indexById; // id → acc 下标，本轮内一次性建立
            indexById.reserve(acc->size());
            for (int i = 0; i < acc->size(); ++i) {
                (*acc)[i].insert("offline", true); // 先全标记离线，命中的再翻回在线
                indexById.insert(acc->at(i).value("id").toString(), i);
            }
            for (const QJsonValue &cv : conns) {
                QJsonObject c = cv.toObject();
                const QString id = c.value("id").toString();
                const int idx = indexById.value(id, -1);
                if (idx >= 0) {
                    const double prevDl = acc->at(idx).value("download").toDouble();
                    c.insert("sort", c.value("download").toDouble() - prevDl);
                    c.insert("offline", false);
                    (*acc)[idx] = c;
                } else {
                    c.insert("sort", 0);
                    c.insert("offline", false);
                    indexById.insert(id, acc->size());
                    acc->append(c);
                }
            }
            std::sort(acc->begin(), acc->end(), [](const QJsonObject &a, const QJsonObject &b) {
                return a.value("sort").toDouble() > b.value("sort").toDouble();
            });
            // 上限：挂 BT 等场景离线连接会无界堆积；按活跃度排序后截去尾部（最不活跃/离线的最旧项）
            while (acc->size() > 500) {
                acc->removeLast();
            }
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

void MainWindow::persistConfigBool(const QString &key, bool value)
{
    // 轻量持久化：只改 config.yaml 的单个键、保留其余内容（不整体重写，供各处 toggled 即时落盘用）
    const QString path = QDir(m_userDir).filePath("config.yaml");
    QString yaml;
    QFile in(path);
    if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        yaml = QString::fromUtf8(in.readAll());
        in.close();
    }
    const QString line = QStringLiteral("%1: %2").arg(key, value ? "true" : "false");
    const QRegularExpression re(QStringLiteral("(?m)^%1:.*$").arg(QRegularExpression::escape(key)));
    if (re.match(yaml).hasMatch()) {
        yaml.replace(re, line);
    } else {
        if (!yaml.isEmpty() && !yaml.endsWith('\n')) {
            yaml += '\n';
        }
        yaml += line + '\n';
    }
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        out.write(yaml.toUtf8());
        out.close();
    }
}

void MainWindow::setMirrorEnabled(bool on)
{
    // 「国内加速」(设置页) 与「国内代理下载」(更新弹窗) 共用偏好：任一处切换都即时落盘到 config.mirror，
    // 并回同步设置页勾选框（设置页只构建一次，否则更新弹窗改了后、设置页「应用」会用旧值把它冲掉）。
    if (m_mirror == on) {
        return;
    }
    m_mirror = on;
    if (m_cnAccelCheck) {
        const QSignalBlocker blocker(m_cnAccelCheck); // 避免回同步再触发 toggled → 递归
        m_cnAccelCheck->setChecked(on);
    }
    persistConfigBool(QStringLiteral("mirror"), on);
}

namespace {
// 比较版本号（忽略前缀字母，如 v0.1.81 → 0.1.81），remote 更新则返回 true
bool versionNewer(const QString &remote, const QString &local)
{
    auto parse = [](QString s) {
        s.remove(QRegularExpression(QStringLiteral("[^0-9.]")));
        QVector<int> v;
        for (const QString &p : s.split('.', Qt::SkipEmptyParts)) {
            v << p.toInt();
        }
        return v;
    };
    const QVector<int> a = parse(remote);
    const QVector<int> b = parse(local);
    for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a.at(i) : 0;
        const int y = i < b.size() ? b.at(i) : 0;
        if (x != y) {
            return x > y;
        }
    }
    return false;
}
} // namespace

void MainWindow::showUpdateDialog()
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QString::fromUtf8("Clash Auto 更新"));
    dialog->setStyleSheet(styleSheet());
    dialog->resize(600, 560); // 对齐旧项目 update 窗 600x540
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
        auto *h2 = new QLabel(QString::fromUtf8("下载资源"), page);
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

    // 「内核」tab：显示本地 mihomo 版本与最新版本、更新说明，并可在此直接更新内核
    //（下载与「国内代理下载」共用同一开关；更新按钮复用 updateMihomoCore 全流程）
    auto *corePage = new QWidget();
    auto *coreLayout = new QVBoxLayout(corePage);
    coreLayout->setContentsMargins(10, 10, 10, 10);
    coreLayout->setSpacing(4);
    auto *coreVer = new QLabel(QString::fromUtf8("内核版本: 检测中..."), corePage);
    coreVer->setObjectName("sectionTitle");
    coreVer->setWordWrap(true);
    auto *coreNoteHead = new QLabel(QString::fromUtf8("更新说明"), corePage);
    coreNoteHead->setObjectName("subCardMeta");
    auto *coreBody = new QTextEdit(corePage);
    coreBody->setReadOnly(true);
    coreBody->setMinimumHeight(200);
    coreBody->setPlainText(QString::fromUtf8("正在获取..."));
    auto *coreUpdateBtn = new QPushButton(QString::fromUtf8("更新内核"), corePage);
    coreUpdateBtn->setObjectName("primaryButton");
    coreUpdateBtn->setFixedSize(110, 30);
    coreUpdateBtn->setToolTip(QString::fromUtf8("从 GitHub 获取最新 mihomo 内核并替换（amd64 使用兼容版）"));
    auto *coreBtnRow = new QHBoxLayout();
    coreBtnRow->addStretch();
    coreBtnRow->addWidget(coreUpdateBtn);
    coreLayout->addWidget(coreVer);
    coreLayout->addWidget(coreNoteHead);
    coreLayout->addWidget(coreBody, 1);
    coreLayout->addLayout(coreBtnRow);
    tabs->addTab(corePage, QString::fromUtf8("内核"));

    auto *progress = new QProgressBar(dialog);
    progress->setFixedHeight(26);
    progress->setValue(0);
    progress->hide();
    root->addWidget(progress);

    auto *bottom = new QHBoxLayout();
    auto *noTip = new QCheckBox(QString::fromUtf8("不再提示"), dialog);
    bottom->addWidget(noTip);
    bottom->addStretch();
    // 国内代理下载：与「更新内核」的「国内加速」同机制——给 GitHub 下载链接加国内镜像前缀
    // （ghfast.top）并「直连」下载（镜像国内可直达，不套核心节点代理——否则绕道节点出国再
    // 回国内镜像，又慢又费流量）。查询 release 列表仍走 applyDownloadProxy（api.github.com
    // 镜像代理不了，墙内需经节点）。
    auto *mirrorCheck = new QCheckBox(QString::fromUtf8("国内代理下载"), dialog);
    mirrorCheck->setToolTip(QString::fromUtf8("勾选后直连国内镜像（ghfast.top）加速下载更新包/内核，不经代理节点"));
    mirrorCheck->setChecked(m_mirror); // 与设置页「国内加速」共用偏好
    connect(mirrorCheck, &QCheckBox::toggled, this, &MainWindow::setMirrorEnabled); // 勾选即持久化
    bottom->addWidget(mirrorCheck);
    // 「内核」tab 的更新按钮：与设置页「更新内核」同一流程，镜像偏好取本弹窗勾选框
    connect(coreUpdateBtn, &QPushButton::clicked, dialog, [this, coreUpdateBtn, mirrorCheck] {
        updateMihomoCore(coreUpdateBtn, mirrorCheck->isChecked());
    });
    auto *closeBtn = new QPushButton(QString::fromUtf8("关闭"), dialog);
    closeBtn->setObjectName("nodeButton");
    closeBtn->setFixedSize(80, 30);
    auto *updateBtn = new QPushButton(QString::fromUtf8("一键更新"), dialog);
    updateBtn->setObjectName("primaryButton");
    updateBtn->setFixedSize(100, 30);
    updateBtn->setEnabled(false); // 未选资源时禁用（对齐旧项目 address===''）
    updateBtn->setToolTip(QString::fromUtf8("下载后自动静默安装并重启，恢复更新前的增强/网页开关状态"));
    bottom->addWidget(closeBtn);
    bottom->addWidget(updateBtn);
    root->addLayout(bottom);

    connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::reject);
    auto *nam = new QNetworkAccessManager(dialog);
    applyDownloadProxy(nam); // 核心在跑则经代理查 release 列表/内核版本（未勾镜像时下载也走它）

    // 选中资源即启用「更新」（作用于当前 tab 的当前选中项）
    auto currentAssets = [tabs, rel, beta]() -> QListWidget * {
        if (tabs->currentIndex() == 0) {
            return rel.assets;
        }
        if (tabs->currentIndex() == 1) {
            return beta.assets;
        }
        return nullptr; // 「内核」tab 用页内的「更新内核」按钮，底部「一键更新」置灰
    };
    auto refreshUpdateBtn = [updateBtn, currentAssets] {
        QListWidget *lw = currentAssets();
        updateBtn->setEnabled(lw && lw->currentItem() != nullptr);
    };
    connect(rel.assets, &QListWidget::itemSelectionChanged, dialog, refreshUpdateBtn);
    connect(beta.assets, &QListWidget::itemSelectionChanged, dialog, refreshUpdateBtn);
    connect(tabs, &QTabWidget::currentChanged, dialog, [refreshUpdateBtn](int) { refreshUpdateBtn(); });

    // 「一键更新」：下载所选资源到临时目录 → Windows 安装包走静默安装 + 自动重启
    // （launchSilentUpdateAndRestart）；其余资源退回旧行为：打开文件并退出应用。
    connect(updateBtn, &QPushButton::clicked, dialog, [this, dialog, nam, progress, updateBtn, currentAssets, mirrorCheck] {
        QListWidget *lw = currentAssets();
        QListWidgetItem *item = lw ? lw->currentItem() : nullptr;
        if (!item) {
            return;
        }
        const QString url = item->data(Qt::UserRole).toString();
        const QString name = item->text();
        if (url.isEmpty()) {
            return;
        }
        // 官方 <资源名>.sha256 边车的直连 url（fill 时按名登记；老版本无边车则为空）。
        // 下载后据此校验安装包完整性——边车必须官方直连，绝不加镜像前缀，否则失去信任锚。
        const QString sidecarUrl = item->data(Qt::UserRole + 1).toString();
        // 国内代理下载：给下载链接加国内镜像前缀，并用「直连」的 QNAM 下载——镜像在国内
        // 可直达，绝不套核心节点代理（与「更新内核」一致）。未勾选则沿用 nam（核心在跑经节点）。
        const bool useMirror = mirrorCheck && mirrorCheck->isChecked();
        const QString downloadUrl = useMirror ? QStringLiteral("https://ghfast.top/") + url : url;
        QNetworkAccessManager *dlNam = nam;
        if (useMirror) {
            dlNam = new QNetworkAccessManager(dialog);
            dlNam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        }
        QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        if (dir.isEmpty()) {
            dir = QDir::tempPath();
        }
        const QString savePath = QDir(dir).filePath(name);
        QNetworkRequest dreq{QUrl(downloadUrl)};
        dreq.setRawHeader("User-Agent", "clashauto-cpp");
        dreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        // 关掉 HTTP/2：Qt 的 HTTP/2 下大文件（尤其经代理隧道到 GitHub CDN）常「下一点就卡死」，
        // 进度条冻住、永不 finished。强制 HTTP/1.1 更稳。
        dreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        dreq.setTransferTimeout(30000); // 30s 无数据即判失败，避免永久卡在进度条不动（而不是无声挂起）
#endif
        QNetworkReply *reply = dlNam->get(dreq);
        // 边下边写到磁盘：不把整包（~40MB）缓存在内存，也及时清空 reply 缓冲，避免堆积卡顿。
        // out 挂在 reply 名下，取消/关窗时随 reply 一起销毁（QFile 析构会关闭文件）。
        auto *out = new QFile(savePath);
        out->setParent(reply);
        if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            reply->abort();
            reply->deleteLater();
            updateBtn->setEnabled(true);
            appendLog(QString::fromUtf8("保存失败（无法写入临时文件）: %1").arg(savePath));
            QMessageBox::warning(dialog, QString::fromUtf8("更新"), QString::fromUtf8("无法写入临时文件：%1").arg(savePath));
            return;
        }
        updateBtn->setEnabled(false);
        progress->setValue(0);
        progress->show();
        appendLog(QString::fromUtf8("开始下载: %1%2").arg(name, useMirror ? QString::fromUtf8("（国内代理）") : QString()));
        connect(reply, &QNetworkReply::downloadProgress, dialog, [progress](qint64 received, qint64 total) {
            if (total > 0) {
                progress->setValue(static_cast<int>(received * 100 / total));
            }
        });
        connect(reply, &QNetworkReply::readyRead, dialog, [reply, out] {
            out->write(reply->readAll()); // 收到即写，避免 reply 内部缓冲无限堆积
        });
        connect(reply, &QNetworkReply::finished, dialog, [this, dialog, reply, nam, progress, updateBtn, out, savePath, name, sidecarUrl, useMirror] {
            out->write(reply->readAll()); // 收尾残余
            out->close();
            const bool ok = reply->error() == QNetworkReply::NoError;
            const QString err = reply->errorString();
            reply->deleteLater(); // 连带删除 out
            if (!ok) {
                QFile::remove(savePath); // 删掉不完整文件，避免半包被当成安装包运行
                progress->hide();
                updateBtn->setEnabled(true);
                appendLog(QString::fromUtf8("下载失败: %1 (%2)").arg(name, err));
                QMessageBox::warning(dialog, QString::fromUtf8("更新"),
                                     QString::fromUtf8("下载失败：%1\n\n若在墙内，可勾选「国内代理下载」走国内镜像，或启动核心以经代理下载后重试。").arg(err));
                return;
            }
            progress->setValue(100);

            // 校验通过后的真正执行动作（静默安装并重启 / 打开安装包并退出）。
            auto doExecute = [this, savePath, name] {
#if defined(Q_OS_WIN)
                // 一键更新：NSIS 安装包支持 /S 静默安装——等本进程退出后原地升级并自动重启。
                // 增强/网页开关每次切换已落盘（config 的 use:/web:），重启后按其自动恢复。
                if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
                    appendLog(QString::fromUtf8("下载完成，正在静默安装并自动重启: %1").arg(savePath));
                    launchSilentUpdateAndRestart(savePath);
                    return;
                }
#elif defined(Q_OS_MACOS)
                // 一键更新：挂载 DMG → ditto 覆盖当前 .app → 卸载 → 自动重启（新版签名+公证）。
                // 增强/网页开关每次切换已落盘（config 的 use:/web:），重启后按其自动恢复。
                if (name.endsWith(QStringLiteral(".dmg"), Qt::CaseInsensitive)) {
                    appendLog(QString::fromUtf8("下载完成，正在安装并自动重启: %1").arg(savePath));
                    launchSilentUpdateAndRestartMac(savePath);
                    return;
                }
#endif
                appendLog(QString::fromUtf8("下载完成，启动安装并退出: %1").arg(savePath));
                QDesktopServices::openUrl(QUrl::fromLocalFile(savePath)); // 启动安装包 / 打开文件
                qApp->quit();                                             // 退出应用让安装继续（对齐旧项目）
            };

            // 边车缺失 / 取不到校验文件时的处置：镜像下载不可信 → 拒绝并清理；直连老版本可能
            // 本就没有边车 → 向后兼容，跳过校验照常执行。
            auto onSidecarUnavailable = [this, dialog, progress, updateBtn, savePath, useMirror, doExecute] {
                if (useMirror) {
                    QFile::remove(savePath); // 经镜像下载又无校验锚，直接销毁
                    progress->hide();
                    updateBtn->setEnabled(true);
                    appendLog(QString::fromUtf8("更新已取消: 经镜像下载但未取到校验文件（.sha256），无法校验完整性"));
                    QMessageBox::warning(dialog, QString::fromUtf8("更新"),
                                         QString::fromUtf8("无法校验完整性（未取到校验文件），经镜像下载已取消，请关闭「国内代理下载」后重试。"));
                    return;
                }
                appendLog(QString::fromUtf8("未取到校验文件，跳过完整性校验（直连）"));
                doExecute();
            };

            if (sidecarUrl.isEmpty()) {
                onSidecarUnavailable();
                return;
            }

            // 完整性校验：直连（复用已 applyDownloadProxy 的 nam，绝不加 ghfast.top 镜像前缀）
            // 下载官方 <资源名>.sha256 边车，比对刚下好的安装包摘要。
            appendLog(QString::fromUtf8("正在校验安装包完整性..."));
            QNetworkRequest sreq{QUrl(sidecarUrl)};
            sreq.setRawHeader("User-Agent", "clashauto-cpp");
            sreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            sreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            sreq.setTransferTimeout(30000);
#endif
            QNetworkReply *sreply = nam->get(sreq);
            connect(sreply, &QNetworkReply::finished, dialog,
                    [this, dialog, sreply, progress, updateBtn, savePath, doExecute, onSidecarUnavailable] {
                const bool sok = sreply->error() == QNetworkReply::NoError;
                const QByteArray sdata = sreply->readAll();
                sreply->deleteLater();
                if (!sok || sdata.trimmed().isEmpty()) {
                    appendLog(QString::fromUtf8("校验文件下载失败"));
                    onSidecarUnavailable(); // 取不到边车按镜像/直连策略处置
                    return;
                }
                if (verifySha256(savePath, QString::fromUtf8(sdata))) {
                    appendLog(QString::fromUtf8("安装包完整性校验通过"));
                    doExecute();
                } else {
                    QFile::remove(savePath); // 删除可疑安装包，绝不执行
                    progress->hide();
                    updateBtn->setEnabled(true);
                    appendLog(QString::fromUtf8("安装包校验失败，可能被篡改，已取消"));
                    QMessageBox::warning(dialog, QString::fromUtf8("更新"),
                                         QString::fromUtf8("安装包校验失败，可能被篡改，已取消。"));
                }
            });
        });
    });
    // 资源过滤（对齐旧项目 formatAssets）：仅当前平台+架构；本项目命名 windows/linux/darwin + x64/arm64
    const QString plat =
#if defined(Q_OS_WIN)
        QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
        QStringLiteral("macos"); // 本项目 mac 资产命名 macos-universal（通用二进制 DMG）
#else
        QStringLiteral("linux");
#endif
    const QString arch = QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"))
                             ? QStringLiteral("arm64")
                             : QStringLiteral("x64");

    QNetworkRequest req(QUrl("https://api.github.com/repos/ClashrAuto/clashauto/releases"));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "clashauto-cpp");
    // 与 checkForUpdate 同配：跟随 301（组织/仓库改名）；加超时，避免网络不通时
    // 「更新说明」永远停在「正在获取...」。
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(15000);
#endif
    rel.body->setPlainText(QString::fromUtf8("正在获取..."));
    beta.body->setPlainText(QString::fromUtf8("正在获取..."));
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, dialog, [reply, rel, beta, plat, arch] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (!ok) {
            // 403 限流等情况 API 会带 message，比 Qt 的 errorString 更能说明问题
            const QString apiMsg = QJsonDocument::fromJson(body).object().value(QStringLiteral("message")).toString();
            const QString failText = QString::fromUtf8("获取失败: ")
                                     + (apiMsg.isEmpty() ? reply->errorString() : apiMsg);
            rel.body->setPlainText(failText);
            beta.body->setPlainText(failText);
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(body).array();
        auto fill = [plat, arch](const QJsonObject &r, const Refs &refs) {
            // VERSION 去字母（v0.1.81 → 0.1.81），对齐旧项目
            const QString tag = r.value("tag_name").toString();
            refs.ver->setText(QString::fromUtf8("VERSION: ")
                              + QString(tag).remove(QRegularExpression(QStringLiteral("[A-Za-z]"))));
            // 老版本 CI 没写 release notes（body 为空），给出明确占位而不是一片空白
            const QString notes = r.value("body").toString();
            refs.body->setPlainText(notes.trimmed().isEmpty()
                                        ? QString::fromUtf8("（此版本未附更新说明）")
                                        : notes);
            refs.assets->clear();
            const QJsonArray assets = r.value("assets").toArray();
            // 先全量登记 assetName → browser_download_url（含 .sha256 边车），供下载后按
            // <资源名>.sha256 查边车做完整性校验。本 release 内建表，避免 rel/beta 同名资源串味。
            QHash<QString, QString> urlByName;
            for (const QJsonValue &av : assets) {
                const QJsonObject a = av.toObject();
                urlByName.insert(a.value("name").toString(), a.value("browser_download_url").toString());
            }
            for (const QJsonValue &av : assets) {
                const QJsonObject a = av.toObject();
                const QString name = a.value("name").toString();
                if (!name.contains(plat, Qt::CaseInsensitive)
                    || !(name.contains(arch, Qt::CaseInsensitive)
                         || name.contains(QStringLiteral("universal"), Qt::CaseInsensitive))) {
                    continue; // 非当前平台/架构（universal=mac 通用二进制，任意架构可用）
                }
                if (name.endsWith(QStringLiteral(".yml"), Qt::CaseInsensitive)
                    || name.endsWith(QStringLiteral(".yaml"), Qt::CaseInsensitive)) {
                    continue;
                }
                if (name.endsWith(QStringLiteral(".sha256"), Qt::CaseInsensitive)) {
                    continue; // 校验边车，不作为可下载资源展示（仅供下方按名查表校验）
                }
                auto *item = new QListWidgetItem(name);
                item->setData(Qt::UserRole, a.value("browser_download_url").toString());
                // 边车（<资源名>.sha256）的官方直连 url，供下载后校验；缺失则为空（老版本无边车）
                item->setData(Qt::UserRole + 1, urlByName.value(name + QStringLiteral(".sha256")));
                refs.assets->addItem(item);
            }
            // 一键更新：自动选中推荐资源（优先 setup 安装包，支持静默安装+自动重启），
            // 弹窗一开「一键更新」即可点，无需先手选资源
            if (refs.assets->count() > 0) {
                int pick = 0;
                for (int i = 0; i < refs.assets->count(); ++i) {
                    if (refs.assets->item(i)->text().contains(QStringLiteral("setup"), Qt::CaseInsensitive)) {
                        pick = i;
                        break;
                    }
                }
                refs.assets->setCurrentRow(pick);
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

    // 「内核」tab：本地版本用 `mihomo -v` 探测（无内核则「未安装」），最新版本走 GitHub API
    QString localCoreVer;
    bool coreInstalled = false;
    {
        const QString exe = AppConfigLoader::load().clashExecutable();
        if (QFile::exists(exe)) {
            coreInstalled = true;
            QProcess p;
#if defined(Q_OS_WIN)
            p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
                a->flags |= 0x08000000u; // CREATE_NO_WINDOW：GUI 下静默执行，不闪控制台
            });
#endif
            p.start(exe, {QStringLiteral("-v")});
            // -v 即刻返回；3s 兜底防止损坏的二进制卡住 UI 线程
            if (p.waitForFinished(3000)) {
                // 输出形如 "Mihomo Meta v1.19.12 windows amd64 with go1.24 ..."，取 v 开头版本号
                const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("\\bv\\d+[0-9A-Za-z.\\-]*"))
                                                      .match(QString::fromUtf8(p.readAllStandardOutput()));
                if (m.hasMatch()) {
                    localCoreVer = m.captured(0);
                }
            } else {
                p.kill();
            }
        }
    }
    // 有内核但 -v 解析不出版本（损坏/超时）显示「未知」，与「未安装」区分开
    const QString localShown = !coreInstalled ? QString::fromUtf8("未安装")
                               : localCoreVer.isEmpty() ? QString::fromUtf8("未知")
                                                        : localCoreVer;
    coreVer->setText(QString::fromUtf8("内核版本: %1（正在查询最新版...）").arg(localShown));
    QNetworkRequest coreReq(QUrl(QStringLiteral("https://api.github.com/repos/MetaCubeX/mihomo/releases/latest")));
    coreReq.setRawHeader("Accept", "application/vnd.github+json");
    coreReq.setRawHeader("User-Agent", "clashauto-cpp");
    coreReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    coreReq.setTransferTimeout(10000);
#endif
    QNetworkReply *coreReply = nam->get(coreReq);
    connect(coreReply, &QNetworkReply::finished, dialog, [coreReply, coreVer, coreBody, localCoreVer, localShown] {
        const bool ok = coreReply->error() == QNetworkReply::NoError;
        const QByteArray body = coreReply->readAll();
        const QString err = coreReply->errorString();
        coreReply->deleteLater();
        if (!ok) {
            coreVer->setText(QString::fromUtf8("内核版本: %1（查询最新版失败）").arg(localShown));
            coreBody->setPlainText(QString::fromUtf8("获取失败: ") + err);
            return;
        }
        const QJsonObject r = QJsonDocument::fromJson(body).object();
        const QString tag = r.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty()) {
            // 多半是被限流/返回错误对象——把 message 展示出来便于排查
            const QString apiMsg = r.value(QStringLiteral("message")).toString();
            coreVer->setText(QString::fromUtf8("内核版本: %1（查询最新版失败）").arg(localShown));
            coreBody->setPlainText(QString::fromUtf8("获取失败: 未取到版本号%1")
                                       .arg(apiMsg.isEmpty() ? QString() : QString::fromUtf8("（%1）").arg(apiMsg)));
            return;
        }
        const bool newer = localCoreVer.isEmpty() || versionNewer(tag, localCoreVer);
        coreVer->setText(QString::fromUtf8("内核版本: %1 → 最新 %2 %3")
                             .arg(localShown, tag,
                                  newer ? QString::fromUtf8("（可更新）") : QString::fromUtf8("（已是最新）")));
        const QString notes = r.value(QStringLiteral("body")).toString();
        coreBody->setPlainText(notes.trimmed().isEmpty()
                                   ? QString::fromUtf8("（此版本未附更新说明）")
                                   : notes);
    });

    // 「不再提示」：勾选并关闭后，记住跳过当前正式版版本号，启动自动检查不再为该版本弹窗
    connect(dialog, &QDialog::finished, dialog, [noTip, rel] {
        if (!noTip->isChecked()) {
            return;
        }
        const QString t = rel.ver->text();
        const QString tag = t.startsWith(QStringLiteral("VERSION: ")) ? t.mid(9).trimmed() : QString();
        if (!tag.isEmpty() && tag != QStringLiteral("-")) {
            QSettings().setValue(QStringLiteral("update/skipTag"), tag);
        }
    });

    dialog->show();
}

bool MainWindow::verifySha256(const QString &filePath, const QString &expectedHexLower) const
{
    // 边车内容通常仅为十六进制摘要（可能带换行）；也兼容 "<hex>  <文件名>" 形式：取首个空白前的段。
    QString expected = expectedHexLower.trimmed();
    const qsizetype ws = expected.indexOf(QRegularExpression(QStringLiteral("\\s")));
    if (ws > 0) {
        expected = expected.left(ws);
    }
    if (expected.isEmpty()) {
        return false;
    }
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) { // 流式读取：数十 MB 安装包/内核不整体读入内存
        return false;
    }
    return QString::fromLatin1(h.result().toHex()).compare(expected, Qt::CaseInsensitive) == 0;
}

void MainWindow::applyDownloadProxy(QNetworkAccessManager *nam) const
{
    // 核心在跑 = 有可用代理：把该 QNAM 的请求经混合端口转发，GitHub 等被墙资源也能下到。
    // 核心没跑就保持直连（无从代理，例如首次「更新内核」时核心还不存在）。
    if (nam && m_core && m_core->isRunning()) {
        nam->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, m_proxyHost, static_cast<quint16>(m_proxyMixedPort)));
    }
}

void MainWindow::checkForUpdate(bool silent, int retriesLeft)
{
    auto *nam = new QNetworkAccessManager(this);
    applyDownloadProxy(nam); // 核心在跑则经代理查更新（墙内直连 api.github.com 也可能不通）
    QNetworkRequest req(QUrl("https://api.github.com/repos/ClashrAuto/clashauto/releases/latest"));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "clashauto-cpp");
    // GitHub 组织/仓库改名会以 301 跳转到新地址——必须跟随，否则拿到空响应而「查不到版本」。
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(10000); // 兜底：网络卡住最多等 10s，触发失败重试而不是永远无声
#endif
    appendLog(silent ? QString::fromUtf8("自动检查更新...") : QString::fromUtf8("正在检查更新..."));
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, silent, retriesLeft] {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // 失败一律记日志（便于排查「到底有没有检查」）；启动静默检查再自动重试几次，
            // 覆盖开机瞬间网络/DNS/代理尚未就绪导致的一次性失败。
            appendLog(QString::fromUtf8("检查更新失败: %1").arg(reply->errorString()));
            if (silent && retriesLeft > 0) {
                appendLog(QString::fromUtf8("将在 30 秒后重试检查更新（剩余 %1 次）").arg(retriesLeft));
                QTimer::singleShot(30000, this, [this, retriesLeft] { checkForUpdate(true, retriesLeft - 1); });
            } else if (!silent) {
                QMessageBox::warning(this, QString::fromUtf8("检查更新"),
                                     QString::fromUtf8("检查失败：%1").arg(reply->errorString()));
            }
            return;
        }
        const QJsonObject r = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = r.value(QStringLiteral("tag_name")).toString();
        const QString local = QString::fromUtf8(APP_VERSION);
        if (tag.isEmpty()) {
            // 多半是被限流/返回了错误对象——把 message 也记下来便于排查
            const QString apiMsg = r.value(QStringLiteral("message")).toString();
            appendLog(QString::fromUtf8("检查更新失败: 未取到版本号%1")
                          .arg(apiMsg.isEmpty() ? QString() : QString::fromUtf8("（%1）").arg(apiMsg)));
            if (!silent) {
                QMessageBox::warning(this, QString::fromUtf8("检查更新"),
                                     QString::fromUtf8("检查失败：未取到版本号"));
            }
            return;
        }
        if (versionNewer(tag, local)) {
            appendLog(QString::fromUtf8("发现新版本 %1（当前 %2）").arg(tag, local));
            if (m_versionLabel) {
                m_versionLabel->setText(QString("Clash Auto: <a href='update' style='color:red;font-weight:bold;text-decoration:none;'>%1 → %2 有新版本</a>").arg(local, tag));
                m_versionLabel->setToolTip(QString::fromUtf8("发现新版本 %1，点击查看/下载").arg(tag));
            }
            if (m_sidebarVersionLabel) { // 侧栏版本行变红并加上 ⬆，点击走同一检查/更新流程
                m_sidebarVersionLabel->setText(QString("<a href='update' style='color:#ff5252;font-weight:bold;text-decoration:none;'>Ver: %1 ⬆</a>").arg(local));
                m_sidebarVersionLabel->setToolTip(QString::fromUtf8("发现新版本 %1，点击查看/下载").arg(tag));
            }
            if (m_tray) {
                m_tray->notify(QString::fromUtf8("发现新版本"), QString::fromUtf8("%1 可更新（当前 %2）").arg(tag, local));
            }
            // 启动自动检查尊重「不再提示该版本」；手动检查（点击版本号）总是弹窗。
            // skipTag 存的是去字母版本号（与更新窗 VERSION 显示一致），比较前同样去字母。
            const QString skipKey = QString(tag).remove(QRegularExpression(QStringLiteral("[A-Za-z]")));
            if (silent && QSettings().value(QStringLiteral("update/skipTag")).toString() == skipKey) {
                return;
            }
            showUpdateDialog();
        } else {
            appendLog(QString::fromUtf8("已是最新版本 %1（服务器 %2）").arg(local, tag)); // 静默时也记，回答「到底查没查」
            if (!silent) {
                QMessageBox::information(this, QString::fromUtf8("检查更新"),
                                        QString::fromUtf8("已是最新版本 %1").arg(local));
            }
        }
    });
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
    if (m_logFile.isOpen()) {
        m_logFile.write(line.toUtf8());
        m_logFile.write("\n");
        m_logFile.flush(); // 逐条落盘：崩溃/被杀时日志不缺尾，也方便外部 tail
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

    // 只保留最近 100 条日志：末位是 addStretch() 的伸缩项（始终在 count-1），
    // 其余条目从新到旧排在它前面，最旧的一条即紧邻伸缩项的 count-2。
    // 条目数 = count-1，超出则从末尾（最旧）逐条移除并释放，避免长时间运行内存无限增长。
    constexpr int kMaxLogEntries = 100;
    while (layout->count() - 1 > kMaxLogEntries) {
        QLayoutItem *old = layout->takeAt(layout->count() - 2);
        if (!old) {
            break;
        }
        if (QWidget *w = old->widget()) {
            w->deleteLater();
        }
        delete old;
    }

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

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
#if defined(Q_OS_MACOS)
    // 只装在 QTabBar 上（构造函数末尾）。QTabBar 的 show/hide/Move 处理器会调
    // QTabBarPrivate::updateMacBorderMetrics → Cocoa registerContentBorderArea →
    // QCocoaWindow::applyContentBorderThickness；后者在无“内容边框渐变”（本应用恒无，
    // 没有 unified toolbar）时无条件执行 window.titlebarAppearsTransparent = NO，
    // 标题栏重新画出不透明底，盖住毛玻璃。事件过滤器先于目标控件处理事件，
    // 此刻破坏尚未发生，故排队到本轮事件处理完之后再重新断言（applyTitleBarColor 会
    // 依次调 configureMacTitleBar + enableMacBlur，均幂等：恢复 titlebarAppearsTransparent、
    // 透明底色与玻璃层）。用 m_macGlassReassertPending 合并同一轮内的多次触发。
    switch (event->type()) {
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::Move:
        if (qobject_cast<QTabBar *>(watched) && !m_macGlassReassertPending) {
            m_macGlassReassertPending = true;
            QMetaObject::invokeMethod(
                this,
                [this] {
                    m_macGlassReassertPending = false;
                    applyTitleBarColor();
                },
                Qt::QueuedConnection);
        }
        break;
    default:
        break;
    }
#endif
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    // 系统标题栏/边框由系统负责缩放与移动，这里不再拦截 WM_NCHITTEST
#if defined(Q_OS_WIN)
    // 交互式拖动/缩放期间只挂起「节点列表刷新」（整表布局+重建，叠在每步 resize 上才是真卡），
    // 结束后一次性补齐。流量图已改静态层缓存+共用定时器，逐帧只画一条折线，
    // 拖动/缩放期间照常滚动，不再暂停。
    if (eventType == "windows_generic_MSG") {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg && msg->message == WM_ERASEBKGND) {
            RECT rc;
            GetClientRect(msg->hwnd, &rc);
            // 纯拖动位置（在移动/缩放循环里且客户区尺寸没变）时「认领擦底但什么都不刷」：
            // backing store 完整覆盖客户区，紧随其后的 flush 会原样贴回内容；若先刷一层底色，
            // 软件合成（无 GPU 虚拟机）可能恰在「刷底」与「贴回」之间合成一帧——整窗内容
            // 闪现为纯色，即拖动位置时的「闪透/闪现」。拖出屏幕边缘再拖回时，系统对重新
            // 露出的条带逐步 WM_PAINT+擦底，每步都闪一次，此处跳过后由 flush 直接贴内容。
            if (m_inSizeMove
                && QSize(rc.right - rc.left, rc.bottom - rc.top) == m_sizeMoveEntrySize) {
                *result = 1;
                return true;
            }
            // 系统擦底默认用窗口类的 COLOR_WINDOW 白刷：拖大窗口/首帧/最大化还原时，
            // 新暴露的客户区在应用重绘跟上前是一片纯白（深色主题下即「白色背景板」）。
            // 改用主题底色擦除（与 applyTitleBarColor 的标题栏同色），白板消失。
            const bool light = (m_theme == "white");
            const COLORREF fill = light ? RGB(0xEE, 0xEE, 0xEE) : RGB(0x22, 0x22, 0x22);
            HBRUSH brush = CreateSolidBrush(fill);
            FillRect(reinterpret_cast<HDC>(msg->wParam), &rc, brush);
            DeleteObject(brush);
            *result = 1; // 告知系统已擦除，不再走默认白刷
            return true;
        }
        if (msg && (msg->message == WM_ENTERSIZEMOVE || msg->message == WM_EXITSIZEMOVE)) {
            m_inSizeMove = (msg->message == WM_ENTERSIZEMOVE);
            if (m_inSizeMove) {
                RECT rc;
                GetClientRect(msg->hwnd, &rc);
                m_sizeMoveEntrySize = QSize(rc.right - rc.left, rc.bottom - rc.top); // 「纯移动」判定基准
            }
            if (!m_inSizeMove && m_nodeResyncPending) {
                m_nodeResyncPending = false;
                if (!m_nodeSwitching) {
                    syncNodeRows(); // 拖动期间集合变了会在内部自动退回 populateNodeList
                } // 切换加载态中不动列表（会误恢复按钮态）；endNodeSwitch 完成时统一刷新
            }
        }
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
    // 提权(--tun-elevated)实例不注册：非提权主实例已注册过，提权上下文无需重复，
    // 且避免在管理员权限下做多余的注册表写入（缩小提权面）。
    if (QCoreApplication::arguments().contains(QStringLiteral("--tun-elevated"))) {
        return;
    }
    // 注册 clash-auto:// 协议到 HKCU（无需管理员）；每次启动幂等刷新指向当前 exe。
    // 用 QSettings 进程内直接写注册表，替代 spawn 三次 reg.exe——reg.exe 启动在 VM 上
    // 阻塞明显，且提权实例里以管理员跑同目录可能被劫持的 reg.exe 是提权面（与 applyAutoStart 同风格）。
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QSettings root("HKEY_CURRENT_USER\\Software\\Classes\\clash-auto", QSettings::NativeFormat);
    root.setValue(".", "URL:Clash Auto Protocol"); // 默认值(@)
    root.setValue("URL Protocol", "");
    QSettings cmd("HKEY_CURRENT_USER\\Software\\Classes\\clash-auto\\shell\\open\\command",
                  QSettings::NativeFormat);
    cmd.setValue(".", QLatin1Char('"') + exe + QLatin1String("\" \"%1\"")); // "<exe>" "%1"
#endif
}

QString MainWindow::extractCoreBinary(const QString &archivePath, const QString &tmpDir)
{
    const QString outDir = QDir(tmpDir).filePath("extract");
    QDir(outDir).removeRecursively();
    QDir().mkpath(outDir);
#if defined(Q_OS_WIN)
    // .zip：优先用系统自带 tar.exe（Win10 1803+/Win11），失败回退 PowerShell Expand-Archive
    int code = runHidden("tar", {"-xf", archivePath, "-C", outDir});
    if (code != 0) {
        code = runHidden("powershell", {"-NoProfile", "-Command",
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

void MainWindow::updateMihomoCore(QPushButton *btnIn, bool useMirror)
{
    // 按钮可能来自更新弹窗（WA_DeleteOnClose，下载中途关窗即销毁），异步回调里
    // 一律经 QPointer 判空访问，避免悬垂指针；流程本身继续在后台完成。
    const QPointer<QPushButton> btn(btnIn);
    btn->setEnabled(false);
    btn->setText(QString::fromUtf8("检查中..."));
    const AppConfig cfg = AppConfigLoader::load();
    // 国内加速：ghproxy 系镜像只代理下载（github.com 发布资源），不代理 api.github.com（返回 403），
    // 故仅给下载链接加镜像前缀；且镜像国内可直达，下载用「直连」QNAM，不套核心节点代理
    // （与更新弹窗「国内代理下载」一致）。查询版本仍走 nam（核心在跑经节点）。
    const QString mirror = useMirror ? QStringLiteral("https://ghfast.top/") : QString();
    auto restore = [btn] {
        if (btn) {
            btn->setEnabled(true);
            btn->setText(QString::fromUtf8("更新内核"));
        }
    };

    auto *nam = new QNetworkAccessManager(this);
    applyDownloadProxy(nam); // 若核心已在跑（更新内核而非首装），经代理拉取版本与下载
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/MetaCubeX/mihomo/releases/latest")));
    req.setRawHeader("User-Agent", "clashauto-cpp");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, btn, cfg, restore, mirror] {
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

        // 第三方 mihomo（MetaCubeX）若为该资产提供同名 .sha256 边车，则下载后直连校验（不加镜像）；
        // 上游不受我们控制，无边车不阻断内核更新（best effort）。
        QString coreSidecarUrl;
        const QString coreSidecarName = assetName + QStringLiteral(".sha256");
        for (const QJsonValue &av : assets) {
            if (av.toObject().value("name").toString() == coreSidecarName) {
                coreSidecarUrl = av.toObject().value("browser_download_url").toString();
                break;
            }
        }

        appendLog(QString::fromUtf8("下载 mihomo %1 ...%2").arg(tag, mirror.isEmpty() ? QString() : QString::fromUtf8("（国内加速）")));
        if (btn) {
            btn->setText(QString::fromUtf8("下载中..."));
        }
        QNetworkRequest dreq{QUrl(mirror + url)};
        dreq.setRawHeader("User-Agent", "clashauto-cpp");
        dreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        dreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false); // 同更新包：关 HTTP/2，避免大文件卡死
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        dreq.setTransferTimeout(30000); // 30s 无数据即失败，不永久卡「下载中」
#endif
        // 国内加速时直连镜像下载（不套节点代理）；挂在 nam 名下，随 nam deleteLater 一并销毁
        QNetworkAccessManager *dlNam = nam;
        if (!mirror.isEmpty()) {
            dlNam = new QNetworkAccessManager(nam);
            dlNam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        }
        QNetworkReply *dl = dlNam->get(dreq);
        if (btn) {
            connect(dl, &QNetworkReply::downloadProgress, btn.data(), [btn](qint64 r, qint64 t) {
                if (btn && t > 0) {
                    btn->setText(QString::fromUtf8("下载中 %1%").arg(int(r * 100 / t)));
                }
            });
        }
        connect(dl, &QNetworkReply::finished, this, [this, dl, nam, btn, cfg, tag, assetName, coreSidecarUrl, restore] {
            const bool ok2 = dl->error() == QNetworkReply::NoError;
            const QByteArray data = dl->readAll();
            const QString err2 = dl->errorString();
            dl->deleteLater();
            if (!ok2 || data.isEmpty()) {
                nam->deleteLater();
                restore();
                appendLog(QString::fromUtf8("内核下载失败: %1").arg(err2));
                return;
            }
            const QString tmpDir = QDir(cfg.userDir).filePath("core-update");
            QDir().mkpath(tmpDir);
            const QString archivePath = QDir(tmpDir).filePath(assetName);
            QFile f(archivePath);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                nam->deleteLater();
                restore();
                appendLog(QString::fromUtf8("内核更新失败: 无法写入临时文件"));
                return;
            }
            f.write(data);
            f.close();

            // 校验通过（或上游无边车）后：解压 → 停核心 → 替换二进制 → 恢复运行。
            auto doReplace = [this, cfg, tag, restore, tmpDir, archivePath, btn] {
                if (btn) {
                    btn->setText(QString::fromUtf8("安装中..."));
                }
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
            };

            // 上游未提供边车：跳过校验，照常替换（不因第三方缺失而阻断内核更新）。
            if (coreSidecarUrl.isEmpty()) {
                nam->deleteLater();
                appendLog(QString::fromUtf8("上游未提供校验文件，跳过完整性校验"));
                doReplace();
                return;
            }

            // 直连（复用已 applyDownloadProxy 的 nam，绝不加镜像前缀）下载 .sha256 边车并校验。
            if (btn) {
                btn->setText(QString::fromUtf8("校验中..."));
            }
            QNetworkRequest sreq{QUrl(coreSidecarUrl)};
            sreq.setRawHeader("User-Agent", "clashauto-cpp");
            sreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            sreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            sreq.setTransferTimeout(30000);
#endif
            QNetworkReply *sreply = nam->get(sreq);
            connect(sreply, &QNetworkReply::finished, this,
                    [this, sreply, nam, restore, tmpDir, archivePath, doReplace] {
                const bool sok = sreply->error() == QNetworkReply::NoError;
                const QByteArray sdata = sreply->readAll();
                sreply->deleteLater();
                nam->deleteLater();
                if (!sok || sdata.trimmed().isEmpty()) {
                    // 边车取不到：上游波动，不阻断——跳过校验继续替换。
                    appendLog(QString::fromUtf8("校验文件下载失败，跳过完整性校验"));
                    doReplace();
                    return;
                }
                if (verifySha256(archivePath, QString::fromUtf8(sdata))) {
                    appendLog(QString::fromUtf8("内核完整性校验通过"));
                    doReplace();
                } else {
                    QDir(tmpDir).removeRecursively(); // 删可疑压缩包，绝不替换
                    restore();
                    appendLog(QString::fromUtf8("内核更新失败: 校验不通过，可能被篡改，已取消"));
                }
            });
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
    // 安全校验：协议可被任意网页/程序触发，只接受 http/https 订阅链接，
    // 堵住 url=C:\...\敏感文件 或 file:// 之类的本地文件读取。
    const QString subScheme = QUrl(subUrl).scheme().toLower();
    if (subScheme != QLatin1String("http") && subScheme != QLatin1String("https")) {
        appendLog(QString::fromUtf8("拒绝导入非 http(s) 订阅链接: %1").arg(subUrl));
        return;
    }
    const QString name = q.queryItemValue("name", QUrl::FullyDecoded).trimmed();
    QString type = q.queryItemValue("type", QUrl::FullyDecoded).trimmed();
    if (type.isEmpty()) {
        type = "sub";
    }
    // 导入前弹确认框（避免外部链接静默添加订阅）；http 明文额外提示可被篡改。
    QString confirmText = QString::fromUtf8("是否导入来自链接的订阅：\n%1").arg(subUrl);
    if (subScheme == QLatin1String("http")) {
        confirmText += QString::fromUtf8("\n\n注意：该订阅使用明文 http，可能被网络篡改。");
    }
    if (QMessageBox::question(this, QString::fromUtf8("导入订阅"), confirmText,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        appendLog(QString::fromUtf8("用户取消导入订阅: %1").arg(subUrl));
        return;
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

bool MainWindow::nodeListTested() const
{
    // 「已测过」判定：列表里只要有一个节点测出了延迟(>0)，就说明这一轮刷新/测速已出结果。
    // testDelays 对整组并发测、全部完成后才 pollNodes 一次性上报，所以不会出现「部分已测部分未测」
    // 的中间态——据此把 delay==0 可靠地判为「测过但超时/不可用」，而非「还没测」。
    for (const NodeInfo &n : m_currentNodes) {
        if (n.delay > 0) {
            return true;
        }
    }
    return false;
}

bool MainWindow::nodeHidden(const NodeInfo &node, bool tested) const
{
    // 「仅可用节点」：仅在本轮已测出结果(tested)后，隐藏 delay<=0（超时/不可用）的节点；一次都没测出
    // 时（刚启动/刚切组）全部显示，避免列表空一片。当前活动节点永不隐藏，防止「正在用的节点消失」。
    return m_nodeOnlyAvailable && tested && node.delay <= 0 && !node.active;
}

void MainWindow::populateNodeList()
{
    if (!m_nodeList) {
        return;
    }
    auto *layout = qobject_cast<QVBoxLayout *>(m_nodeList->layout());
    // 保留滚动位置：整表重建会把滚动条重置到顶部，看着像「列表被清空又跳回去」
    QScrollBar *vbar = m_nodeScroll ? m_nodeScroll->verticalScrollBar() : nullptr;
    const int scrollPos = vbar ? vbar->value() : 0;
    m_delayBadges.clear(); // 下面会销毁旧行（含药丸），先清空登记（QPointer 亦会自动置空，双保险）
    m_nodeRows.clear();
    m_nodeButtons.clear();
    m_nodeNameLabels.clear();
    // 重建期间关闭重绘，整批完成后一次性刷新，避免列表清空→重填的闪烁
    m_nodeList->setUpdatesEnabled(false);
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QString filter = m_nodeSearch ? m_nodeSearch->text().trimmed() : QString();
    const bool tested = nodeListTested();
    int shown = 0;
    for (const NodeInfo &node : std::as_const(m_currentNodes)) {
        if (!filter.isEmpty() && !node.name.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        if (nodeHidden(node, tested)) {
            continue; // 「仅可用节点」：已测出结果后隐藏超时/不可用（delay<=0）的节点
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
    if (vbar) {
        vbar->setValue(scrollPos); // 还原滚动位置
    }

    if (m_nodeTitle) {
        const QString title = QString::fromUtf8("节点 <span style='font-size:9px'>(%1/%2)</span>")
                                  .arg(shown)
                                  .arg(m_currentNodes.size());
        if (m_nodeTitle->text() != title) {
            m_nodeTitle->setText(title); // 计数没变不 setText，避免每秒一次头部重布局
        }
    }
}

void MainWindow::beginNodeSwitch(const QString &target)
{
    if (m_nodeSwitching) {
        return;
    }
    m_nodeSwitching = true;
    m_nodeSwitchTarget = target;      // 该节点的按钮转圈
    m_nodeSwitchFrom = m_selectedNode; // 记录切换前的活动节点，用于确认「已切换」
    m_spinnerFrame = 0;
    if (m_spinnerTimer) {
        m_spinnerTimer->start(120);
    }
    // 延后重画：此刻正处在「被点按钮」的 clicked 槽内，直接 populateNodeList 会 delete 掉这个按钮
    // ——删除正在发信号的对象是未定义行为。放到下一轮事件循环再重画即安全（目标转圈、其余禁用）。
    QTimer::singleShot(0, this, [this] {
        if (m_nodeSwitching) {
            populateNodeList();
        }
    });
    // 兜底：6s 内未确认（PUT 失败/网络异常）自动解除，避免按钮永久卡在加载态
    QTimer::singleShot(6000, this, [this, target] {
        if (m_nodeSwitching && m_nodeSwitchTarget == target) {
            endNodeSwitch();
        }
    });
}

void MainWindow::endNodeSwitch()
{
    if (!m_nodeSwitching) {
        return;
    }
    m_nodeSwitching = false;
    m_nodeSwitchTarget.clear();
    m_nodeSwitchFrom.clear();
    m_spinnerButton = nullptr;
    if (m_spinnerTimer) {
        m_spinnerTimer->stop();
    }
    // 原地恢复：新活动节点置顶、按钮文字复位(应用/禁用)、全部可点——不销毁重建，切换完成也不闪现。
    // 若切换伴随节点增删（如「禁用」触发重建订阅），syncNodeRows 会自动退回 populateNodeList。
    syncNodeRows();
}

void MainWindow::updateNodeBadges()
{
    // 仅延迟/速度变化（节点集合与当前活动节点未变）时调用：只改药丸文字/颜色，不动任何行，
    // 因此列表不会清空、不闪烁、不跳滚动位置（对齐旧项目 Vue 的原地更新）。
    for (const NodeInfo &node : std::as_const(m_currentNodes)) {
        QLabel *badge = m_delayBadges.value(node.name).data();
        if (!badge) {
            continue; // 该节点被搜索过滤掉、或行已销毁（QPointer 置空）
        }
        // 先比较再赋值：QLabel::setText 不做等值短路（会 updateGeometry 引发整表重布局），
        // setStyleSheet 相同字符串也会重新 polish——数据没变时这两处每秒白做一次全表布局
        const QString text = QString("%1/%2").arg(speedText(node.speed),
                                                  node.delay == 0 ? QStringLiteral("-") : QString::number(node.delay));
        if (badge->text() != text) {
            badge->setText(text);
        }
        const QString style = QString("background:%1;").arg(delayColor(node.delay).name(QColor::HexArgb));
        if (badge->styleSheet() != style) {
            badge->setStyleSheet(style);
        }
    }
}

void MainWindow::syncNodeRows()
{
    // 节点集合不变（无增删、过滤条件不变）时的原地更新：更新每行的药丸/名称/按钮态，并把现有行按
    // m_currentNodes 的新次序重排——只「移动」已存在的行，绝不销毁重建，所以既完成排序又不闪现。
    if (!m_nodeList) {
        return;
    }
    auto *layout = qobject_cast<QVBoxLayout *>(m_nodeList->layout());
    if (!layout) {
        return;
    }

    const QString filter = m_nodeSearch ? m_nodeSearch->text().trimmed() : QString();
    const bool tested = nodeListTested();
    QVector<const NodeInfo *> desired; // 目标可见次序（已由 ClashService 按速度/延迟排好）
    for (const NodeInfo &n : std::as_const(m_currentNodes)) {
        if (nodeHidden(n, tested)) {
            continue; // 「仅可用节点」：与 populateNodeList 同一过滤，节点跨越可用边界时会触发整表重建
        }
        if (filter.isEmpty() || n.name.contains(filter, Qt::CaseInsensitive)) {
            desired.push_back(&n);
        }
    }
    // 可见集合与现有行对不上（增删节点，或极少见的行被意外销毁）→ 退回整表重建
    if (desired.size() != m_nodeRows.size()) {
        populateNodeList();
        return;
    }
    for (const NodeInfo *n : std::as_const(desired)) {
        if (!m_nodeRows.value(n->name)) {
            populateNodeList();
            return;
        }
    }

    // 关重绘，整批移动/改文字后一次性刷新，杜绝逐行移动时的中间态闪烁
    m_nodeList->setUpdatesEnabled(false);
    for (int i = 0; i < desired.size(); ++i) {
        const NodeInfo &node = *desired[i];
        QFrame *row = m_nodeRows.value(node.name).data();
        if (!row) {
            continue;
        }
        if (QLabel *badge = m_delayBadges.value(node.name).data()) {
            // 同 updateNodeBadges：先比较再赋值，数据没变不触发重布局/重新 polish
            const QString text = QString("%1/%2").arg(speedText(node.speed),
                                                      node.delay == 0 ? QStringLiteral("-") : QString::number(node.delay));
            if (badge->text() != text) {
                badge->setText(text);
            }
            const QString style = QString("background:%1;").arg(delayColor(node.delay).name(QColor::HexArgb));
            if (badge->styleSheet() != style) {
                badge->setStyleSheet(style);
            }
        }
        if (auto *nameLbl = static_cast<ElidingLabel *>(m_nodeNameLabels.value(node.name).data())) {
            QString display = node.name; // 组行：now 变了要更新「组名 → 使用节点」
            if (!node.now.isEmpty() && node.now != node.name) {
                display = QString::fromUtf8("%1 → %2").arg(node.name, node.now);
            }
            nameLbl->setFullText(display);
        }
        if (QPushButton *btn = m_nodeButtons.value(node.name).data()) {
            const QString label = node.active ? QString::fromUtf8("禁用") : QString::fromUtf8("应用");
            if (btn->text() != label) {
                btn->setText(label);
            }
            if (!btn->isEnabled()) {
                btn->setEnabled(true); // 非切换态：确保可点（切换态不会走到这里）
            }
        }
        if (row->property("active").toBool() != node.active) {
            row->setProperty("active", node.active); // 活动态变了，重刷样式（#nodeRow[active]）
            row->style()->unpolish(row);
            row->style()->polish(row);
        }
        if (layout->indexOf(row) != i) {
            layout->removeWidget(row); // 仅从布局摘下（不销毁、不改父子），移到目标次序
            layout->insertWidget(i, row);
        }
    }
    m_nodeList->setUpdatesEnabled(true);

    if (m_nodeTitle) {
        const QString title = QString::fromUtf8("节点 <span style='font-size:9px'>(%1/%2)</span>")
                                  .arg(desired.size())
                                  .arg(m_currentNodes.size());
        if (m_nodeTitle->text() != title) {
            m_nodeTitle->setText(title); // 同 populateNodeList：计数没变不 setText
        }
    }
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    // 除按钮/下拉/列表等交互控件（它们自行消费点击、不会上抛到这里）外，按住任意背景区域拖窗。
    if (event->button() == Qt::LeftButton && !isMaximized()) {
        // 链接标签例外（侧栏版本号、关于页链接等）：QLabel 开 LinksAccessibleByMouse 时 press 会被
        // ignore 上抛到这里，一旦进了系统移动模态循环，release 就被它吃掉，linkActivated 永远不触发
        // （点了没反应）。放行给默认处理：release 仍会派发回标签，完成链接点击。
        if (auto *lbl = qobject_cast<QLabel *>(childAt(event->pos()))) {
            if (lbl->textInteractionFlags() & Qt::LinksAccessibleByMouse) {
                QMainWindow::mousePressEvent(event);
                return;
            }
        }
        // 交给系统的模态移动循环（与拖系统标题栏同路径）：DWM 整体平移已合成的窗口表面，
        // 拖动全程客户区不重绘。此前逐 mouseMove 调 move()，每步 SetWindowPos 都触发
        // 擦底+重绘，内容闪出底色（拖动闪透）；窗口在指针下位移还会抖动 hover 态加剧闪烁。
        if (windowHandle() && windowHandle()->startSystemMove()) {
            event->accept();
            return; // 系统接管后不再派发后续 move/release，无需手动拖拽状态
        }
        m_dragging = true; // startSystemMove 不可用的平台退回手动拖拽
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
        QMainWindow { background:#222; }
        #root { background:#222; }
        #titleBar { background:#222; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#ccc; font-size:12px; padding-left:0; }
        #titleButton, #closeButton { color:#ccc; background:#222; border:0; border-radius:0; }
        #titleButton:hover { background:#333; }
        #closeButton:hover { background:red; color:white; }
        #body, #page { background:rgba(0,0,0,0); }
        #rightPane { background:#000; border-radius:5px; }
        #sidebar { background:#222; }
        #logo { color:#ffff00; background:transparent; min-width:80px; max-width:80px; min-height:80px; max-height:80px; font-size:70px; font-family:'iconfont'; }
        #logo[state="tun"] { color:#ff0000; }
        #logo[state="proxy"] { color:#ffff00; }
        #logo[state="off"] { color:#ffffff; }
        #menuButton { color:#fff; background:transparent; border:0; border-left:3px solid transparent; border-radius:5px 0 0 5px; text-align:left; padding-left:32px; font-size:14px; }
        #menuButton:hover { background:rgb(62,62,62); padding-left:36px; }
        #menuButton:checked { background:#000; color:#ccc; border-left:3px solid #4898f8; }
        #version { color:#666; font-size:12px; }
        #metricCard { background:#2a2a2a; border:0; border-radius:4px; min-height:70px; }
        #metricIcon { font-size:30px; color:#aaa; font-family:'iconfont'; }
        #metricTitle { color:#bfbfbf; font-size:12px; }
        #metricValue { color:#bfbfbf; font-size:18px; }
        #metricCard[kind="up"] QLabel { color:rgb(168,67,67); }
        #metricCard[kind="down"] QLabel { color:rgb(77,161,62); }
        #metricCard[kind="process"] QLabel { color:rgb(70,110,168); }
        #metricCard[kind="download"] QLabel { color:rgb(72,165,167); }
        #sectionTitle { color:#eee; font-size:18px; font-weight:400; }
        QScrollArea { background:transparent; border:0; }
        QScrollBar:vertical { background:transparent; width:8px; margin:0; }
        QScrollBar::handle:vertical { background:#555; border-radius:4px; min-height:24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; width:0; background:transparent; border:0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
        QScrollBar:horizontal { background:transparent; height:8px; margin:0; }
        QScrollBar::handle:horizontal { background:#555; border-radius:4px; min-width:24px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { height:0; width:0; background:transparent; border:0; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background:transparent; }
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
        #nodeScroll, #nodeScroll > QWidget > QWidget, #nodeListBody { background:#000; }
        #subScroll, #subScroll > QWidget > QWidget, #subListBody { background:#000; }
        #sysScroll, #sysScroll > QWidget > QWidget, #sysBody { background:#000; }
        #nodeRow, #plainCard { background:#252525; border:0; border-left:3px solid transparent; border-radius:4px; min-height:40px; }
        #nodeRow[active="true"] { background:rgba(72,151,248,0.69); border-left:3px solid #83bdff; }
        #nodeName { color:#ccc; font-size:12px; padding-left:5px; }
        #delayBadge { color:#222; border-radius:5px; padding:3px 5px; font-size:12px; }
        #nodeButton { color:#ccc; background:transparent; border:0; border-left:1px solid #000; border-radius:0; font-size:12px; }
        #nodeButton:hover { background:#333; }
        #primaryButton { color:white; background:#4898f8; border:0; border-radius:4px; min-height:30px; }
        #footer { background:#222; }
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
        QMainWindow { background:#eee; }
        #root { background:#eee; }
        #titleBar { background:#eee; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#333; font-size:12px; }
        #titleButton, #closeButton { color:#333; background:#eee; border:0; border-radius:0; }
        #titleButton:hover { background:#fff; }
        #closeButton:hover { background:red; color:white; }
        #body, #page { background:rgba(0,0,0,0); }
        #rightPane { background:#fff; border-radius:5px; }
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
        QScrollBar:vertical { background:transparent; width:8px; margin:0; }
        QScrollBar::handle:vertical { background:#ccc; border-radius:4px; min-height:24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; width:0; background:transparent; border:0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
        QScrollBar:horizontal { background:transparent; height:8px; margin:0; }
        QScrollBar::handle:horizontal { background:#ccc; border-radius:4px; min-width:24px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { height:0; width:0; background:transparent; border:0; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background:transparent; }
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
        #nodeScroll, #nodeScroll > QWidget > QWidget, #nodeListBody { background:#fff; }
        #subScroll, #subScroll > QWidget > QWidget, #subListBody { background:#fff; }
        #sysScroll, #sysScroll > QWidget > QWidget, #sysBody { background:#fff; }
        #nodeRow, #plainCard { background:#eee; border:0; border-left:3px solid transparent; border-radius:4px; min-height:40px; }
        #nodeRow[active="true"] { background:rgba(72,151,248,0.69); border-left:3px solid #1f6fd2; }
        #nodeName { color:#333; font-size:12px; padding-left:5px; }
        #delayBadge { color:#333; border-radius:5px; padding:3px 5px; font-size:12px; }
        #nodeButton { color:#333; background:transparent; border:0; border-left:1px solid #fff; border-radius:0; font-size:12px; }
        #nodeButton:hover { background:#c1c1c1; }
        #primaryButton { color:white; background:#4898f8; border:0; border-radius:4px; min-height:30px; }
        #footer { background:#eee; }
        #footerArrow { color:#999; font-size:14px; font-family:'iconfont'; }
        #footerLog { color:#333; font-size:12px; }
        #switchButton { color:#333; background:#fff; border:0; border-radius:3px; min-height:28px; padding-left:32px; padding-right:8px; font-size:12px; }
        #switchButton:hover { background:#f5f5f5; }
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
        #footer QComboBox { background:#fff; border:0; font-size:12px; }
    )";
}

void MainWindow::applyTheme(const QString &theme)
{
    const bool light = theme.compare("light", Qt::CaseInsensitive) == 0
                       || theme.compare("white", Qt::CaseInsensitive) == 0;
    m_theme = light ? "white" : "black";
    QString style = light ? lightStyle() : appStyle();
#if defined(Q_OS_MACOS)
    // 整窗毛玻璃：窗口底层垫着 NSVisualEffectView（enableMacBlur）。把「窗口壳」——主窗、
    // root、侧栏、页脚——的纯色底覆写成透明，玻璃从这些区域透出；主内容 #rightPane 保持
    // 基础样式表里的不透明纯色（深 #000 / 浅 #fff）+ 5px 圆角，成为浮在玻璃上的实底卡片。
    // 追加在基础样式表之后，同优先级下后者胜，Windows/Linux 不受影响。
    style += R"(
        QMainWindow { background:transparent; }
        #root { background:transparent; }
        #sidebar { background:transparent; }
        #footer { background:transparent; }
    )";
#endif
    setStyleSheet(style);
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
    // 标题栏背景色，与内容框同色：浅色 #eee / 深色 #222（COLORREF = 0x00BBGGRR）
    const COLORREF caption = light ? RGB(0xEE, 0xEE, 0xEE) : RGB(0x22, 0x22, 0x22);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    // 标题文字颜色
    const COLORREF text = light ? RGB(0x33, 0x33, 0x33) : RGB(0xEE, 0xEE, 0xEE);
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
#elif defined(Q_OS_MACOS)
    // 让系统标题栏透明、内容铺满整窗，再给整窗垫毛玻璃（NSVisualEffectView，BehindWindow）：
    // 标题栏与侧栏/页脚等透明区域透出磨砂桌面，主内容 #rightPane 保持不透明圆角卡片。
    // 玻璃深浅随应用主题（深色主题 → DarkAqua 深玻璃）。两个调用均幂等，showEvent 与
    // 每次主题切换重复调用是安全的。
    const bool light = (m_theme == "white");
    configureMacTitleBar(winId());
    enableMacBlur(winId(), !light);
#endif
}

void MainWindow::onToggleTunRequested()
{
    if (!m_core) {
        return;
    }
    const bool turningOn = !m_core->isTunEnabled();
    // 未预装内核：开启增强前先确保内核已下载，否则提权重启也没有核心可跑，先引导下载
    if (turningOn && !m_core->isCoreInstalled()) {
        promptDownloadCore();
        return;
    }
#if defined(Q_OS_WIN)
    // 增强(TUN) 需要管理员权限创建虚拟网卡：若正要开启且当前非提权，先以管理员身份重启自身
    // （对齐旧项目 runAsRoot → restart 的「按需提权」）。关闭 TUN 或已提权时走正常热重载。
    if (turningOn && !isProcessElevated()) {
        relaunchElevatedForTun();
        return;
    }
    m_core->toggleTun();
#elif defined(Q_OS_MACOS)
    // macOS：TUN 需要以 root 运行 mihomo（建 utun/改路由）→ 交给特权 helper。开启前先确保 helper
    // 已注册并启用；未批准则引导去「系统设置 → 登录项」，本次不开启。
    if (turningOn) {
        MacHelper::RegStatus st = MacHelper::status();
        if (st != MacHelper::RegStatus::Enabled) {
            QString err;
            st = MacHelper::registerDaemon(&err);
            if (st == MacHelper::RegStatus::RequiresApproval) {
                MacHelper::openLoginItemsSettings();
                appendLog(QString::fromUtf8("增强模式需在「系统设置 → 登录项」允许 Clash Auto 的后台项，允许后再开启"));
                if (m_tray) {
                    m_tray->notify(QString::fromUtf8("增强模式"),
                                   QString::fromUtf8("请在系统设置里允许后台项后重试"));
                }
                return; // 待批准，本次不开启
            }
            if (st != MacHelper::RegStatus::Enabled) {
                appendLog(QString::fromUtf8("增强模式：特权 helper 不可用：%1").arg(err));
                return;
            }
        }
        // helper 已就绪。若核心正以普通(非 root)方式在跑，切到 helper 后端才能建 utun：先停旧核心，
        // toggleTun 写好 TUN 配置后，startCore 会以 helper(root) 起。
        if (m_core->isRunning() && !m_core->isHelperCore()) {
            m_core->stopCore();
        }
        m_core->toggleTun();
        if (!m_core->isRunning()) {
            m_core->startCore();
        }
    } else {
        m_core->toggleTun(); // 关 TUN：helper-core 在跑则热重载去掉 utun
    }
#else
    m_core->toggleTun();
#endif
    // 切换即落盘：重启/一键更新后按 use: 恢复增强状态
    persistConfigBool(QStringLiteral("use"), m_core->isTunEnabled());
}

void MainWindow::onToggleProxyRequested()
{
    // 网页(系统代理)开关统一入口（底部开关 + 托盘共用）：切换后立即把状态写入 config 的
    // web:，重启/一键更新后按其恢复；并同步设置页「系统代理」勾选，避免「应用」用旧值写回。
    if (!m_core) {
        return;
    }
    m_core->toggleProxy();
    persistConfigBool(QStringLiteral("web"), m_core->isProxyEnabled());
    if (m_webProxyCheck) {
        m_webProxyCheck->setChecked(m_core->isProxyEnabled());
    }
}

#if defined(Q_OS_MACOS)
void MainWindow::refreshMacHelperStatus()
{
    if (!m_helperStatusLabel) {
        return;
    }
    QString text;
    switch (MacHelper::status()) {
        case MacHelper::RegStatus::Enabled:          text = QString::fromUtf8("已启用（代理/增强免密）"); break;
        case MacHelper::RegStatus::RequiresApproval: text = QString::fromUtf8("待批准：请在系统设置→登录项允许"); break;
        case MacHelper::RegStatus::NotRegistered:    text = QString::fromUtf8("未安装"); break;
        case MacHelper::RegStatus::NotFound:         text = QString::fromUtf8("不可用（bundle 布局异常）"); break;
        default:                                     text = QString::fromUtf8("未知"); break;
    }
    m_helperStatusLabel->setText(text);
}

void MainWindow::installMacHelper()
{
    QString err;
    const MacHelper::RegStatus st = MacHelper::registerDaemon(&err);
    refreshMacHelperStatus();
    if (st == MacHelper::RegStatus::Enabled) {
        appendLog(QString::fromUtf8("免密助手已启用"));
        if (m_tray) {
            m_tray->notify(QString::fromUtf8("免密助手"),
                           QString::fromUtf8("已启用，代理/增强模式将不再要求密码"));
        }
    } else if (st == MacHelper::RegStatus::RequiresApproval) {
        MacHelper::openLoginItemsSettings();
        appendLog(QString::fromUtf8("免密助手需在「系统设置 → 登录项」允许 Clash Auto 的后台项，批准后自动生效"));
        startMacHelperApprovalWatch();
    } else {
        appendLog(QString::fromUtf8("免密助手安装失败：%1").arg(err));
    }
}

void MainWindow::uninstallMacHelper()
{
    QString err;
    if (MacHelper::unregisterDaemon(&err)) {
        appendLog(QString::fromUtf8("免密助手已卸载"));
    } else {
        appendLog(QString::fromUtf8("免密助手卸载失败：%1").arg(err));
    }
    if (m_helperApprovalTimer) {
        m_helperApprovalTimer->stop();
    }
    refreshMacHelperStatus();
}

void MainWindow::startMacHelperApprovalWatch()
{
    // 引导用户去系统设置批准后，我们收不到回调——轮询状态，变为已启用即提示（最多盯 ~2 分钟）
    if (!m_helperApprovalTimer) {
        m_helperApprovalTimer = new QTimer(this);
        m_helperApprovalTimer->setInterval(2000);
        connect(m_helperApprovalTimer, &QTimer::timeout, this, [this] {
            refreshMacHelperStatus();
            if (MacHelper::status() == MacHelper::RegStatus::Enabled) {
                m_helperApprovalTimer->stop();
                appendLog(QString::fromUtf8("免密助手已获批准并启用"));
                if (m_tray) {
                    m_tray->notify(QString::fromUtf8("免密助手"),
                                   QString::fromUtf8("已启用，现在可开启增强模式（免密）"));
                }
            } else if (++m_helperApprovalTicks >= 60) {
                m_helperApprovalTimer->stop();
            }
        });
    }
    m_helperApprovalTicks = 0;
    m_helperApprovalTimer->start();
}
#endif

#if defined(Q_OS_WIN)
bool MainWindow::isProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool MainWindow::relaunchElevatedForTun()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const std::wstring exeW = exe.toStdWString();
    const std::wstring paramsW = L"--tun-elevated";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    // NOASYNC：本线程随后即退出，必须等 shell 操作（含 UAC）完成再返回，否则可能启动不了
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas"; // 触发 UAC 以管理员身份运行
    sei.lpFile = exeW.c_str();
    sei.lpParameters = paramsW.c_str();
    sei.nShow = SW_SHOWNORMAL;

    // 先落盘 use:true：提权实例启动即读 config，保证它按「增强开启」恢复；取消授权时下面回滚
    persistConfigBool(QStringLiteral("use"), true);
    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            CloseHandle(sei.hProcess);
        }
        // 提权实例已启动：立即硬杀本(非提权)核心并还原系统代理，把 9090/代理让给新实例，再退出。
        m_core->killCoreNow();
        qApp->quit();
        return true;
    }
    persistConfigBool(QStringLiteral("use"), false); // 未提权成功：回滚，避免下次启动误弹 UAC「恢复」
    const DWORD err = GetLastError();
    if (err == ERROR_CANCELLED) {
        appendLog(QString::fromUtf8("增强(TUN) 需要管理员权限：已取消授权，未开启"));
        if (m_tray) {
            m_tray->notify(QString::fromUtf8("增强模式"),
                           QString::fromUtf8("需要管理员权限，已取消授权，未开启"));
        }
    } else {
        appendLog(QString::fromUtf8("增强(TUN) 提权失败（错误码 %1）").arg(err));
    }
    return false;
}

void MainWindow::launchSilentUpdateAndRestart(const QString &installerPath)
{
    // 一键更新收尾：交给隐藏的 PowerShell 守护进程——等本进程退出（释放 exe 占用）→
    // NSIS 静默安装（/S，/D= 装回当前安装目录，安装版/便携版都原地升级）→ 重新拉起新版。
    // 增强/网页开关状态已随每次切换写入 config（use:/web:），新实例启动时自动恢复。
    QDir exeDir(QCoreApplication::applicationDirPath());
    QString installRoot;
    if (exeDir.dirName() == QStringLiteral("clashauto-c++")) {
        QDir root = exeDir;
        root.cdUp();
        installRoot = root.absolutePath(); // 标准布局（<root>/clashauto-c++/exe）：原地升级
    } else {
        // 非常规布局（如开发目录）：装到安装包默认位置
        installRoot = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(QStringLiteral("ClashAuto"));
    }
    const QString newExe = QDir(installRoot).filePath(QStringLiteral("clashauto-c++/clashauto-cpp.exe"));
    const auto psq = [](const QString &s) { // PowerShell 单引号串：内嵌单引号翻倍即可，无其他转义
        QString r = QDir::toNativeSeparators(s);
        r.replace(QLatin1Char('\''), QStringLiteral("''"));
        return r;
    };
    // 用 ProcessStartInfo.Arguments 原样传参：NSIS 要求 /D= 是最后一个参数且路径不能带引号，
    // Start-Process 的 -ArgumentList 会给含空格参数自动加引号，故不能用。
    const QString script = QStringLiteral(
        "Wait-Process -Id %1 -Timeout 60 -ErrorAction SilentlyContinue\n"
        "$psi = New-Object System.Diagnostics.ProcessStartInfo\n"
        "$psi.FileName = '%2'\n"
        "$psi.Arguments = '/S /D=%3'\n"
        "$psi.UseShellExecute = $false\n"
        "[System.Diagnostics.Process]::Start($psi).WaitForExit()\n"
        "Start-Process -FilePath '%4'\n")
        .arg(QString::number(QCoreApplication::applicationPid()),
             psq(installerPath), psq(installRoot), psq(newExe));
    // -EncodedCommand（UTF-16LE Base64）：中文/空格路径无需再转义，也不落临时脚本文件
    const QByteArray utf16(reinterpret_cast<const char *>(script.utf16()),
                           static_cast<qsizetype>(script.size()) * 2);
    const QString params = QStringLiteral("-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand ")
        + QString::fromLatin1(utf16.toBase64());
    const std::wstring fileW = L"powershell.exe";
    const std::wstring paramsW2 = params.toStdWString();

    SHELLEXECUTEINFOW sei2{};
    sei2.cbSize = sizeof(sei2);
    sei2.fMask = SEE_MASK_NOASYNC;
    sei2.lpFile = fileW.c_str();
    sei2.lpParameters = paramsW2.c_str();
    sei2.nShow = SW_HIDE; // 全程隐藏 PowerShell 窗口
    if (ShellExecuteExW(&sei2)) {
        qApp->quit(); // aboutToQuit 停核心并还原系统代理，守护进程随后接手安装+重启
    } else {
        // 守护进程起不来：退回旧行为——打开安装包由用户手动安装
        appendLog(QString::fromUtf8("自动安装启动失败（错误码 %1），已打开安装包请手动安装").arg(GetLastError()));
        QDesktopServices::openUrl(QUrl::fromLocalFile(installerPath));
        qApp->quit();
    }
}
#endif

#if defined(Q_OS_MACOS)
void MainWindow::launchSilentUpdateAndRestartMac(const QString &dmgPath)
{
    // 一键更新收尾：写一个分离运行的 shell 脚本——等本进程退出 → 挂载 DMG → ditto 覆盖当前
    // .app 包 → 卸载 → 去隔离标记 → 重新拉起新版。增强/网页开关每次切换已落盘（config 的
    // use:/web:），新实例启动时自动恢复。
    // 当前 .app 路径：applicationDirPath = <bundle>.app/Contents/MacOS，上溯两级得包根。
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp(); // Contents
    dir.cdUp(); // <bundle>.app
    const QString appBundle = dir.absolutePath();
    if (!appBundle.endsWith(QStringLiteral(".app"))) {
        // 非打包运行（开发目录）：没有可替换的 .app，退回打开 DMG 手动安装
        appendLog(QString::fromUtf8("非 .app 方式运行，改为打开安装包手动安装"));
        QDesktopServices::openUrl(QUrl::fromLocalFile(dmgPath));
        qApp->quit();
        return;
    }

    // shell 单引号转义：整体包成 '...'，内嵌单引号写成 '\'' 收尾再续。
    const auto shq = [](const QString &s) {
        QString r = s;
        r.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QLatin1Char('\'') + r + QLatin1Char('\'');
    };

    // do_replace 只做「挂载 DMG → ditto 覆盖 .app → 卸载」这一步。普通身份先试；失败（如
    // /Applications 里的包归 root）就用 osascript 以管理员身份只重跑 do_replace（`$0 replace`）。
    // 关键：等待旧进程退出、去隔离、open 重启这些都留在普通身份实例里做——绝不能让 open 跑在
    // 管理员(root)身份下，否则新 app 会以 root 启动。
    const QString script = QStringLiteral(
        "#!/bin/sh\n"
        "PID=%1\n"
        "DMG=%2\n"
        "APP=%3\n"
        "do_replace() {\n"
        "  MNT=$(mktemp -d /tmp/clashauto-upd.XXXXXX)\n"
        "  hdiutil attach \"$DMG\" -nobrowse -noverify -mountpoint \"$MNT\" >/dev/null 2>&1 || return 2\n"
        "  SRC=$(ls -d \"$MNT\"/*.app 2>/dev/null | head -n1)\n"
        "  RC=1\n"
        "  if [ -n \"$SRC\" ] && rm -rf \"$APP\" 2>/dev/null && ditto \"$SRC\" \"$APP\" 2>/dev/null; then RC=0; fi\n"
        "  hdiutil detach \"$MNT\" >/dev/null 2>&1\n"
        "  return $RC\n"
        "}\n"
        "if [ \"$1\" = replace ]; then do_replace; exit $?; fi\n"
        "i=0; while kill -0 \"$PID\" 2>/dev/null; do sleep 0.5; i=$((i+1)); [ $i -ge 120 ] && break; done\n"
        "if ! do_replace; then\n"
        "  osascript -e \"do shell script \\\"/bin/sh '$0' replace\\\" with administrator privileges\" || { open \"$DMG\"; exit 1; }\n"
        "fi\n"
        "xattr -dr com.apple.quarantine \"$APP\" 2>/dev/null\n"
        "open -n \"$APP\"\n"
        "rm -f \"$0\"\n")
        .arg(QString::number(QCoreApplication::applicationPid()), shq(dmgPath), shq(appBundle));

    const QString scriptPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(QStringLiteral("clashauto-update.sh"));
    QFile f(scriptPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog(QString::fromUtf8("无法写更新脚本，改为打开安装包手动安装"));
        QDesktopServices::openUrl(QUrl::fromLocalFile(dmgPath));
        qApp->quit();
        return;
    }
    f.write(script.toUtf8());
    f.close();

    // 分离启动：脱离本进程，本进程退出后脚本继续跑（等待→安装→重启）。
    if (QProcess::startDetached(QStringLiteral("/bin/sh"), {scriptPath})) {
        qApp->quit(); // aboutToQuit 停核心并还原系统代理，脚本随后接手安装+重启
    } else {
        appendLog(QString::fromUtf8("自动安装启动失败，已打开安装包请手动安装"));
        QDesktopServices::openUrl(QUrl::fromLocalFile(dmgPath));
        qApp->quit();
    }
}
#endif

void MainWindow::promptDownloadCore()
{
    if (m_coreMissingPrompted) {
        return; // 防重入：模态框已开着时别再叠一个
    }
    if (m_core && m_core->isCoreInstalled()) {
        return; // 期间内核已就位，无需提示
    }
    m_coreMissingPrompted = true;
    const auto choice = QMessageBox::question(
        this,
        QString::fromUtf8("未检测到内核"),
        QString::fromUtf8("未检测到 mihomo 内核，无法启动核心。\n是否前往「设置 → 系统」下载内核？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    m_coreMissingPrompted = false;
    if (choice == QMessageBox::Yes) {
        goToCoreDownload();
    }
}

void MainWindow::goToCoreDownload()
{
    setCurrentPage(2); // 设置页（0状态 / 1订阅 / 2设置 / 3日志 / 4关于）
    if (m_settingsTabs) {
        m_settingsTabs->setCurrentIndex(m_settingsTabs->count() - 1); // 系统 tab（最后一个）
    }
    if (m_coreUpdateBtn && m_sysScroll) {
        // 等 tab 布局完成后再滚动到「更新内核」按钮并聚焦，帮助用户定位
        QTimer::singleShot(0, this, [this] {
            m_sysScroll->ensureWidgetVisible(m_coreUpdateBtn);
            m_coreUpdateBtn->setFocus(Qt::OtherFocusReason);
        });
    }
}
