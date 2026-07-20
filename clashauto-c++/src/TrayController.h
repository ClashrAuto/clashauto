#pragma once

#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

class QAction;
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

#if defined(Q_OS_MACOS)
public:
    // 原生托盘菜单/点击的回调入口（由 MacSpeedItem 的 C 回调转到这里，再发成 Qt 信号）。
    void macOpenWindow();
    void macToggleCore();
    void macToggleProxy();
    void macToggleTun();
    void macQuit();
#endif

private:
    void buildMenu();   // 只在构造时建一次；之后 setStatus/setTraffic 仅改对应行的文本
    void refreshIcon(); // 按状态给托盘图标着色 + macOS 上把上/下行速率画在图标右侧
    QString speedText(qint64 value) const;        // 菜单里的长文本："2.00 MB"
    QString speedTextCompact(qint64 value) const; // 图标旁的紧凑文本："2.0 MB/s"

    MainWindow *m_window = nullptr;
    QSystemTrayIcon m_tray;
    QMenu m_menu;                    // 常驻托盘菜单（此前每秒 new 一个换给 setContextMenu，旧的全泄漏）
    QAction *m_upAction = nullptr;   // UP/DOWN 实时速率行
    QAction *m_downAction = nullptr;
    QAction *m_coreAction = nullptr; // 三个开关行（文本随状态翻转）
    QAction *m_proxyAction = nullptr;
    QAction *m_tunAction = nullptr;
    bool m_tun = false;
    bool m_proxy = false;
    bool m_core = false;
    bool m_statusKnown = false; // 首次 setStatus 必须全量应用（成员默认值可能恰与首个状态相同）
    qint64 m_up = -1;           // -1 保证首次 setTraffic(0,0) 也会刷新文本
    qint64 m_down = -1;
};
