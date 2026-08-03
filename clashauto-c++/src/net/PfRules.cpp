#include "PfRules.h"

#include <QtGlobal>

#if defined(Q_OS_MACOS)

#include "../MacHelperClient.h" // 非 root 时 pfctl/sysctl 一律经特权 helper 执行

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// 固定 anchor 名。**别改成带 pid/随机串的名字**：崩溃后要靠这个名字把陈旧规则删掉，
// 名字对不上就删不掉，而残留的 rdr 规则会把被覆盖设备的流量一直往一个没人监听的口送。
//
// ★★ **必须挂在 `com.apple/` 下，不能用顶层名字。** macOS 默认的 /etc/pf.conf 主规则集里
//    只有一组 `com.apple/*` 引用：
//        scrub-anchor/nat-anchor/rdr-anchor/dummynet-anchor/anchor "com.apple/*"
//    没有被主规则集引用的 anchor **永远不会被求值** —— 而 pfctl 照样让你把规则装进去、
//    `pfctl -s nat` 照样列得出来。于是 install() 的回读检查（"有没有 rdr"）会通过，上层
//    以为接管成功，实际一个包都不会被重定向，**被接管设备全裸奔**。
//    真机对照实验（macOS 13.7.8，lo0 上 rdr 9999→9998，连得通=被求值）：
//        anchor "coast"            装入 1 条 rdr，探测 9999 = 拒绝 → **未被求值**
//        anchor "com.apple/coast"  装入 1 条 rdr，探测 9999 = 连通 → **被求值**
//    这就是为什么必须写成 "com.apple/coast"：借用系统已经引用好的通配挂载点，
//    **不用改用户的 /etc/pf.conf**（改它既要 root，又会在系统升级时被覆盖，还容易把系统
//    启动时加的规则一起冲掉 —— pfctl 自己就会警告这件事）。
//    代价：若某个系统服务重载了 com.apple anchor，我们这一支可能被一并清掉；靠上层的周期
//    重装兜底（与 Linux 侧 nft 被外部 flush 后的重装同理）。
constexpr const char *kAnchor = "com.apple/coast";

// 早期版本用过的顶层名字。它装了也不会被求值（见上），但 `pfctl -s nat` 里会留着，
// 既误导排查、也可能在用户手工把它引用起来后突然生效。启动清理时一并抹掉。
constexpr const char *kLegacyAnchor = "coast";

// ── Apple XNU 的 pf 用户态定义 ──────────────────────────────────────────────
// Apple **不在 SDK 里导出 net/pfvar.h**（实测 MacOSX.sdk 与 /usr/include 都没有），
// 所以这里按 apple-oss-distributions/xnu 的 bsd/net/pfvar.h 原样重声明。
// ★ 别照抄 OpenBSD：macOS 的端口字段是 union pf_state_xport（4 字节），OpenBSD 是裸的
//   u_int16_t（2 字节），照抄会让 af/proto/direction 全部错位、ioctl 读回垃圾地址。
struct coast_pf_addr {
    union {
        struct in_addr _v4addr;
        struct in6_addr _v6addr;
        u_int8_t _addr8[16];
        u_int16_t _addr16[8];
        u_int32_t _addr32[4];
    } pfa;
};
union coast_pf_state_xport {
    u_int16_t port;
    u_int16_t call_id;
    u_int32_t spi;
};
struct coast_pfioc_natlook {
    struct coast_pf_addr saddr, daddr, rsaddr, rdaddr;
    union coast_pf_state_xport sxport, dxport, rsxport, rdxport;
    sa_family_t af;
    u_int8_t proto, proto_variant, direction;
};
#define COAST_DIOCNATLOOK _IOWR('D', 23, struct coast_pfioc_natlook)
enum { COAST_PF_INOUT, COAST_PF_IN, COAST_PF_OUT };

// 同步跑一个命令，返回退出码；stdout+stderr 合并进 *out。
int run(const QString &prog, const QStringList &args, QString *out = nullptr,
        const QByteArray &stdinData = {})
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(prog, args);
    if (!p.waitForStarted(3000)) {
        if (out)
            *out = QStringLiteral("无法启动 %1").arg(prog);
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
    if (run(QStringLiteral("/usr/sbin/sysctl"), {QStringLiteral("-n"), key}, &out) == 0)
        return out.trimmed();
    return {};
}

void sysctlWrite(const QString &key, const QString &value)
{
    if (value.isEmpty())
        return;
    run(QStringLiteral("/usr/sbin/sysctl"), {QStringLiteral("-w"),
                                             key + QLatin1Char('=') + value});
}

// 被 install 改过的 sysctl 的原值存档。与 Linux 侧 TproxyRules 的 /run 存档同一套思路、
// 同样的理由：SIGKILL/OOM/断电不走析构，内存里的原值随进程消失，remove() 就永远还原不回去，
// 于是 ip.forwarding 被我们永久留成 1。放 /var/run（macOS 上是 /private/var/run，重启即空）。
QString sysctlStatePath()
{
    return QStringLiteral("/var/run/coast-pf-sysctl");
}

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
            return; // 已记过原值，别用我们写进去的值覆盖它
    lines.append(key + QLatin1Char('=') + value);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(lines.join(QLatin1Char('\n')).toUtf8());
        f.close();
    }
}

void sysctlStateRestoreAll()
{
    QFile f(sysctlStatePath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return;
    const QStringList lines =
            QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    f.close();
    for (const QString &l : lines) {
        const int eq = l.indexOf(QLatin1Char('='));
        if (eq > 0)
            sysctlWrite(l.left(eq), l.mid(eq + 1));
    }
    QFile::remove(sysctlStatePath());
}

} // namespace

const char *PfRules::anchorName()
{
    return kAnchor;
}

bool PfRules::available(QString *why)
{
    if (!QFile::exists(QStringLiteral("/sbin/pfctl"))) {
        if (why)
            *why = QStringLiteral("缺少 /sbin/pfctl");
        return false;
    }
    // /dev/pf 需要 root 才能打开。真机实测（macOS 13.7.8，/dev/pf 权限 crw------- root:wheel）：
    //     普通用户：O_RDWR 与 **O_RDONLY 都** Permission denied；`pfctl -s info` 同样失败
    //     root    ：O_RDONLY 成功（只读即够，DIOCNATLOOK 是 _IOWR 但设备只需读权限打开）
    // 所以 root 是**两侧都要**的硬门槛，不是只有核心需要：
    //   · 本进程（App）要 root —— install/remove/syncDevices 全靠 pfctl；
    //   · 核心进程也要 root —— 它自己打开 /dev/pf 做 DIOCNATLOOK 还原原始目的地
    //     （见 PfRules.h 里那段：非 root 时它不报错、只是静默丢连接）。
    // 用 O_RDONLY 探测：够用且不会因为写权限问题误判成"不可用"。
    const int fd = ::open("/dev/pf", O_RDONLY);
    if (fd >= 0) {
        ::close(fd);
        return true; // 本进程就是 root（headless 自测 / sudo 运行）：直接自己干
    }
    // ★ 打不开是**常态而非异常**：正式的 .app 是普通用户 uid 启动的 GUI，真机实测
    //   （macOS 13.7.8，uid 501）open("/dev/pf", O_RDONLY) = Permission denied、
    //   `pfctl -s info` 同样 Permission denied。若就此判 available()=false，pf 网关对
    //   **每一个正常用户**都是不可用的 —— 那这条数据面等于没做。
    //   正确出路是走已有的特权 helper（它是 root，已经在替我们起核心/开 BPF）。
    if (MacHelper::isReady(1)) // 1 次 ping：这里是可用性探测，不该为冷启动等满 3 次超时
        return true;
    if (why)
        *why = QStringLiteral("/dev/pf 打不开（%1）且免密助手未就绪——pf 网关需要 root，"
                              "请在「设置 → 系统」安装并批准免密助手")
                       .arg(QString::fromLocal8Bit(strerror(errno)));
    return false;
}

namespace {
/// 本进程是不是 root。false 时 pfctl/sysctl 一律走 helper（见 available() 的说明）。
bool selfIsRoot()
{
    return ::geteuid() == 0;
}
} // namespace

void PfRules::removeStale()
{
    if (!selfIsRoot()) {
        // 非 root：pfctl 连 /dev/pf 都打不开，自己清是空转。交给 helper（它是 root）。
        // helper 没装/没批准时清不了残留 —— 但那种情况下我们也从没装成功过，无残留可言。
        QString err;
        MacHelper::pfRemove(&err);
        return;
    }
    // 幂等清理：anchor 里的规则 + sysctl 还原。上次若被 kill -9，这些都还留在系统里。
    // ★ 只清我们自己的 anchor，**绝不碰主规则集**：`pfctl -f` 主规则集会把系统启动时
    //   加的规则一起冲掉（pfctl 自己会警告 "could result in flushing of rules present
    //   in the main ruleset added by the system at startup"）。
    run(QStringLiteral("/sbin/pfctl"),
        {QStringLiteral("-a"), QString::fromLatin1(kAnchor), QStringLiteral("-F"),
         QStringLiteral("all")});
    // 顺手清掉早期版本留在顶层 "coast" 里的规则（那批规则不会被求值，但会留在 pfctl 输出里）。
    run(QStringLiteral("/sbin/pfctl"),
        {QStringLiteral("-a"), QString::fromLatin1(kLegacyAnchor), QStringLiteral("-F"),
         QStringLiteral("all")});
    sysctlStateRestoreAll();
}

PfRules::~PfRules()
{
    remove();
}

bool PfRules::install(const Spec &spec, QString *err)
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
    if (spec.redirPort == 0) {
        if (err)
            *err = QStringLiteral("redirPort 未设置");
        return false;
    }
    m_spec = spec;

    if (!selfIsRoot()) {
        // 正式 .app 走的就是这条：pfctl / 写 forwarding 都要 root，GUI 进程一件都做不了。
        // helper 端做的事与下面进程内那套**逐条对应**（含临时文件、回读核实、挂载点检查），
        // 改任何一边都要同步另一边。
        if (!MacHelper::pfInstall(spec.redirPort, spec.dnsPort, spec.ifnames, err))
            return false;
        m_installed = true;
        return true;
    }

    removeStale(); // 上一次可能是被 kill -9 打死的

    // 打开转发。原值落盘存档：SIGKILL 不走析构，只靠成员变量的话 ip.forwarding 会被永久留成 1。
    const QString fwd = sysctlRead(QStringLiteral("net.inet.ip.forwarding"));
    sysctlStateSave(QStringLiteral("net.inet.ip.forwarding"), fwd);
    sysctlWrite(QStringLiteral("net.inet.ip.forwarding"), QStringLiteral("1"));

    // ── anchor 内的规则 ────────────────────────────────────────────────────
    //  table <coast_proxied>：被接管设备的 IPv4。**空表时 rdr 一条都不命中**，等价于「没开
    //    代理」，所以装规则本身是安全的 —— 真正决定谁被接管的是 syncDevices()。
    //  DNS 那条必须排在前面：否则 53 会被下面那条通配 rdr 一起截走，DNS 就走不到核心的
    //    DNS 监听上（与 Linux 侧 prerouting 里 DNS 先 return 同理）。
    QString rules = QStringLiteral("table <coast_proxied> persist\n");
    const QStringList ifs = spec.ifnames.isEmpty() ? QStringList{QStringLiteral("en0")}
                                                   : spec.ifnames;
    for (const QString &ifn : ifs) {
        if (spec.dnsPort != 0) {
            rules += QStringLiteral("rdr pass on %1 inet proto udp from <coast_proxied> "
                                    "to any port 53 -> 127.0.0.1 port %2\n")
                             .arg(ifn)
                             .arg(spec.dnsPort);
        }
        rules += QStringLiteral("rdr pass on %1 inet proto tcp from <coast_proxied> "
                                "to any -> 127.0.0.1 port %2\n")
                         .arg(ifn)
                         .arg(spec.redirPort);
    }

    // ★ **必须写临时文件，不能用 `pfctl -f -` 从 stdin 喂**。真机实测（macOS 13.7.8）：
    //   同样的规则内容，`pfctl -a coast -f 文件` 装上 1 条；`pfctl -a coast -f -` 从 stdin 喂
    //   则**静默失败** —— 不报错、退出码 0、规则数 0。照 Linux 那边 `nft -f -` 的习惯写就会
    //   踩这个坑，而且因为没有任何错误输出，会表现为「install 返回成功但设备完全没被接管」。
    //   （Linux 的 nft 从 stdin 读是正常工作的，所以这是 pfctl 独有的行为差异。）
    QString tmpPath = QStringLiteral("/tmp/coast-pf-%1.conf").arg(QCoreApplication::applicationPid());
    {
        QFile tf(tmpPath);
        if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (err)
                *err = QStringLiteral("无法写临时规则文件 %1").arg(tmpPath);
            remove();
            return false;
        }
        tf.write(rules.toUtf8());
        tf.close();
    }
    QString out;
    const int rc = run(QStringLiteral("/sbin/pfctl"),
                       {QStringLiteral("-a"), QString::fromLatin1(kAnchor), QStringLiteral("-f"),
                        tmpPath},
                       &out);
    QFile::remove(tmpPath);
    if (rc != 0) {
        if (err)
            *err = QStringLiteral("pfctl 装载 anchor 失败：%1").arg(out);
        remove(); // 不留半成品
        return false;
    }
    // pf 本身可能是关的（macOS 默认 Disabled）。-e 在已启用时返回非 0，属正常，不当失败。
    run(QStringLiteral("/sbin/pfctl"), {QStringLiteral("-e")});

    // ★ 回读核实：pfctl 退出码为 0 **不代表规则真装上了**（上面 stdin 那个坑就是退出码 0
    //   却一条没装）。这里把 anchor 里的规则读回来数一遍，装不上就当失败并拆干净 ——
    //   否则上层会以为接管成功、而设备实际全裸奔。
    QString check;
    run(QStringLiteral("/sbin/pfctl"),
        {QStringLiteral("-a"), QString::fromLatin1(kAnchor), QStringLiteral("-s"),
         QStringLiteral("nat")},
        &check);
    if (!check.contains(QStringLiteral("rdr"))) {
        if (err)
            *err = QStringLiteral("规则装载后回读为空（pfctl 报成功却一条没装）");
        remove();
        return false;
    }

    // ★ 回读到 rdr **仍不等于规则会被求值**：没有被主规则集引用的 anchor，pfctl 照样让你装、
    //   照样列得出来，但内核根本不会走到它（真机对照实验见文件顶部 kAnchor 处）。这正是
    //   上面那条 contains("rdr") 检查曾经漏掉的失效形态 —— 装载成功、UI 显示已接管、设备全裸奔。
    //   所以这里再核一次挂载点：主规则集必须有 `rdr-anchor "com.apple/*"`（macOS 默认就有；
    //   用户把 /etc/pf.conf 改过才会没有）。查不到就**失败并拆干净**，而不是留个假的成功。
    QString mainNat;
    run(QStringLiteral("/sbin/pfctl"), {QStringLiteral("-s"), QStringLiteral("nat")}, &mainNat);
    if (!mainNat.contains(QStringLiteral("rdr-anchor \"com.apple/"))) {
        if (err)
            *err = QStringLiteral("pf 主规则集没有 `rdr-anchor \"com.apple/*\"` 挂载点，"
                                  "装进 %1 的规则不会被求值（/etc/pf.conf 被改过？）。"
                                  "恢复默认 pf.conf 后重试。")
                           .arg(QString::fromLatin1(kAnchor));
        remove();
        return false;
    }

    m_installed = true;
    return true;
}

void PfRules::remove()
{
    // ★ **只拆自己装过的**。原先这里是 `if (!m_installed && !存档存在) return;` —— 于是任何
    //   没 install 过的临时实例，只要系统里存在别人留下的存档文件，析构时就会把**正在生效的**
    //   anchor 规则清掉。真机踩到过：一个只用来做静态 lookup 的驱动进程退出时，把刚装好的
    //   rdr 规则整条删了（rdr 规则数 1 → 0），表现为"规则莫名其妙消失"，极难定位。
    //   崩溃残留的清理有专门入口 removeStale()（启动时调一次），不该由析构越权代劳。
    if (!m_installed)
        return;
    if (!selfIsRoot()) {
        QString err;
        MacHelper::pfRemove(&err);
        m_installed = false;
        return;
    }
    run(QStringLiteral("/sbin/pfctl"),
        {QStringLiteral("-a"), QString::fromLatin1(kAnchor), QStringLiteral("-F"),
         QStringLiteral("all")});
    // ★ **不执行 `pfctl -d`**：pf 可能是用户自己开着在用的（防火墙规则、其它 anchor），
    //   我们只负责把自己的 anchor 清掉。关掉整个 pf 等于替用户关防火墙。
    sysctlStateRestoreAll();
    m_installed = false;
}

bool PfRules::syncDevices(const QStringList &ipv4, QString *err)
{
    if (!m_installed) {
        if (err)
            *err = QStringLiteral("规则未安装");
        return false;
    }
    if (!selfIsRoot())
        return MacHelper::pfSyncProxied(ipv4, err);
    // 整体替换而不是逐个增删：调用方给的是「当前应当被代理的全集」。
    // 空集合要用 -T flush（-T replace 不接受空参数列表）。
    QStringList args{QStringLiteral("-a"), QString::fromLatin1(kAnchor), QStringLiteral("-t"),
                     QStringLiteral("coast_proxied"), QStringLiteral("-T")};
    if (ipv4.isEmpty()) {
        args << QStringLiteral("flush");
    } else {
        args << QStringLiteral("replace") << ipv4;
    }
    QString out;
    if (run(QStringLiteral("/sbin/pfctl"), args, &out) != 0) {
        if (err)
            *err = QStringLiteral("pfctl 更新 table 失败：%1").arg(out);
        return false;
    }
    return true;
}

bool PfRules::lookupOriginalDest(const QString &clientIp, quint16 clientPort,
                                 const QString &proxyIp, quint16 proxyPort, QString *origIp,
                                 quint16 *origPort, QString *err)
{
    const int fd = ::open("/dev/pf", O_RDWR);
    if (fd < 0) {
        if (err)
            *err = QStringLiteral("/dev/pf 打不开（需 root）：%1")
                           .arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    struct coast_pfioc_natlook nl;
    std::memset(&nl, 0, sizeof(nl));
    nl.af = AF_INET;
    nl.proto = IPPROTO_TCP;
    // ★ 方向必须是 PF_OUT。直觉上「设备发进来」像是 PF_IN，但 pf 记的是**被重定向后**那条
    //   状态的方向：包在 rdr 之后是从本机出去到 127.0.0.1 的，所以查 out 方向。
    //   填错方向不会报错，只会一直 ENOENT —— 排查时极易误判成"规则没生效"。
    nl.direction = COAST_PF_OUT;
    nl.saddr.pfa._v4addr.s_addr = ::inet_addr(clientIp.toLatin1().constData());
    nl.daddr.pfa._v4addr.s_addr = ::inet_addr(proxyIp.toLatin1().constData());
    nl.sxport.port = htons(clientPort);
    nl.dxport.port = htons(proxyPort);

    if (::ioctl(fd, COAST_DIOCNATLOOK, &nl) < 0) {
        const int e = errno;
        ::close(fd);
        if (err)
            *err = QStringLiteral("DIOCNATLOOK 失败：%1")
                           .arg(QString::fromLocal8Bit(strerror(e)));
        return false;
    }
    ::close(fd);
    // rdaddr/rdxport = 重定向**之前**的真实目的地，正是我们要的。
    char buf[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &nl.rdaddr.pfa._v4addr, buf, sizeof(buf));
    if (origIp)
        *origIp = QString::fromLatin1(buf);
    if (origPort)
        *origPort = ntohs(nl.rdxport.port);
    return true;
}

QString PfRules::dumpAnchor()
{
    // ★ 必须合并 stderr：pfctl 把规则列表写到 **stderr**，只读 stdout 恒为空
    //   （run() 用 MergedChannels，所以这里天然是对的；手工敲命令时记得 2>&1）。
    QString rules;
    run(QStringLiteral("/sbin/pfctl"),
        {QStringLiteral("-a"), QString::fromLatin1(kAnchor), QStringLiteral("-s"),
         QStringLiteral("nat")},
        &rules);
    // ★★ 表内容要**单独读**：`pfctl -s all` / `-s Tables` 只列出表**名**，不含成员地址。
    //    真机上踩过——自测据此判定"设备没进 table"，而实际上 -T replace 早就写进去了，
    //    错的是核对方法不是产品代码。要看成员必须 `-t <表> -T show`。
    QString table;
    run(QStringLiteral("/sbin/pfctl"),
        {QStringLiteral("-a"), QString::fromLatin1(kAnchor), QStringLiteral("-t"),
         QStringLiteral("coast_proxied"), QStringLiteral("-T"), QStringLiteral("show")},
        &table);
    return rules + QStringLiteral("\n--- table coast_proxied ---\n") + table;
}

int runPfRulesSelfTest()
{
    auto say = [](const char *fmt, const QString &a = {}) {
        std::fprintf(stderr, fmt, a.toLocal8Bit().constData());
        std::fflush(stderr);
    };
    QString why;
    if (!PfRules::available(&why)) {
        say("PF-SELFTEST: SKIP —— %s\n", why);
        return 0;
    }
    // ★ 本自测**不产生任何真实流量、也不接管任何设备**：rdr 只对 <coast_proxied> 表里的源地址
    //   生效，而这里只往表里放 TEST-NET-1（192.0.2.0/24，RFC 5737 保留给文档/测试，网上不可路由）。
    //   所以在一台正在用的机器上跑它是安全的 —— 与 Linux 侧 TPROXY 自测同一套约定。
    PfRules::removeStale();

    PfRules r;
    PfRules::Spec spec;
    spec.redirPort = 17897;
    spec.dnsPort = 17853;
    spec.ifnames = QStringList{QStringLiteral("lo0")}; // 环回：规则装得上，又绝不影响真实网卡
    QString err;
    if (!r.install(spec, &err)) {
        say("PF-SELFTEST: FAIL 装载失败 —— %s\n", err);
        return 1;
    }
    QString dump = PfRules::dumpAnchor();
    const bool hasRedir = dump.contains(QStringLiteral("port 17897"));
    const bool hasDns = dump.contains(QStringLiteral("port 17853"));
    const bool hasTable = dump.contains(QStringLiteral("coast_proxied"));
    if (!hasRedir || !hasDns || !hasTable) {
        say("PF-SELFTEST: FAIL 规则不完整 —— %s",
            QStringLiteral("redir=") + QString::number(hasRedir) + " dns="
                + QString::number(hasDns) + " table=" + QString::number(hasTable));
        say("\n%s\n", dump);
        r.remove();
        return 1;
    }

    if (!r.syncDevices({QStringLiteral("192.0.2.10"), QStringLiteral("192.0.2.11")}, &err)) {
        say("PF-SELFTEST: FAIL 设备集合写入失败 —— %s\n", err);
        r.remove();
        return 1;
    }
    dump = PfRules::dumpAnchor();
    if (!dump.contains(QStringLiteral("192.0.2.10"))
        || !dump.contains(QStringLiteral("192.0.2.11"))) {
        say("PF-SELFTEST: FAIL 设备没进 table\n%s\n", dump);
        r.remove();
        return 1;
    }
    // 换成只剩一台：验证「整体替换」而不是只增不减。
    if (!r.syncDevices({QStringLiteral("192.0.2.11")}, &err)
        || PfRules::dumpAnchor().contains(QStringLiteral("192.0.2.10"))) {
        say("PF-SELFTEST: FAIL 设备移除没生效\n");
        r.remove();
        return 1;
    }

    r.remove();
    dump = PfRules::dumpAnchor();
    if (dump.contains(QStringLiteral("rdr")) || dump.contains(QStringLiteral("192.0.2."))) {
        say("PF-SELFTEST: FAIL 拆除后仍有残留\n%s\n", dump);
        return 1;
    }
    // 再跑一次 removeStale：崩溃后启动就是"什么都没有"这个场景，必须能安全调用。
    PfRules::removeStale();
    say("PF-SELFTEST: PASS（装载 + 挂载点核实 + 设备增删 + 幂等拆除）\n");
    return 0;
}

#else // 非 macOS：空实现，让跨平台调用方不必写平台分支（与 TproxyRules 同一套做法）

QString PfRules::dumpAnchor()
{
    return {};
}

int runPfRulesSelfTest()
{
    std::fprintf(stderr, "PF-SELFTEST: SKIP —— pf 数据面仅 macOS 可用\n");
    return 0;
}


bool PfRules::available(QString *why)
{
    if (why)
        *why = QStringLiteral("pf 数据面仅 macOS 可用");
    return false;
}

void PfRules::removeStale() {}

PfRules::~PfRules() = default;

bool PfRules::install(const Spec &, QString *err)
{
    if (err)
        *err = QStringLiteral("pf 数据面仅 macOS 可用");
    return false;
}

void PfRules::remove() {}

bool PfRules::syncDevices(const QStringList &, QString *err)
{
    if (err)
        *err = QStringLiteral("pf 数据面仅 macOS 可用");
    return false;
}

bool PfRules::lookupOriginalDest(const QString &, quint16, const QString &, quint16, QString *,
                                 quint16 *, QString *err)
{
    if (err)
        *err = QStringLiteral("pf 数据面仅 macOS 可用");
    return false;
}

const char *PfRules::anchorName()
{
    return "coast";
}

#endif
