#pragma once

// TUN 激活层 —— 把 TunEndpoint 建出来的网卡真正接进系统路由。
//
// 设备层（TunEndpoint_*.cpp）只负责收发帧；「配地址、接管默认路由、Linux 的策略路由规则、
// 以及把这一切**原样还原**」是这里的事。分开是因为二者的失败模式完全不同：设备层失败
// 只是 TUN 起不来，激活层失败会**改变整机路由**。
//
// ★★ 本类的第一设计目标是**还原**，不是接管。
//   接管默认路由这件事，做错了的代价是整机断网 —— 而且是在一台你可能只能通过网络访问的机器上。
//   所以：
//     · 每应用一条命令，**先把它的 undo 记下来**再执行；stop() 逆序跑 undo。
//     · undo 记录在**应用之前**入栈：如果一条命令执行到一半失败（比如加了地址没加成路由），
//       栈里已经有它的 undo 了，回滚不会漏。
//     · 提供**看门狗**：armWatchdog(sec) 之后若没有按时 disarm，自动 stop()。真机测试时它是
//       唯一能保证"我把自己网锯断了还能自愈"的东西，别嫌麻烦。
//     · start() 任一步失败 → 立刻自行回滚已做的部分，绝不留半截状态。
//
// 为什么用 0.0.0.0/1 + 128.0.0.0/1 而不是改默认路由：这两条比 0.0.0.0/0 更具体，会在路由查表中
// 胜出，同时**原来的默认路由原封不动**留在表里 —— 于是「排除自身流量」那层（SelfRouteGuard 把
// socket 钉在物理网卡上）仍有一条完整的出路可走，而且还原时只要删掉这两条，不必去恢复默认路由。
// wireguard / mihomo 都是这个路子。
//
// 权限：Linux/macOS 需 root，Windows 需 Administrator。
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;

class TunSession
{
public:
    struct Config
    {
        QString ifname;                        // 设备层给出的真实网卡名（macOS 的 utunN 由内核定）
        QString addr4 = QStringLiteral("198.18.0.1");  // TUN 上我们这一端的地址
        QString peer4 = QStringLiteral("198.18.0.2");  // macOS 的点对点对端（Linux/Win 不用）
        QString mask4 = QStringLiteral("255.255.255.0");
        int prefix4 = 24;
        int mtu = 1500;
        bool takeDefault = true; // false = 只配地址不接管路由（真机验证命令通路时用，安全）
    };

    ~TunSession();

    // 应用配置。失败时 *err 说明原因，且**已做的部分已回滚**。
    bool start(const Config &cfg, QString *err);

    // 还原。可重复调用（第二次是空操作）。析构时自动调。
    void stop();

    bool active() const { return m_active; }

    // 实际执行过的命令（含 undo），给日志/排查用。真机上路由出问题时，第一件事就是看这个。
    QStringList trace() const { return m_trace; }

    // 看门狗：sec 秒内没 disarmWatchdog() 就自动 stop()。
    // **真机测默认路由接管时必须开** —— 万一把自己的网锯断了，这是唯一的自愈路径。
    void armWatchdog(int sec);
    void disarmWatchdog();

private:
    bool run(const QString &prog, const QStringList &args, QString *err); // 执行并记 trace
    void pushUndo(const QString &prog, const QStringList &args);         // 应用**之前**入栈

    struct Cmd
    {
        QString prog;
        QStringList args;
    };
    QVector<Cmd> m_undo;
    QStringList m_trace;
    bool m_active = false;
    QTimer *m_watchdog = nullptr;
};
