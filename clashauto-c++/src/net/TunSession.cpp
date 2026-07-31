#include "TunSession.h"

#include "core/SelfRouteGuard.h"

#include <QProcess>
#include <QTimer>

TunSession::~TunSession()
{
    stop();
}

void TunSession::pushUndo(const QString &prog, const QStringList &args)
{
    m_undo.push_back({prog, args});
}

bool TunSession::run(const QString &prog, const QStringList &args, QString *err)
{
    QProcess p;
    p.start(prog, args);
    if (!p.waitForFinished(8000)) {
        if (err)
            *err = QStringLiteral("%1 %2 超时").arg(prog, args.join(QLatin1Char(' ')));
        m_trace << QStringLiteral("[超时] %1 %2").arg(prog, args.join(QLatin1Char(' ')));
        return false;
    }
    const int rc = p.exitCode();
    const QString out = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
    m_trace << QStringLiteral("[rc=%1] %2 %3%4")
                       .arg(rc)
                       .arg(prog, args.join(QLatin1Char(' ')),
                            out.isEmpty() ? QString() : QStringLiteral("  // ") + out);
    if (rc != 0) {
        if (err)
            *err = QStringLiteral("%1 %2 失败(rc=%3): %4")
                           .arg(prog, args.join(QLatin1Char(' ')))
                           .arg(rc)
                           .arg(out);
        return false;
    }
    return true;
}

bool TunSession::start(const Config &cfg, QString *err)
{
    if (m_active) {
        if (err)
            *err = QStringLiteral("已经激活");
        return false;
    }
    if (cfg.ifname.isEmpty()) {
        if (err)
            *err = QStringLiteral("网卡名为空");
        return false;
    }
    m_trace.clear();
    m_undo.clear();

    // 任一步失败就地回滚 —— 半截状态比彻底失败更难查，也更容易把整机路由留在坏掉的样子。
    auto bail = [&](const QString &why) {
        if (err)
            *err = why;
        stop();
        return false;
    };

#if defined(Q_OS_LINUX)
    const QString ip = QStringLiteral("ip");
    // 地址。undo **先入栈**：万一 addr add 成功而后面某步失败，这条 undo 已经在了。
    pushUndo(ip, {"addr", "del", cfg.addr4 + QStringLiteral("/%1").arg(cfg.prefix4), "dev",
                  cfg.ifname});
    if (!run(ip, {"addr", "add", cfg.addr4 + QStringLiteral("/%1").arg(cfg.prefix4), "dev",
                  cfg.ifname},
             err))
        return bail(*err);
    if (!run(ip, {"link", "set", cfg.ifname, "mtu", QString::number(cfg.mtu), "up"}, err))
        return bail(*err);

    if (cfg.takeDefault) {
        // 策略路由：给本进程打了 fwmark 的包走 main 表（物理默认路由还在那儿），
        // 优先级要**高于**（数字小于）下面这两条 /1 所在的表。这一条是自身流量排除在 Linux
        // 上真正起作用的地方 —— SO_MARK 只是打标，导流靠它。
        const QString mark = QStringLiteral("0x%1").arg(SelfRouteGuard::fwmark(), 0, 16);
        pushUndo(ip, {"rule", "del", "fwmark", mark, "lookup", "main", "pref", "100"});
        if (!run(ip, {"rule", "add", "fwmark", mark, "lookup", "main", "pref", "100"}, err))
            return bail(*err);

        for (const char *half : {"0.0.0.0/1", "128.0.0.0/1"}) {
            pushUndo(ip, {"route", "del", QString::fromLatin1(half), "dev", cfg.ifname});
            if (!run(ip, {"route", "add", QString::fromLatin1(half), "dev", cfg.ifname}, err))
                return bail(*err);
        }
    }

#elif defined(Q_OS_MACOS)
    const QString ifconfig = QStringLiteral("/sbin/ifconfig");
    const QString route = QStringLiteral("/sbin/route");
    pushUndo(ifconfig, {cfg.ifname, "down"});
    if (!run(ifconfig, {cfg.ifname, "inet", cfg.addr4, cfg.peer4, "mtu", QString::number(cfg.mtu),
                        "up"},
             err))
        return bail(*err);

    if (cfg.takeDefault) {
        for (const char *half : {"0.0.0.0/1", "128.0.0.0/1"}) {
            pushUndo(route, {"-q", "delete", "-net", QString::fromLatin1(half), "-interface",
                             cfg.ifname});
            if (!run(route, {"-q", "add", "-net", QString::fromLatin1(half), "-interface",
                             cfg.ifname},
                     err))
                return bail(*err);
        }
    }

#elif defined(Q_OS_WIN)
    const QString netsh = QStringLiteral("netsh");
    const QString name = QStringLiteral("name=\"%1\"").arg(cfg.ifname);
    pushUndo(netsh, {"interface", "ip", "set", "address", name, "source=dhcp"});
    if (!run(netsh,
             {"interface", "ip", "set", "address", name, "static", cfg.addr4, cfg.mask4}, err))
        return bail(*err);
    if (!run(netsh, {"interface", "ipv4", "set", "subinterface", name,
                     QStringLiteral("mtu=%1").arg(cfg.mtu), "store=active"},
             err))
        return bail(*err);

    if (cfg.takeDefault) {
        for (const char *half : {"0.0.0.0/1", "128.0.0.0/1"}) {
            pushUndo(netsh, {"interface", "ipv4", "delete", "route",
                             QString::fromLatin1(half), name});
            if (!run(netsh,
                     {"interface", "ipv4", "add", "route", QString::fromLatin1(half), name,
                      "store=active"},
                     err))
                return bail(*err);
        }
    }

#else
    if (err)
        *err = QStringLiteral("本平台未实现 TUN 激活");
    return false;
#endif

    m_active = true;
    return true;
}

void TunSession::stop()
{
    disarmWatchdog();
    // 逆序回滚。**每条都要试**，中间失败不能中断 —— 半途而废留下的残留路由正是要避免的东西。
    for (int i = m_undo.size() - 1; i >= 0; --i) {
        QString ignored;
        run(m_undo[i].prog, m_undo[i].args, &ignored);
    }
    m_undo.clear();
    m_active = false;
}

void TunSession::armWatchdog(int sec)
{
    disarmWatchdog();
    m_watchdog = new QTimer();
    m_watchdog->setSingleShot(true);
    QObject::connect(m_watchdog, &QTimer::timeout, [this] {
        m_trace << QStringLiteral("[看门狗] 超时未 disarm —— 自动还原");
        m_watchdog->deleteLater();
        m_watchdog = nullptr;
        stop();
    });
    m_watchdog->start(sec * 1000);
}

void TunSession::disarmWatchdog()
{
    if (!m_watchdog)
        return;
    m_watchdog->stop();
    m_watchdog->deleteLater();
    m_watchdog = nullptr;
}
