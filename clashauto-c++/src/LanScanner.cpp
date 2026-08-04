// Windows 的默认路由改用 IP Helper API（见 defaultRoutes）。winsock2.h 必须排在任何会拉进
// windows.h 的头之前，故这段放在最前面，并用编译器内建宏 _WIN32 作守卫（此时还没有 Qt 的 Q_OS_WIN）。
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#endif

#include "LanScanner.h"

#include <QDateTime>
#include <QFile>
#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

#include <functional>
#include <memory>

namespace {
// 探测端口集：既为逼系统 ARP，也作类型指纹。
//  80/443 通用；445 SMB(Windows)；62078 iOS lockdown；22 ssh；
//  8009 Chromecast；5555 adb(Android)；9100 打印机 raw；554 RTSP(摄像头)；32469 DLNA。
const quint16 kProbePorts[] = {80, 443, 445, 62078, 22, 8009, 5555, 9100, 554, 32469};
constexpr quint16 kArpPort = 80;          // 阶段①：逼系统做 ARP 只需要连这一个端口
// 同时在途 TCP 探测上限 / 每轮事件循环最多新建的 socket 数。
//
// 这两个数管的是**两件不同的事**，曾经被混为一谈：卡顿来自「一次事件循环里建 120 个 socket」
// （实测按住 GUI 线程 1846ms），治它的是 kPumpSlice —— 分摊到多轮事件循环去建；而 kMaxInFlight
// 决定的是吞吐。当时把 120 一路压到 24，等于顺手把扫描拖慢了 5 倍：网段里绝大多数 IP 是空的，
// 每个都要等满 kProbeTimeoutMs 才收口，阶段①的墙上时间 ≈ ceil(253/并发) × 1.2s ——
// 24 并发要 12.7s，列表就得干等这么久才刷出来（「设备列表刷新很慢」正是它）。
// 现在：并发拉回 128（/24 两批 ≈ 2.4s 探完），建 socket 仍按每轮 16 个切片摊开（≈8ms/轮）。
// 改这里前先想清楚：要顺滑就调 kPumpSlice，要快就调 kMaxInFlight，别再用后者去治前者的病。
constexpr int kMaxInFlight = 128;
constexpr int kPumpSlice = 16;
constexpr int kNbnsSlice = 32;            // 每轮事件循环最多发的 NBNS 查询数
constexpr int kProbeTimeoutMs = 1200;
constexpr int kSettleMs = 2500;           // 探测发完后等收 ARP/名称回包的静默期
constexpr int kScanWatchdogMs = 45000;    // 两阶段都卡死时的兜底（强制收尾，不让 UI 停在「扫描中」）

QString macFromLine(const QString &line)
{
    static const QRegularExpression re(
        "([0-9A-Fa-f]{1,2}[:-]){5}[0-9A-Fa-f]{1,2}");
    const auto m = re.match(line);
    return m.hasMatch() ? m.captured(0) : QString();
}
QString ipv4FromLine(const QString &line)
{
    static const QRegularExpression re("(\\d{1,3}\\.){3}\\d{1,3}");
    const auto m = re.match(line);
    return m.hasMatch() ? m.captured(0) : QString();
}

// ARP 表里的非设备条目：组播（224.0.0.0/4 与 01:00:5e/33:33 开头的 MAC）、广播（255.255.255.255、
// 网段广播地址，MAC 全 f）、空 MAC。系统 arp 表里这些一直都在，不过滤会当成一堆假设备显示。
bool isJunkArpEntry(const QString &ip, const QString &mac)
{
    if (ip.isEmpty() || mac.isEmpty())
        return true;
    const QHostAddress a(ip);
    if (a.protocol() != QAbstractSocket::IPv4Protocol)
        return true;
    const quint32 v = a.toIPv4Address();
    if (v == 0 || v == 0xFFFFFFFFu)
        return true;
    if ((v & 0xF0000000u) == 0xE0000000u)
        return true; // 224.0.0.0/4 组播
    if (mac == QStringLiteral("ff:ff:ff:ff:ff:ff") || mac == QStringLiteral("00:00:00:00:00:00"))
        return true; // 广播 / 空
    return mac.startsWith(QStringLiteral("01:00:5e")) || mac.startsWith(QStringLiteral("33:33"));
}
} // namespace

LanScanner::LanScanner(QObject *parent) : QObject(parent)
{
    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    connect(m_settleTimer, &QTimer::timeout, this, &LanScanner::assemble);
    loadOui();
    setupMdns();
    setupSsdp();
    setupNbns();
}

LanScanner::~LanScanner() = default;

LanScanner::Signals &LanScanner::sig(const QString &ip)
{
    auto it = m_sig.find(ip);
    if (it == m_sig.end())
        it = m_sig.insert(ip, Signals{});
    return *it;
}

// ———————————————————————————— 拓扑探测 ————————————————————————————
namespace {
// mihomo TUN 的假网段：198.18.0.0/15（当前 TUN inet4 默认）、172.19.0.0/16（旧默认）。
bool isTunSubnet(quint32 ip)
{
    return (ip & 0xFFFE0000u) == 0xC6120000u || (ip & 0xFFFF0000u) == 0xAC130000u;
}

// 虚拟网卡判定。**必须排除**：这些网卡不能做二层收发，它们的网段也不是「局域网」。
// 尤其是开「增强模式」后 mihomo 建的 TUN（Windows 叫 Meta，mac/Linux 是 utun/tun）——
// 它在 QNetworkInterface::allInterfaces() 里往往排在最前面，一旦被选成主网卡，本机 MAC 取空、
// 网段变成 /30 假网段，本机就从快照里消失 → 被 15s 陈旧判定判成「掉线」。
bool isVirtualIface(const QNetworkInterface &iface, quint32 ip)
{
    static const QRegularExpression re(
        "meta|mihomo|clash|utun|wintun|tunnel|\\btun\\d|\\btap\\d|vethernet|hyper-?v|vmware|"
        "virtualbox|vbox|docker|\\bwsl|zerotier|tailscale|wireguard|\\bwg\\d|loopback|teredo|isatap",
        QRegularExpression::CaseInsensitiveOption);
    if (re.match(iface.humanReadableName()).hasMatch() || re.match(iface.name()).hasMatch())
        return true;
    if (DeviceStore::normalizeMac(iface.hardwareAddress()).isEmpty())
        return true; // 没有 MAC = 三层虚拟网卡（wintun 等），二层收发无从谈起
    return isTunSubnet(ip);
}

// 系统路由表里的全部默认路由 → (网关 IP, 网卡标识)。
// 网卡标识一律是接口名（Windows 上即适配器 GUID，由接口索引查得）；macOS 拿不到时给空串。
// 调用方按「接口名或接口 IP 对得上」配对，两种都兼容。
QVector<QPair<QString, QString>> defaultRoutes()
{
    QVector<QPair<QString, QString>> out;
    auto validIp = [](const QString &s) {
        const QHostAddress a(s);
        return a.protocol() == QAbstractSocket::IPv4Protocol && a.toIPv4Address() != 0;
    };
#if defined(Q_OS_LINUX)
    QFile rf("/proc/net/route");
    if (rf.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = rf.readAll().split('\n');
        for (const QByteArray &l : lines) {
            const QList<QByteArray> f = l.simplified().split(' ');
            // 字段：Iface Destination Gateway Flags ...；目的地 00000000 即默认路由（多网卡多行）。
            if (f.size() >= 3 && f.at(1) == "00000000") {
                bool ok = false;
                // 网关是十六进制、按内存(小端)存的：值的第 0 字节即第一个点分段。
                const quint32 v = f.at(2).toUInt(&ok, 16);
                const QString gw = QString("%1.%2.%3.%4")
                                       .arg(v & 0xFF).arg((v >> 8) & 0xFF)
                                       .arg((v >> 16) & 0xFF).arg((v >> 24) & 0xFF);
                if (ok && v && validIp(gw))
                    out.append({gw, QString::fromLatin1(f.at(0))});
            }
        }
        rf.close();
    }
#elif defined(Q_OS_MACOS)
    {
        QProcess p;
        p.start("netstat", {"-rn", "-f", "inet"});
        if (p.waitForFinished(2000)) {
            const QString text = QString::fromLocal8Bit(p.readAllStandardOutput());
            // 行："default  192.168.1.1  UGScg  en0 [Expire]"
            for (const QString &row : text.split('\n')) {
                const QStringList c = row.simplified().split(' ');
                if (c.size() >= 4 && c.at(0) == QStringLiteral("default") && validIp(c.at(1)))
                    out.append({c.at(1), c.at(3)});
            }
        }
    }
    if (out.isEmpty()) { // 兜底：只问默认路由（拿不到接口名）
        QProcess p;
        p.start("route", {"-n", "get", "default"});
        if (p.waitForFinished(2000)) {
            static const QRegularExpression re("gateway:\\s*(\\S+)");
            const auto m = re.match(QString::fromLocal8Bit(p.readAllStandardOutput()));
            if (m.hasMatch() && validIp(m.captured(1)))
                out.append({m.captured(1), QString()});
        }
    }
#else // Windows
    // 走 IP Helper API，**不再 spawn `route print`**：那是个同步 QProcess + waitForFinished(2000)，
    // 实测占住 GUI 线程 ~60ms，而 refreshLiveness 每 5 秒就要来一次 —— 设备页里最稳定的一段
    // 周期性卡顿就是它。GetIpForwardTable2 是纯内存查询，微秒级，还免了「解析本地化命令输出」。
    {
        MIB_IPFORWARD_TABLE2 *table = nullptr;
        if (GetIpForwardTable2(AF_INET, &table) == NO_ERROR && table) {
            for (ULONG i = 0; i < table->NumEntries; ++i) {
                const MIB_IPFORWARD_ROW2 &r = table->Table[i];
                if (r.DestinationPrefix.PrefixLength != 0)
                    continue; // 只要默认路由（0.0.0.0/0）
                const quint32 gw = ntohl(r.NextHop.Ipv4.sin_addr.s_addr);
                const QString gwStr = QHostAddress(gw).toString();
                if (!validIp(gwStr))
                    continue;
                // 网卡标识用 Qt 的接口名（Windows 上即适配器 GUID）——调用方拿它和 LocalIface::name
                // 比对。索引→接口由 Qt 查表完成，与 QNetworkInterface::allInterfaces() 同源。
                const QString ifName =
                    QNetworkInterface::interfaceFromIndex(static_cast<int>(r.InterfaceIndex)).name();
                out.append({gwStr, ifName});
            }
            FreeMibTable(table);
        }
    }
#endif
    return out;
}
} // namespace

// 带缓存的拓扑探测：refreshLiveness 每 5s 一次、scanFull 每次进页面都要，而网卡/路由几乎不变。
// force=true（扫描入口）永远重算；其余走 kTopologyTtlMs 缓存。
void LanScanner::ensureTopology(bool force)
{
    if (!force && m_topologyAge.isValid() && m_topologyAge.elapsed() < kTopologyTtlMs)
        return;
    detectLocalTopology();
    m_topologyAge.restart();
}

void LanScanner::detectLocalTopology()
{
    m_localIp.clear();
    m_localMac.clear();
    m_ifaceName.clear();
    m_netBase = m_netMask = 0;
    m_physIfaces.clear();
    m_localMacs.clear();
    m_gatewayIps.clear();

    // 1) 枚举全部「活动、非回环、有 IPv4」接口：MAC 一律进 m_localMacs（本机判定用），
    //    非虚拟的才进 m_physIfaces（主网卡候选 / 局域网网段）。
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack))
            continue;
        const QString mac = DeviceStore::normalizeMac(iface.hardwareAddress());
        if (!mac.isEmpty())
            m_localMacs.insert(mac);
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLoopback())
                continue;
            const quint32 ip32 = ip.toIPv4Address();
            const quint32 mask = entry.netmask().toIPv4Address();
            if (mask == 0)
                continue;
            if (isVirtualIface(iface, ip32))
                continue;
            LocalIface f;
            f.ip = ip.toString();
            f.mac = mac;
            // 用 OS 级接口名 name()：二层层要用它绑定网卡（AF_PACKET SIOCGIFINDEX / BPF
            // BIOCSETIF / Windows 走 Npcap）。注意 Windows 上 Qt6 的 name() 是 **LUID 名**
            // （"ethernet_32775"）而非适配器 GUID，Npcap 需要的 \Device\NPF_{GUID} 由
            // L2Endpoint_win::adapterFor() 反查——这里不要想当然按 GUID 用。
            f.name = iface.name();
            f.mask = mask;
            f.base = ip32 & mask;
            m_physIfaces.append(f);
        }
    }

    // 2) 全部默认路由 → m_gatewayIps。每个网络的路由器都要能被认出来，否则同时连两个网络时，
    //    另一个网络的路由器会被当成普通设备、还能对它开代理（劫持路由器会把那个网络打瘫）。
    const QVector<QPair<QString, QString>> routes = defaultRoutes();
    for (const auto &r : routes)
        m_gatewayIps.insert(r.first);

    // 3) 主网卡：优先「带默认路由的那张物理网卡」（Windows 按接口 IP 配对，Linux/mac 按接口名；
    //    配不上则退化为网关落在其子网内），否则第一张物理网卡。
    int primary = -1;
    for (int i = 0; i < m_physIfaces.size() && primary < 0; ++i) {
        const LocalIface &f = m_physIfaces.at(i);
        for (const auto &r : routes) {
            const bool sameIface = !r.second.isEmpty()
                                   && (r.second == f.ip || r.second == f.name);
            const quint32 gw = QHostAddress(r.first).toIPv4Address();
            if (sameIface || (gw && (gw & f.mask) == f.base)) {
                primary = i;
                break;
            }
        }
    }
    if (primary < 0 && !m_physIfaces.isEmpty())
        primary = 0;
    if (primary > 0)
        m_physIfaces.move(primary, 0); // 约定：m_physIfaces[0] 即主网卡

    if (!m_physIfaces.isEmpty()) {
        const LocalIface &f = m_physIfaces.constFirst();
        m_localIp = f.ip;
        m_localMac = f.mac;
        m_ifaceName = f.name;
        m_netBase = f.base;
        m_netMask = f.mask;
    }

    // 4) **每张**物理网卡各自的网关（网关劫持要用，多网卡时每张卡都得有）：
    //    优先同网卡的默认路由，其次落在该卡网段内的路由，最后 base+1 兜底。
    for (LocalIface &f : m_physIfaces) {
        f.gatewayIp.clear();
        for (const auto &r : routes) {
            if (!r.second.isEmpty() && (r.second == f.ip || r.second == f.name)) {
                f.gatewayIp = r.first;
                break;
            }
        }
        if (f.gatewayIp.isEmpty() && f.mask) {
            for (const auto &r : routes) {
                const quint32 gw = QHostAddress(r.first).toIPv4Address();
                if (gw && (gw & f.mask) == f.base) {
                    f.gatewayIp = r.first;
                    break;
                }
            }
        }
        if (f.gatewayIp.isEmpty() && f.base)
            f.gatewayIp = u32ToIp(f.base + 1);
        if (!f.gatewayIp.isEmpty())
            m_gatewayIps.insert(f.gatewayIp);
    }
    m_gatewayIp = m_physIfaces.isEmpty() ? QString() : m_physIfaces.constFirst().gatewayIp;

    // 5) IPv6 拓扑（尽力）：每张物理网卡的 v6 路由器 LL/MAC、本机全局 v6/前缀。缺了不影响 v4。
    detectIpv6Topology();
}

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
namespace {
// 同步跑一条命令收全部 stdout（短超时；失败/超时返回空）。仅供 v6 拓扑发现这类轻量只读命令用。
// ★ Linux 与 macOS 共用：两边的 v6 拓扑发现都是"跑一条只读命令再解析"（Linux 用 ip、
//   macOS 用 netstat/ndp/ifconfig）。Windows 那边走 IP Helper API，不需要它。
QString runCmd(const QString &program, const QStringList &args)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(1500))
        return {};
    if (!p.waitForFinished(2000)) {
        p.kill();
        p.waitForFinished(500);
        return {};
    }
    return QString::fromLocal8Bit(p.readAllStandardOutput());
}
#ifdef Q_OS_LINUX
// /proc/net/if_inet6 里 32 位十六进制地址串（无冒号）→ 规范化 v6 串（"2408:...")。
// 只有 Linux 用得上（macOS 那边从 ifconfig 直接拿到可读地址），单独守卫免得 mac 上告警未使用。
QString hex32ToV6(const QString &hex32)
{
    if (hex32.size() != 32)
        return {};
    Q_IPV6ADDR raw;
    bool ok = true;
    for (int i = 0; i < 16; ++i) {
        raw[i] = static_cast<quint8>(hex32.mid(i * 2, 2).toUInt(&ok, 16));
        if (!ok)
            return {};
    }
    return QHostAddress(raw).toString();
}
#endif // Q_OS_LINUX
} // namespace
#endif

// 三端都要实现，缺一端就等于**那一端的 IPv6 完全绕过代理**（见各分支里的说明）：
//   Linux：`ip -6 route` / `ip -6 neigh` + /proc/net/if_inet6
//   macOS：`netstat -rn -f inet6` / `ndp -an` / `ifconfig <dev> inet6`
//   Windows：GetIpForwardTable2 / GetIpNetTable2（IP Helper）
// 解析失败留空 → NdpSpoofer no-op（安全降级，但 v6 就不受管了，所以别把"留空"当成可接受的常态）。
void LanScanner::detectIpv6Topology()
{
#ifdef Q_OS_LINUX
    // (a) 每张网卡的 v6 默认路由器链路本地地址：`ip -6 route show default`
    //     形如：default via fe80::1 dev eth0 proto ra metric 1024 ...
    {
        const QString out = runCmd(QStringLiteral("ip"),
                                   {QStringLiteral("-6"), QStringLiteral("route"),
                                    QStringLiteral("show"), QStringLiteral("default")});
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString &ln : lines) {
            const QStringList tok = ln.split(' ', Qt::SkipEmptyParts);
            const int viaIdx = tok.indexOf(QStringLiteral("via"));
            const int devIdx = tok.indexOf(QStringLiteral("dev"));
            if (viaIdx < 0 || devIdx < 0 || viaIdx + 1 >= tok.size() || devIdx + 1 >= tok.size())
                continue;
            const QString via = tok.at(viaIdx + 1);
            const QString dev = tok.at(devIdx + 1);
            for (LocalIface &f : m_physIfaces)
                if (f.name == dev && f.gatewayLL6.isEmpty())
                    f.gatewayLL6 = via;
        }
    }
    // (b) 路由器 MAC：`ip -6 neigh` 形如：fe80::1 dev eth0 lladdr aa:bb:cc:dd:ee:ff REACHABLE
    {
        const QString out = runCmd(QStringLiteral("ip"),
                                   {QStringLiteral("-6"), QStringLiteral("neigh")});
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString &ln : lines) {
            const QStringList tok = ln.split(' ', Qt::SkipEmptyParts);
            if (tok.size() < 5)
                continue;
            const QString addr = tok.at(0);
            const int devIdx = tok.indexOf(QStringLiteral("dev"));
            const int llIdx = tok.indexOf(QStringLiteral("lladdr"));
            if (devIdx < 0 || llIdx < 0 || llIdx + 1 >= tok.size())
                continue;
            const QString dev = tok.at(devIdx + 1);
            const QString mac = DeviceStore::normalizeMac(tok.at(llIdx + 1));
            for (LocalIface &f : m_physIfaces)
                if (f.name == dev && f.gatewayLL6.compare(addr, Qt::CaseInsensitive) == 0)
                    f.gatewayMac6 = mac;
        }
    }
    // (c) 本机全局 v6 + 前缀长度：/proc/net/if_inet6
    //     每行：<32hex addr> <ifindex> <prefixlen hex> <scope hex> <flags hex> <ifname>
    //     scope 00 = 全局。取每张卡的第一个全局地址。
    {
        QFile f(QStringLiteral("/proc/net/if_inet6"));
        if (f.open(QIODevice::ReadOnly)) {
            const QStringList lines = QString::fromLatin1(f.readAll()).split('\n', Qt::SkipEmptyParts);
            f.close();
            for (const QString &ln : lines) {
                const QStringList tok = ln.split(' ', Qt::SkipEmptyParts);
                if (tok.size() < 6)
                    continue;
                if (tok.at(3) != QStringLiteral("00")) // 仅全局作用域
                    continue;
                const QString dev = tok.at(5);
                const QString v6 = hex32ToV6(tok.at(0));
                bool okp = false;
                const int plen = tok.at(2).toInt(&okp, 16);
                if (v6.isEmpty())
                    continue;
                for (LocalIface &lf : m_physIfaces) {
                    if (lf.name == dev && lf.global6.isEmpty()) {
                        lf.global6 = v6;
                        if (okp)
                            lf.prefix6 = v6 + QStringLiteral("/") + QString::number(plen);
                    }
                }
            }
        }
    }
#elif defined(Q_OS_MACOS)
    // macOS：等价于 Linux 那边的 `ip -6 route` + `ip -6 neigh`，用 `netstat -rn -f inet6`
    // 取 v6 默认路由器的链路本地地址，再用 `ndp -an` 查它的 MAC。
    //
    // ★ 为什么非补这一段不可（与 Windows 那段是**同一个 bug 的第三次**）：这个函数原本只有
    //   Linux 与 Windows 两个分支，macOS 落进空实现 —— NicSpec 的 routerLinkLocal6/routerMac6
    //   恒空，NdpSpoofer 直接停用（真机日志：`NdpSpoofer: 无可用 v6 路由器，该卡 v6 劫持停用`），
    //   于是**被接管设备的 IPv6 完全绕过网关**：它的 v6 默认路由器仍是真路由器，v6 流量既不
    //   经代理、也不受每设备策略约束（"禁网"的设备照样能用 v6 上网）。
    //   真机实证（2026-08-03，Mac 做网关接管 Windows .51）：接管后 Windows 的 v6 网关 MAC
    //   仍是真路由器 70-A7-41-A4-19-7B，`ipv6.baidu.com` 照常 200 —— 而 v4 已经走了 pf。
    //   现代 App 大量优先走 v6，所以表现就是"开了代理却像没生效"。
    {
        // (a) v6 默认路由：`netstat -rn -f inet6` 里以 default 开头的行，
        //     形如：default   fe80::72a7:41ff:fea4:197b%en0   UGcg   en0
        //     网关列带 %网卡 后缀，网卡名也可从最后一列拿到；两者取其一即可。
        const QString out = runCmd(QStringLiteral("netstat"),
                                   {QStringLiteral("-rn"), QStringLiteral("-f"),
                                    QStringLiteral("inet6")});
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString &ln : lines) {
            if (!ln.startsWith(QStringLiteral("default")))
                continue;
            const QStringList tok = ln.split(' ', Qt::SkipEmptyParts);
            if (tok.size() < 2)
                continue;
            QString via = tok.at(1);
            const int pct = via.indexOf(QLatin1Char('%'));
            QString dev = pct >= 0 ? via.mid(pct + 1) : QString();
            if (pct >= 0)
                via = via.left(pct);
            // 只要链路本地下一跳（fe80::/10）—— NDP 投毒的目标就是它；utun/隧道那些跳过。
            if (!via.startsWith(QStringLiteral("fe80"), Qt::CaseInsensitive))
                continue;
            if (dev.isEmpty())
                dev = tok.last();
            for (LocalIface &f : m_physIfaces)
                if (f.name == dev && f.gatewayLL6.isEmpty())
                    f.gatewayLL6 = via;
        }
    }
    // (b) 路由器 MAC：`ndp -an` 形如
    //     fe80::72a7:41ff:fea4:197b%en0   70:a7:41:a4:19:7b   en0  23h59m56s  S  R
    {
        const QString out = runCmd(QStringLiteral("ndp"), {QStringLiteral("-an")});
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString &ln : lines) {
            const QStringList tok = ln.split(' ', Qt::SkipEmptyParts);
            if (tok.size() < 3)
                continue;
            QString addr = tok.at(0);
            const int pct = addr.indexOf(QLatin1Char('%'));
            QString dev = pct >= 0 ? addr.mid(pct + 1) : QString();
            if (pct >= 0)
                addr = addr.left(pct);
            const QString mac = DeviceStore::normalizeMac(tok.at(1));
            if (mac.isEmpty())
                continue; // "(incomplete)" 之类
            if (dev.isEmpty())
                dev = tok.at(2);
            for (LocalIface &f : m_physIfaces)
                if (f.name == dev && f.gatewayLL6.compare(addr, Qt::CaseInsensitive) == 0)
                    f.gatewayMac6 = mac;
        }
    }
    // (c) 本机全局 v6 + 前缀长度：`ifconfig <dev> inet6`，取第一个非链路本地、非临时地址。
    {
        for (LocalIface &f : m_physIfaces) {
            if (!f.global6.isEmpty() || f.name.isEmpty())
                continue;
            const QString out = runCmd(QStringLiteral("ifconfig"), {f.name, QStringLiteral("inet6")});
            for (const QString &ln : out.split('\n', Qt::SkipEmptyParts)) {
                const QStringList tok = ln.split(' ', Qt::SkipEmptyParts);
                const int i6 = tok.indexOf(QStringLiteral("inet6"));
                if (i6 < 0 || i6 + 1 >= tok.size())
                    continue;
                QString a = tok.at(i6 + 1);
                const int pct = a.indexOf(QLatin1Char('%'));
                if (pct >= 0)
                    a = a.left(pct);
                if (a.startsWith(QStringLiteral("fe80"), Qt::CaseInsensitive))
                    continue; // 链路本地不是我们要的"本机全局地址"
                if (ln.contains(QStringLiteral("temporary")))
                    continue; // 临时地址会轮换，拿它做前缀没意义
                f.global6 = a;
                const int pl = tok.indexOf(QStringLiteral("prefixlen"));
                if (pl >= 0 && pl + 1 < tok.size())
                    f.prefix6 = a + QStringLiteral("/") + tok.at(pl + 1);
                break;
            }
        }
    }
#elif defined(Q_OS_WIN)
    // Windows：用 IP Helper 的 GetIpForwardTable2 取每张网卡的 v6 默认路由器链路本地地址，
    // 再用 GetIpNetTable2（v6 邻居表）查它的 MAC。等价于 Linux 那边的 `ip -6 route` + `ip -6 neigh`。
    //
    // 为什么非补这一段不可：之前整个函数只在 Q_OS_LINUX 下有实现，Windows 上是空的 —— 于是
    // NicSpec 的 routerLinkLocal6/routerMac6 恒空，effectiveRouterLL6() 只能回落到「从线上 RA
    // 学到的」，可 RA 是路由器发的组播、常被网关的源 MAC BPF 过滤挡在内核外。两条路都断，
    // NdpSpoofer 永久 no-op —— 真机实证：被代理的 Android 其 v6 网关仍指着真路由器，v6 流量
    // 完全绕过代理（而现代 App 大量优先走 v6，表现就是「开了代理却像没生效」）。
    {
        // (a) v6 默认路由（DestinationPrefix 长度 0）的下一跳链路本地地址，按接口 LUID 归拢。
        PMIB_IPFORWARD_TABLE2 rt = nullptr;
        if (GetIpForwardTable2(AF_INET6, &rt) == NO_ERROR && rt) {
            for (ULONG i = 0; i < rt->NumEntries; ++i) {
                const MIB_IPFORWARD_ROW2 &r = rt->Table[i];
                if (r.DestinationPrefix.PrefixLength != 0)
                    continue; // 只要默认路由
                const IN6_ADDR &nh = r.NextHop.Ipv6.sin6_addr;
                // 下一跳必须是链路本地（fe80::/10）—— NDP 投毒的目标就是它。
                if (!(nh.u.Byte[0] == 0xFE && (nh.u.Byte[1] & 0xC0) == 0x80))
                    continue;
                wchar_t name[IF_MAX_STRING_SIZE + 1] = {0};
                if (ConvertInterfaceLuidToNameW(&r.InterfaceLuid, name, IF_MAX_STRING_SIZE) != NO_ERROR)
                    continue;
                const QString dev = QString::fromWCharArray(name);
                char buf[46] = {0};
                if (!InetNtopA(AF_INET6, &nh, buf, sizeof buf))
                    continue;
                const QString ll = QString::fromLatin1(buf);
                // f.name 在 Windows 上就是 QNetworkInterface::name()（= LUID 名，见 detectLocalTopology）。
                for (LocalIface &f : m_physIfaces)
                    if (f.name == dev && f.gatewayLL6.isEmpty())
                        f.gatewayLL6 = ll;
            }
            FreeMibTable(rt);
        }
        // (b) v6 邻居表查路由器 LL 的 MAC。
        PMIB_IPNET_TABLE2 nb = nullptr;
        if (GetIpNetTable2(AF_INET6, &nb) == NO_ERROR && nb) {
            for (ULONG i = 0; i < nb->NumEntries; ++i) {
                const MIB_IPNET_ROW2 &row = nb->Table[i];
                if (row.PhysicalAddressLength != 6)
                    continue;
                // ★ 跳过全零 MAC：邻居表里同一个 LL 常有多条记录，INCOMPLETE/STALE 的表项
                //   PhysicalAddress 是 00:00:00:00:00:00。拿它当路由器 MAC 会让 NDP 投毒指向
                //   一个不存在的地址（真机 GetIpNetTable2 实测：真路由器那条 MAC 正确，但同一
                //   LL 还夹着七八条全零记录）。只认非零的那条。
                bool allZero = true;
                for (int b = 0; b < 6; ++b)
                    if (row.PhysicalAddress[b] != 0) { allZero = false; break; }
                if (allZero)
                    continue;
                char buf[46] = {0};
                if (!InetNtopA(AF_INET6, &row.Address.Ipv6.sin6_addr, buf, sizeof buf))
                    continue;
                const QString addr = QString::fromLatin1(buf);
                QString mac;
                for (int b = 0; b < 6; ++b)
                    mac += QString::asprintf(b ? ":%02x" : "%02x", row.PhysicalAddress[b]);
                for (LocalIface &f : m_physIfaces)
                    if (f.gatewayMac6.isEmpty() && !f.gatewayLL6.isEmpty()
                        && f.gatewayLL6.compare(addr, Qt::CaseInsensitive) == 0)
                        f.gatewayMac6 = mac;
            }
            FreeMibTable(nb);
        }
    }
    // (c) 本机全局 v6 + 前缀：QNetworkInterface 已有，直接取（省一次系统调用）。
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        for (LocalIface &f : m_physIfaces) {
            if (f.name != iface.name())
                continue;
            for (const QNetworkAddressEntry &e : iface.addressEntries()) {
                const QHostAddress ip = e.ip();
                if (ip.protocol() != QAbstractSocket::IPv6Protocol)
                    continue;
                const Q_IPV6ADDR raw = ip.toIPv6Address();
                if ((raw[0] & 0xE0) != 0x20) // 只要全局单播 2000::/3
                    continue;
                if (f.global6.isEmpty())
                    f.global6 = ip.toString();
                if (f.prefix6.isEmpty() && e.prefixLength() > 0)
                    f.prefix6 = ip.toString() + QStringLiteral("/") + QString::number(e.prefixLength());
            }
        }
    }
#endif
}

QSet<QString> LanScanner::gatewayMacs() const
{
    QSet<QString> macs;
    for (const QString &gw : m_gatewayIps) {
        const QString mac = m_arp.value(gw);
        if (!mac.isEmpty())
            macs.insert(mac);
    }
    return macs;
}

QString LanScanner::localNetmask() const
{
    // m_netMask 为主机序 quint32（0=未知）；QHostAddress(quint32) 按主机序解读 → 点分掩码。
    return m_netMask ? QHostAddress(m_netMask).toString() : QString();
}

bool LanScanner::inPrimarySubnet(const QString &ip) const
{
    if (ip.isEmpty() || !m_netBase || !m_netMask)
        return false;
    const QHostAddress a(ip);
    if (a.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    return (a.toIPv4Address() & m_netMask) == m_netBase;
}

// 落在**任意一张**物理网卡的网段里即可被劫持（有线接 A 路由、WiFi 接 B 路由 → 两边都能代理）。
bool LanScanner::inAnyLanSubnet(const QString &ip) const
{
    if (ip.isEmpty())
        return false;
    const QHostAddress a(ip);
    if (a.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    const quint32 v = a.toIPv4Address();
    for (const LocalIface &f : m_physIfaces) {
        if (f.base && f.mask && (v & f.mask) == f.base)
            return true;
    }
    return false;
}

QVector<LanScanner::NicInfo> LanScanner::physicalNics() const
{
    QVector<NicInfo> out;
    out.reserve(m_physIfaces.size());
    for (const LocalIface &f : m_physIfaces) {
        if (f.name.isEmpty() || f.ip.isEmpty() || f.mac.isEmpty() || !f.mask)
            continue;
        NicInfo n;
        n.name = f.name;
        n.ip = f.ip;
        n.mac = f.mac;
        n.netmask = QHostAddress(f.mask).toString();
        n.gatewayIp = f.gatewayIp;
        n.gatewayMac = m_arp.value(f.gatewayIp); // 扫过一轮后才有值；网关配置每轮刷新
        n.routerLinkLocal6 = f.gatewayLL6;
        n.routerMac6 = f.gatewayMac6;
        n.localGlobal6 = f.global6;
        n.prefix6 = f.prefix6;
        out.append(n);
    }
    return out;
}

QVector<quint32> LanScanner::hostsToProbe() const
{
    QVector<quint32> hosts;
    // 每张物理网卡的网段都扫（同时连两个网络时，两边的设备都要能发现）。
    for (const LocalIface &f : m_physIfaces) {
        if (!f.base || !f.mask)
            continue;
        const quint32 hostBits = ~f.mask;
        // 只扫 /24 及更小（>256 主机的网段太大，逐个探测代价过高——退化为只读 ARP 表）。
        if (hostBits >= 256)
            continue;
        const quint32 selfU = QHostAddress(f.ip).toIPv4Address();
        const quint32 broadcast = f.base | hostBits;
        for (quint32 ip = f.base + 1; ip < broadcast; ++ip) {
            if (ip == selfU)
                continue; // 本机不探
            hosts.append(ip);
        }
        if (hosts.size() >= 1024)
            break; // 上限兜底：网卡再多也不至于把探测量放飞
    }
    return hosts;
}

void LanScanner::appendSelfRecords(QVector<DeviceRecord> &out) const
{
    // 本机每张物理网卡各一条记录（同时连两个网络 → 两条「本机」，各自属于自己的网段）。
    for (const LocalIface &f : m_physIfaces) {
        if (f.ip.isEmpty() || f.mac.isEmpty())
            continue;
        DeviceRecord me;
        me.mac = f.mac;
        me.ip = f.ip;
        me.online = true; // 程序在跑，本机必然在线——绝不靠 ARP 表判定自己
        me.isSelf = true;
        me.isGateway = false;
        me.inLanSubnet = false; // 本机不参与劫持
        me.lastSeen = QDateTime::currentDateTime();
        me.autoName = QHostInfo::localHostName();
        me.vendor = ouiVendor(f.mac);
        me.autoType = DeviceType::Computer;
        out.append(me);
    }
}

// ———————————————————————————— 主动探测 ————————————————————————————
// 通用探测泵：把 (ip,port) 作业按 kMaxInFlight 限流跑完，全部收口后回调 onDone。
//
// 两条「别把 GUI 线程按住」的硬规矩（都是实测逼出来的，别回退）：
//  1) 每轮事件循环最多新建 kPumpSlice 个 socket。建 socket / connect / abort 在 Windows 上各是
//     几百微秒的系统调用，一口气推 120 个，单次事件循环实测能卡 1.8 秒（页面直接冻住）。
//  2) 收口后的补位**不在原地递归调用泵**，而是排到下一轮事件循环（且合并成一次）。否则一批
//     socket 同时报错/同时超时时，finish→pump→新 socket→立即报错→finish… 会在一个事件循环里
//     连锁跑完剩下的所有作业。
void LanScanner::runProbes(QVector<QPair<quint32, quint16>> jobs, std::function<void()> onDone)
{
    if (jobs.isEmpty()) {
        if (onDone)
            onDone();
        return;
    }
    auto queue = std::make_shared<QVector<QPair<quint32, quint16>>>(std::move(jobs));
    auto inFlight = std::make_shared<int>(0);
    auto left = std::make_shared<int>(queue->size());
    auto finished = std::make_shared<std::function<void()>>(std::move(onDone));
    auto pumpScheduled = std::make_shared<bool>(false);
    auto pump = std::make_shared<std::function<void()>>();

    // 合并式排程：一轮事件循环里无论多少个 socket 收口，都只排一次泵。
    auto schedulePump = [this, pump, pumpScheduled] {
        if (*pumpScheduled)
            return;
        *pumpScheduled = true;
        QTimer::singleShot(0, this, [pump, pumpScheduled] {
            *pumpScheduled = false;
            (*pump)();
        });
    };

    *pump = [this, queue, inFlight, left, finished, schedulePump, pump]() {
        int made = 0;
        while (*inFlight < kMaxInFlight && !queue->isEmpty() && made < kPumpSlice) {
            const auto job = queue->takeFirst();
            ++(*inFlight);
            ++made;
            auto *sock = new QTcpSocket(this);
            auto *timer = new QTimer(sock);
            timer->setSingleShot(true);
            timer->setInterval(kProbeTimeoutMs);

            const QString ipStr = u32ToIp(job.first);
            const quint16 port = job.second;

            // 每个 socket 的 connected / errorOccurred / 超时 三路只允许收口一次，否则会重复
            // 递减计数（导致 left 提前归零 / 变负、onDone 误触发）。
            auto done = std::make_shared<bool>(false);
            auto finish = [this, sock, inFlight, left, finished, schedulePump, done](
                              bool open, const QString &ip, quint16 pt) {
                if (*done)
                    return;
                *done = true;
                if (open)
                    sig(ip).ports.insert(pt);
                sock->abort();
                sock->deleteLater();
                --(*inFlight);
                if (--(*left) <= 0) {
                    if (*finished)
                        (*finished)();
                    return;
                }
                schedulePump(); // 补位——下一轮事件循环再建，绝不在这里递归
            };

            connect(sock, &QTcpSocket::connected, sock, [finish, ipStr, port]() {
                finish(true, ipStr, port);
            });
            connect(sock, &QTcpSocket::errorOccurred, sock,
                    [finish, ipStr, port](QAbstractSocket::SocketError) {
                        // 连不上（拒绝/超时/不可达）也无所谓——SYN 已发出，系统已做 ARP。
                        finish(false, ipStr, port);
                    });
            connect(timer, &QTimer::timeout, sock, [finish, ipStr, port]() {
                finish(false, ipStr, port);
            });
            timer->start();
            sock->connectToHost(ipStr, port);
        }
        // 名额还有、队列还有 → 说明是被 kPumpSlice 截断的，下一轮继续。
        if (!queue->isEmpty() && *inFlight < kMaxInFlight)
            schedulePump();
    };
    (*pump)();
}

// 阶段②：只给「ARP 表里真的存在」的主机补其余指纹端口 + 反向 DNS。
// 阶段①已经用一个端口把整段网段扫过一遍，空 IP 不必再连 9 次——作业量从 主机数×端口数
// 降到 主机数 + 存活数×(端口数-1)（/24 实测 2530 → 约 330）。
void LanScanner::startFingerprintPhase()
{
    QVector<QPair<quint32, quint16>> jobs;
    for (auto it = m_arp.constBegin(); it != m_arp.constEnd(); ++it) {
        const QHostAddress a(it.key());
        if (a.protocol() != QAbstractSocket::IPv4Protocol)
            continue;
        const quint32 ip = a.toIPv4Address();
        for (quint16 port : kProbePorts) {
            if (port == kArpPort)
                continue; // 阶段①已经连过
            jobs.append({ip, port});
        }
        // 反向 DNS 也只对存活主机做：原先对整个 /24 发 253 条 PTR，绝大多数注定失败，
        // 白占 Qt 的 host-info 线程池和 DNS 往返。
        reverseDnsLookup(it.key());
    }
    // 抢先出一版列表：此刻在线设备已经确定（差的只是名称/端口指纹）。
    emitSnapshot(false);
    runProbes(std::move(jobs), [this] {
        // 指纹阶段收口 → 再读一次 ARP 表（此时系统对存活主机的解析最全），
        // 起静默计时器等 mDNS/SSDP/NBNS 的名称回包，到点 assemble。
        readArpTable([this] { m_settleTimer->start(kSettleMs); });
    });
}

void LanScanner::probePort(quint32 ip, quint16 port)
{
    Q_UNUSED(ip);
    Q_UNUSED(port);
    // 预留（refreshLiveness 用定向单端口探测已在下方内联实现）。
}

// ———————————————————————————— ARP 表 ————————————————————————————
// onDone 在 ARP 输出解析完后回调（进程失败/被杀也一样会回调，调用链不会断在这里）。
void LanScanner::readArpTable(std::function<void()> onDone)
{
    auto *p = new QProcess(this);
#if defined(Q_OS_WIN)
    p->start("arp", {"-a"});
#elif defined(Q_OS_MACOS)
    p->start("arp", {"-an"});
#else
    p->start("arp", {"-n"});
#endif
    auto called = std::make_shared<bool>(false);
    auto fire = [called, cb = std::move(onDone)] {
        if (*called || !cb)
            return;
        *called = true;
        cb();
    };
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, p, fire](int, QProcess::ExitStatus) {
                onArpOutput(p->readAllStandardOutput());
                p->deleteLater();
                fire();
            });
    // arp 命令偶发不存在/超时：3s 兜底杀掉（finished 随后照常触发 fire），走 assemble 也不会卡。
    QTimer::singleShot(3000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
    // 进程压根起不来（arp 不存在）时 finished 不一定来，兜底把链子接上。
    connect(p, &QProcess::errorOccurred, this, [fire](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            fire();
    });
}

void LanScanner::onArpOutput(const QByteArray &out)
{
    const QString text = QString::fromLocal8Bit(out);
    const QStringList lines = text.split('\n');
    for (const QString &line : lines) {
        const QString ip = ipv4FromLine(line);
        const QString mac = DeviceStore::normalizeMac(macFromLine(line));
        if (isJunkArpEntry(ip, mac))
            continue; // 组播/广播条目不是设备
        m_arp.insert(ip, mac);
        Signals &s = sig(ip);
        s.mac = mac;
        s.alive = true;
    }
}

// ———————————————————————————— mDNS (5353) ————————————————————————————
namespace {
// 解析 DNS 名字（含 0xC0 压缩指针），返回名字并把 pos 推进到该名字之后（跟随指针时 pos 停在指针后）。
QString parseDnsName(const QByteArray &buf, int &pos)
{
    QStringList labels;
    int p = pos;
    bool jumped = false;
    int safety = 0;
    while (p < buf.size() && safety++ < 128) {
        const quint8 len = static_cast<quint8>(buf.at(p));
        if (len == 0) {
            ++p;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            if (p + 1 >= buf.size())
                break;
            const int ptr = ((len & 0x3F) << 8) | static_cast<quint8>(buf.at(p + 1));
            if (!jumped)
                pos = p + 2; // 外层游标停在指针后
            jumped = true;
            p = ptr;
            continue;
        }
        if (p + 1 + len > buf.size())
            break;
        labels << QString::fromLatin1(buf.mid(p + 1, len));
        p += 1 + len;
    }
    if (!jumped)
        pos = p;
    return labels.join('.');
}
} // namespace

void LanScanner::setupMdns()
{
    m_mdns = new QUdpSocket(this);
    // 绑 5353（ShareAddress 允许与系统 mDNS 共存），加入组播组。失败则仅能收单播/放弃，不致命。
    m_mdns->bind(QHostAddress::AnyIPv4, 5353,
                 QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if ((iface.flags() & QNetworkInterface::CanMulticast)
            && (iface.flags() & QNetworkInterface::IsRunning))
            m_mdns->joinMulticastGroup(QHostAddress("224.0.0.251"), iface);
    }
    connect(m_mdns, &QUdpSocket::readyRead, this, &LanScanner::onMdnsDatagram);
}

void LanScanner::sendMdnsQueries()
{
    if (!m_mdns)
        return;
    // 查 "_services._dns-sd._udp.local" PTR：触发所有响应者枚举其服务；再直接问几个常见服务。
    const char *names[] = {
        "_services._dns-sd._udp.local", "_device-info._tcp.local",
        "_airplay._tcp.local", "_googlecast._tcp.local", "_ipp._tcp.local",
        "_ipps._tcp.local", "_printer._tcp.local", "_smb._tcp.local",
        "_raop._tcp.local", "_spotify-connect._tcp.local", "_homekit._tcp.local",
        "_workstation._tcp.local",
    };
    for (const char *n : names) {
        QByteArray q;
        q.append(char(0x00)); q.append(char(0x00)); // id
        q.append(char(0x00)); q.append(char(0x00)); // flags (query)
        q.append(char(0x00)); q.append(char(0x01)); // qdcount=1
        q.append(char(0x00)); q.append(char(0x00)); // ancount
        q.append(char(0x00)); q.append(char(0x00)); // nscount
        q.append(char(0x00)); q.append(char(0x00)); // arcount
        for (const QByteArray &label : QByteArray(n).split('.')) {
            q.append(char(label.size()));
            q.append(label);
        }
        q.append(char(0x00));                        // 名字结束
        q.append(char(0x00)); q.append(char(0x0C));  // QTYPE=PTR
        q.append(char(0x00)); q.append(char(0x01));  // QCLASS=IN
        m_mdns->writeDatagram(q, QHostAddress("224.0.0.251"), 5353);
    }
}

void LanScanner::onMdnsDatagram()
{
    while (m_mdns && m_mdns->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_mdns->receiveDatagram();
        const QByteArray buf = dg.data();
        const QString senderIp = dg.senderAddress().toString().split('%').first();
        if (buf.size() < 12)
            continue;

        auto rd16 = [&](int at) -> int {
            return (static_cast<quint8>(buf.at(at)) << 8) | static_cast<quint8>(buf.at(at + 1));
        };
        const int qd = rd16(4), an = rd16(6), ns = rd16(8), ar = rd16(10);
        int pos = 12;
        // 跳过问题段
        for (int i = 0; i < qd && pos < buf.size(); ++i) {
            parseDnsName(buf, pos);
            pos += 4; // qtype+qclass
        }
        // 中间累积：host→ip、host→services/model/friendly
        const int total = an + ns + ar;
        for (int i = 0; i < total && pos < buf.size(); ++i) {
            const QString name = parseDnsName(buf, pos);
            if (pos + 10 > buf.size())
                break;
            const int type = rd16(pos);
            const int rdlen = rd16(pos + 8);
            pos += 10;
            if (pos + rdlen > buf.size())
                break;
            const QByteArray rdata = buf.mid(pos, rdlen);

            if (type == 1 && rdlen == 4) { // A 记录：name(host) → IPv4
                const QString ip = QString("%1.%2.%3.%4")
                                       .arg(quint8(rdata[0])).arg(quint8(rdata[1]))
                                       .arg(quint8(rdata[2])).arg(quint8(rdata[3]));
                Signals &s = sig(ip);
                if (s.friendly.isEmpty() && !name.isEmpty())
                    s.friendly = name.section(".local", 0, 0);
            } else if (type == 12) { // PTR：name 即服务类型
                const QString svc = name.section('.', 0, 0); // 取 _airplay 等
                if (svc.startsWith('_')) {
                    // 关联到发送者 IP（同一响应者）
                    sig(senderIp).services.insert(svc);
                }
            } else if (type == 16) { // TXT：找 model= / md=
                int tp = 0;
                while (tp < rdata.size()) {
                    const int l = static_cast<quint8>(rdata.at(tp));
                    if (l == 0 || tp + 1 + l > rdata.size())
                        break;
                    const QString kv = QString::fromLatin1(rdata.mid(tp + 1, l));
                    if (kv.startsWith("model=", Qt::CaseInsensitive)
                        || kv.startsWith("md=", Qt::CaseInsensitive)) {
                        sig(senderIp).model = kv.section('=', 1);
                    }
                    tp += 1 + l;
                }
            } else if (type == 33) { // SRV：记录名 instance._svc._tcp.local → 服务
                const QString svc = name.section('.', 1, 1);
                if (svc.startsWith('_'))
                    sig(senderIp).services.insert(svc);
            }
            pos += rdlen;
        }
        // 发送者若在 ARP 表里，直接把服务/型号并到该 IP
        Signals &s = sig(senderIp);
        s.alive = true;
    }
}

// ———————————————————————————— SSDP (1900) ————————————————————————————
void LanScanner::setupSsdp()
{
    m_ssdp = new QUdpSocket(this);
    m_ssdp->bind(QHostAddress::AnyIPv4, 0); // 临时端口，收 M-SEARCH 单播响应
    connect(m_ssdp, &QUdpSocket::readyRead, this, &LanScanner::onSsdpDatagram);
}

void LanScanner::sendSsdpQuery()
{
    if (!m_ssdp)
        return;
    const QByteArray msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: ssdp:all\r\n\r\n";
    m_ssdp->writeDatagram(msearch, QHostAddress("239.255.255.250"), 1900);
}

void LanScanner::onSsdpDatagram()
{
    while (m_ssdp && m_ssdp->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_ssdp->receiveDatagram();
        const QString ip = dg.senderAddress().toString().split('%').first();
        const QString text = QString::fromLatin1(dg.data());
        sig(ip).alive = true;
        // 抽 LOCATION 头 → 拉设备描述 XML。
        static const QRegularExpression re("LOCATION:\\s*(\\S+)", QRegularExpression::CaseInsensitiveOption);
        const auto m = re.match(text);
        if (m.hasMatch())
            fetchSsdpLocation(m.captured(1), ip);
    }
}

void LanScanner::fetchSsdpLocation(const QString &url, const QString &ip)
{
    static QNetworkAccessManager *nam = nullptr;
    if (!nam) {
        nam = new QNetworkAccessManager(this);
        nam->setProxy(QNetworkProxy::NoProxy); // 直连局域网设备描述，绕开系统代理(可能指向 mihomo)
    }
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(3000);
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ip]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QString xml = QString::fromUtf8(reply->readAll());
        auto tag = [&](const QString &t) -> QString {
            const QRegularExpression re(QString("<%1>([^<]*)</%1>").arg(t),
                                        QRegularExpression::CaseInsensitiveOption);
            const auto m = re.match(xml);
            return m.hasMatch() ? m.captured(1).trimmed() : QString();
        };
        Signals &s = sig(ip);
        const QString friendly = tag("friendlyName");
        const QString manuf = tag("manufacturer");
        const QString modelName = tag("modelName");
        if (!friendly.isEmpty() && s.friendly.isEmpty())
            s.friendly = friendly;
        if (!modelName.isEmpty() && s.model.isEmpty())
            s.model = modelName;
        if (!manuf.isEmpty() && s.vendor.isEmpty())
            s.vendor = manuf;
        s.services.insert("_upnp");
        emit scanningChanged(m_scanning); // 轻触发 UI 复算（名称迟到时也刷新）
    });
}

// ———————————————————————————— NetBIOS (137) ————————————————————————————
void LanScanner::setupNbns()
{
    m_nbns = new QUdpSocket(this);
    m_nbns->bind(QHostAddress::AnyIPv4, 0);
    connect(m_nbns, &QUdpSocket::readyRead, this, &LanScanner::onNbnsDatagram);
}

// 整段网段的 NBNS 查询切片发送：每轮事件循环最多 kNbnsSlice 个，剩下的排到下一轮。
// （一次性 writeDatagram 253 下实测占住 GUI 线程 20~50ms——本身不致命，但进页面那一瞬间
//  正好和 QML 首帧、探测启动挤在一起，能省就省。）
void LanScanner::sendNbnsQueriesSliced(const QVector<quint32> &hosts)
{
    if (hosts.isEmpty() || !m_nbns)
        return;
    auto rest = std::make_shared<QVector<quint32>>(hosts);
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, rest, step] {
        const int n = qMin<int>(kNbnsSlice, rest->size());
        for (int i = 0; i < n; ++i)
            sendNbnsQuery(rest->at(i));
        rest->remove(0, n);
        if (!rest->isEmpty())
            QTimer::singleShot(0, this, [step] { (*step)(); });
    };
    (*step)();
}

void LanScanner::sendNbnsQuery(quint32 ip)
{
    if (!m_nbns)
        return;
    // NBSTAT (node status) 查询：QNAME = "*" 编码为 CK...(32 字节)；QTYPE=0x21 NBSTAT，QCLASS=IN。
    QByteArray q;
    q.append(char(0x00)); q.append(char(0x00)); // id
    q.append(char(0x00)); q.append(char(0x10)); // flags: broadcast
    q.append(char(0x00)); q.append(char(0x01)); // qdcount
    q.append(char(0x00)); q.append(char(0x00));
    q.append(char(0x00)); q.append(char(0x00));
    q.append(char(0x00)); q.append(char(0x00));
    // 名字 "*" + 15 个 0x00 的一级编码：每半字节 +'A'
    q.append(char(0x20)); // 名长 32
    const char name16[16] = {'*', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        const quint8 c = static_cast<quint8>(name16[i]);
        q.append(char('A' + ((c >> 4) & 0x0F)));
        q.append(char('A' + (c & 0x0F)));
    }
    q.append(char(0x00));
    q.append(char(0x00)); q.append(char(0x21)); // QTYPE=NBSTAT
    q.append(char(0x00)); q.append(char(0x01)); // QCLASS=IN
    m_nbns->writeDatagram(q, QHostAddress(ip), 137);
}

void LanScanner::onNbnsDatagram()
{
    while (m_nbns && m_nbns->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_nbns->receiveDatagram();
        const QString ip = dg.senderAddress().toString().split('%').first();
        const QByteArray b = dg.data();
        // 定位应答名列表：头12 + 编码名(34) + rrtype/class/ttl(8) + rdlen(2) → number of names(1)
        int pos = 12 + 34 + 8 + 2;
        if (pos >= b.size())
            continue;
        const int num = static_cast<quint8>(b.at(pos));
        ++pos;
        QString workstation;
        for (int i = 0; i < num && pos + 18 <= b.size(); ++i) {
            QString nm = QString::fromLatin1(b.mid(pos, 15)).trimmed();
            const quint8 suffix = static_cast<quint8>(b.at(pos + 15));
            const quint8 flagsHi = static_cast<quint8>(b.at(pos + 16));
            const bool group = flagsHi & 0x80;
            // <00> 且非组 = 唯一工作站名（机器名）。
            if (suffix == 0x00 && !group && workstation.isEmpty() && !nm.isEmpty())
                workstation = nm;
            pos += 18;
        }
        if (!workstation.isEmpty()) {
            Signals &s = sig(ip);
            if (s.hostname.isEmpty())
                s.hostname = workstation;
            s.alive = true;
        }
    }
}

void LanScanner::reverseDnsLookup(const QString &ip)
{
    QHostInfo::lookupHost(ip, this, [this, ip](const QHostInfo &info) {
        if (info.error() != QHostInfo::NoError)
            return;
        QString name = info.hostName();
        if (name.isEmpty() || name == ip)
            return;
        name = name.section('.', 0, 0); // 去域名后缀
        Signals &s = sig(ip);
        if (s.hostname.isEmpty())
            s.hostname = name;
    });
}

// ———————————————————————————— OUI ————————————————————————————
void LanScanner::loadOui()
{
    // 内嵌 OUI 表（:/assets/oui.txt，行格式 "AABBCC<TAB>Vendor"）。文件可由 tools/fetch_oui.py 生成；
    // 缺失时厂商列留空，分类退化为靠服务/端口/主机名。
    QFile f(":/assets/oui.txt");
    if (!f.open(QIODevice::ReadOnly))
        return;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        const int tab = line.indexOf('\t');
        if (tab != 6)
            continue;
        const QString prefix = QString::fromLatin1(line.left(6)).toLower();
        const QString vendor = QString::fromUtf8(line.mid(tab + 1)).trimmed();
        if (!vendor.isEmpty())
            m_oui.insert(prefix, vendor);
    }
    f.close();
    m_ouiLoaded = true;
}

QString LanScanner::ouiVendor(const QString &mac) const
{
    if (m_oui.isEmpty())
        return {};
    const QString prefix = QString(mac).remove(':').left(6).toLower();
    return m_oui.value(prefix);
}

// ———————————————————————————— 分类 ————————————————————————————
DeviceType LanScanner::classify(const QString &mac, const QString &name, const QString &model,
                                const QString &vendor, const QSet<quint16> &ports,
                                const QSet<QString> &services) const
{
    Q_UNUSED(mac);
    const QString n = (name + ' ' + model + ' ' + vendor).toLower();
    auto has = [&](const char *k) { return n.contains(QLatin1String(k)); };
    auto svc = [&](const char *k) { return services.contains(QLatin1String(k)); };

    // 服务/端口是最强信号
    if (svc("_googlecast") || svc("_airplay") || svc("_raop") || ports.contains(8009))
        return DeviceType::TvBox;
    if (svc("_ipp") || svc("_ipps") || svc("_printer") || ports.contains(9100))
        return DeviceType::Printer;
    if (svc("_spotify-connect"))
        return DeviceType::Speaker;
    if (ports.contains(554) || svc("_rtsp"))
        return DeviceType::Camera;
    if (ports.contains(62078) || has("iphone") || has("ipad"))
        return has("ipad") ? DeviceType::Tablet : DeviceType::Phone;

    // 型号/名称/厂商关键词
    if (has("iphone")) return DeviceType::Phone;
    if (has("ipad")) return DeviceType::Tablet;
    if (has("macbook") || has("imac") || has("mac mini") || has("desktop") || has("laptop")
        || has("pc-") || ports.contains(445))
        return DeviceType::Computer;
    if (has("android") || has("pixel") || has("redmi") || has("huawei") || has("honor")
        || has("oppo") || has("vivo") || has("oneplus") || has("galaxy"))
        return DeviceType::Phone;
    if (has("tv") || has("bravia") || has("aquos") || has("chromecast") || has("firetv")
        || has("appletv") || has("mibox") || has("shield"))
        return DeviceType::TvBox;
    if (has("router") || has("gateway") || has("openwrt") || has("mikrotik") || has("asus")
        || has("tp-link") || has("tplink") || has("netgear") || has("xiaomi router")
        || has("ax") || has("wifi"))
        return DeviceType::Router;
    if (has("nas") || has("synology") || has("qnap") || has("truenas"))
        return DeviceType::Nas;
    if (has("playstation") || has("ps4") || has("ps5") || has("xbox") || has("nintendo")
        || has("switch"))
        return DeviceType::GameConsole;
    if (has("printer") || has("hp ") || has("epson") || has("canon") || has("brother"))
        return DeviceType::Printer;
    if (has("camera") || has("ipcam") || has("hikvision") || has("dahua") || has("ezviz"))
        return DeviceType::Camera;
    if (has("echo") || has("homepod") || has("sonos") || has("speaker") || has("nest audio"))
        return DeviceType::Speaker;
    if (has("printer")) return DeviceType::Printer;
    if (svc("_smb") || svc("_workstation") || svc("_device-info"))
        return DeviceType::Computer;
    if (svc("_homekit") || svc("_hap"))
        return DeviceType::IoT;
    return DeviceType::Unknown;
}

// ———————————————————————————— 汇总 ————————————————————————————
void LanScanner::assemble()
{
    emitSnapshot(true);
}

// final=false 时是「阶段①刚探完」的抢先快照：ARP 表已经知道哪些设备在线（实测进页面 ~1.8s），
// 先把这批发给 UI，别让用户对着空列表干等到指纹阶段结束（~8s）。名称/类型等最终快照再刷新。
// final=true 才结束扫描态（scanningChanged(false)），指示器在补齐名称期间继续转。
void LanScanner::emitSnapshot(bool final)
{
    QVector<DeviceRecord> out;
    const QSet<QString> gwMacs = gatewayMacs();
    // 以 ARP 表为准（有 MAC 才算一台设备）。名称/服务信号按 IP 合并进来。
    for (auto it = m_arp.constBegin(); it != m_arp.constEnd(); ++it) {
        const QString ip = it.key();
        const QString mac = it.value();
        if (isJunkArpEntry(ip, mac))
            continue;
        if (m_localMacs.contains(mac))
            continue; // 本机各网卡统一由 appendSelfRecords 产出（信息更全）
        DeviceRecord d;
        d.mac = mac;
        d.ip = ip;
        d.online = true;
        d.lastSeen = QDateTime::currentDateTime();
        const Signals s = m_sig.value(ip);
        d.autoName = !s.hostname.isEmpty() ? s.hostname : s.friendly;
        d.model = s.model;
        d.vendor = !s.vendor.isEmpty() ? s.vendor : ouiVendor(mac);
        d.isGateway = m_gatewayIps.contains(ip) || gwMacs.contains(mac);
        d.isSelf = false;
        d.inLanSubnet = inAnyLanSubnet(ip);
        d.autoType = d.isGateway ? DeviceType::Router
                                 : classify(mac, d.autoName, d.model, d.vendor, s.ports, s.services);
        out.append(d);
    }
    // 本机（每张物理网卡一条）也作为设备加入（不参与劫持，UI 标「本机·保护」）。
    appendSelfRecords(out);

    if (final) {
        m_scanning = false;
        emit scanningChanged(false);
    }
    emit discovered(out);
}

// ———————————————————————————— 入口 ————————————————————————————
void LanScanner::scanFull()
{
    if (m_scanning)
        return;
    m_scanning = true;
    emit scanningChanged(true);

    m_arp.clear();
    m_sig.clear();

    ensureTopology(true); // 扫描入口：拓扑一定重算
    const QVector<quint32> hosts = hostsToProbe();

    // 先发名称查询（组播/单播），与 TCP 探测并行；回包在静默期内陆续到达。
    sendMdnsQueries();
    sendSsdpQuery();
    sendNbnsQueriesSliced(hosts); // 253 个 UDP 一口气发要占住 GUI 线程 20~50ms，切片发

    // 阶段⓪：先把系统 ARP 缓存里**已经有**的设备立刻出一版（一次 `arp -a`，几十毫秒）。
    // 点「刷新」后列表马上就有反应，不用干等阶段①把整段网段探完；平时在网里说话的设备
    // 系统早就解析过了，绝大多数会在这一版里直接出现。名称/类型等后面的快照再补。
    readArpTable([this, hosts] {
        emitSnapshot(false);
        if (hosts.isEmpty()) {
            // 大网段/无网段：没法逐台探，直接进静默期收尾。
            m_settleTimer->start(kSettleMs);
            return;
        }
        // 阶段①：整段网段每台主机只连 **一个** 端口，目的仅是逼系统做 ARP 解析。
        // 收口后读 ARP 表，再进阶段②只给真实存在的主机做端口指纹（见 startFingerprintPhase）。
        QVector<QPair<quint32, quint16>> arpJobs;
        arpJobs.reserve(hosts.size());
        for (quint32 ip : hosts)
            arpJobs.append({ip, kArpPort});
        runProbes(std::move(arpJobs), [this] { readArpTable([this] { startFingerprintPhase(); }); });
    });

    // 兜底：万一某一阶段的收口计数异常（回调链断了），超时后也强制读表 + assemble，
    // 避免扫描永久停在「扫描中」。两阶段都算上，给足余量。
    QTimer::singleShot(kScanWatchdogMs, this, [this] {
        if (m_scanning) {
            readArpTable([this] { m_settleTimer->start(600); });
        }
    });
}

void LanScanner::refreshLiveness(const QStringList &knownIps)
{
    // 轻量：定向探测已知 IP（触发/维持 ARP）+ 重读 ARP 表，产出「仅在线态」快照。
    if (m_scanning)
        return;
    ensureTopology(false); // 5s 一次的热更新走缓存：网卡/路由几乎不变，没必要每次重算
    for (const QString &ip : knownIps) {
        auto *sock = new QTcpSocket(this);
        // ★ 存活探测**必须直连**：目标是「这台局域网设备在不在」，经代理出去问的是
        //   代理服务器能不能连上它 —— 答案与我们要的问题无关，开着系统代理时还会把
        //   一整批内网地址送给代理。Qt 默认对每个 socket 查一次
        //   `QNetworkProxyFactory::systemProxyForQuery`，macOS 上那是读 SCDynamicStore，
        //   设备一多就是每 5 秒 N 次系统查询（103 台的 Time Profiler 采样里
        //   systemProxyForQuery/resolveProxy 各占 15 个样本，紧跟在 refreshLiveness 之后）。
        //   显式设 NoProxy 既修正了语义，也把这批查询整个省掉。
        sock->setProxy(QNetworkProxy::NoProxy);
        connect(sock, &QTcpSocket::connected, sock, &QTcpSocket::deleteLater);
        connect(sock, &QTcpSocket::errorOccurred, sock,
                [sock](QAbstractSocket::SocketError) { sock->deleteLater(); });
        QTimer::singleShot(kProbeTimeoutMs, sock, &QTcpSocket::deleteLater);
        sock->connectToHost(ip, 80);
    }
    // 探测后读表并产出快照（只含在线设备的 ip/mac；名称沿用 store 内已有）。
    QTimer::singleShot(kProbeTimeoutMs + 300, this, [this] {
        auto *p = new QProcess(this);
#if defined(Q_OS_WIN)
        p->start("arp", {"-a"});
#elif defined(Q_OS_MACOS)
        p->start("arp", {"-an"});
#else
        p->start("arp", {"-n"});
#endif
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, p](int, QProcess::ExitStatus) {
                    const QString text = QString::fromLocal8Bit(p->readAllStandardOutput());
                    p->deleteLater();
                    // arp -a 给的是完整表 → 整表替换 m_arp（顺带让 gatewayMac() 保持新鲜，
                    // 网关劫持配置要用）。**必须整表替换而不是累加**：只有「本轮表里没了」
                    // 才会走陈旧判定→离线，累加会让设备永远显示在线。
                    QHash<QString, QString> fresh;
                    for (const QString &line : text.split('\n')) {
                        const QString ip = ipv4FromLine(line);
                        const QString mac = DeviceStore::normalizeMac(macFromLine(line));
                        if (isJunkArpEntry(ip, mac))
                            continue;
                        fresh.insert(ip, mac);
                    }
                    m_arp = fresh;
                    const QSet<QString> gwMacs = gatewayMacs();
                    QVector<DeviceRecord> out;
                    for (auto it = fresh.constBegin(); it != fresh.constEnd(); ++it) {
                        const QString ip = it.key();
                        const QString mac = it.value();
                        if (m_localMacs.contains(mac))
                            continue; // 本机由 appendSelfRecords 统一产出
                        DeviceRecord d;
                        d.mac = mac;
                        d.ip = ip;
                        d.online = true;
                        d.lastSeen = QDateTime::currentDateTime();
                        d.isGateway = m_gatewayIps.contains(ip) || gwMacs.contains(mac);
                        d.inLanSubnet = inAnyLanSubnet(ip);
                        out.append(d);
                    }
                    // 本机每张物理网卡都必须每轮上报，否则 15s 陈旧判定会把本机判成掉线。
                    appendSelfRecords(out);
                    emit discovered(out);
                });
    });
}
