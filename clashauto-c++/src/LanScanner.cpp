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
constexpr int kMaxInFlight = 120;         // 同时在途 TCP 探测上限，防 FD 耗尽
constexpr int kProbeTimeoutMs = 1200;
constexpr int kSettleMs = 2500;           // 探测发完后等收 ARP/名称回包的静默期

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
void LanScanner::detectLocalTopology()
{
    m_localIp.clear();
    m_localMac.clear();
    m_ifaceName.clear();
    m_netBase = m_netMask = 0;

    // 选一个「活动、非回环、有 IPv4」的接口作为本机网段。优先带默认路由的；否则第一个可用。
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (ip.isLoopback())
                continue;
            const quint32 ip32 = ip.toIPv4Address();
            const quint32 mask = entry.netmask().toIPv4Address();
            if (mask == 0)
                continue;
            m_localIp = ip.toString();
            m_localMac = DeviceStore::normalizeMac(iface.hardwareAddress());
            m_ifaceName = iface.humanReadableName();
            m_netMask = mask;
            m_netBase = ip32 & mask;
            break;
        }
        if (!m_localIp.isEmpty())
            break;
    }

    // 默认网关（用于「网关·保护」徽章）。取不到就用网段首个主机 base+1 兜底。
    m_gatewayIp.clear();
#if defined(Q_OS_LINUX)
    QFile rf("/proc/net/route");
    if (rf.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = rf.readAll().split('\n');
        for (const QByteArray &l : lines) {
            const QList<QByteArray> f = l.simplified().split(' ');
            if (f.size() >= 3 && f.at(1) == "00000000") {
                bool ok = false;
                // /proc/net/route 里网关是十六进制、按内存(小端)存的：值的第 0 字节即第一个点分段。
                const quint32 v = f.at(2).toUInt(&ok, 16);
                if (ok && v) {
                    m_gatewayIp = QString("%1.%2.%3.%4")
                                      .arg(v & 0xFF).arg((v >> 8) & 0xFF)
                                      .arg((v >> 16) & 0xFF).arg((v >> 24) & 0xFF);
                    break;
                }
            }
        }
        rf.close();
    }
#else
    {
        QProcess p;
#if defined(Q_OS_MACOS)
        p.start("route", {"-n", "get", "default"});
#else // Windows
        p.start("route", {"print", "0.0.0.0"});
#endif
        if (p.waitForFinished(2000)) {
            const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
#if defined(Q_OS_MACOS)
            static const QRegularExpression re("gateway:\\s*(\\S+)");
            const auto m = re.match(out);
            if (m.hasMatch())
                m_gatewayIp = m.captured(1);
#else
            // route print: 行 "0.0.0.0  0.0.0.0  <网关>  <接口>  <跃点>"
            const QStringList rows = out.split('\n');
            for (const QString &r : rows) {
                const QString t = r.simplified();
                if (t.startsWith("0.0.0.0 0.0.0.0")) {
                    const QStringList c = t.split(' ');
                    if (c.size() >= 3)
                        m_gatewayIp = c.at(2);
                    break;
                }
            }
#endif
        }
    }
#endif
    if (m_gatewayIp.isEmpty() && m_netBase)
        m_gatewayIp = u32ToIp(m_netBase + 1);
}

QVector<quint32> LanScanner::hostsToProbe() const
{
    QVector<quint32> hosts;
    if (!m_netBase || !m_netMask)
        return hosts;
    const quint32 hostBits = ~m_netMask;
    // 只扫 /24 及更小（>256 主机的网段太大，逐个探测代价过高——退化为只读 ARP 表）。
    if (hostBits >= 256)
        return hosts;
    const quint32 localU = QHostAddress(m_localIp).toIPv4Address();
    const quint32 broadcast = m_netBase | hostBits;
    for (quint32 ip = m_netBase + 1; ip < broadcast; ++ip) {
        if (ip == localU)
            continue; // 本机不探
        hosts.append(ip);
    }
    return hosts;
}

// ———————————————————————————— 主动探测 ————————————————————————————
void LanScanner::probeArp(const QVector<quint32> &hosts)
{
    // 构造 (ip,port) 作业清单，按 kMaxInFlight 限流泵出。用共享的待办队列 + 计数。
    auto jobs = std::make_shared<QVector<QPair<quint32, quint16>>>();
    for (quint32 ip : hosts)
        for (quint16 port : kProbePorts)
            jobs->append({ip, port});

    auto inFlight = std::make_shared<int>(0);
    auto pump = std::make_shared<std::function<void()>>();
    *pump = [this, jobs, inFlight, pump]() {
        while (*inFlight < kMaxInFlight && !jobs->isEmpty()) {
            const auto job = jobs->takeFirst();
            ++(*inFlight);
            ++m_pendingProbes;
            auto *sock = new QTcpSocket(this);
            auto *timer = new QTimer(sock);
            timer->setSingleShot(true);
            timer->setInterval(kProbeTimeoutMs);

            const QString ipStr = u32ToIp(job.first);
            const quint16 port = job.second;

            // 每个 socket 的 connected / errorOccurred / 超时 三路只允许收口一次，否则会重复
            // 递减计数（导致 pending 提前归零 / 变负、settle 误触发）。
            auto done = std::make_shared<bool>(false);
            auto finish = [this, sock, inFlight, pump, done](bool open, const QString &ip, quint16 pt) {
                if (*done)
                    return;
                *done = true;
                if (open)
                    sig(ip).ports.insert(pt);
                sock->abort();
                sock->deleteLater();
                --(*inFlight);
                if (--m_pendingProbes <= 0) {
                    // 探测全部收口 → 立刻读一次 ARP 表（此时系统已对存活主机解析完 MAC），
                    // 再起静默计时器，等 mDNS/SSDP/NBNS 名称回包后 assemble。
                    readArpTable();
                    m_settleTimer->start(kSettleMs);
                }
                (*pump)(); // 补位下一批
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
    };
    (*pump)();
}

void LanScanner::probePort(quint32 ip, quint16 port)
{
    Q_UNUSED(ip);
    Q_UNUSED(port);
    // 预留（refreshLiveness 用定向单端口探测已在下方内联实现）。
}

// ———————————————————————————— ARP 表 ————————————————————————————
void LanScanner::readArpTable()
{
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
                onArpOutput(p->readAllStandardOutput());
                p->deleteLater();
            });
    // arp 命令偶发不存在/超时：3s 兜底杀掉，走 assemble 也不会卡。
    QTimer::singleShot(3000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
}

void LanScanner::onArpOutput(const QByteArray &out)
{
    const QString text = QString::fromLocal8Bit(out);
    const QStringList lines = text.split('\n');
    for (const QString &line : lines) {
        const QString ip = ipv4FromLine(line);
        if (ip.isEmpty())
            continue;
        const QString rawMac = macFromLine(line);
        if (rawMac.isEmpty())
            continue;
        const QString mac = DeviceStore::normalizeMac(rawMac);
        if (mac.isEmpty())
            continue;
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
    QVector<DeviceRecord> out;
    const QString localMac = m_localMac;
    // 以 ARP 表为准（有 MAC 才算一台设备）。名称/服务信号按 IP 合并进来。
    for (auto it = m_arp.constBegin(); it != m_arp.constEnd(); ++it) {
        const QString ip = it.key();
        const QString mac = it.value();
        if (mac.isEmpty())
            continue;
        DeviceRecord d;
        d.mac = mac;
        d.ip = ip;
        d.online = true;
        d.lastSeen = QDateTime::currentDateTime();
        const Signals s = m_sig.value(ip);
        d.autoName = !s.hostname.isEmpty() ? s.hostname : s.friendly;
        d.model = s.model;
        d.vendor = !s.vendor.isEmpty() ? s.vendor : ouiVendor(mac);
        d.isGateway = (ip == m_gatewayIp);
        d.isSelf = false;
        d.autoType = d.isGateway ? DeviceType::Router
                                 : classify(mac, d.autoName, d.model, d.vendor, s.ports, s.services);
        out.append(d);
    }
    // 本机也作为一台设备加入（不参与劫持，UI 标「本机·保护」）。
    if (!m_localIp.isEmpty() && !localMac.isEmpty()) {
        DeviceRecord me;
        me.mac = localMac;
        me.ip = m_localIp;
        me.online = true;
        me.isSelf = true;
        me.lastSeen = QDateTime::currentDateTime();
        me.autoName = QHostInfo::localHostName();
        me.vendor = ouiVendor(localMac);
        me.autoType = DeviceType::Computer;
        out.append(me);
    }

    m_scanning = false;
    emit scanningChanged(false);
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
    m_pendingProbes = 0;

    detectLocalTopology();
    const QVector<quint32> hosts = hostsToProbe();

    // 先发名称查询（组播/单播），与 TCP 探测并行；回包在静默期内陆续到达。
    sendMdnsQueries();
    sendSsdpQuery();
    for (quint32 ip : hosts)
        sendNbnsQuery(ip);
    for (quint32 ip : hosts)
        reverseDnsLookup(u32ToIp(ip));

    if (hosts.isEmpty()) {
        // 大网段/无网段：只读一次 ARP 表后收口。
        readArpTable();
        m_settleTimer->start(kSettleMs);
        return;
    }
    probeArp(hosts);
    // ARP 表在探测过程中由系统填充；所有探测收口(m_pendingProbes→0)后，finish 里读一次 ARP 表
    // 并起静默计时器 → 到点 assemble（用最新 m_arp + 迟到的名称信号）。
    // 兜底：万一探测计数异常未归零，超时后也强制读表 + assemble，避免永久不出结果。
    QTimer::singleShot(kProbeTimeoutMs + kSettleMs + 2000, this, [this] {
        if (m_scanning) {
            readArpTable();
            m_settleTimer->start(600);
        }
    });
}

void LanScanner::refreshLiveness(const QStringList &knownIps)
{
    // 轻量：定向探测已知 IP（触发/维持 ARP）+ 重读 ARP 表，产出「仅在线态」快照。
    if (m_scanning)
        return;
    detectLocalTopology();
    for (const QString &ip : knownIps) {
        auto *sock = new QTcpSocket(this);
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
                    QVector<DeviceRecord> out;
                    for (const QString &line : text.split('\n')) {
                        const QString ip = ipv4FromLine(line);
                        const QString mac = DeviceStore::normalizeMac(macFromLine(line));
                        if (ip.isEmpty() || mac.isEmpty())
                            continue;
                        DeviceRecord d;
                        d.mac = mac;
                        d.ip = ip;
                        d.online = true;
                        d.lastSeen = QDateTime::currentDateTime();
                        d.isGateway = (ip == m_gatewayIp);
                        out.append(d);
                    }
                    if (!m_localIp.isEmpty() && !m_localMac.isEmpty()) {
                        DeviceRecord me;
                        me.mac = m_localMac;
                        me.ip = m_localIp;
                        me.online = true;
                        me.isSelf = true;
                        me.lastSeen = QDateTime::currentDateTime();
                        out.append(me);
                    }
                    emit discovered(out);
                });
    });
}
