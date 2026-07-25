#include "GatewaySelfTest.h"

#include <QtGlobal> // 必须先引入才有 Q_OS_LINUX 宏，否则下面的 #if 恒假→整个实现被跳过→链接未定义

#if defined(Q_OS_LINUX)

#include "../DeviceStore.h"
#include "IL2Endpoint.h"
#include "NetStack.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QSocketNotifier>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtGlobal>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

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

// 自测用 TAP 二层端点：打开已存在的 TAP（脚本 `ip tuntap add` 建好），读写完整以太帧。
// 不声明新信号 → 无需 Q_OBJECT（复用基类 frameReceived，functor connect）。
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

    auto *ep = new TapEndpoint(localMac, qApp);
    QString err;
    if (!ep->openTap(tap, &err)) {
        std::fprintf(stderr, "SELFTEST: %s\n", err.toLatin1().constData());
        return 3;
    }
    auto *net = new NetStack(ep, socksPort, qApp);
    if (!net->init(localMac, &err)) {
        std::fprintf(stderr, "SELFTEST: NetStack.init 失败: %s\n", err.toLatin1().constData());
        return 3;
    }
    // 关键：把二层收到的帧接进用户态栈。真实路径由 LanGateway 做此连接（并按 victim MAC 过滤）；
    // 自测直连 NetStack，必须在这里手动接线，否则读到的帧无人消费（= 之前 0 条 NETSTACK IN 的原因）。
    QObject::connect(ep, &IL2Endpoint::frameReceived, net, &NetStack::inputFrame);
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

#endif // Q_OS_LINUX
