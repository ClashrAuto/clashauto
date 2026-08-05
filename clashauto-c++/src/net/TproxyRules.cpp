#include "TproxyRules.h"

#include <QtGlobal>

#if defined(Q_OS_LINUX)

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <cstdio>

namespace {

// 固定表名。**别改成带 pid/随机串的名字**：崩溃后要靠这个名字把陈旧规则删掉，名字对不上就删不掉，
// 而残留的 tproxy 规则会让被覆盖的设备完全断网。
constexpr const char *kTable = "coast_tproxy";

// 同步跑一个命令，返回退出码；stdout+stderr 合并进 *out。
int run(const QString &prog, const QStringList &args, QString *out = nullptr,
        const QByteArray &stdinData = {})
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(prog, args);
    if (!p.waitForStarted(3000)) {
        if (out)
            *out = QStringLiteral("无法启动 %1（未安装？）").arg(prog);
        return -1;
    }
    if (!stdinData.isEmpty()) {
        p.write(stdinData);
        p.closeWriteChannel();
    }
    if (!p.waitForFinished(8000)) {
        p.kill();
        if (out)
            *out = QStringLiteral("%1 超时").arg(prog);
        return -1;
    }
    if (out)
        *out = QString::fromUtf8(p.readAll()).trimmed();
    return p.exitCode();
}

QString sysctlRead(const QString &key)
{
    QString out;
    if (run(QStringLiteral("sysctl"), {QStringLiteral("-n"), key}, &out) == 0)
        return out.trimmed();
    return {};
}

void sysctlWrite(const QString &key, const QString &value)
{
    if (value.isEmpty())
        return;
    run(QStringLiteral("sysctl"), {QStringLiteral("-qw"), key + QLatin1Char('=') + value});
}

// 被 install 改动过的 sysctl 的原值存档（key=value 一行一条）。放 /run（tmpfs，重启即空）：
// 重启后 sysctl 本来就回到系统默认，没有陈旧值要还原；只需覆盖「同一次开机内 kill -9 后重来」。
//
// ★ 为什么要落盘、而不只靠成员变量：SIGKILL / OOM / 拔电这类退出不会走析构,内存里存的原值
//   随进程一起没了。而 install 会把 ip_forward=1、route_localnet=1、send_redirects=0 全写进
//   内核；其中 route_localnet 尤其危险 —— 它让「目的地写成 127/8 的外来包」能被路由进本机只
//   绑回环的服务(核心 API 9191、混合口 7890、网关 socks 7899、DNS 1053…)，本来靠 rawpre 那条
//   raw-hook 链兜着，可 nft 表随崩溃一起没了，防护和风险就此解耦。真机实测复现过:崩在 tproxy、
//   再以 lwIP 重启后 route_localnet 一直是 1、rawpre 链却不在了。所以这三个 sysctl 的原值必须
//   落盘,下次启动无论以哪种模式起来都先把存档吃掉、还原干净。
QString sysctlStatePath()
{
    return QStringLiteral("/run/coast-tproxy-sysctl");
}

// 追加一条 key=value 到存档（存在则不重复，保留最早那次的原值）。
void sysctlStateSave(const QString &key, const QString &value)
{
    QFile f(sysctlStatePath());
    QStringList lines;
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        f.close();
    }
    for (const QString &l : lines)
        if (l.startsWith(key + QLatin1Char('=')))
            return; // 已记过原值，别用「我们写进去的值」覆盖它
    lines.append(key + QLatin1Char('=') + value);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(lines.join(QLatin1Char('\n')).toUtf8());
        f.close();
    }
}

// 把存档里记下的每个 sysctl 原值写回，然后删掉存档。**幂等**：存档不存在就什么都不做。
// remove()（正常拆除）与 removeStale()（启动时的崩溃清理）都调它，所以无论上次是优雅退出
// 还是 SIGKILL、无论这次以 tproxy 还是 lwIP 模式启动，被改过的 sysctl 都能回到真正的原值。
void sysctlStateRestoreAll()
{
    QFile f(sysctlStatePath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return;
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'),
                                                                    Qt::SkipEmptyParts);
    f.close();
    for (const QString &l : lines) {
        const int eq = l.indexOf(QLatin1Char('='));
        if (eq > 0)
            sysctlWrite(l.left(eq), l.mid(eq + 1));
    }
    QFile::remove(sysctlStatePath());
}

} // namespace

TproxyRules::~TproxyRules()
{
    remove();
}

bool TproxyRules::available(QString *why)
{
    for (const char *prog : {"nft", "ip"}) {
        if (QStandardPaths::findExecutable(QString::fromLatin1(prog)).isEmpty()
            && !QFile::exists(QStringLiteral("/usr/sbin/") + QString::fromLatin1(prog))) {
            if (why)
                *why = QStringLiteral("缺少 %1（nftables / iproute2）").arg(QString::fromLatin1(prog));
            return false;
        }
    }
    return true;
}

void TproxyRules::removeStale()
{
    // 每一条都可能"本来就不存在"，全部忽略返回码。**顺序无关，都是幂等删除。**
    run(QStringLiteral("nft"), {QStringLiteral("delete"), QStringLiteral("table"),
                                QStringLiteral("inet"), QString::fromLatin1(kTable)});
    // ip rule/route 的表号必须与 Spec::routeTable 默认值一致——removeStale 是静态的，拿不到实例，
    // 所以这里只清默认表号。改默认值时记得同步这里（这也是它写成常量而不是配置项的原因）。
    for (int i = 0; i < 4; ++i) { // 同一条规则可能被重复插入过，多删几次
        if (run(QStringLiteral("ip"), {QStringLiteral("rule"), QStringLiteral("del"),
                                       QStringLiteral("fwmark"), QStringLiteral("0x1"),
                                       QStringLiteral("lookup"), QStringLiteral("99")}) != 0)
            break;
    }
    run(QStringLiteral("ip"), {QStringLiteral("route"), QStringLiteral("flush"),
                               QStringLiteral("table"), QStringLiteral("99")});
    // v6 侧同样可能有残留（kill -9 之后），独立删一遍
    for (int i = 0; i < 4; ++i) {
        if (run(QStringLiteral("ip"), {QStringLiteral("-6"), QStringLiteral("rule"),
                                       QStringLiteral("del"), QStringLiteral("fwmark"),
                                       QStringLiteral("0x1"), QStringLiteral("lookup"),
                                       QStringLiteral("99")}) != 0)
            break;
    }
    run(QStringLiteral("ip"), {QStringLiteral("-6"), QStringLiteral("route"),
                               QStringLiteral("flush"), QStringLiteral("table"),
                               QStringLiteral("99")});
    // iptables 侧那条放行也要清 —— 它按 comment 找，静态调用也删得掉（kill -9 之后就靠这个）。
    TproxyRules().removeIptablesForward();
    // sysctl 的崩溃恢复：把上次改动过的 ip_forward / route_localnet / 逐网卡 send_redirects
    // 全部还原。装载路径（install）在改这些之前也会走到这里，所以此处覆盖的是"上次崩在 tproxy、
    // 这次以 lwIP 模式起来、没人会调 install"的情况 —— 那时 route_localnet=1 正裸露着本机回环
    // 服务（保护它的 rawpre 链已随 nft 表消失），必须在这儿吃掉存档还原。真机确认过这个残留。
    sysctlStateRestoreAll();
}

bool TproxyRules::runIp(const QStringList &args, QString *err)
{
    QString out;
    if (run(QStringLiteral("ip"), args, &out) != 0) {
        if (err)
            *err = QStringLiteral("ip %1 失败：%2").arg(args.join(QLatin1Char(' ')), out);
        return false;
    }
    return true;
}

bool TproxyRules::applyNft(const QString &script, QString *err)
{
    QString out;
    if (run(QStringLiteral("nft"), {QStringLiteral("-f"), QStringLiteral("-")}, &out,
            script.toUtf8())
        != 0) {
        if (err)
            *err = QStringLiteral("nft 装载失败：%1").arg(out);
        return false;
    }
    return true;
}

// 放行转发。见头文件第 1 条：Docker 会把 FORWARD 设成 policy DROP，而 br_netfilter 还会把
// 同网桥的二层流量也塞进 FORWARD。两处不放行 = 设备完全不通。
//
// 用**独立的 nft 链**而不是往 iptables 的 FORWARD 里插规则：iptables 那条链是 Docker 在管的，
// 我们插进去既容易被它重排/清掉，退出时也不好精确撤销。独立链跟着我们的表一起建、一起删。
bool TproxyRules::ensureForwardAccept(QString *err)
{
    // priority filter - 10：排在 Docker/防火墙的 filter 链之前，先 accept 掉我们认得的流量。
    // 只放行「被代理设备发出的」与「回给它们的」，不做无差别放行——不该由我们来削弱用户的防火墙。
    const QString script = QStringLiteral(R"(
table inet %1 {
  chain forward_accept {
    type filter hook forward priority filter - 10; policy accept;
    ip saddr @proxied counter accept
    ip daddr @proxied counter accept
    ip6 saddr @proxied6 counter accept
    ip6 daddr @proxied6 counter accept
  }
}
)")
                               .arg(QString::fromLatin1(kTable));
    return applyNft(script, err);
}

// 关掉**每一张**网卡的 ICMP 重定向发送，并记下原值。见 install() 里的论证：
// 内核取 all 与 per-device 的或，逐网卡关才有效。
void TproxyRules::saveAndDisableRedirects()
{
    // 无需在此做崩溃恢复：install() 一进来就先调 removeStale()，那里已经把 /run 存档里的
    // 一切（含上一条命的 send_redirects）还原并清空，所以此刻读到的就是真正的原值。
    QDir dir(QStringLiteral("/proc/sys/net/ipv4/conf"));
    const QStringList ifs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &n : ifs) {
        const QString key = QStringLiteral("net.ipv4.conf.%1.send_redirects").arg(n);
        const QString cur = sysctlRead(key);
        if (cur.isEmpty())
            continue;
        sysctlStateSave(key, cur); // 与 ip_forward / route_localnet 共用同一份崩溃安全存档
        sysctlWrite(key, QStringLiteral("0"));
    }
}


// iptables 侧的放行。**这条不是冗余，是必需的。**
//
// 头文件里原来的说法是「用独立 nft 链就能绕开 Docker 把 FORWARD 设成 policy DROP」——真机实测
// 推翻了它：netfilter 在同一个 hook 上会依次跑**所有** base chain，我们在自己的 nft 链里 accept
// 只结束自己那条链的遍历，iptables 的 filter/FORWARD 照样跑，它的 DROP 才是最终裁决。
// 证据（Pi 上 Docker 环境，10 个 ping）：
//     Chain FORWARD (policy DROP 42724893 → 42724903)   ← 正好 +10
//   而 FORWARD 里的 ACCEPT 全部限定在 docker0 上，eth0→eth0 的转发一条都不匹配，直落 policy。
// 症状是「TCP 上网完全正常、ICMP 全丢」：TCP/UDP 在 prerouting 就被 tproxy 截走，压根不进转发
// 路径；只有 ICMP 这类既不是 TCP 也不是 UDP 的流量才真的走内核转发，于是只有它撞上 DROP。
//
// 插到 DOCKER-USER（Docker 官方留给用户的钩子，它保证在自己的规则之前执行且不会清掉用户规则）；
// 没装 Docker 就直接插 FORWARD 头部。规则用 --comment 打上标记，退出/崩溃后按标记精确删除。
static const char *kIptComment = "coast-tproxy";

static QString iptChain()
{
    // DOCKER-USER 存在就优先用它——直接改 FORWARD 头部会被 Docker 重启时的规则重排冲掉。
    if (run(QStringLiteral("iptables"), {QStringLiteral("-n"), QStringLiteral("-L"),
                                         QStringLiteral("DOCKER-USER")})
        == 0)
        return QStringLiteral("DOCKER-USER");
    return QStringLiteral("FORWARD");
}

void TproxyRules::ensureIptablesForward()
{
    if (QStandardPaths::findExecutable(QStringLiteral("iptables")).isEmpty()
        && !QFile::exists(QStringLiteral("/usr/sbin/iptables")))
        return; // 纯 nft 系统且没有 Docker，多半 policy 就是 accept，无需插手
    removeIptablesForward(); // 先清残留，避免重复插
    const QString chain = iptChain();
    for (const QString &in : m_spec.ifnames) {
        for (const QString &out : m_spec.ifnames) {
            run(QStringLiteral("iptables"),
                {QStringLiteral("-I"), chain, QStringLiteral("1"), QStringLiteral("-i"), in,
                 QStringLiteral("-o"), out, QStringLiteral("-m"), QStringLiteral("comment"),
                 QStringLiteral("--comment"), QString::fromLatin1(kIptComment),
                 QStringLiteral("-j"), QStringLiteral("ACCEPT")});
        }
    }
}

void TproxyRules::removeIptablesForward()
{
    if (QStandardPaths::findExecutable(QStringLiteral("iptables")).isEmpty()
        && !QFile::exists(QStringLiteral("/usr/sbin/iptables")))
        return;
    // 按 comment 找回自己插过的规则再删 —— 不依赖当时的网卡名，所以 kill -9 之后的清理也管用。
    for (const QString &chain : {QStringLiteral("DOCKER-USER"), QStringLiteral("FORWARD")}) {
        for (int guard = 0; guard < 32; ++guard) { // 防御性上限，别在异常输出上死循环
            QString out;
            if (run(QStringLiteral("iptables"), {QStringLiteral("-S"), chain}, &out) != 0)
                break;
            QString victim;
            for (const QString &line : out.split(QLatin1Char('\n'))) {
                if (line.startsWith(QStringLiteral("-A "))
                    && line.contains(QString::fromLatin1(kIptComment))) {
                    victim = line;
                    break;
                }
            }
            if (victim.isEmpty())
                break;
            QStringList args = victim.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            args[0] = QStringLiteral("-D");
            // --comment 的值在 -S 输出里带引号，去掉才能原样传回去
            for (QString &a : args)
                a.remove(QLatin1Char('"'));
            if (run(QStringLiteral("iptables"), args) != 0)
                break;
        }
    }
}

bool TproxyRules::install(const Spec &spec, QString *err)
{
    if (m_installed) {
        if (err)
            *err = QStringLiteral("已经装过了");
        return false;
    }
    QString why;
    if (!available(&why)) {
        if (err)
            *err = why;
        return false;
    }
    if (spec.tproxyPort == 0) {
        if (err)
            *err = QStringLiteral("tproxyPort 未设置");
        return false;
    }
    m_spec = spec;

    // 先清一遍：上一次可能是被 kill -9 打死的。
    removeStale();

    // 记下并打开转发（只在需要时打开，remove() 还原——不擅自改变用户的系统设置）。
    // ★ 原值同时**落盘**到 /run 存档：SIGKILL/OOM 不走析构，只靠成员变量 remove() 时会丢，
    //   于是 ip_forward=1 永久留下（下面 route_localnet 更危险，见那段）。存档让下次启动无论
    //   以哪种模式起来都能还原。见 sysctlStatePath() 上方那段论证。
    m_savedIpForward = sysctlRead(QStringLiteral("net.ipv4.ip_forward"));
    sysctlStateSave(QStringLiteral("net.ipv4.ip_forward"), m_savedIpForward);
    sysctlWrite(QStringLiteral("net.ipv4.ip_forward"), QStringLiteral("1"));
    // ★ 必须同时关掉 ICMP 重定向的**发送**，否则我们会亲手把流量赶出代理。
    //
    // 打开转发之后，内核对「从 eth0 进来、又要从 eth0 出去」的包会给上一跳发 ICMP redirect
    // （"这条路你直接走，别绕我"）。而透明网关的整个前提就是 ARP 抢答把双向流量都引到本机，
    // 所以**每一个**转发包都命中这个条件。真机抓到的原话：
    //     192.168.20.91 > 192.168.20.1: ICMP redirect 192.168.20.239 to host 192.168.20.239
    // ——我们在告诉**真实路由器**：发给这台被劫持设备的包别走我，直接给它。路由器一采信，
    // 该设备的回程流量就整条绕开代理。同一机制对被劫持设备侧同样成立（去程绕开）。
    // 症状先从 ICMP 露出来：TPROXY 模式下 ping 网关 1000 包只回 56 个，重测直接 100% 丢；
    // 而 TCP 看着一切正常（TCP 在 prerouting 就被 tproxy 截走，压根不进转发路径，不触发重定向）
    // —— 「能上网但 ping 不通」正是这个洞的伪装。lwIP 那条路没有此问题：它全程用户态处理，
    // 从不使用内核转发。
    //
    // ★ 必须**逐网卡**关。内核对 send_redirects 取的是 all 与 per-device 的**或**
    //   （IN_DEV_ORCONF），只把 all 写成 0 挡不住任何一张已存在的网卡；`default` 更只对
    //   之后新建的网卡生效。真机验证过这个区别：只关 all+default 时，重定向照发不误
    //   （IcmpOutRedirects 在 10 个 ping 期间仍 +6，tcpdump 也照样抓得到）。
    //   所以这里遍历 /proc/sys/net/ipv4/conf/* 全关，并逐张记下原值，remove() 时精确还原。
    saveAndDisableRedirects();
    ensureIptablesForward(); // Docker 的 FORWARD policy DROP 会吃掉一切内核转发，见上

    // ★ **故意不打开 IPv6 转发**，而且下面还有一条 v6 兜底丢弃链。
    //
    // 这条 tproxy 规则集目前只覆盖 IPv4。第一版这里跟着 v4 一起把 ipv6 forwarding 也打开了，
    // 于是出现一个**安全洞**：NdpSpoofer 照常把设备的 v6 默认路由指到本机，内核老老实实转发，
    // 而 nft 里一条 v6 规则都没有 —— 结果是**被代理设备的 IPv6 完全绕过网关**：不进 TPROXY、
    // 不受每设备策略约束、连「禁网」都形同虚设。真机实测（设备设为 reject）：
    //     curl -4 www.baidu.com → http=000（被拦，符合预期）
    //     curl -6 www.baidu.com → http=200（畅通无阻）
    // lwIP 那条路是**处理 v6 的**（NdpSpoofer + NetStack::addDeviceV6），所以这是 tproxy 独有的回归。
    //
    // 在 v6 规则补齐之前，正确的取向是 fail-closed：宁可让被代理设备的 v6 不通，也不能让它
    // 绕过策略出去。双栈客户端有 Happy Eyeballs，v6 不通会自动回落 v4，影响可控；
    // 而"策略被静默绕过"是不可接受的。
    //
    // ★ 2026-08-03：v6 规则已补齐（proxied6 集合 + prerouting6 链 + ip -6 策略路由 +
    //   forward_accept 的 v6 两条 + listener 改听 `::`），所以现在**可以**打开 v6 转发了。
    //   顺序很要紧：必须先有 prerouting6 把被接管设备的 v6 截给核心，再开转发；反过来就是
    //   上面描述的那个安全洞。v6guard 仍然留着 —— 它兜住「劫持已生效、但该设备的 v6 地址还没
    //   被学到（尚未进 proxied6）」那个窗口，此时依旧 fail-closed 丢弃，不放行。
    //   原值同样落盘存档，崩溃后能还原（与 ip_forward / route_localnet 同一套机制）。
    m_savedIpForward6 = sysctlRead(QStringLiteral("net.ipv6.conf.all.forwarding"));
    sysctlStateSave(QStringLiteral("net.ipv6.conf.all.forwarding"), m_savedIpForward6);
    sysctlWrite(QStringLiteral("net.ipv6.conf.all.forwarding"), QStringLiteral("1"));
    if (m_spec.dnsPort != 0) {
        // route_localnet：允许把包路由到 127.0.0.0/8。DNS 劫持那条 dnat 到回环，没有它内核会把
        // 改写后的包当 martian 丢掉。
        //
        // ★ 它的风险是真实的：开了之后，**外来包只要以 127.0.0.0/8 为目的地就能被路由进本机的
        //   回环服务**（本进程自己就有好几个只绑回环的口：核心 API 9191、混合口 7890、
        //   网关 socks 7899）。所以必须配一道 rawpre 链把这类包丢掉——它挂在 raw 钩子上，
        //   **早于 nat/dstnat**，所以只挡真正"从网卡进来就写着 127/8"的包；我们自己在 dstnat 里
        //   改写出来的目的地是在它之后产生的，不受影响。两者缺一不可，别只留一个。
        m_savedRouteLocalnet = sysctlRead(QStringLiteral("net.ipv4.conf.all.route_localnet"));
        // 同样落盘：这是三个 sysctl 里最危险的一个。它开着而保护它的 rawpre 链随崩溃消失后，
        // 局域网任意机器都能把包路由进本机只绑回环的服务。真机确认过这个残留(见 sysctlStatePath)。
        sysctlStateSave(QStringLiteral("net.ipv4.conf.all.route_localnet"), m_savedRouteLocalnet);
        sysctlWrite(QStringLiteral("net.ipv4.conf.all.route_localnet"), QStringLiteral("1"));
    }

    // 策略路由：TPROXY 打上 fwmark 之后，包的目的地址仍是原始目的地，必须靠这条把它留在本机
    // （`local default dev lo`），否则会被照常转发出去、根本到不了核心的监听 socket。
    const QString mark = QStringLiteral("0x%1").arg(m_spec.fwmark, 0, 16);
    const QString table = QString::number(m_spec.routeTable);
    if (!runIp({QStringLiteral("rule"), QStringLiteral("add"), QStringLiteral("fwmark"), mark,
                QStringLiteral("lookup"), table},
               err)
        || !runIp({QStringLiteral("route"), QStringLiteral("add"), QStringLiteral("local"),
                   QStringLiteral("default"), QStringLiteral("dev"), QStringLiteral("lo"),
                   QStringLiteral("table"), table},
                  err)) {
        remove(); // 不留半成品
        return false;
    }
    // IPv6 的策略路由：v4/v6 在内核里是**两套独立的规则表**，`ip rule` 加的只对 v4 生效，
    // v6 必须再来一遍 `ip -6 rule`。少了它，prerouting6 打了 mark 的包照样会被转发出去、
    // 到不了核心监听，表现为「v6 规则命中了但连接不通」。
    // 失败**不致命**：这台机器可能压根没有 v6（无 v6 地址/内核关了 v6），那时 v6 本来就无从谈起，
    // 而 v4 已经配好了，不该因此把整条数据面拆掉。失败时 v6 仍由 v6guard 兜底丢弃（fail-closed）。
    if (!runIp({QStringLiteral("-6"), QStringLiteral("rule"), QStringLiteral("add"),
                QStringLiteral("fwmark"), mark, QStringLiteral("lookup"), table})
        || !runIp({QStringLiteral("-6"), QStringLiteral("route"), QStringLiteral("add"),
                   QStringLiteral("local"), QStringLiteral("default"), QStringLiteral("dev"),
                   QStringLiteral("lo"), QStringLiteral("table"), table})) {
        std::fprintf(stderr, "[TPROXY] IPv6 策略路由未配上，v6 将继续被 fail-closed 丢弃\n");
        std::fflush(stderr);
    }

    // —— 主表 ——
    //  set proxied：被代理设备的 IPv4。**空集合时下面的规则一条都不会命中**，等价于「没开代理」，
    //    所以装规则本身是安全的：真正决定谁被接管的是 syncDevices()。
    //  dnsnat 链（nat/dstnat）：53 端口 redirect 到核心的 DNS 监听（理由见头文件第 3 条）。
    //  prerouting 链（filter/mangle）：其余 TCP/UDP 打 fwmark + tproxy 给核心。
    //    DNS 那条先 return，避免被下面的 tproxy 再截一次。
    //
    // ★★ 下面这段 nft 脚本里**只许出现 ASCII**，解释一律写在这里（C++ 注释里）。
    //    MSVC 的传统预处理器不能正确处理原始字符串字面量：脚本里以 `#` 开头的行会被它当成
    //    预处理指令去解析，指令名里一旦出现 CJK 标点（「」、。★ 之类）就报
    //        error C3872: '0x300c': this character is not allowed in an identifier
    //        error C3873: '0x2605': ... not allowed as a first character of an identifier
    //    ——**只有 MSVC 会**：同一份源码用 MinGW/GCC 构建是通过的，Linux/macOS 也通过，
    //    所以本地怎么编都发现不了。真机代价：v1.1 分支从加入这些注释的那一次提交起，
    //    Windows x64/arm64 连红 14 次，而作业日志要仓库 admin 权限才看得到，红了很久没人知道。
    //
    // ── v6guard 链在做什么（原先写在脚本里的那段说明搬到这里）──────────────────
    //  被劫持设备的 IPv6 **兜底**：走到这条说明 prerouting6 没把它 tproxy 走（即该设备的 v6
    //  地址还没进 proxied6 集合 —— NdpSpoofer 刚把它的默认路由指过来、但我们还没学到它的
    //  全局地址）。这种「劫持已生效、接管尚未生效」的窗口必须 fail-closed 丢弃：放行就等于
    //  让它绕过代理与每设备策略直连出网，连「禁网」都形同虚设（真机验证过这个洞）。
    //  真正被接管的 v6 在 prerouting6 里已 accept，根本到不了这条。
    //  ★ 前两条 accept 先放行**已接管设备**的 v6（地址已在 proxied6 里）。它们的 TCP/UDP 早在
    //    prerouting6 被 tproxy 截走、到不了这里；能走到的是 **ICMPv6 等非 TCP/UDP 流量**，
    //    那些本就该像 v4 的 ICMP 一样交给内核转发。少了这两条，被接管设备的 ping6 会被兜底
    //    全丢（真机实测：ping6 5 个包、v6guard 计数正好 +5，而 lwIP 模式下 ping6 是通的）。
    //
    // ── isolate 链在做什么 ────────────────────────────────────────────────────
    //  局域网隔离的**兜底**层。真正拦下绝大多数包的是 prerouting 链里同样一条规则 —— 实测这条
    //  forward 规则只命中 1 包：TPROXY 在 prerouting(mangle) 就把被接管设备的包截给本地 socket
    //  了，路由判决都不做，forward 钩子根本轮不到。放着不删是因为它覆盖 prerouting 放行之后仍
    //  走转发的残余路径（跨网段本机地址旁路那几条 return）。
    //  顺带：只有 prerouting 那条能避免「TPROXY 本地 socket 先把 TCP 握手做完、再由核心 REJECT」
    //  的假连通 —— 那正是 `nc -z` 会误报「通」的原因，别再拿 -z 验隔离。
    QString script = QStringLiteral(R"(
table inet %1 {
  set proxied {
    type ipv4_addr
    flags interval
  }
  set proxied6 {
    type ipv6_addr
    flags interval
  }
  set victimmacs {
    type ether_addr
  }
  set isolated {
    type ipv4_addr
  }
  set lansubnets {
    type ipv4_addr
    flags interval
  }
  chain v6guard {
    type filter hook forward priority filter - 30; policy accept;
    ip6 saddr @proxied6 counter accept
    ip6 daddr @proxied6 counter accept
    ether saddr @victimmacs meta nfproto ipv6 counter drop
  }
  chain isolate {
    type filter hook forward priority filter - 20; policy accept;
    ip saddr @isolated ip daddr @lansubnets ct state new counter drop
  }
  chain prerouting {
    type filter hook prerouting priority mangle; policy accept;
    ip saddr @isolated ip daddr @lansubnets ct state new counter drop
)")
                         .arg(QString::fromLatin1(kTable));
    if (m_spec.dnsPort != 0)
        script += QStringLiteral("    ip saddr @proxied udp dport 53 return\n");
    // ★ 按**入口网卡**分发：每张卡投给它自己那个核心入站（那个入站带 interface-name，决定这条
    //   连接从哪张网卡出去）。转发到本机的包天然带着 iif，比按设备 IP 分组稳 —— 设备换址
    //   （DHCP 续约）不影响归属，也不需要维护第二份「设备→网卡」的集合。
    //   最后仍留一条不限 iif 的兜底：认不出入口网卡时按老行为投给 tproxyPort。
    //   **宁可走错上行，也不要断网** —— 这条链上没有第二个人来兜底。
    for (const Spec::NicPort &np : m_spec.nicPorts) {
        if (np.ifname.isEmpty() || np.port == 0)
            continue;
        script += QStringLiteral("    iifname \"%1\" ip saddr @proxied meta l4proto { tcp, udp } "
                                 "counter meta mark set %2 tproxy ip to :%3 accept\n")
                          .arg(np.ifname)
                          .arg(mark)
                          .arg(np.port);
    }
    script += QStringLiteral(
                  "    ip saddr @proxied meta l4proto { tcp, udp } counter meta mark set %1 "
                  "tproxy ip to :%2 accept\n  }\n")
                  .arg(mark)
                  .arg(m_spec.tproxyPort);
    // ── IPv6 的 TPROXY：与上面 v4 那条完全对称，只是 `tproxy ip6 to` + 走 proxied6 集合 ──
    //   没有这一段时 v6 会被 v6guard 无条件丢弃 —— 而 lwIP 那条路**是支持 v6 的**（NdpSpoofer
    //   + NetStack::addDeviceV6），真机实测被劫持设备 v6 正常（ipv6.baidu.com 200、ping6 公网
    //   0% 丢包）。所以少了它，TPROXY 相对 lwIP 就是**功能回归**，也就没法把 TPROXY 设成默认。
    //   DNS 同样要先 return，避免被 tproxy 再截一次（与 v4 同理）。
    script += QStringLiteral("  chain prerouting6 {\n"
                             "    type filter hook prerouting priority mangle; policy accept;\n");
    if (m_spec.dnsPort != 0)
        script += QStringLiteral("    ip6 saddr @proxied6 udp dport 53 return\n");
    // 按入口网卡分发 + 兜底，与上面 v4 那段完全对称。
    for (const Spec::NicPort &np : m_spec.nicPorts) {
        if (np.ifname.isEmpty() || np.port == 0)
            continue;
        script += QStringLiteral("    iifname \"%1\" ip6 saddr @proxied6 meta l4proto { tcp, udp } "
                                 "counter meta mark set %2 tproxy ip6 to :%3 accept\n")
                          .arg(np.ifname)
                          .arg(mark)
                          .arg(np.port);
    }
    script += QStringLiteral(
                  "    ip6 saddr @proxied6 meta l4proto { tcp, udp } counter meta mark set %1 "
                  "tproxy ip6 to :%2 accept\n  }\n")
                  .arg(mark)
                  .arg(m_spec.tproxyPort);
    if (m_spec.dnsPort != 0) {
        // DNS 劫持：**dnat 到 127.0.0.1**，不是 redirect。
        //
        // 核心的 DNS 监听绑在 127.0.0.1:1053（与 lwIP 那条路一致，见 NetStack::kDnsHijackPort）。
        // `redirect` 把目的地改成**入口网卡的本机 IP**，绑回环的监听收不到；要用 redirect 就得把
        // 监听改绑 0.0.0.0——那等于**把解析器暴露给整个局域网**，任何一台机器都能拿它当 DNS 用。
        // 所以改成显式 dnat 到回环，监听维持 127.0.0.1 不动。代价是要开 route_localnet
        // （见下面 install 里那段），而它的风险由 rawpre 链兜住。
        //
        // 真机实测（被接管设备 dig @1.1.1.1 www.baidu.com）：返回 198.18.0.73，正是核心的 fake-ip。
        script += QStringLiteral(R"(  chain dnsnat {
    type nat hook prerouting priority dstnat; policy accept;
    ip saddr @proxied udp dport 53 counter dnat ip to 127.0.0.1:%1
  }
  chain rawpre {
    type filter hook prerouting priority raw; policy accept;
    iifname != "lo" ip daddr 127.0.0.0/8 counter drop
  }
)")
                      .arg(m_spec.dnsPort);
    }
    script += QStringLiteral("}\n");

    if (!applyNft(script, err)) {
        remove();
        return false;
    }
    if (!ensureForwardAccept(err)) {
        remove();
        return false;
    }
    m_installed = true;
    return true;
}

bool TproxyRules::syncDevices(const QStringList &ipv4, const QStringList &macs, QString *err,
                              const QStringList &ipv6)
{
    if (!m_installed) {
        if (err)
            *err = QStringLiteral("规则未安装");
        return false;
    }
    // 整体替换而不是逐个增删：调用方给的是「当前应当被代理的全集」，逐个 diff 既容易漏、
    // 也会在中间态出现「刚删掉又加回来」的抖动（那一瞬间设备会断一下）。
    QString script = QStringLiteral("flush set inet %1 proxied\nflush set inet %1 proxied6\n"
                                    "flush set inet %1 victimmacs\n")
                         .arg(QString::fromLatin1(kTable));
    if (!ipv4.isEmpty()) {
        script += QStringLiteral("add element inet %1 proxied { %2 }\n")
                      .arg(QString::fromLatin1(kTable), ipv4.join(QStringLiteral(", ")));
    }
    // v6 地址集合：空也没关系 —— 那时 prerouting6 一条都不命中，v6 由 v6guard 兜底丢弃，
    // 与补 v6 之前的行为完全一致（fail-closed），不会因为「还没学到 v6 地址」而放行。
    if (!ipv6.isEmpty()) {
        script += QStringLiteral("add element inet %1 proxied6 { %2 }\n")
                      .arg(QString::fromLatin1(kTable), ipv6.join(QStringLiteral(", ")));
    }
    if (!macs.isEmpty()) {
        script += QStringLiteral("add element inet %1 victimmacs { %2 }\n")
                      .arg(QString::fromLatin1(kTable), macs.join(QStringLiteral(", ")));
    }
    return applyNft(script, err);
}

bool TproxyRules::syncIsolation(const QStringList &isolatedIpv4, const QStringList &lanCidrs,
                                QString *err)
{
    if (!m_installed) {
        if (err)
            *err = QStringLiteral("规则未安装");
        return false;
    }
    // 同 syncDevices：整体替换，不做增量 diff。
    // isolate 链优先级 filter-20，**排在 forward_accept(filter-10) 之前**——后者会无条件 accept
    // 被代理设备的流量，隔离必须先于它判定，否则永远轮不到。
    QString script = QStringLiteral("flush set inet %1 isolated\nflush set inet %1 lansubnets\n")
                         .arg(QString::fromLatin1(kTable));
    if (!isolatedIpv4.isEmpty()) {
        script += QStringLiteral("add element inet %1 isolated { %2 }\n")
                      .arg(QString::fromLatin1(kTable), isolatedIpv4.join(QStringLiteral(", ")));
    }
    if (!lanCidrs.isEmpty()) {
        script += QStringLiteral("add element inet %1 lansubnets { %2 }\n")
                      .arg(QString::fromLatin1(kTable), lanCidrs.join(QStringLiteral(", ")));
    }
    return applyNft(script, err);
}

void TproxyRules::remove()
{
    if (!m_installed && m_savedIpForward.isEmpty())
        return;
    const QString mark = QStringLiteral("0x%1").arg(m_spec.fwmark, 0, 16);
    const QString table = QString::number(m_spec.routeTable);
    run(QStringLiteral("nft"), {QStringLiteral("delete"), QStringLiteral("table"),
                                QStringLiteral("inet"), QString::fromLatin1(kTable)});
    run(QStringLiteral("ip"), {QStringLiteral("rule"), QStringLiteral("del"),
                               QStringLiteral("fwmark"), mark, QStringLiteral("lookup"), table});
    run(QStringLiteral("ip"), {QStringLiteral("route"), QStringLiteral("flush"),
                               QStringLiteral("table"), table});
    // v6 那套是独立的规则表，必须单独删（v4 的 `ip rule del` 删不掉它）
    run(QStringLiteral("ip"), {QStringLiteral("-6"), QStringLiteral("rule"), QStringLiteral("del"),
                               QStringLiteral("fwmark"), mark, QStringLiteral("lookup"), table});
    run(QStringLiteral("ip"), {QStringLiteral("-6"), QStringLiteral("route"),
                               QStringLiteral("flush"), QStringLiteral("table"), table});
    // 三个 sysctl（ip_forward / route_localnet / 逐网卡 send_redirects）统一从 /run 存档还原。
    // 不再各自留成员变量：存档是唯一真相，且能跨 SIGKILL（那时本对象根本不会被析构）。
    sysctlStateRestoreAll();
    removeIptablesForward();
    m_savedIpForward.clear();
    m_savedRouteLocalnet.clear();
    m_installed = false;
}

QString TproxyRules::dumpRuleset()
{
    QString out;
    // ★ 表不存在时 nft 以非 0 退出并把「No such file or directory」打到 stderr，而 run() 是
    //   MergedChannels —— 直接返回 out 会把这段错误文本当成「规则内容」，于是「拆干净了没」
    //   这个判据永远为假。只有退出码为 0 才算真有内容。
    if (run(QStringLiteral("nft"), {QStringLiteral("list"), QStringLiteral("table"),
                                    QStringLiteral("inet"), QString::fromLatin1(kTable)},
            &out)
        != 0)
        return {};
    return out;
}

// ————————————————————————— 自测 —————————————————————————
// 只验「规则层」：装上之后内核里确实有这些东西、设备集合增删生效、拆完不留痕。
// **不需要真设备、不改任何路由决策**（set 为空时 tproxy 规则一条都不会命中）。
int runTproxyRulesSelfTest()
{
    auto say = [](const char *fmt, const QString &a = {}) {
        std::fprintf(stderr, fmt, a.toLocal8Bit().constData());
        std::fflush(stderr);
    };
    QString why;
    if (!TproxyRules::available(&why)) {
        say("TPROXY-SELFTEST: SKIP —— %s\n", why);
        return 0;
    }
    TproxyRules::removeStale();

    TproxyRules r;
    TproxyRules::Spec spec;
    spec.tproxyPort = 17893;
    spec.dnsPort = 17853;
    QString err;
    if (!r.install(spec, &err)) {
        say("TPROXY-SELFTEST: FAIL 装载失败 —— %s\n", err);
        return 1;
    }
    QString dump = TproxyRules::dumpRuleset();
    const bool hasTproxy = dump.contains(QStringLiteral("tproxy"));
    const bool hasDnsNat = dump.contains(QStringLiteral("dnat ip to 127.0.0.1:17853"));
    const bool hasRawGuard = dump.contains(QStringLiteral("127.0.0.0/8"));
    const bool hasV6Guard = dump.contains(QStringLiteral("nfproto ipv6"));
    const bool hasFwdChain = dump.contains(QStringLiteral("forward_accept"));
    if (!hasTproxy || !hasDnsNat || !hasFwdChain || !hasRawGuard || !hasV6Guard) {
        say("TPROXY-SELFTEST: FAIL 规则不完整 —— tproxy=%s",
            QString::number(hasTproxy) + " dnsnat=" + QString::number(hasDnsNat)
                + " forward=" + QString::number(hasFwdChain)
                + " rawguard=" + QString::number(hasRawGuard)
                + " v6guard=" + QString::number(hasV6Guard));
        say("\n%s\n", dump);
        r.remove();
        return 1;
    }

    if (!r.syncDevices({QStringLiteral("192.0.2.10"), QStringLiteral("192.0.2.11")},
                       {QStringLiteral("02:00:00:00:00:aa")}, &err)) {
        say("TPROXY-SELFTEST: FAIL 设备集合写入失败 —— %s\n", err);
        r.remove();
        return 1;
    }
    dump = TproxyRules::dumpRuleset();
    if (!dump.contains(QStringLiteral("192.0.2.10")) || !dump.contains(QStringLiteral("192.0.2.11"))) {
        say("TPROXY-SELFTEST: FAIL 设备没进 set\n%s\n", dump);
        r.remove();
        return 1;
    }
    // 换成只剩一台：验证「整体替换」而不是只增不减。
    if (!r.syncDevices({QStringLiteral("192.0.2.11")}, {}, &err)
        || TproxyRules::dumpRuleset().contains(QStringLiteral("192.0.2.10"))) {
        say("TPROXY-SELFTEST: FAIL 设备移除没生效\n");
        r.remove();
        return 1;
    }

    r.remove();
    if (!TproxyRules::dumpRuleset().isEmpty()) {
        say("TPROXY-SELFTEST: FAIL 拆除后仍有残留\n");
        return 1;
    }
    // 再跑一次 removeStale：它必须能安全地在"什么都没有"时调用（崩溃后启动就是这个场景）。
    TproxyRules::removeStale();
    say("TPROXY-SELFTEST: PASS（装载 + 设备增删 + 幂等拆除）\n");
    return 0;
}

#else // !Q_OS_LINUX

TproxyRules::~TproxyRules() = default;
bool TproxyRules::available(QString *why)
{
    if (why)
        *why = QStringLiteral("TPROXY 仅 Linux 可用");
    return false;
}
void TproxyRules::removeStale() {}
bool TproxyRules::install(const Spec &, QString *err)
{
    if (err)
        *err = QStringLiteral("TPROXY 仅 Linux 可用");
    return false;
}
bool TproxyRules::syncDevices(const QStringList &, const QStringList &, QString *err,
                              const QStringList &)
{
    if (err)
        *err = QStringLiteral("TPROXY 仅 Linux 可用");
    return false;
}
bool TproxyRules::syncIsolation(const QStringList &, const QStringList &, QString *err)
{
    if (err)
        *err = QStringLiteral("TPROXY 仅 Linux 可用");
    return false;
}
void TproxyRules::remove() {}
QString TproxyRules::dumpRuleset()
{
    return {};
}
bool TproxyRules::runIp(const QStringList &, QString *)
{
    return false;
}
bool TproxyRules::applyNft(const QString &, QString *)
{
    return false;
}
bool TproxyRules::ensureForwardAccept(QString *)
{
    return false;
}
int runTproxyRulesSelfTest()
{
    std::fputs("TPROXY-SELFTEST: SKIP（仅 Linux）\n", stderr);
    return 0;
}

#endif
