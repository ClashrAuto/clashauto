#include "PfRules.h"

#include <QtGlobal>

#if defined(Q_OS_MACOS)

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
constexpr const char *kAnchor = "coast";

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
    // /dev/pf 需要 root 才能打开；打不开就取不到原始目的地，整条路走不通。
    const int fd = ::open("/dev/pf", O_RDWR);
    if (fd < 0) {
        if (why)
            *why = QStringLiteral("/dev/pf 打不开（需要 root）：%1")
                           .arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    ::close(fd);
    return true;
}

void PfRules::removeStale()
{
    // 幂等清理：anchor 里的规则 + sysctl 还原。上次若被 kill -9，这些都还留在系统里。
    // ★ 只清我们自己的 anchor，**绝不碰主规则集**：`pfctl -f` 主规则集会把系统启动时
    //   加的规则一起冲掉（pfctl 自己会警告 "could result in flushing of rules present
    //   in the main ruleset added by the system at startup"）。
    run(QStringLiteral("/sbin/pfctl"),
        {QStringLiteral("-a"), QString::fromLatin1(kAnchor), QStringLiteral("-F"),
         QStringLiteral("all")});
    sysctlStateRestoreAll();
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

#else // 非 macOS：空实现，让跨平台调用方不必写平台分支（与 TproxyRules 同一套做法）

bool PfRules::available(QString *why)
{
    if (why)
        *why = QStringLiteral("pf 数据面仅 macOS 可用");
    return false;
}

void PfRules::removeStale() {}

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
