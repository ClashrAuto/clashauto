#include "TunSession.h"

#include "core/SelfRouteGuard.h"

#include <QProcess>
#include <QTimer>

namespace {
// Linux：TUN 默认路由专用的路由表号。取一个不太可能撞的值；main=254 / default=253 / local=255
// 都是保留的，别用。989 只是个好记的空位。
constexpr int kTunTable = 989;
} // namespace

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

    // /32 主机路由（环路排除主层）。COAST_TUN_NO_HOSTROUTES=1 只给测试用：
    // 反向对照（tools/tunroute 的验法）需要「故意不装」的那一组来证明测试测得出区别。
    const bool wantHostRoutes = !qEnvironmentVariableIsSet("COAST_TUN_NO_HOSTROUTES");
    if (!wantHostRoutes)
        m_trace << QStringLiteral("[对照] COAST_TUN_NO_HOSTROUTES=1 —— 跳过全部 /32 主机路由");
    if (cfg.takeDefault && wantHostRoutes && !cfg.hostRoutes4.isEmpty()
        && (cfg.gateway4.isEmpty() || cfg.physIfname.isEmpty())) {
        // ★ 没有网关的 /32 路由是装不出来的。这里**必须**拒绝接管而不是继续：
        //   继续 = 本进程出站全部环路 = 整机断网，比 TUN 不开更糟。
        if (err)
            *err = QStringLiteral("有 %1 个地址要排除在 TUN 之外，但探不到物理默认网关"
                                  "（gateway4='%2' 物理口='%3'）——拒绝接管默认路由")
                           .arg(cfg.hostRoutes4.size())
                           .arg(cfg.gateway4, cfg.physIfname);
        stop();
        return false;
    }

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
        // ★★ Linux 的接管**不能**把 /1 路由加进 main 表 —— 那样 SO_MARK 会彻底失效。
        //   一开始就是这么写的（rule "fwmark X lookup main" + 路由进 main），逻辑上自相矛盾：
        //   打了标的包查 main，而 main 里正躺着那两条指向 TUN 的 /1，于是我们自己的出站
        //   照样进 TUN → 环路，SO_MARK 一点用没有。
        //   正确形状（wireguard / mihomo 同款）是**两张表**：
        //     · TUN 的 /1 放进独立表 kTable，配一条 pref 200 的 "from all lookup kTable"；
        //     · pref 100 的 "fwmark X lookup main" 排在它**前面** —— 打了标的包先查 main，
        //       main 里只有物理默认路由，于是走物理口出去。
        //   两条 rule 的**先后（pref 数值小者先）就是全部的机制**，写反了同样是环路。
        //   已用 `ip route get <ip> [mark …]` 在容器里验过两组配置（tools/tunroute/rulecheck.sh）：
        //   错误配置下带 mark 仍解析到 coast0（SO_MARK 形同虚设），本配置下带 mark 解析到物理口。
        const QString mark = QStringLiteral("0x%1").arg(SelfRouteGuard::fwmark(), 0, 16);
        const QString table = QString::number(kTunTable);

        // 本进程的包：pref 100，查 main（只有物理默认路由）
        pushUndo(ip, {"rule", "del", "fwmark", mark, "lookup", "main", "pref", "100"});
        if (!run(ip, {"rule", "add", "fwmark", mark, "lookup", "main", "pref", "100"}, err))
            return bail(*err);

        // ★★ QUIC/Hysteria2 的补丁 rule（pref 90，排在 TUN 表 pref 200 之前）——**保险层**。
        //   真机实证（Pi，tcpdump）：只有 fwmark 那条时，Hy2 的 UDP 握手包**全部走 coast0 出去**
        //   （5 个包在 coast0、0 个在物理口）→ 死循环，握手永不完成、curl=000。根因：msquic 自持
        //   UDP socket，SelfRouteGuard 够不着它的 fd、**打不上 fwmark**，于是这些包不匹配 pref 100 的
        //   fwmark 规则，落到 pref 200 的 TUN 表 → 进 coast0。
        //   但 msquic **有**被 QUIC_PARAM_CONN_LOCAL_ADDRESS 把源地址钉在物理出口 IP 上（见
        //   QuicTransport.cpp），所以按**源地址**兜一条 `from <物理IP> lookup main` 就能把它们捞回物理口。
        //   只匹配「显式绑定了物理 IP 的 socket」——正是 msquic 那些；本机应用的 socket 未绑此源，
        //   路由决定进 coast0 后源地址才变成 TUN 网卡地址，不会误命中，故不影响 TUN 对本机流量的接管。
        //   ★ /32 主机路由（下面那组，协议无关的主层）拿到真机证据后，这条**可以撤掉**——
        //   它只救得了「钉了源地址」的 socket，/32 谁都救。现在先并存：它已被 tcpdump 验证有效，
        //   在 /32 出问题时是第二道防线，删掉是净损失。COAST_TUN_NO_QUICRULE=1 只给对照测试用
        //  （单独验证 /32 这一层时要把它关掉，否则测不出 /32 自己的作用）。
        if (qEnvironmentVariableIsSet("COAST_TUN_NO_QUICRULE")) {
            m_trace << QStringLiteral("[对照] COAST_TUN_NO_QUICRULE=1 —— 跳过 pref 90 源地址规则");
        } else {
            for (const bool v6 : {false, true}) {
                const QString phys = SelfRouteGuard::physicalAddress(v6);
                if (phys.isEmpty())
                    continue;
                const QString from = phys + (v6 ? QStringLiteral("/128") : QStringLiteral("/32"));
                pushUndo(ip, {"rule", "del", "from", from, "lookup", "main", "pref", "90"});
                QString ignore;
                // 不致命：v6 物理地址常常没有；只要 v4 那条装上，IPv4 的 Hy2 就不再环路。装失败也不回滚整个接管。
                run(ip, {"rule", "add", "from", from, "lookup", "main", "pref", "90"}, &ignore);
            }
        }

        // ★★ /32(/128) 主机路由 —— 环路排除的**主层**（协议无关；代理服务器 + 系统 DNS）。
        //   Linux 上这组路由**必须进 TUN 专用表 %table，不能进 main**：未打标、未钉源的包
        //   （正是 /32 要救的那类）在 pref 200 就被引去查 %table，main 里的 /32 永远轮不到。
        //   放进 %table 后，查表命中 /32（比 /1 更具体）→ 经物理网关出去，环路闭合点就此拆掉。
        //   放在 rule 200 / /1 之前装：等 /1 落表时排除路由已经就位，没有裸奔窗口。
        //   单条失败不致命（比如同名路由已存在）：还有 socket 选项 + pref 90 两道保险层兜着，
        //   为一条路由放弃整个 TUN 不划算；但 rc 都在 trace 里，排查时第一眼就能看到。
        if (wantHostRoutes) {
            for (const QString &h : cfg.hostRoutes4) {
                // 地址就是网关本身（网关兼任 DNS 的家用网络里必现）：不能写「经它自己」，
                // 用接口路由。macOS 上写 via 自己会直接报 Can't assign requested address。
                if (h == cfg.gateway4) {
                    pushUndo(ip, {"route", "del", h + QStringLiteral("/32"), "dev",
                                  cfg.physIfname, "table", table});
                    QString ignore;
                    run(ip, {"route", "add", h + QStringLiteral("/32"), "dev", cfg.physIfname,
                             "table", table},
                        &ignore);
                    continue;
                }
                pushUndo(ip, {"route", "del", h + QStringLiteral("/32"), "via", cfg.gateway4,
                              "dev", cfg.physIfname, "table", table});
                QString ignore;
                run(ip, {"route", "add", h + QStringLiteral("/32"), "via", cfg.gateway4, "dev",
                         cfg.physIfname, "table", table},
                    &ignore);
            }
            if (!cfg.gateway6.isEmpty()) {
                for (const QString &h : cfg.hostRoutes6) {
                    pushUndo(ip, {"-6", "route", "del", h + QStringLiteral("/128"), "via",
                                  cfg.gateway6, "dev", cfg.physIfname, "table", table});
                    QString ignore;
                    run(ip, {"-6", "route", "add", h + QStringLiteral("/128"), "via", cfg.gateway6,
                             "dev", cfg.physIfname, "table", table},
                        &ignore);
                }
            }
        }

        // 其它所有包：pref 200，查 TUN 专用表
        pushUndo(ip, {"rule", "del", "lookup", table, "pref", "200"});
        if (!run(ip, {"rule", "add", "lookup", table, "pref", "200"}, err))
            return bail(*err);

        for (const char *half : {"0.0.0.0/1", "128.0.0.0/1"}) {
            pushUndo(ip, {"route", "del", QString::fromLatin1(half), "dev", cfg.ifname, "table",
                          table});
            if (!run(ip, {"route", "add", QString::fromLatin1(half), "dev", cfg.ifname, "table",
                          table},
                     err))
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
        // /32(/128) 主机路由 —— 环路排除主层（理由见 Linux 块的注释；macOS 单表，直接更具体即胜出）。
        // 先装排除、再装 /1：/1 落表时排除已就位，没有裸奔窗口。单条失败不致命（trace 里有 rc）。
        if (wantHostRoutes) {
            for (const QString &h : cfg.hostRoutes4) {
                pushUndo(route, {"-q", "delete", "-host", h});
                QString ignore;
                // 网关本身：不能「经它自己」（route 报 Can't assign requested address），走接口路由
                if (h == cfg.gateway4)
                    run(route, {"-q", "add", "-host", h, "-interface", cfg.physIfname}, &ignore);
                else
                    run(route, {"-q", "add", "-host", h, cfg.gateway4}, &ignore);
            }
            if (!cfg.gateway6.isEmpty()) {
                for (const QString &h : cfg.hostRoutes6) {
                    pushUndo(route, {"-q", "delete", "-inet6", "-host", h});
                    QString ignore;
                    // gateway6 可能是 fe80::x%en0（带 zone）——route(8) 认这个格式
                    run(route, {"-q", "add", "-inet6", "-host", h, cfg.gateway6}, &ignore);
                }
            }
        }
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
        // /32(/128) 主机路由 —— 环路排除主层（理由见 Linux 块的注释；Windows 单表，更具体即胜出）。
        // 先装排除、再装 /1。挂在**物理接口**上、下一跳=物理网关。单条失败不致命（trace 里有 rc）。
        const QString physName = QStringLiteral("name=\"%1\"").arg(cfg.physIfname);
        if (wantHostRoutes) {
            for (const QString &h : cfg.hostRoutes4) {
                // 网关本身：不带 nexthop（on-link 接口路由），别写「经它自己」
                if (h == cfg.gateway4) {
                    pushUndo(netsh, {"interface", "ipv4", "delete", "route",
                                     h + QStringLiteral("/32"), physName});
                    QString ignore;
                    run(netsh,
                        {"interface", "ipv4", "add", "route", h + QStringLiteral("/32"), physName,
                         "store=active"},
                        &ignore);
                    continue;
                }
                pushUndo(netsh, {"interface", "ipv4", "delete", "route",
                                 h + QStringLiteral("/32"), physName, cfg.gateway4});
                QString ignore;
                run(netsh,
                    {"interface", "ipv4", "add", "route", h + QStringLiteral("/32"), physName,
                     cfg.gateway4, "store=active"},
                    &ignore);
            }
            if (!cfg.gateway6.isEmpty()) {
                for (const QString &h : cfg.hostRoutes6) {
                    pushUndo(netsh, {"interface", "ipv6", "delete", "route",
                                     h + QStringLiteral("/128"), physName, cfg.gateway6});
                    QString ignore;
                    run(netsh,
                        {"interface", "ipv6", "add", "route", h + QStringLiteral("/128"), physName,
                         cfg.gateway6, "store=active"},
                        &ignore);
                }
            }
        }
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
