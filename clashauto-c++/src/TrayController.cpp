#include "TrayController.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTimer>

namespace {
// 画「单独的地球」托盘图标：size×size 透明底，地球字形(U+E600)填 globeColor；
// badge 非空则在右下角叠一个白色圆角方块，中间写字母（增强 T / 网页 W / 核心开 C / 核心关 N）。
QPixmap renderTrayGlobe(int size, const QColor &globeColor, const QString &badge)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    QFont gf(QStringLiteral("iconfont"));
    gf.setPixelSize(qRound(size * 0.94));
    // 按字形【实际墨迹】居中：AlignCenter 只按字体度量盒居中，本地球字形墨迹在盒内偏右~2%，
    // 会显得没居中；用 tightBoundingRect 量出墨迹，令其正中对齐图标中心。
    const QString glyph(QChar(char16_t(0xE600)));
    const QRectF ink = QFontMetricsF(gf).tightBoundingRect(glyph);
    p.setFont(gf);
    p.setPen(globeColor);
    p.drawText(QPointF(size / 2.0 - ink.x() - ink.width() / 2.0,
                       size / 2.0 - ink.y() - ink.height() / 2.0),
               glyph);

    if (!badge.isEmpty()) {
        const qreal bs = size * 0.5; // 角标边长（约图标一半），叠在右下
        const QRectF br(size - bs, size - bs, bs, bs);
        QPainterPath bp;
        bp.addRoundedRect(br, bs * 0.28, bs * 0.28);
        p.fillPath(bp, Qt::white);
        QFont bf(QStringLiteral("MiSans"));
        bf.setPixelSize(qRound(bs * 0.66));
        bf.setBold(true);
        p.setFont(bf);
        p.setPen(QColor(0x48, 0x98, 0xF8));
        p.drawText(br, Qt::AlignCenter, badge);
    }
    p.end();
    return pm;
}
} // namespace

#if defined(Q_OS_MACOS)
#include "MacSpeedItem.h" // 原生托盘（图标+两行速率合成一项 + 原生菜单），取代 Qt 托盘项
// C 回调 → 转到 TrayController 成员（ctx 即 this）。
static void macCbOpen(void *c) { static_cast<TrayController *>(c)->macOpenWindow(); }
static void macCbCore(void *c) { static_cast<TrayController *>(c)->macToggleCore(); }
static void macCbProxy(void *c) { static_cast<TrayController *>(c)->macToggleProxy(); }
static void macCbTun(void *c) { static_cast<TrayController *>(c)->macToggleTun(); }
static void macCbQuit(void *c) { static_cast<TrayController *>(c)->macQuit(); }
#endif

TrayController::TrayController(MainWindow *window, QObject *parent)
    : QObject(parent), m_window(window)
{
#if defined(Q_OS_MACOS)
    // macOS 不用 Qt 托盘：改用原生一项（图标在左、两行速率在右、定宽不抖动 + 原生菜单）。
    // 先把图标/初始状态喂给原生层缓存（这些调用不要求状态项已存在），NSStatusItem 本体
    // 推迟到事件循环第一拍再创建：本构造函数跑在 app.exec() 之前（Cocoa 尚未完成启动），
    // 此时创建的状态项可能不会被真正排进菜单栏；旧版（分离的纯文字项）也是构造期创建、
    // 但要等运行期 setVisible(YES) 才现身。singleShot(0) 复刻这一已知可用的时序，
    // 配合原生层安装时显式 visible=YES（还破除旧版持久化进 defaults 的隐藏态）。
    updateMacTrayIcon(); // mac 托盘用白色地球(+状态角标) PNG 喂原生层（先缓存，install 时渲染）
    macTraySetStatus(false, false, false);
    QTimer::singleShot(0, this, [this] {
        MacTrayHandlers h{this, &macCbOpen, &macCbCore, &macCbProxy, &macCbTun, &macCbQuit};
        macTrayInstall(h); // 幂等；安装时按缓存的图标/状态/速率渲染
    });
#else
    refreshIcon(); // 初始（核心未起）用原色图标
    m_tray.setToolTip("Clash Auto");
    connect(&m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            emit openWindowRequested(); // QML 版：main_qml 连到 QQuickWindow 重开
            if (m_window) {
                m_window->showNormal();
                m_window->raise();
                m_window->activateWindow();
            }
        }
    });
    buildMenu();
    m_tray.show();
#endif
}

#if defined(Q_OS_MACOS)
void TrayController::macOpenWindow()
{
    emit openWindowRequested(); // QML 版：main_qml 连到 QQuickWindow 重开（mac 菜单栏「控制面板」）
    if (m_window) {
        m_window->showNormal();
        m_window->raise();
        m_window->activateWindow();
    }
}
void TrayController::macToggleCore() { emit toggleCoreRequested(); }
void TrayController::macToggleProxy() { emit toggleProxyRequested(); }
void TrayController::macToggleTun() { emit toggleTunRequested(); }
void TrayController::macQuit() { qApp->quit(); }
#endif

void TrayController::buildMenu()
{
    // 菜单只建这一次，之后 setStatus/setTraffic 只改各行 QAction 的文本。
    // 此前流量每秒 rebuildMenu()：new QMenu 换给 setContextMenu（它不接管旧菜单
    // 所有权），旧菜单+全部 QAction 每秒泄漏一份；用户正开着菜单时还会被整个换掉。
    m_panelAction = m_menu.addAction(tr("控制面板"), this, [this] {
        emit openWindowRequested(); // QML 版：main_qml 连到 QQuickWindow 重开
        if (m_window) {
            m_window->showNormal();
            m_window->raise();
            m_window->activateWindow();
        }
    });
    m_menu.addSeparator();
    m_upAction = m_menu.addAction(QString("UP: %1").arg(speedText(0)));
    m_upAction->setEnabled(false);
    m_downAction = m_menu.addAction(QString("DOWN: %1").arg(speedText(0)));
    m_downAction->setEnabled(false);
    m_menu.addSeparator();
    m_coreAction = m_menu.addAction(tr("启动核心"), this, &TrayController::toggleCoreRequested);
    m_proxyAction = m_menu.addAction(tr("打开网页代理"), this, &TrayController::toggleProxyRequested);
    m_tunAction = m_menu.addAction(tr("打开增强模式"), this, &TrayController::toggleTunRequested);
    m_menu.addSeparator();
    m_quitAction = m_menu.addAction(tr("退出程序"), qApp, &QApplication::quit);
    m_tray.setContextMenu(&m_menu);
}

void TrayController::setStatus(bool tun, bool proxy, bool core)
{
    // emitStatus 会在启动/停止/重载等多个节点重复发同一状态；没变就不碰托盘
    // （setIcon/setToolTip 是到 shell 的往返，重复调用会让图标肉眼可见地闪）。
    if (m_statusKnown && tun == m_tun && proxy == m_proxy && core == m_core) {
        return;
    }
    m_statusKnown = true;
    m_tun = tun;
    m_proxy = proxy;
    m_core = core;
#if defined(Q_OS_MACOS)
    macTraySetStatus(tun, proxy, core); // 原生托盘：更新菜单项文字 + 核心在跑才显示速率
    updateMacTrayIcon();                 // 状态变了重画白色地球 + 角标（T/W/C/N）
#else
    m_tray.setToolTip(QString("Clash Auto - %1").arg(core ? tr("运行中") : tr("已停止")));
    refreshIcon(); // 状态变了刷新图标颜色（增强=红、核心在跑=黄）
    m_coreAction->setText(core ? tr("停止核心") : tr("启动核心"));
    m_proxyAction->setText(proxy ? tr("关闭网页代理") : tr("打开网页代理"));
    m_tunAction->setText(tun ? tr("关闭增强模式") : tr("打开增强模式"));
#endif
}

// 语言切换后重刷托盘菜单/状态文案（托盘在翻译器安装前就已构造，故启动后与切换时都需调）。
void TrayController::retranslate()
{
#if defined(Q_OS_MACOS)
    // 原生托盘：重刷动态项文案（控制面板/退出程序等静态项由 macTrayInstall 建时定，切换语言后需重启生效）。
    macTraySetStatus(m_tun, m_proxy, m_core);
#else
    if (m_panelAction) m_panelAction->setText(tr("控制面板"));
    if (m_quitAction) m_quitAction->setText(tr("退出程序"));
    if (m_coreAction) m_coreAction->setText(m_core ? tr("停止核心") : tr("启动核心"));
    if (m_proxyAction) m_proxyAction->setText(m_proxy ? tr("关闭网页代理") : tr("打开网页代理"));
    if (m_tunAction) m_tunAction->setText(m_tun ? tr("关闭增强模式") : tr("打开增强模式"));
    m_tray.setToolTip(QString("Clash Auto - %1").arg(m_core ? tr("运行中") : tr("已停止")));
#endif
}

void TrayController::refreshIcon()
{
    // 托盘图标 = 单独的蓝色(#4898F8)地球 + 右下状态角标（增强 T / 网页 W / 核心开 C / 核心关 N）。
    const QString badge = m_tun ? QStringLiteral("T")
                        : m_proxy ? QStringLiteral("W")
                        : m_core ? QStringLiteral("C")
                        : QStringLiteral("N");
    m_tray.setIcon(QIcon(renderTrayGlobe(64, QColor(0x48, 0x98, 0xF8), badge)));
}

#if defined(Q_OS_MACOS)
void TrayController::updateMacTrayIcon()
{
    // mac：单独的白色地球 + 右下状态角标（T/W/C/N），转 PNG 喂原生托盘层。
    const QString badge = m_tun ? QStringLiteral("T")
                        : m_proxy ? QStringLiteral("W")
                        : m_core ? QStringLiteral("C")
                        : QStringLiteral("N");
    const QPixmap pm = renderTrayGlobe(64, QColor(Qt::white), badge);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    pm.save(&buf, "PNG");
    buf.close();
    if (!png.isEmpty())
        macTraySetIconPng(png.constData(), static_cast<long>(png.size()));
}
#endif

void TrayController::setTraffic(qint64 up, qint64 down)
{
    if (up == m_up && down == m_down) {
        return; // 空闲时常年 0/0，一秒一改毫无必要
    }
    m_up = up;
    m_down = down;
#if defined(Q_OS_MACOS)
    // 原生托盘：两行速率（上=上传、下=下载，无 ↑↓ 标识），图标固定不抖。
    macTraySetSpeed(speedTextCompact(up), speedTextCompact(down));
#else
    m_upAction->setText(QString("UP: %1").arg(speedText(up)));
    m_downAction->setText(QString("DOWN: %1").arg(speedText(down)));
#endif
}

void TrayController::notify(const QString &title, const QString &message)
{
#if defined(Q_OS_MACOS)
    macTrayNotify(title, message); // 原生气泡（不再走 Qt 托盘，因 macOS 已改原生托盘）
#else
    if (QSystemTrayIcon::supportsMessages()) {
        m_tray.showMessage(title, message, QSystemTrayIcon::Information, 3000);
    }
#endif
}

void TrayController::reinitForNotifications()
{
#if defined(Q_OS_MACOS)
    // macOS 原生托盘：重装一次（幂等）。系统通知是否显示由 macOS 通知设置决定，程序无法强制放开。
    MacTrayHandlers h{this, &macCbOpen, &macCbCore, &macCbProxy, &macCbTun, &macCbQuit};
    macTrayInstall(h);
#else
    // 非 mac：hide→show 让 Qt 重新 Shell_NotifyIcon(NIM_ADD)，重建通知注册（对显式 OS 屏蔽无效）。
    if (m_tray.isVisible())
        m_tray.hide();
    m_tray.show();
#endif
}

QString TrayController::speedText(qint64 value) const
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

QString TrayController::speedTextCompact(qint64 value) const
{
    // 图标旁的紧凑速率："0 B/s"、"12.3 KB/s"、"1.2 MB/s"（B 不带小数，其余一位）。
    double number = static_cast<double>(qMax<qint64>(0, value));
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (number >= 1024.0 && unit + 1 < units.size()) {
        number /= 1024.0;
        ++unit;
    }
    const QString num = QString::number(number, 'f', unit == 0 ? 0 : 1);
    return num + " " + units[unit] + "/s";
}
