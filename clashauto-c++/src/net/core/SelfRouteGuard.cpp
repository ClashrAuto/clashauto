#include "SelfRouteGuard.h"

#include <QFile>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QProcess>
#include <QStringList>
#include <QTcpSocket>
#include <QTextStream>

#include <cerrno>

#if defined(Q_OS_WIN)
#  include <winsock2.h>
// clang-format off
#  include <ws2ipdef.h>
#  include <iphlpapi.h>
#  include <netioapi.h>
// clang-format on
#else
#  include <arpa/inet.h>
#  include <net/if.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

SelfRouteGuard::Iface g_iface;
bool g_enabled = false;

// Linux fwmark：取一个不太可能和别人撞的值。要与 `ip rule add fwmark 0x43535431 lookup main`
// 一致（0x43'53'54'31 = "CST1"，日志/`ip rule` 输出里一眼认得出是谁打的）。
constexpr quint32 kFwmark = 0x43535431u;

#if defined(Q_OS_LINUX)
// 从 /proc/net/route 找默认路由（Destination 全 0）里 metric 最小的那条。
// 跳过我们自己的 TUN：名字前缀 "coast" —— 否则 TUN 开起来后会把自己探成"物理网卡"，
// 那就等于没排除，照样环路。
SelfRouteGuard::Iface probeLinux()
{
    SelfRouteGuard::Iface out;
    QFile f(QStringLiteral("/proc/net/route"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    QTextStream ts(&f);
    ts.readLine(); // 表头
    int bestMetric = -1;
    while (!ts.atEnd()) {
        const QStringList c = ts.readLine().split(QLatin1Char('\t'), Qt::SkipEmptyParts);
        if (c.size() < 7)
            continue;
        const QString name = c[0].trimmed();
        if (c[1].trimmed() != QLatin1String("00000000"))
            continue; // 不是默认路由
        if (name.startsWith(QLatin1String("coast")))
            continue; // 我们自己的 TUN，不能当成出口
        const int metric = c[6].trimmed().toInt();
        if (bestMetric < 0 || metric < bestMetric) {
            bestMetric = metric;
            out.name = name;
            out.ifIndex = int(::if_nametoindex(name.toLatin1().constData()));
        }
    }
    return out;
}
#endif

#if defined(Q_OS_MACOS)
// `route -n get default` 的 "interface: enX"。走命令而不是 sysctl(NET_RT_DUMP)：这条路一次
// 网络变化才跑一次，解析 routing socket 的二进制结构不值当。
SelfRouteGuard::Iface probeMac()
{
    SelfRouteGuard::Iface out;
    QProcess p;
    p.start(QStringLiteral("/sbin/route"), {QStringLiteral("-n"), QStringLiteral("get"),
                                            QStringLiteral("default")});
    if (!p.waitForFinished(4000))
        return out;
    const QStringList lines = QString::fromLocal8Bit(p.readAllStandardOutput())
                                      .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        if (!t.startsWith(QLatin1String("interface:")))
            continue;
        const QString name = t.mid(10).trimmed();
        if (name.startsWith(QLatin1String("utun")))
            break; // 默认路由已经在某条 utun 上 —— 探不出物理出口，宁可返回无效
        out.name = name;
        out.ifIndex = int(::if_nametoindex(name.toLatin1().constData()));
        break;
    }
    return out;
}
#endif

#if defined(Q_OS_WIN)
// 找**物理**默认路由网卡。
//
// ★ 这里**不能**用 GetBestRoute2：它回答的是「现在这个包会走哪」，而只要有任何 TUN
//   （我们自己的、或者用户还开着的 mihomo，其网卡名通常是 "Meta"）抢了默认路由，
//   它就会把那张 TUN 报给你 —— 于是"排除自身流量"的出口本身还是 TUN，环路照旧。
//   这不是假想：本机第一次跑自检就探到了 ifIndex=3 "Meta"。
//   所以改成自己枚举默认路由，**按接口类型**筛掉一切虚拟/隧道网卡，只认以太网和 WiFi。
//   按名字筛是筛不干净的（谁知道下一个 TUN 叫什么）。
SelfRouteGuard::Iface probeWin()
{
    SelfRouteGuard::Iface out;
    MIB_IPFORWARD_TABLE2 *table = nullptr;
    if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR || !table)
        return out;

    ULONG bestMetric = ULONG_MAX;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2 &r = table->Table[i];
        if (r.DestinationPrefix.PrefixLength != 0)
            continue; // 只看默认路由

        MIB_IF_ROW2 ifr;
        ZeroMemory(&ifr, sizeof(ifr));
        ifr.InterfaceLuid = r.InterfaceLuid;
        if (GetIfEntry2(&ifr) != NO_ERROR)
            continue;
        // 只认真实的以太网 / WiFi。TUN 一律是 PROP_VIRTUAL / TUNNEL / PPP 之流。
        if (ifr.Type != IF_TYPE_ETHERNET_CSMACD && ifr.Type != IF_TYPE_IEEE80211)
            continue;
        if (ifr.OperStatus != IfOperStatusUp)
            continue;

        // 只用路由 metric：接口 metric 在 MIB_IPINTERFACE_ROW 里（MIB_IF_ROW2 没这个字段），
        // 为它再查一次表不值当 —— 物理网卡之间按路由 metric 排序已足够选出主出口。
        const ULONG metric = r.Metric;
        if (metric < bestMetric) {
            bestMetric = metric;
            out.ifIndex = int(r.InterfaceIndex);
            out.name = QString::fromWCharArray(ifr.Alias);
        }
    }
    FreeMibTable(table);
    return out;
}
#endif

} // namespace

SelfRouteGuard::Iface SelfRouteGuard::probePhysicalInterface()
{
#if defined(Q_OS_LINUX)
    return probeLinux();
#elif defined(Q_OS_MACOS)
    return probeMac();
#elif defined(Q_OS_WIN)
    return probeWin();
#else
    return {};
#endif
}

SelfRouteGuard::Iface SelfRouteGuard::refreshPhysicalInterface()
{
    g_iface = probePhysicalInterface();
    return g_iface;
}

SelfRouteGuard::Iface SelfRouteGuard::currentInterface()
{
    return g_iface;
}

void SelfRouteGuard::setEnabled(bool on)
{
    g_enabled = on;
}

bool SelfRouteGuard::enabled()
{
    return g_enabled;
}

quint32 SelfRouteGuard::fwmark()
{
    return kFwmark;
}

QString SelfRouteGuard::physicalAddress(bool v6)
{
    if (!g_iface.valid())
        return {};
    // 按 ifIndex 找那张网卡，取它的第一个**非回环、非链路本地**地址。
    // 用 ifIndex 而不是名字：名字在各平台的口径不一（见 L2Endpoint_win.cpp 里那段 LUID 名的坑）。
    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &ni : all) {
        if (ni.index() != g_iface.ifIndex)
            continue;
        const QList<QNetworkAddressEntry> entries = ni.addressEntries();
        for (const QNetworkAddressEntry &e : entries) {
            const QHostAddress a = e.ip();
            if (a.isLoopback() || a.isLinkLocal())
                continue;
            const bool isV6 = (a.protocol() == QAbstractSocket::IPv6Protocol);
            if (isV6 == v6)
                return a.toString();
        }
    }
    return {};
}

bool SelfRouteGuard::applyToFd(qintptr fd, int family, QString *err)
{
    if (!g_enabled)
        return true; // 没开 TUN：不该平白改变 socket 行为
    if (fd < 0) {
        if (err)
            *err = QStringLiteral("无效 fd");
        return false;
    }

#if defined(Q_OS_LINUX)
    Q_UNUSED(family);
    const quint32 mark = kFwmark;
    if (::setsockopt(int(fd), SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) != 0) {
        if (err)
            *err = QStringLiteral("SO_MARK 失败：%1（需要 CAP_NET_ADMIN）").arg(errno);
        return false;
    }
    return true;

#elif defined(Q_OS_MACOS)
    if (!g_iface.valid()) {
        if (err)
            *err = QStringLiteral("未探到物理出口网卡");
        return false;
    }
    const unsigned idx = unsigned(g_iface.ifIndex);
    const int level = (family == AF_INET6) ? IPPROTO_IPV6 : IPPROTO_IP;
    const int opt = (family == AF_INET6) ? IPV6_BOUND_IF : IP_BOUND_IF;
    if (::setsockopt(int(fd), level, opt, &idx, sizeof(idx)) != 0) {
        if (err)
            *err = QStringLiteral("%1 失败：%2")
                           .arg(family == AF_INET6 ? QStringLiteral("IPV6_BOUND_IF")
                                                   : QStringLiteral("IP_BOUND_IF"))
                           .arg(errno);
        return false;
    }
    return true;

#elif defined(Q_OS_WIN)
    if (!g_iface.valid()) {
        if (err)
            *err = QStringLiteral("未探到物理出口网卡");
        return false;
    }
    // ★ v4 要**网络序**的 ifindex，v6 要**主机序**。写反了 setsockopt 照样返回 0，只是不生效。
    if (family == AF_INET6) {
        const DWORD idx = DWORD(g_iface.ifIndex);
        if (::setsockopt(SOCKET(fd), IPPROTO_IPV6, IPV6_UNICAST_IF,
                         reinterpret_cast<const char *>(&idx), sizeof(idx))
            != 0) {
            if (err)
                *err = QStringLiteral("IPV6_UNICAST_IF 失败：%1").arg(WSAGetLastError());
            return false;
        }
    } else {
        const DWORD idx = htonl(DWORD(g_iface.ifIndex));
        if (::setsockopt(SOCKET(fd), IPPROTO_IP, IP_UNICAST_IF,
                         reinterpret_cast<const char *>(&idx), sizeof(idx))
            != 0) {
            if (err)
                *err = QStringLiteral("IP_UNICAST_IF 失败：%1").arg(WSAGetLastError());
            return false;
        }
    }
    return true;

#else
    Q_UNUSED(family);
    if (err)
        *err = QStringLiteral("本平台未实现");
    return false;
#endif
}

bool SelfRouteGuard::prepareSocket(QAbstractSocket *sock, QString *err)
{
    if (!g_enabled)
        return true;
    if (!sock) {
        if (err)
            *err = QStringLiteral("空 socket");
        return false;
    }

    // Qt 在 connectToHost 之前不建 socket —— 先 bind 把 fd 逼出来。
    // 绑 QHostAddress::Any = 双栈（Qt 会关掉 IPV6_V6ONLY），这样后面连 v4 还是 v6 都行，
    // 不必先解析域名。个别环境下双栈绑不上（禁了 IPv6），退回纯 v4。
    if (sock->socketDescriptor() == -1) {
        if (!sock->bind(QHostAddress(QHostAddress::Any), 0)
            && !sock->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
            if (err)
                *err = QStringLiteral("bind 失败：%1").arg(sock->errorString());
            return false;
        }
    }
    const qintptr fd = sock->socketDescriptor();
    if (fd == -1) {
        if (err)
            *err = QStringLiteral("bind 后仍拿不到 fd");
        return false;
    }

    // v4/v6 两个都打。双栈 socket 上 v4-mapped 的流量吃 v4 那个选项，纯 v6 的吃 v6 那个；
    // 只打一个会在另一族上静默失去保护。有一个成功就算保护住了（纯 v4 环境下 v6 那次必然失败）。
    QString e4, e6;
    const bool ok4 = applyToFd(fd, AF_INET, &e4);
    const bool ok6 = applyToFd(fd, AF_INET6, &e6);
    if (!ok4 && !ok6) {
        if (err)
            *err = QStringLiteral("v4/v6 均失败：%1 / %2").arg(e4, e6);
        return false;
    }
    return true;
}

bool SelfRouteGuard::selfTest(QString *report)
{
    QStringList log;
    bool ok = true;
    const bool wasEnabled = g_enabled;
    g_enabled = true;

    const Iface ifc = refreshPhysicalInterface();
    log << QStringLiteral("物理出口：%1 (ifIndex=%2)")
                   .arg(ifc.name.isEmpty() ? QStringLiteral("<未探到>") : ifc.name)
                   .arg(ifc.ifIndex);
    if (!ifc.valid()) {
        log << QStringLiteral("✗ 探不到物理默认路由网卡 —— 后面全免谈");
        ok = false;
    }

#if !defined(Q_OS_WIN)
    if (ok) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            log << QStringLiteral("✗ 建 socket 失败");
            ok = false;
        } else {
            QString err;
            if (!applyToFd(fd, AF_INET, &err)) {
                log << QStringLiteral("✗ applyToFd 失败：%1").arg(err);
                ok = false;
            } else {
                // ★ 读回来核对。这些 setsockopt 写错参数时多半**不报错也不生效**，
                //   只看 rc==0 等于没验 —— 这一步才是这个自检的意义所在。
#  if defined(Q_OS_LINUX)
                quint32 got = 0;
                socklen_t len = sizeof(got);
                if (::getsockopt(fd, SOL_SOCKET, SO_MARK, &got, &len) != 0 || got != kFwmark) {
                    log << QStringLiteral("✗ SO_MARK 读回不符：期望 0x%1 实得 0x%2")
                                   .arg(kFwmark, 0, 16)
                                   .arg(got, 0, 16);
                    ok = false;
                } else {
                    log << QStringLiteral("✓ SO_MARK 读回一致：0x%1").arg(got, 0, 16);
                }
#  elif defined(Q_OS_MACOS)
                unsigned got = 0;
                socklen_t len = sizeof(got);
                if (::getsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &got, &len) != 0
                    || int(got) != ifc.ifIndex) {
                    log << QStringLiteral("✗ IP_BOUND_IF 读回不符：期望 %1 实得 %2")
                                   .arg(ifc.ifIndex)
                                   .arg(got);
                    ok = false;
                } else {
                    log << QStringLiteral("✓ IP_BOUND_IF 读回一致：%1").arg(got);
                }
#  endif
                // 连通性：钉了网卡之后还得能连出去，否则这层保护就是把网关死。
                if (ok) {
                    QTcpSocket s;
                    s.setSocketDescriptor(qintptr(fd), QAbstractSocket::UnconnectedState);
                    s.connectToHost(QStringLiteral("223.5.5.5"), 80);
                    if (s.waitForConnected(5000)) {
                        log << QStringLiteral("✓ 钉住网卡后仍连得通 223.5.5.5:80（本地 %1）")
                                       .arg(s.localAddress().toString());
                    } else {
                        log << QStringLiteral("✗ 钉住网卡后连不通：%1").arg(s.errorString());
                        ok = false;
                    }
                    s.abort();
                }
#  if defined(Q_OS_MACOS)
                // ★★ 反向对照 —— 没有这一条，上面的 PASS 是假的。
                //   读回一致只证明「选项被存下了」，不证明它真的在**导流**。若 IP_BOUND_IF
                //   实际没生效，正向连接照样会通，整个自检就变成一句空话。所以这里故意钉到
                //   **回环口**再连公网：必须**连不通**。连通了 = 绑定没起作用 = 这层保护是空的。
                if (ok) {
                    const unsigned lo = ::if_nametoindex("lo0");
                    const int nfd = ::socket(AF_INET, SOCK_STREAM, 0);
                    if (lo != 0 && nfd >= 0
                        && ::setsockopt(nfd, IPPROTO_IP, IP_BOUND_IF, &lo, sizeof(lo)) == 0) {
                        QTcpSocket n;
                        n.setSocketDescriptor(qintptr(nfd), QAbstractSocket::UnconnectedState);
                        n.connectToHost(QStringLiteral("223.5.5.5"), 80);
                        if (n.waitForConnected(3000)) {
                            log << QStringLiteral(
                                    "✗ 反向对照失败：钉到 lo0 竟然也连得通 —— IP_BOUND_IF 没在导流，"
                                    "上面那条 ✓ 不作数");
                            ok = false;
                        } else {
                            log << QStringLiteral("✓ 反向对照：钉到 lo0 如期连不通（%1）")
                                           .arg(n.errorString());
                        }
                        n.abort();
                    } else {
                        log << QStringLiteral("· 反向对照跳过（拿不到 lo0 或建 socket 失败）");
                        if (nfd >= 0)
                            ::close(nfd);
                    }
                }
#  elif defined(Q_OS_LINUX)
                // Linux 这层**测不出导流**：SO_MARK 本身只是给包打标，真正决定走哪张网卡的是
                // 策略路由规则（`ip rule ... fwmark`）。没装规则时标记不改变任何行为，装了规则
                // 才有效 —— 所以这里只能验到「标打上了」。导流是否正确必须在 TUN 起来、规则装好
                // 之后用真流量验，那是端到端那一关的事，别在这里假装验过了。
                log << QStringLiteral("· Linux 侧到此为止：SO_MARK 只是打标，导流由 ip rule 决定，"
                                      "本自检验不到（需 TUN+规则就位后端到端验）");
#  endif
            }
            if (!ok)
                ::close(fd);
        }
    }
#else
    // 自检里我们自己建裸 socket，而 winsock 未初始化时 ::socket() 必然失败（WSANOTINITIALISED）。
    // 真实运行时 Qt 早已初始化过，但自检可能跑在任何网络代码之前 —— 本机第一次跑就栽在这。
    // WSAStartup 是引用计数的，多调一次无害。
    WSADATA wsa;
    const bool wsaOk = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    if (!wsaOk) {
        log << QStringLiteral("✗ WSAStartup 失败");
        ok = false;
    }
    if (ok) {
        const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            log << QStringLiteral("✗ 建 socket 失败");
            ok = false;
        } else {
            QString err;
            if (!applyToFd(qintptr(s), AF_INET, &err)) {
                log << QStringLiteral("✗ applyToFd 失败：%1").arg(err);
                ok = false;
            } else {
                // ★ set/get **字节序不对称**（真机实测，不是猜的）：setsockopt 要网络序的
                //   ifindex，getsockopt 却返回**主机序**。写 htonl(22)=0x16000000 进去，
                //   读回来是 22。所以这里按主机序比，apply 那边按网络序写，两处都对。
                //   一开始我两边都按网络序比，自检当场报「期望 369098752 实得 22」——
                //   这条不对称就是这么被逮住的。
                DWORD got = 0;
                int len = sizeof(got);
                const DWORD want = DWORD(ifc.ifIndex);
                if (::getsockopt(s, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<char *>(&got), &len)
                            != 0
                    || got != want) {
                    log << QStringLiteral("✗ IP_UNICAST_IF 读回不符：期望 %1 实得 %2")
                                   .arg(want)
                                   .arg(got);
                    ok = false;
                } else {
                    log << QStringLiteral("✓ IP_UNICAST_IF 读回一致：%1（写入时为网络序 %2）")
                                   .arg(got)
                                   .arg(htonl(got));
                }
                if (ok) {
                    QTcpSocket t;
                    t.setSocketDescriptor(qintptr(s), QAbstractSocket::UnconnectedState);
                    t.connectToHost(QStringLiteral("223.5.5.5"), 80);
                    if (t.waitForConnected(5000)) {
                        log << QStringLiteral("✓ 钉住网卡后仍连得通 223.5.5.5:80（本地 %1）")
                                       .arg(t.localAddress().toString());
                    } else {
                        log << QStringLiteral("✗ 钉住网卡后连不通：%1").arg(t.errorString());
                        ok = false;
                    }
                    t.abort();
                }
                // ★★ 反向对照 —— 同 macOS 那条的理由：读回一致只证明选项存下了，不证明在导流。
                //   IP_UNICAST_IF 尤其需要它：**v4 的 ifindex 要网络序**，写成主机序时
                //   setsockopt 照样返回 0、getsockopt 也照样读回你写进去的值，只是不生效 ——
                //   少了这条对照，把字节序写反的版本会显示 PASS。
                if (ok) {
                    SOCKADDR_INET lb;
                    ZeroMemory(&lb, sizeof(lb));
                    lb.si_family = AF_INET;
                    lb.Ipv4.sin_family = AF_INET;
                    lb.Ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    MIB_IPFORWARD_ROW2 lrow;
                    SOCKADDR_INET lbest;
                    const SOCKET ns = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (GetBestRoute2(nullptr, 0, nullptr, &lb, 0, &lrow, &lbest) == NO_ERROR
                        && ns != INVALID_SOCKET) {
                        const DWORD lidx = htonl(DWORD(lrow.InterfaceIndex));
                        ::setsockopt(ns, IPPROTO_IP, IP_UNICAST_IF,
                                     reinterpret_cast<const char *>(&lidx), sizeof(lidx));
                        QTcpSocket n;
                        n.setSocketDescriptor(qintptr(ns), QAbstractSocket::UnconnectedState);
                        n.connectToHost(QStringLiteral("223.5.5.5"), 80);
                        if (n.waitForConnected(3000)) {
                            log << QStringLiteral("✗ 反向对照失败：钉到回环口(ifIndex=%1)竟然也连得通"
                                                  " —— IP_UNICAST_IF 没在导流，上面那条 ✓ 不作数")
                                           .arg(lrow.InterfaceIndex);
                            ok = false;
                        } else {
                            log << QStringLiteral("✓ 反向对照：钉到回环口(ifIndex=%1)如期连不通（%2）")
                                           .arg(lrow.InterfaceIndex)
                                           .arg(n.errorString());
                        }
                        n.abort();
                    } else {
                        log << QStringLiteral("· 反向对照跳过（拿不到回环接口或建 socket 失败）");
                        if (ns != INVALID_SOCKET)
                            ::closesocket(ns);
                    }
                }
            }
            if (!ok)
                ::closesocket(s);
        }
    }
#endif

    // ———— prepareSocket()：真正会被出站用到的那条路 ————
    // 上面验的是「裸 fd + setsockopt」，而生产代码走的是 Qt 的 QTcpSocket。这两者之间夹着
    // 一个假设：**Qt 的 bind() 会把 fd 建出来，且之后 connectToHost 会沿用同一个 fd**。
    // 假设不成立的话，选项打在一个随后被丢弃的 fd 上 —— 照样一切正常、照样毫无保护。
    // 所以这里单独验它，并且同样配反向对照。
    if (ok) {
        QTcpSocket a;
        QString e;
        if (!prepareSocket(&a, &e)) {
            log << QStringLiteral("✗ prepareSocket 失败：%1").arg(e);
            ok = false;
        } else {
            a.connectToHost(QStringLiteral("223.5.5.5"), 80);
            if (a.waitForConnected(5000)) {
                log << QStringLiteral("✓ prepareSocket 后连得通（本地 %1）")
                               .arg(a.localAddress().toString());
            } else {
                log << QStringLiteral("✗ prepareSocket 后连不通：%1").arg(a.errorString());
                ok = false;
            }
            a.abort();
        }
    }
#if !defined(Q_OS_LINUX)
    // 反向对照：把"物理出口"临时改成回环口，prepareSocket 之后必须连不通。
    // 连通了 = bind→打选项→connect 这条链上有一环没生效（多半是 connectToHost 换了 fd）。
    // Linux 除外：SO_MARK 不导流，换成回环 ifindex 也不会有任何变化，这条对照在那边是空的。
    if (ok) {
        const Iface saved = g_iface;
#  if defined(Q_OS_WIN)
        g_iface.ifIndex = 1; // Loopback Pseudo-Interface
#  else
        g_iface.ifIndex = int(::if_nametoindex("lo0"));
#  endif
        g_iface.name = QStringLiteral("<loopback>");
        if (g_iface.ifIndex != 0) {
            QTcpSocket b;
            QString e;
            prepareSocket(&b, &e);
            b.connectToHost(QStringLiteral("223.5.5.5"), 80);
            if (b.waitForConnected(3000)) {
                log << QStringLiteral("✗ prepareSocket 反向对照失败：钉到回环口竟然也连得通 —— "
                                      "bind→setsockopt→connect 这条链上有一环没生效");
                ok = false;
            } else {
                log << QStringLiteral("✓ prepareSocket 反向对照：钉到回环口如期连不通（%1）")
                               .arg(b.errorString());
            }
            b.abort();
        } else {
            log << QStringLiteral("· prepareSocket 反向对照跳过（拿不到回环 ifIndex）");
        }
        g_iface = saved;
    }
#endif

    g_enabled = wasEnabled;
    log << (ok ? QStringLiteral("=== SelfRouteGuard 自检 PASS ===")
               : QStringLiteral("=== SelfRouteGuard 自检 FAIL ==="));
    if (report)
        *report = log.join(QLatin1Char('\n'));
    return ok;
}
