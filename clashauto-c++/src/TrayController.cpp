#include "TrayController.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

TrayController::TrayController(MainWindow *window, QObject *parent)
    : QObject(parent), m_window(window)
{
    refreshIcon(); // 初始（核心未起）用原色图标
    m_tray.setToolTip("Clash Auto");
    connect(&m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger && m_window) {
            m_window->showNormal();
            m_window->raise();
            m_window->activateWindow();
        }
    });
    buildMenu();
    m_tray.show();
}

void TrayController::buildMenu()
{
    // 菜单只建这一次，之后 setStatus/setTraffic 只改各行 QAction 的文本。
    // 此前流量每秒 rebuildMenu()：new QMenu 换给 setContextMenu（它不接管旧菜单
    // 所有权），旧菜单+全部 QAction 每秒泄漏一份；用户正开着菜单时还会被整个换掉。
    m_menu.addAction("控制面板", this, [this] {
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
    m_tray.setToolTip(QString("Clash Auto - %1").arg(core ? "运行中" : "已停止"));
    refreshIcon(); // 状态变了刷新图标颜色（增强=红、核心在跑=黄）
    m_coreAction->setText(core ? "停止核心" : "启动核心");
    m_proxyAction->setText(proxy ? "关闭网页代理" : "打开网页代理");
    m_tunAction->setText(tun ? "关闭增强模式" : "打开增强模式");
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

    // 按状态给图标着色（SourceIn 把非透明像素整体染成状态色，保留轮廓）。
    QPixmap icon = base.pixmap(64, 64);
    if (tint.isValid() && !icon.isNull()) {
        QPainter p(&icon);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(icon.rect(), tint);
        p.end();
    }
    if (icon.isNull()) {
        m_tray.setIcon(base);
        return;
    }

#ifdef Q_OS_MACOS
    // macOS 菜单栏空间足够，把上/下速率画在图标右侧两行（上=上传、下=下载）。
    // 核心没跑时无流量，只显示图标。Windows/Linux 托盘槽只有 ~16px，加文字会被
    // 缩成一团，故仅 macOS 走这条。
    if (m_core) {
        const int side = 44;        // 图标边长（逻辑像素，菜单栏会再缩放）
        const int gap = 6;
        const int pad = 4;
        QFont font = QApplication::font();
        font.setPixelSize(19);      // 两行文字塞进 side 高度内
        font.setBold(true);
        const QFontMetrics fm(font);
        const QString upStr = "▲ " + speedTextCompact(m_up < 0 ? 0 : m_up);   // ▲ 上传
        const QString downStr = "▼ " + speedTextCompact(m_down < 0 ? 0 : m_down); // ▼ 下载
        const int textW = qMax(fm.horizontalAdvance(upStr), fm.horizontalAdvance(downStr));
        const int totalW = side + gap + textW + pad;

        const qreal dpr = 4.0; // 高分屏下先高清渲染，再交给菜单栏缩放
        QPixmap canvas(qRound(totalW * dpr), qRound(side * dpr));
        canvas.setDevicePixelRatio(dpr);
        canvas.fill(Qt::transparent);
        QPainter p(&canvas);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawPixmap(QRect(0, 0, side, side), icon);
        p.setFont(font);
        // 菜单栏文字颜色随系统深浅色：亮色栏用深字、暗色栏用浅字。
        p.setPen(QApplication::palette().color(QPalette::WindowText));
        const int lineH = side / 2;
        p.drawText(QRect(side + gap, 0, textW, lineH), Qt::AlignLeft | Qt::AlignVCenter, upStr);
        p.drawText(QRect(side + gap, lineH, textW, lineH), Qt::AlignLeft | Qt::AlignVCenter, downStr);
        p.end();
        m_tray.setIcon(QIcon(canvas));
        return;
    }
#endif
    m_tray.setIcon(QIcon(icon));
}

void TrayController::setTraffic(qint64 up, qint64 down)
{
    if (up == m_up && down == m_down) {
        return; // 空闲时常年 0/0，一秒一改毫无必要
    }
    m_up = up;
    m_down = down;
    m_upAction->setText(QString("UP: %1").arg(speedText(up)));
    m_downAction->setText(QString("DOWN: %1").arg(speedText(down)));
    refreshIcon(); // macOS 上把新速率重画到图标右侧
}

void TrayController::notify(const QString &title, const QString &message)
{
    if (QSystemTrayIcon::supportsMessages()) {
        m_tray.showMessage(title, message, QSystemTrayIcon::Information, 3000);
    }
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
