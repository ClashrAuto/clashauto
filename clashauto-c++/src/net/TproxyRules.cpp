#include "TproxyRules.h"

#include <QtGlobal>

#if defined(Q_OS_LINUX)

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
  }
}
)")
                               .arg(QString::fromLatin1(kTable));
    return applyNft(script, err);
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
    m_savedIpForward = sysctlRead(QStringLiteral("net.ipv4.ip_forward"));
    m_savedIp6Forward = sysctlRead(QStringLiteral("net.ipv6.conf.all.forwarding"));
    sysctlWrite(QStringLiteral("net.ipv4.ip_forward"), QStringLiteral("1"));
    sysctlWrite(QStringLiteral("net.ipv6.conf.all.forwarding"), QStringLiteral("1"));

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

    // —— 主表 ——
    //  set proxied：被代理设备的 IPv4。**空集合时下面的规则一条都不会命中**，等价于「没开代理」，
    //    所以装规则本身是安全的：真正决定谁被接管的是 syncDevices()。
    //  dnsnat 链（nat/dstnat）：53 端口 redirect 到核心的 DNS 监听（理由见头文件第 3 条）。
    //  prerouting 链（filter/mangle）：其余 TCP/UDP 打 fwmark + tproxy 给核心。
    //    DNS 那条先 return，避免被下面的 tproxy 再截一次。
    QString script = QStringLiteral(R"(
table inet %1 {
  set proxied {
    type ipv4_addr
    flags interval
  }
  chain prerouting {
    type filter hook prerouting priority mangle; policy accept;
)")
                         .arg(QString::fromLatin1(kTable));
    if (m_spec.dnsPort != 0)
        script += QStringLiteral("    ip saddr @proxied udp dport 53 return\n");
    script += QStringLiteral(
                  "    ip saddr @proxied meta l4proto { tcp, udp } counter meta mark set %1 "
                  "tproxy ip to :%2 accept\n  }\n")
                  .arg(mark)
                  .arg(m_spec.tproxyPort);
    if (m_spec.dnsPort != 0) {
        script += QStringLiteral(R"(  chain dnsnat {
    type nat hook prerouting priority dstnat; policy accept;
    ip saddr @proxied udp dport 53 counter redirect to :%1
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

bool TproxyRules::syncDevices(const QStringList &ipv4, QString *err)
{
    if (!m_installed) {
        if (err)
            *err = QStringLiteral("规则未安装");
        return false;
    }
    // 整体替换而不是逐个增删：调用方给的是「当前应当被代理的全集」，逐个 diff 既容易漏、
    // 也会在中间态出现「刚删掉又加回来」的抖动（那一瞬间设备会断一下）。
    QString script = QStringLiteral("flush set inet %1 proxied\n").arg(QString::fromLatin1(kTable));
    if (!ipv4.isEmpty()) {
        script += QStringLiteral("add element inet %1 proxied { %2 }\n")
                      .arg(QString::fromLatin1(kTable), ipv4.join(QStringLiteral(", ")));
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
    sysctlWrite(QStringLiteral("net.ipv4.ip_forward"), m_savedIpForward);
    sysctlWrite(QStringLiteral("net.ipv6.conf.all.forwarding"), m_savedIp6Forward);
    m_savedIpForward.clear();
    m_savedIp6Forward.clear();
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
    const bool hasDnsNat = dump.contains(QStringLiteral("redirect to :17853"));
    const bool hasFwdChain = dump.contains(QStringLiteral("forward_accept"));
    if (!hasTproxy || !hasDnsNat || !hasFwdChain) {
        say("TPROXY-SELFTEST: FAIL 规则不完整 —— tproxy=%s",
            QString::number(hasTproxy) + " dnsnat=" + QString::number(hasDnsNat)
                + " forward=" + QString::number(hasFwdChain));
        say("\n%s\n", dump);
        r.remove();
        return 1;
    }

    if (!r.syncDevices({QStringLiteral("192.0.2.10"), QStringLiteral("192.0.2.11")}, &err)) {
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
    if (!r.syncDevices({QStringLiteral("192.0.2.11")}, &err)
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
bool TproxyRules::syncDevices(const QStringList &, QString *err)
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
