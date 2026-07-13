#include "TrayController.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QMenu>
#include <QPainter>
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
    rebuildMenu();
    m_tray.show();
}

void TrayController::setStatus(bool tun, bool proxy, bool core)
{
    m_tun = tun;
    m_proxy = proxy;
    m_core = core;
    m_tray.setToolTip(QString("Clash Auto - %1").arg(core ? "运行中" : "已停止"));
    refreshIcon(); // 状态变了刷新图标颜色（增强=红、核心在跑=黄）
    rebuildMenu();
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
    m_up = up;
    m_down = down;
    rebuildMenu();
}

void TrayController::notify(const QString &title, const QString &message)
{
    if (QSystemTrayIcon::supportsMessages()) {
        m_tray.showMessage(title, message, QSystemTrayIcon::Information, 3000);
    }
}

void TrayController::rebuildMenu()
{
    auto *menu = new QMenu();
    menu->addAction("控制面板", this, [this] {
        if (m_window) {
            m_window->showNormal();
            m_window->raise();
            m_window->activateWindow();
        }
    });
    menu->addSeparator();
    menu->addAction(QString("UP: %1").arg(speedText(m_up)))->setEnabled(false);
    menu->addAction(QString("DOWN: %1").arg(speedText(m_down)))->setEnabled(false);
    menu->addSeparator();
    menu->addAction(m_core ? "停止核心" : "启动核心", this, &TrayController::toggleCoreRequested);
    menu->addAction(m_proxy ? "关闭网页代理" : "打开网页代理", this, &TrayController::toggleProxyRequested);
    menu->addAction(m_tun ? "关闭增强模式" : "打开增强模式", this, &TrayController::toggleTunRequested);
    menu->addSeparator();
    menu->addAction("退出程序", qApp, &QApplication::quit);
    m_tray.setContextMenu(menu);
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
