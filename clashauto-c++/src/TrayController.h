#pragma once

#include <QObject>
#include <QSystemTrayIcon>

class MainWindow;

class TrayController final : public QObject
{
    Q_OBJECT

public:
    explicit TrayController(MainWindow *window, QObject *parent = nullptr);

public slots:
    void setStatus(bool tun, bool proxy, bool core);
    void setTraffic(qint64 up, qint64 down);
    void notify(const QString &title, const QString &message);

signals:
    void toggleCoreRequested();
    void toggleProxyRequested();
    void toggleTunRequested();

private:
    void rebuildMenu();
    void refreshIcon(); // 按状态给托盘图标着色：增强(TUN)开=红，核心在跑=黄，否则原色
    QString speedText(qint64 value) const;

    MainWindow *m_window = nullptr;
    QSystemTrayIcon m_tray;
    bool m_tun = false;
    bool m_proxy = false;
    bool m_core = false;
    qint64 m_up = 0;
    qint64 m_down = 0;
};
