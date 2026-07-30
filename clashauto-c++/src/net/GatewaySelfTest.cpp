#include "GatewaySelfTest.h"

#include <QtGlobal> // 必须先引入才有 Q_OS_LINUX 宏，否则下面的 #if 恒假→整个实现被跳过→链接未定义

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)

#include "../DeviceStore.h"
#include "IL2Endpoint.h"
#include "NdpSpoofer.h"
#include "NetStack.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QSocketNotifier>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>
#include <QtGlobal>

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

QByteArray parseMac(const QString &mac)
{
    const QStringList p = mac.split(':');
    if (p.size() != 6)
        return {};
    QByteArray out(6, char(0));
    for (int i = 0; i < 6; ++i) {
        bool ok = false;
        out[i] = char(p[i].toUInt(&ok, 16));
        if (!ok)
            return {};
    }
    return out;
}

#if defined(Q_OS_LINUX)
// Linux 自测用 TAP 二层端点：打开已存在的 TAP（脚本 `ip tuntap add` 建好），读写完整以太帧。
// 不声明新信号 → 无需 Q_OBJECT（复用基类 frameReceived，functor connect）。
// mac 不用它：mac 自测直接用真实 BPF 端点(createL2Endpoint)绑到 feth 接口。
class TapEndpoint final : public IL2Endpoint
{
public:
    TapEndpoint(const QByteArray &mac, QObject *parent = nullptr)
        : IL2Endpoint(parent), m_mac(mac)
    {
    }
    ~TapEndpoint() override { close(); }

    bool openTap(const QString &dev, QString *err)
    {
        m_fd = ::open("/dev/net/tun", O_RDWR);
        if (m_fd < 0) {
            if (err) *err = QStringLiteral("open /dev/net/tun 失败: ") + strerror(errno);
            return false;
        }
        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
        std::strncpy(ifr.ifr_name, dev.toLatin1().constData(), IFNAMSIZ - 1);
        if (::ioctl(m_fd, TUNSETIFF, &ifr) < 0) {
            if (err) *err = QStringLiteral("TUNSETIFF(%1) 失败: %2").arg(dev, strerror(errno));
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        ::fcntl(m_fd, F_SETFL, O_NONBLOCK);
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        QObject::connect(m_notifier, &QSocketNotifier::activated, this, [this] { drain(); });
        return true;
    }

    bool open(const QString &, QString *) override { return m_fd >= 0; }
    void close() override
    {
        if (m_notifier) { delete m_notifier; m_notifier = nullptr; }
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
    }
    bool isOpen() const override { return m_fd >= 0; }
    bool send(const QByteArray &f) override
    {
        return m_fd >= 0 && ::write(m_fd, f.constData(), f.size()) == f.size();
    }
    QByteArray localMac() const override { return m_mac; }
    int ifIndex() const override { return 0; }
    int mtu() const override { return 1500; }

private:
    void drain()
    {
        char buf[65536];
        ssize_t n;
        while ((n = ::read(m_fd, buf, sizeof(buf))) > 0)
            emit frameReceived(QByteArray(buf, static_cast<int>(n)));
    }
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QByteArray m_mac;
};
#endif // Q_OS_LINUX（TapEndpoint）

// 极简假 SOCKS5 服务器：完成 greeting/(可选)用户名认证/CONNECT，记录用户名，回一段 HTTP 标记响应。
// 收到「带期望用户名的 CONNECT」即判定核心链路通过 → 退出码 0。
class FakeSocks : public QObject
{
public:
    FakeSocks(quint16 port, const QString &expectUser, QObject *parent = nullptr)
        : QObject(parent), m_expectUser(expectUser)
    {
        connect(&m_server, &QTcpServer::newConnection, this, &FakeSocks::onConn);
        if (!m_server.listen(QHostAddress::LocalHost, port))
            std::fprintf(stderr, "SELFTEST: 假 SOCKS 监听 %u 失败\n", port);
        else
            std::fprintf(stderr, "SELFTEST: 假 SOCKS 就绪于 127.0.0.1:%u\n", port);
    }

private:
    struct Conn { int phase = 0; QByteArray buf; QString user; };

    void onConn()
    {
        while (QTcpSocket *s = m_server.nextPendingConnection()) {
            auto *c = new Conn;
            connect(s, &QTcpSocket::readyRead, s, [this, s, c] { onData(s, c); });
            connect(s, &QTcpSocket::disconnected, s, [s, c] { delete c; s->deleteLater(); });
        }
    }

    void onData(QTcpSocket *s, Conn *c)
    {
        c->buf += s->readAll();
        // phase 0: greeting [05, n, methods...]
        if (c->phase == 0) {
            if (c->buf.size() < 2) return;
            const int n = quint8(c->buf[1]);
            if (c->buf.size() < 2 + n) return;
            const QByteArray methods = c->buf.mid(2, n);
            c->buf.remove(0, 2 + n);
            const bool userpass = methods.contains(char(0x02));
            char reply[2] = {0x05, char(userpass ? 0x02 : 0x00)};
            s->write(reply, 2);
            c->phase = userpass ? 1 : 2;
        }
        // phase 1: user/pass auth [01, ulen, user, plen, pass]
        if (c->phase == 1) {
            if (c->buf.size() < 2) return;
            const int ulen = quint8(c->buf[1]);
            if (c->buf.size() < 2 + ulen + 1) return;
            const int plen = quint8(c->buf[2 + ulen]);
            if (c->buf.size() < 2 + ulen + 1 + plen) return;
            c->user = QString::fromLatin1(c->buf.mid(2, ulen));
            c->buf.remove(0, 2 + ulen + 1 + plen);
            char reply[2] = {0x01, 0x00};
            s->write(reply, 2);
            c->phase = 2;
        }
        // phase 2: CONNECT [05,01,00,atyp,addr,port]
        if (c->phase == 2) {
            if (c->buf.size() < 4) return;
            const int atyp = quint8(c->buf[3]);
            int need = 4 + 2; // + port
            if (atyp == 0x01) need += 4;
            else if (atyp == 0x04) need += 16;
            else if (atyp == 0x03) { if (c->buf.size() < 5) return; need += 1 + quint8(c->buf[4]); }
            else { s->close(); return; }
            if (c->buf.size() < need) return;
            c->buf.remove(0, need);
            const char ok[10] = {0x05, 0, 0, 0x01, 0, 0, 0, 0, 0, 0};
            s->write(ok, 10);
            // 回一段 HTTP 标记，供外部 curl 确认回程通路。
            const QByteArray body = "COAST_SELFTEST_OK\n";
            s->write("HTTP/1.0 200 OK\r\nContent-Length: "
                     + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            c->phase = 3;
            std::fprintf(stderr, "SELFTEST: 收到 CONNECT，用户名='%s'（期望='%s'）\n",
                         c->user.toLatin1().constData(), m_expectUser.toLatin1().constData());
            if (m_expectUser.isEmpty() || c->user == m_expectUser) {
                std::fprintf(stderr, "SELFTEST: PASS —— 整条链路(帧→lwIP握手→catch-all→SOCKS+身份)通\n");
                QTimer::singleShot(500, qApp, [] { QCoreApplication::exit(0); }); // 留时间冲刷回程帧
            } else {
                std::fprintf(stderr, "SELFTEST: FAIL —— 用户名不匹配\n");
                QTimer::singleShot(200, qApp, [] { QCoreApplication::exit(2); });
            }
        }
    }

    QTcpServer m_server;
    QString m_expectUser;
};

QByteArray envOr(const char *key, const QByteArray &def)
{
    return qEnvironmentVariableIsSet(key) ? qgetenv(key) : def;
}

} // namespace

int runGatewaySelfTest()
{
    const QString tap = QString::fromLatin1(envOr("COAST_SELFTEST_TAP", "cst0"));
    const QByteArray localMac = parseMac(QString::fromLatin1(
        envOr("COAST_SELFTEST_LOCALMAC", "02:00:00:00:00:01")));
    const QString victimIp = QString::fromLatin1(envOr("COAST_SELFTEST_VICTIM_IP", "10.9.9.1"));
    const QString victimMacStr = QString::fromLatin1(envOr("COAST_SELFTEST_VICTIM_MAC", ""));
    const QByteArray victimMac = parseMac(victimMacStr);
    const quint16 socksPort = QString::fromLatin1(envOr("COAST_SELFTEST_SOCKSPORT", "7899")).toUShort();
    const QString expectUser = victimMacStr.isEmpty() ? QString()
                                                      : DeviceStore::socksUser(victimMacStr);

    if (localMac.isEmpty() || victimMac.isEmpty()) {
        std::fprintf(stderr, "SELFTEST: 环境缺 LOCALMAC/VICTIM_MAC\n");
        return 3;
    }

    auto *socks = new FakeSocks(socksPort, expectUser, qApp);
    Q_UNUSED(socks);

    QString err;
    IL2Endpoint *ep = nullptr;
#if defined(Q_OS_LINUX)
    // Linux：自建 TAP 端点（拥有 /dev/net/tun 的 fd，脚本已 `ip tuntap add` 建好 cst0）。
    auto *tapEp = new TapEndpoint(localMac, qApp);
    if (!tapEp->openTap(tap, &err)) {
        std::fprintf(stderr, "SELFTEST: %s\n", err.toLatin1().constData());
        return 3;
    }
    ep = tapEp;
#else
    // mac：直接用真实 BPF 端点（createL2Endpoint），绑到脚本建好的 feth 接口——顺带真跑一遍 MacL2Endpoint。
    ep = createL2Endpoint(qApp);
    if (!ep || !ep->open(tap, &err)) {
        std::fprintf(stderr, "SELFTEST: 打开二层端点(%s)失败: %s\n",
                     tap.toLatin1().constData(), err.toLatin1().constData());
        return 3;
    }
#endif
    auto *net = new NetStack(socksPort, qApp);
    if (!net->init(&err)) {
        std::fprintf(stderr, "SELFTEST: NetStack.init 失败: %s\n", err.toLatin1().constData());
        return 3;
    }
    // 挂上这张（唯一的）测试网卡。本机 IP/掩码要让 victimIp 落在同一子网里——netif 的出方向
    // 路由靠子网匹配（见 NetStack 头注释），默认 10.9.9.254/24 配默认 victim 10.9.9.1。
    const QString selfIp = QString::fromLatin1(envOr("COAST_SELFTEST_LOCAL_IP", "10.9.9.254"));
    const QString selfMask = QString::fromLatin1(envOr("COAST_SELFTEST_NETMASK", "255.255.255.0"));
    if (!net->addNic(ep, localMac, selfIp, selfMask, &err)) {
        std::fprintf(stderr, "SELFTEST: NetStack.addNic 失败: %s\n", err.toLatin1().constData());
        return 3;
    }
    // 关键：把二层收到的帧接进用户态栈。真实路径由 LanGateway 做此连接（并按 victim MAC 过滤）；
    // 自测直连 NetStack，必须在这里手动接线，否则读到的帧无人消费（= 之前 0 条 NETSTACK IN 的原因）。
    QObject::connect(ep, &IL2Endpoint::frameReceived, net,
                     [net, ep](const QByteArray &f) { net->inputFrame(ep, f); });
    net->addDevice(victimIp, victimMac, expectUser);
    std::fprintf(stderr, "SELFTEST: 就绪 tap=%s victim=%s user=%s，等待 curl…\n",
                 tap.toLatin1().constData(), victimIp.toLatin1().constData(),
                 expectUser.toLatin1().constData());

    QTimer::singleShot(15000, qApp, [] {
        std::fprintf(stderr, "SELFTEST: 超时（15s 内未见成功的 CONNECT）\n");
        QCoreApplication::exit(1);
    });
    return qApp->exec();
}

// ———————————— NdpSpoofer::parseRouterAdvert 的纯解析自测（说明见头文件）————————————
// 这里手工拼 RA 字节流，不碰网络、不需要 root。Linux/mac 都编（解析器本身是跨平台纯字节操作）。
namespace {

// 拼一帧可配置的 RA。默认值是一条**合法**的 RA，各用例只改自己要测的那一项。
struct RaSpec {
    quint8 hopLimit = 255;        // NDP 强制 255
    quint16 routerLifetime = 1800; // 0 = 「我不是默认路由器」
    QByteArray srcIp6;            // 空 = 用默认的 fe80::1
    QByteArray ethSrcMac = QByteArray::fromHex("aabbccddeeff");
    bool withSlla = true;         // 带 Source Link-Layer Address 选项（MAC 与 ethSrc 故意不同）
    QByteArray sllaMac = QByteArray::fromHex("112233445566");
    bool withPrefix = true;
    quint8 prefixLen = 64;
    quint8 prefixFlags = 0x80;    // bit7 = on-link(L)
    bool zeroLenOption = false;   // 构造 len==0 的畸形选项（必须被拒且不死循环）
    bool withRdnss = false;       // 带 RDNSS 选项（type 25，携带递归 DNS 服务器地址）
    int rdnssCount = 1;           // RDNSS 里放几个地址（每个 16 字节）
};

QByteArray buildRa(const RaSpec &s)
{
    QByteArray f;
    f.append(QByteArray::fromHex("333300000001"));                  // dst MAC（ff02::1 的组播 MAC）
    f.append(s.ethSrcMac);
    f.append(QByteArray::fromHex("86dd"));                          // ethertype
    // IPv6 头（40）：version/tc/fl(4) payloadLen(2) nextHdr(1) hopLimit(1) src(16) dst(16)
    f.append(QByteArray::fromHex("60000000"));
    f.append(QByteArray::fromHex("0000"));                          // payload len（解析器不校验）
    f.append(char(58));                                             // next header = ICMPv6
    f.append(char(s.hopLimit));
    QByteArray src = s.srcIp6;
    if (src.isEmpty()) {
        src = QByteArray(16, char(0));
        src[0] = char(0xFE);
        src[1] = char(0x80);
        src[15] = char(0x01);                                       // fe80::1
    }
    f.append(src);
    QByteArray dst(16, char(0));
    dst[0] = char(0xFF);
    dst[1] = char(0x02);
    dst[15] = char(0x01);                                           // ff02::1
    f.append(dst);
    // RA 固定部分（16）：type code cksum curHopLimit flags routerLifetime reachable retrans
    f.append(char(134));
    f.append(char(0));
    f.append(QByteArray::fromHex("0000"));                          // checksum（解析器不校验）
    f.append(char(64));                                             // curHopLimit
    f.append(char(0));                                              // flags
    f.append(char((s.routerLifetime >> 8) & 0xFF));
    f.append(char(s.routerLifetime & 0xFF));
    f.append(QByteArray::fromHex("00000000"));                      // reachable time
    f.append(QByteArray::fromHex("00000000"));                      // retrans timer
    if (s.zeroLenOption) {
        f.append(char(1));
        f.append(char(0)); // len=0 → 非法，解析器必须停下而不是原地打转
        f.append(QByteArray(6, char(0)));
        return f;
    }
    if (s.withSlla) {
        f.append(char(1));  // type = Source Link-Layer Address
        f.append(char(1));  // len = 1 × 8 字节
        f.append(s.sllaMac);
    }
    if (s.withPrefix) {
        f.append(char(3));  // type = Prefix Information
        f.append(char(4));  // len = 4 × 8 = 32 字节
        f.append(char(s.prefixLen));
        f.append(char(s.prefixFlags));
        f.append(QByteArray::fromHex("00278d00"));                  // valid lifetime
        f.append(QByteArray::fromHex("00093a80"));                  // preferred lifetime
        f.append(QByteArray::fromHex("00000000"));                  // reserved
        QByteArray pfx(16, char(0));
        pfx[0] = char(0x24);
        pfx[1] = char(0x08);                                        // 2408:: 之类的运营商前缀
        f.append(pfx);
    }
    if (s.withRdnss && s.rdnssCount > 0) {
        // RDNSS（RFC 8106）：type(1) len(1) reserved(2) lifetime(4) + N×16 字节地址。len = 1 + 2N。
        f.append(char(25));                                         // type = RDNSS
        f.append(char(1 + 2 * s.rdnssCount));                       // len（单位 8 字节）
        f.append(QByteArray::fromHex("0000"));                      // reserved
        f.append(QByteArray::fromHex("00000e10"));                  // lifetime
        for (int i = 0; i < s.rdnssCount; ++i) {
            QByteArray dns(16, char(0));
            dns[0] = char(0x24);
            dns[1] = char(0x08);                                    // 与前缀同段：路由器链上全局地址
            dns[15] = char(0x01 + i);                               // ...::1, ...::2 …
            f.append(dns);
        }
    }
    return f;
}

int g_fail = 0;
void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_fail;
        std::fprintf(stderr, "NDP-RA-SELFTEST: FAIL — %s\n", what);
    }
}

} // namespace

int runNdpRaSelfTest()
{
    g_fail = 0;

    // 1) 正常 RA：LL 从 IPv6 源地址取，MAC 以 SLLA 选项为准（**不是**以太源 MAC），前缀取到。
    {
        QString ll, mac;
        QByteArray pfx;
        int plen = -1;
        const bool ok = NdpSpoofer::parseRouterAdvert(buildRa({}), &ll, &mac, &pfx, &plen);
        check(ok, "合法 RA 应解析成功");
        check(ll == QStringLiteral("fe80::1"), "routerLL 应取 IPv6 源地址");
        check(mac == QStringLiteral("11:22:33:44:55:66"), "routerMac 应优先取 SLLA 选项");
        check(plen == 64, "前缀长度应为 64");
        check(pfx.size() == 16 && quint8(pfx[0]) == 0x24 && quint8(pfx[1]) == 0x08, "前缀内容不对");
    }
    // 2) 没有 SLLA 选项 → 回落到以太源 MAC。
    {
        RaSpec s;
        s.withSlla = false;
        QString ll, mac;
        check(NdpSpoofer::parseRouterAdvert(buildRa(s), &ll, &mac, nullptr, nullptr), "无 SLLA 也应成功");
        check(mac == QStringLiteral("aa:bb:cc:dd:ee:ff"), "无 SLLA 时应回落以太源 MAC");
    }
    // 3) 非 on-link 前缀（L 位为 0）不采纳 —— 它不代表「本链路直连可达」，拿来做旁路判据是错的。
    {
        RaSpec s;
        s.prefixFlags = 0x40; // 只有 A(autonomous)，没有 L(on-link)
        QByteArray pfx;
        int plen = -1;
        check(NdpSpoofer::parseRouterAdvert(buildRa(s), nullptr, nullptr, &pfx, &plen), "应仍解析成功");
        check(plen == -1, "非 on-link 前缀不应被采纳");
    }
    // 3b) RDNSS 选项：解析器把递归 DNS 服务器地址原样抽出（前缀/全局过滤在调用方做，不在这里）。
    {
        RaSpec s;
        s.withRdnss = true;
        s.rdnssCount = 2;
        QVector<QByteArray> rd;
        check(NdpSpoofer::parseRouterAdvert(buildRa(s), nullptr, nullptr, nullptr, nullptr, &rd),
              "带 RDNSS 的 RA 应解析成功");
        check(rd.size() == 2, "应抽出 2 个 RDNSS 地址");
        if (rd.size() == 2) {
            check(rd[0].size() == 16 && quint8(rd[0][0]) == 0x24 && quint8(rd[0][15]) == 0x01,
                  "第 1 个 RDNSS 地址内容不对");
            check(quint8(rd[1][15]) == 0x02, "第 2 个 RDNSS 地址内容不对");
        }
    }
    // 3c) 不传 rdnss 出参（老调用方）也不能崩；SLLA/前缀照常解析。
    {
        RaSpec s;
        s.withRdnss = true;
        QString ll;
        check(NdpSpoofer::parseRouterAdvert(buildRa(s), &ll, nullptr, nullptr, nullptr),
              "rdnss 出参省略也应解析成功");
        check(ll == QStringLiteral("fe80::1"), "省略 rdnss 时其它字段应照常");
    }
    // 4) 四个必须拒绝的用例。
    {
        RaSpec s;
        s.hopLimit = 64; // 跨网段伪造的 RA 到达时 hop limit 必然 <255
        check(!NdpSpoofer::parseRouterAdvert(buildRa(s), nullptr, nullptr, nullptr, nullptr),
              "hop limit != 255 必须拒绝");
    }
    {
        RaSpec s;
        s.routerLifetime = 0; // 「我不是默认路由器」
        check(!NdpSpoofer::parseRouterAdvert(buildRa(s), nullptr, nullptr, nullptr, nullptr),
              "Router Lifetime == 0 必须拒绝");
    }
    {
        RaSpec s;
        s.srcIp6 = QByteArray(16, char(0));
        s.srcIp6[0] = char(0x24);
        s.srcIp6[1] = char(0x08); // 全局单播，不是 fe80::/10
        check(!NdpSpoofer::parseRouterAdvert(buildRa(s), nullptr, nullptr, nullptr, nullptr),
              "源地址非链路本地必须拒绝");
    }
    {
        RaSpec s;
        s.zeroLenOption = true;
        QString ll;
        // 畸形选项：解析本身仍算成功（固定部分是好的），关键是**不能死循环**、也不能读越界。
        check(NdpSpoofer::parseRouterAdvert(buildRa(s), &ll, nullptr, nullptr, nullptr),
              "len==0 选项不应让解析崩掉");
        check(ll == QStringLiteral("fe80::1"), "len==0 选项前已解析出的字段应保留");
    }
    // 5) 截断帧不得越界（逐字节截断跑一遍，主要靠 ASAN/崩溃暴露问题）。带 RDNSS 的帧也跑一遍，
    //    专门压 RDNSS 地址逐个读取的边界（naddr 与 off+16 的越界判定）。
    {
        RaSpec s;
        s.withRdnss = true;
        s.rdnssCount = 3;
        const QByteArray full = buildRa(s);
        for (int n = 0; n < full.size(); ++n) {
            QVector<QByteArray> rd;
            NdpSpoofer::parseRouterAdvert(full.left(n), nullptr, nullptr, nullptr, nullptr, &rd);
        }
    }

    if (g_fail == 0)
        std::fprintf(stderr, "NDP-RA-SELFTEST: PASS（解析 + SLLA 优先 + on-link 判据 + RDNSS 抽取 + 4 个拒绝用例 + 截断）\n");
    else
        std::fprintf(stderr, "NDP-RA-SELFTEST: %d 个断言失败\n", g_fail);
    std::fflush(stderr);
    return g_fail == 0 ? 0 : 1;
}

#endif // Q_OS_LINUX || Q_OS_MACOS
