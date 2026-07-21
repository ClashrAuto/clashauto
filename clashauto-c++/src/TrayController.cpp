#include "TrayController.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QTimer>

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
    {
        // 把 Qt 的图标资源（和以前托盘同一张）转 PNG 传给原生层，避免依赖可能为空的
        // applicationIconImage 导致整项透明「消失」。
        const QPixmap pm = QIcon(QStringLiteral(":/assets/icon.ico")).pixmap(64, 64);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        pm.save(&buf, "PNG");
        buf.close();
        if (!png.isEmpty()) {
            macTraySetIconPng(png.constData(), static_cast<long>(png.size()));
        }
    }
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
    m_menu.addAction("控制面板", this, [this] {
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
    m_coreAction = m_menu.addAction("启动核心", this, &TrayController::toggleCoreRequested);
    m_proxyAction = m_menu.addAction("打开网页代理", this, &TrayController::toggleProxyRequested);
    m_tunAction = m_menu.addAction("打开增强模式", this, &TrayController::toggleTunRequested);
    m_menu.addSeparator();
    m_menu.addAction("退出程序", qApp, &QApplication::quit);
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
#else
    m_tray.setToolTip(QString("Clash Auto - %1").arg(core ? "运行中" : "已停止"));
    refreshIcon(); // 状态变了刷新图标颜色（增强=红、核心在跑=黄）
    m_coreAction->setText(core ? "停止核心" : "启动核心");
    m_proxyAction->setText(proxy ? "关闭网页代理" : "打开网页代理");
    m_tunAction->setText(tun ? "关闭增强模式" : "打开增强模式");
#endif
}

void TrayController::refreshIcon()
{
    // 托盘状态色（比侧栏 logo 更柔和、不刺眼）：增强(TUN)开=暗红、核心在跑=米色、否则原色。
    // TUN 必然核心在跑，故暗红优先于米色。
    QColor tint;
    if (m_tun) {
        tint = QColor(0xB0, 0x3A, 0x3A); // 暗红（增强/TUN）
    } else if (m_core) {
        tint = QColor(0xD6, 0xC6, 0x8E); // 米色（核心在跑）
    }

    const QIcon base(":/assets/icon.ico");
    if (!tint.isValid()) {
        m_tray.setIcon(base); // 空闲：原色应用图标
        return;
    }
    // 用 SourceIn 把图标非透明像素整体染成状态色（保留图标轮廓，只换颜色）。
    QPixmap pm = base.pixmap(64, 64);
    if (pm.isNull()) {
        m_tray.setIcon(base);
        return;
    }
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), tint);
    p.end();
    m_tray.setIcon(QIcon(pm));
}

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
