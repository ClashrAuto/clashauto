#include "TrayController.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QMenu>

TrayController::TrayController(MainWindow *window, QObject *parent)
    : QObject(parent), m_window(window)
{
    m_tray.setIcon(QIcon(":/assets/icon.ico"));
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
    rebuildMenu();
}

void TrayController::setTraffic(qint64 up, qint64 down)
{
    m_up = up;
    m_down = down;
    rebuildMenu();
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
