#include "GatewaySelfTest.h"

#include <QtGlobal> // 必须先引入才有 Q_OS_LINUX 宏，否则下面的 #if 恒假→整个实现被跳过→链接未定义

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)

#include "../DeviceStore.h"
#include "IL2Endpoint.h"
#include "InprocTelemetry.h" // 组合自测的第三方对照：两条路都该在控制面留下连接记录
#include "LocalTunService.h" // 组合自测：网关在跑的同时开进程内 TUN
#include "NdpSpoofer.h"
#include "NetStack.h"
#include "Socks5Client.h"           // 组合自测的回退工厂（打到假 SOCKS = 网关那条路的判据）
#include "core/CoreDialerFactory.h" // 网关与 TUN **共用**的那一份出站工厂
#include "core/ProxyConfig.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QProcess>
#include <QSocketNotifier>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <QtGlobal>

#include <functional>
#include <memory>

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
// autoExit=false 时不退出进程，只累加 hits()/lastUser()——组合自测要在同一个进程里反复计数，
// 「收到第一条 CONNECT 就 exit(0)」那套单次判定在那里用不了。
class FakeSocks : public QObject
{
public:
    FakeSocks(quint16 port, const QString &expectUser, QObject *parent = nullptr,
              bool autoExit = true)
        : QObject(parent), m_expectUser(expectUser), m_autoExit(autoExit)
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
            ++m_hits;
            m_lastUser = c->user;
            std::fprintf(stderr, "SELFTEST: 收到 CONNECT，用户名='%s'（期望='%s'，累计 %d 次）\n",
                         c->user.toLatin1().constData(), m_expectUser.toLatin1().constData(),
                         m_hits);
            if (!m_autoExit)
                return; // 组合自测自己按 hits() 差值判定，不在这里下结论
            if (m_expectUser.isEmpty() || c->user == m_expectUser) {
                std::fprintf(stderr, "SELFTEST: PASS —— 整条链路(帧→lwIP握手→catch-all→SOCKS+身份)通\n");
                QTimer::singleShot(500, qApp, [] { QCoreApplication::exit(0); }); // 留时间冲刷回程帧
            } else {
                std::fprintf(stderr, "SELFTEST: FAIL —— 用户名不匹配\n");
                QTimer::singleShot(200, qApp, [] { QCoreApplication::exit(2); });
            }
        }
    }

public:
    int hits() const { return m_hits; }          // 收到过几条完整 CONNECT（组合自测按差值判定）
    QString lastUser() const { return m_lastUser; } // 最后一条 CONNECT 的用户名（验每设备身份）

private:
    QTcpServer m_server;
    QString m_expectUser;
    bool m_autoExit = true;
    int m_hits = 0;
    QString m_lastUser;
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

// ———————————— 组合自测：网关 + 进程内 TUN 同时跑（说明见头文件）————————————
#if defined(Q_OS_LINUX)
namespace {

// 组合自测里「网关那一半」的持有者。**刻意放在一条独立工作线程上**，与正式 App 的 GatewayWorker
// 同形（见 LanGateway_linux.cpp 的线程模型）：进程内 TUN 借栈时那一整段跨线程 marshal 的代码，
// 只有栈真的不在主线程上才会被执行到 —— 全塞主线程跑等于把要测的东西绕过去了。
class ComboGwWorker : public QObject
{
public:
    ComboGwWorker(quint16 socksPort, ProxyConfigStore *store, const QString &serverIp)
        : m_socksPort(socksPort), m_store(store), m_serverIp(serverIp)
    {
    }
    ~ComboGwWorker() override { teardown(); }

    // 建栈（幂等）。等价于 GatewayWorker::ensureStackLocal —— 进程内 TUN 借的正是它。
    NetStack *ensureStack(QString *err)
    {
        if (m_net)
            return m_net;
        auto *net = new NetStack(m_socksPort, this);
        if (!net->init(err)) {
            delete net;
            return nullptr;
        }
        m_net = net;
        // 出站工厂**只有一份**，网关和 TUN 共用（正式 App 里由 applyCoastCoreLocal 装）。
        // 这里让它给两条路各自一条**独立可判**的去向，免得一条路冒充另一条：
        //   · 目的地 = 网关靶 IP → 返回空 → 回退 Socks5 → 打到假 SOCKS（网关那条路的唯一判据）
        //   · 其余（TUN 的 curl 目标）→ DIRECT，真出网（TUN 那条路的唯一判据）
        auto *f = new CoreDialerFactory(m_store, new Socks5OutboundFactory(m_socksPort));
        const QString serverIp = m_serverIp;
        f->setRouter([serverIp](const QString &dst, const QString &) -> QString {
            return dst == serverIp ? QString() : QStringLiteral("DIRECT");
        });
        m_net->setOutboundFactory(f);
        return m_net;
    }

    bool addGatewayNic(const QString &tap, const QByteArray &localMac, const QString &selfIp,
                       const QString &mask, const QString &victimIp, const QByteArray &victimMac,
                       const QString &user, QString *err)
    {
        if (!ensureStack(err))
            return false;
        auto *ep = new TapEndpoint(localMac, this);
        if (!ep->openTap(tap, err)) {
            delete ep;
            return false;
        }
        if (!m_net->addNic(ep, localMac, selfIp, mask, err)) {
            delete ep;
            return false;
        }
        // 同线程直连（sender/context 都在本工作线程）——零拷贝帧的硬约束，见 IL2Endpoint.h。
        QObject::connect(ep, &IL2Endpoint::frameReceived, m_net,
                         [this, ep](const QByteArray &f) { m_net->inputFrame(ep, f); });
        m_net->addDevice(victimIp, victimMac, user);
        m_ep = ep;
        return true;
    }

    void teardown()
    {
        if (m_ep && m_net) {
            QObject::disconnect(m_ep, nullptr, m_net, nullptr);
            m_net->removeNic(m_ep);
        }
        delete m_ep;
        m_ep = nullptr;
        delete m_net; // 借栈的 TUN 会在 aboutToDestroy 里就地摘干净自己
        m_net = nullptr;
    }

private:
    NetStack *m_net = nullptr;
    TapEndpoint *m_ep = nullptr;
    quint16 m_socksPort = 0;
    ProxyConfigStore *m_store = nullptr;
    QString m_serverIp;
};

// 事件循环全程在转的 curl（★ 绝不能用 waitForFinished：那不泵主事件循环，
// TUN fd 的通知器与 lwIP 的泵会被掐死，看起来像"不通"，其实是测试驱动自己弄死的）。
QString comboHttpCode(const QString &url, int timeoutSec)
{
    QProcess p;
    QEventLoop loop;
    QObject::connect(&p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop,
                     &QEventLoop::quit);
    p.start(QStringLiteral("curl"),
            {QStringLiteral("-s"), QStringLiteral("-o"), QStringLiteral("/dev/null"),
             QStringLiteral("-w"), QStringLiteral("%{http_code}"), QStringLiteral("--max-time"),
             QString::number(timeoutSec), url});
    QTimer::singleShot(timeoutSec * 1000 + 3000, &loop, &QEventLoop::quit);
    loop.exec();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

} // namespace
#endif // Q_OS_LINUX

int runComboSelfTest()
{
#if !defined(Q_OS_LINUX)
    std::fprintf(stderr, "COMBO: 仅 Linux 支持（需要 TAP + TUN + root）\n");
    return 3;
#else
    const int mode = qEnvironmentVariable("COAST_COMBO_SELFTEST").toInt(); // 1=网关先，2=TUN 先
    const QString tap = QString::fromLatin1(envOr("COAST_SELFTEST_TAP", "cst0"));
    const QByteArray localMac =
            parseMac(QString::fromLatin1(envOr("COAST_SELFTEST_LOCALMAC", "02:00:00:00:00:01")));
    const QString victimIp = QString::fromLatin1(envOr("COAST_SELFTEST_VICTIM_IP", "10.9.9.1"));
    const QString victimMacStr = QString::fromLatin1(envOr("COAST_SELFTEST_VICTIM_MAC", ""));
    const QByteArray victimMac = parseMac(victimMacStr);
    const quint16 socksPort =
            QString::fromLatin1(envOr("COAST_SELFTEST_SOCKSPORT", "7899")).toUShort();
    const QString selfIp = QString::fromLatin1(envOr("COAST_SELFTEST_LOCAL_IP", "10.9.9.254"));
    const QString selfMask = QString::fromLatin1(envOr("COAST_SELFTEST_NETMASK", "255.255.255.0"));
    const QString serverIp = QString::fromLatin1(envOr("COAST_SELFTEST_SERVER_IP", "192.0.2.10"));
    const QString tunTarget =
            QString::fromLatin1(envOr("COAST_TUNSERVICE_TARGET", "http://223.5.5.5/"));
    const QString gwUrl = QStringLiteral("http://%1/").arg(serverIp);
    if (localMac.isEmpty() || victimMac.isEmpty()) {
        std::fprintf(stderr, "COMBO: 环境缺 LOCALMAC/VICTIM_MAC\n");
        return 3;
    }
    const QString expectUser = DeviceStore::socksUser(victimMacStr);
    std::fprintf(stderr, "=== 组合自测：%s ｜ tap=%s 网关靶=%s TUN 靶=%s ===\n",
                 mode == 2 ? "先 TUN 后网关（反向顺序）" : "先网关后 TUN（复现线上顺序）",
                 qUtf8Printable(tap), qUtf8Printable(gwUrl), qUtf8Printable(tunTarget));

    auto *socks = new FakeSocks(socksPort, expectUser, qApp, /*autoExit=*/false);

    // 出站配置：只有内建 DIRECT。分流由 ComboGwWorker 里那个 router 决定（两条路各自可判）。
    auto store = std::make_shared<ProxyConfigStore>();
    {
        QVector<ProxyNode> nodes;
        nodes.push_back(ProxyNode::direct());
        store->reload(std::make_shared<const ProxyConfig>(nodes, QStringLiteral("DIRECT"),
                                                          ProxyConfig::Mode::Direct));
    }

    QThread gwThread;
    gwThread.setObjectName(QStringLiteral("ComboGwWorker"));
    ComboGwWorker worker(socksPort, store.get(), serverIp);
    worker.moveToThread(&gwThread);
    gwThread.start();
    auto onGw = [&worker](const std::function<void()> &fn) {
        QMetaObject::invokeMethod(&worker, fn, Qt::BlockingQueuedConnection);
    };
    // 无论从哪条分支返回，都要把工作线程上的东西在工作线程上拆掉再退出。
    auto shutdown = [&] {
        onGw([&] { worker.teardown(); });
        gwThread.quit();
        gwThread.wait();
    };

    QString err;
    bool ok = false;
    if (mode != 2) {
        onGw([&] {
            ok = worker.addGatewayNic(tap, localMac, selfIp, selfMask, victimIp, victimMac,
                                      expectUser, &err);
        });
        if (!ok) {
            std::fprintf(stderr, "COMBO: 网关侧起不来：%s\n", qUtf8Printable(err));
            shutdown();
            return 3;
        }
        std::fprintf(stderr, "COMBO: 网关侧就绪（协议栈在独立工作线程上）\n");
    }

    // ——— ① 前提：两条路各自本来就通。少了这步，「失败」可能只是环境本来就没网 ———
    const QString tunBefore = comboHttpCode(tunTarget, 8);
    std::fprintf(stderr, "COMBO: [前提] 本机直连 %s → curl=%s\n", qUtf8Printable(tunTarget),
                 qUtf8Printable(tunBefore));
    if (tunBefore.isEmpty() || tunBefore == QStringLiteral("000")) {
        std::fprintf(stderr, "COMBO: ✗ 前提不成立：本机本来就连不通目标\n");
        shutdown();
        return 2;
    }
    if (mode != 2) {
        const int h0 = socks->hits();
        const QString c = comboHttpCode(gwUrl, 8);
        std::fprintf(stderr, "COMBO: [前提] 网关路 curl=%s，假 SOCKS CONNECT +%d\n",
                     qUtf8Printable(c), socks->hits() - h0);
        if (socks->hits() <= h0) {
            std::fprintf(stderr, "COMBO: ✗ 前提不成立：网关路本来就不通（TAP/路由/邻居没配好？）\n");
            shutdown();
            return 2;
        }
    }

    // ——— ② 开 TUN。修复前这一步必然失败：「已有一个网关协议栈实例在运行」———
    LocalTunService svc;
    QObject::connect(&svc, &LocalTunService::logged,
                     [](const QString &l) { std::fprintf(stderr, "  | %s\n", qUtf8Printable(l)); });
    svc.setStackProvider([&](QString *e) -> NetStack * {
        NetStack *n = nullptr;
        QMetaObject::invokeMethod(&worker, [&] { n = worker.ensureStack(e); },
                                  Qt::BlockingQueuedConnection);
        return n;
    });
    if (!svc.start(store, nullptr, /*socksFallbackPort=*/0, &err)) {
        std::fprintf(stderr,
                     "COMBO: ✗ 开 TUN 失败：%s\n"
                     "       （若是「已有一个网关协议栈实例在运行」，就是本次要修的那条故障）\n",
                     qUtf8Printable(err));
        shutdown();
        return 1;
    }
    std::fprintf(stderr, "COMBO: TUN 已开（网卡 %s）\n", qUtf8Printable(svc.ifname()));

    if (mode == 2) {
        // 反向顺序：TUN 先占了栈，网关此刻才来挂自己的网卡。
        onGw([&] {
            ok = worker.addGatewayNic(tap, localMac, selfIp, selfMask, victimIp, victimMac,
                                      expectUser, &err);
        });
        if (!ok) {
            std::fprintf(stderr, "COMBO: ✗ TUN 开着时网关挂不上网卡：%s\n", qUtf8Printable(err));
            svc.stop();
            shutdown();
            return 1;
        }
        std::fprintf(stderr, "COMBO: 网关网卡已挂到 TUN 建起的那份栈上\n");
    }

    // ——— ③ 组合期间**两条路同时通**（本次修复的验收标准）———
    // 三个互相独立的判据，谁也替不了谁：
    //   · 网关路：假 SOCKS 的 CONNECT 计数 **且用户名必须是被劫持设备的 dev-<mac>**。
    //     ★ 用户名这一条不是锦上添花 —— 第一次真机跑就是靠它抓出「本机 curl 被 TUN 的
    //       pref 200 规则拐走了、根本没经过 TAP」（CONNECT 照样有，用户名却是 'local'）。
    //       只看「有没有 CONNECT」会误判成通过。脚手架侧的修法见 validate/combo_selftest.sh。
    //   · TUN 路：经真目标绕回来的真实 HTTP 返回码，必须与起服务前一致。
    //   · 控制面：InprocTelemetry 的连接增量。**分两段量**（各自 curl 前后各抄一次），这样每条
    //     增量都能归到具体那条路上；合在一起量只能得到一个总数，少了一条根本不知道少的是谁。
    //     ★ 只对 TUN 那条路断言 >0。网关那条路在本自测里被 router 判成「回退」（这才打得到假
    //       SOCKS），而 CoreDialerFactory **有意不登记回退连接**（那些由 mihomo 的 /connections
    //       报，登了就是重复计数，见它 registerConn 处的注释）—— 所以 teleGw==0 是正确行为。
    //       第一版在这里断言了 teleGw>0，等于要求代码做它明确声明不做的事，是断言写错了。
    auto &tele = InprocTelemetry::instance();
    const int hitsBefore = socks->hits();
    const quint64 tele0 = tele.totalConns();
    const QString gwDuring = comboHttpCode(gwUrl, 10);
    const int gwDelta = socks->hits() - hitsBefore;
    const quint64 teleAfterGw = tele.totalConns();
    const QString tunDuring = comboHttpCode(tunTarget, 12);
    const quint64 teleAfterTun = tele.totalConns();
    const quint64 teleGw = teleAfterGw - tele0;   // 网关那条路登记了几条
    const quint64 teleTun = teleAfterTun - teleAfterGw; // TUN 那条路登记了几条
    const bool gwOk = gwDelta > 0 && socks->lastUser() == expectUser;
    // ★ TUN 路的判定必须是「返回码对得上 **且** 控制面真的记到一条进程内连接」——
    //   只看返回码是可以被蒙混过去的：万一流量压根没进 TUN（走了物理口直出），返回码照样正确。
    //   teleTun 是「这条 curl 确实被我们的用户态栈终结过」的独立物证。
    const bool tunOk = (tunDuring == tunBefore) && teleTun > 0;
    std::fprintf(stderr,
                 "COMBO: [组合] 网关路 curl=%s CONNECT+%d user='%s'(期望'%s') 控制面+%llu(回退连接"
                 "不登记，0 属正常) %s\n"
                 "COMBO: [组合] TUN 路  curl=%s（起服务前 %s） 控制面+%llu %s\n",
                 qUtf8Printable(gwDuring), gwDelta, qUtf8Printable(socks->lastUser()),
                 qUtf8Printable(expectUser), static_cast<unsigned long long>(teleGw),
                 gwOk ? "✓" : "✗", qUtf8Printable(tunDuring), qUtf8Printable(tunBefore),
                 static_cast<unsigned long long>(teleTun), tunOk ? "✓" : "✗");

    // ——— ④ 关掉 TUN：网关那条路必须毫发无损，本机网络必须恢复 ———
    const int hitsBeforeStop = socks->hits();
    svc.stop();
    const QString gwAfter = comboHttpCode(gwUrl, 8);
    const int gwDeltaAfter = socks->hits() - hitsBeforeStop;
    const QString tunAfter = comboHttpCode(tunTarget, 8);
    const bool gwStillOk = gwDeltaAfter > 0;
    const bool restored = (tunAfter == tunBefore);
    std::fprintf(stderr,
                 "COMBO: [关 TUN 后] 网关路 curl=%s CONNECT+%d %s ｜ 本机 curl=%s %s\n",
                 qUtf8Printable(gwAfter), gwDeltaAfter, gwStillOk ? "✓ 网关不受影响" : "✗ 被拖累了",
                 qUtf8Printable(tunAfter), restored ? "✓ 路由已还原" : "✗ 没还原干净");

    shutdown();
    const bool pass = gwOk && tunOk && gwStillOk && restored;
    std::fprintf(stderr, "=== 组合自测 %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
#endif
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
