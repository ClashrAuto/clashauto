// 「本机 TUN 入口」的端到端验证台：TUN → NetStack(lwIP) → 进程内出站 → 真目标。
//
// 它回答的是接「增强」按钮之前必须先答的那个问题：**TunEndpoint 伪装成以太端点这条路，
// 到底能不能把一条真实 TCP 连接从 TUN 一路送到进程内出站再回来。** 设备层单测（tundrv）
// 只证明了「包收得到、ethertype 对」，那不等于整条链路成立。
//
// ── 环路问题怎么被隔离掉 ──────────────────────────────────────────────
// TUN 一旦接管默认路由，coast 自己的出站也会被路由进 TUN → 死循环。生产上要靠
// SO_MARK+策略路由(Linux) / IP_UNICAST_IF(Windows) / IP_BOUND_IF(macOS)，那是另一件事。
// 这里用一个干净的办法把它整个绕开：**把 coast0 移进独立 netns `app`，"应用"在里面跑**。
//   · tun fd 留在本进程（主 netns），移网卡不影响它 —— 帧照样进我们的 fd；
//   · coast 的出站在主 netns，完全不经 coast0 —— 天然无环路。
// 于是能先把架构验通，再单独去解生产环境的环路。
//
// ── 严格模式 = 「真的没经过 mihomo」的凭据 ────────────────────────────
// 出站工厂只装内建 DIRECT，fallback 传 nullptr 且 setStrict(true)：任何一次「判不了 / 想回退
// 核心」都会让该连接**当场失败**而不是静默改道。所以 curl 拿到 200 + cc=N/0/0/0/0/0
// 就等于「这 N 条连接全部走进程内」。
//
// ── 靶机 ──────────────────────────────────────────────────────────────
//   ip netns add srv; ip link add veth-h type veth peer name veth-s
//   ip link set veth-s netns srv; ip addr add 10.99.0.1/24 dev veth-h; ip link set veth-h up
//   ip netns exec srv ip link set lo up
//   ip netns exec srv ip addr add 10.99.0.2/24 dev veth-s; ip netns exec srv ip link set veth-s up
//   ip netns exec srv ip route add default via 10.99.0.1
//   ip netns exec srv ./httpsrv 8000 4          # 同目录的 httpsrv.c
// 放进 netns 而不是本机地址，是因为**本机地址会被内核本地直投短路**，那样测的就不是这条路。
//
// ── 编译（无 CMake 目标；拿主构建的 .o 拼即可）─────────────────────────
//   cd <repo>/clashauto-c++ && D=build/CMakeFiles/coast.dir
//   OBJS=$(find $D/src/net $D/third_party -name "*.o" | grep -v LanGateway | tr "\n" " ")
//   OBJS="$OBJS $D/src/DeviceStore.cpp.o $D/src/Sqlite.cpp.o"
//   g++ -std=c++17 -fPIC -o /tmp/tunstack tools/gwbench/tunstack.cpp <手工 moc 出来的 moc_*.cpp> \
//     $OBJS $(pkg-config --cflags --libs Qt6Core Qt6Network Qt6Sql) -lssl -lcrypto \
//     -Isrc/net -Isrc -Ithird_party/lwip/src/include -Isrc/net/lwip_port -DCOAST_HAVE_OPENSSL
//   （LanGateway 的 .o 要排掉：它 Linux-only 且把整套 ARP 投毒拖进来，本测试用不上。）
//
// ── 跑 ────────────────────────────────────────────────────────────────
//   ./tunstack [目标IP] [端口]            # 默认 10.99.0.2 8000
//   COAST_TUNTEST_N=10 ./tunstack         # 连打 10 次
//   COAST_GATEWAY_DEBUG=1 ./tunstack      # 每帧的 NETSTACK IN/OUT/ACCEPT
//   COAST_TUNTEST_BLOCK=1 ./tunstack      # 见下方 blockMode：**故意**复现一个假故障
#include "GatewayDiag.h"
#include "IL2Endpoint.h"
#include "NetStack.h"
#include "TunEndpoint.h"
#include "core/CoreDialerFactory.h"
#include "core/CoreRouter.h"
#include "core/ProxyConfig.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTimer>

#include <cstdio>
#include <memory>

static int run(const QString &c, const QStringList &a, bool quiet = false)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(c, a);
    p.waitForFinished(8000);
    const QString out = QString::fromUtf8(p.readAll()).trimmed();
    if (!quiet) {
        std::fprintf(stderr, "  $ %s %s → rc=%d%s%s\n", qUtf8Printable(c),
                     qUtf8Printable(a.join(QLatin1Char(' '))), p.exitCode(),
                     out.isEmpty() ? "" : " | ", qUtf8Printable(out));
    }
    return p.exitCode();
}

static QString runOut(const QString &c, const QStringList &a)
{
    QProcess p;
    p.start(c, a);
    p.waitForFinished(8000);
    return QString::fromUtf8(p.readAllStandardOutput());
}

// 计数用的透传端点：NetStack 只看见它，send() 转给真端点并计数。
// 不改一行生产代码就能回答「回程帧到底有没有被写回 TUN」——这条在排查时是关键分界线。
class CountingEndpoint final : public IL2Endpoint
{
public:
    explicit CountingEndpoint(IL2Endpoint *inner, QObject *parent = nullptr)
        : IL2Endpoint(parent), m_inner(inner)
    {
        connect(m_inner, &IL2Endpoint::frameReceived, this, [this](const QByteArray &f) {
            ++rx;
            rxBytes += f.size();
            if (rx <= 6) {
                const auto *u = reinterpret_cast<const unsigned char *>(f.constData());
                std::fprintf(stderr,
                             "  [rx#%lld] %d B eth=%02x%02x proto=%d %d.%d.%d.%d -> %d.%d.%d.%d\n",
                             rx, int(f.size()), u[12], u[13], u[14 + 9], u[14 + 12], u[14 + 13],
                             u[14 + 14], u[14 + 15], u[14 + 16], u[14 + 17], u[14 + 18], u[14 + 19]);
            }
            emit frameReceived(f);
        });
    }

    bool open(const QString &ifname, QString *err) override { return m_inner->open(ifname, err); }
    void close() override { m_inner->close(); }
    bool isOpen() const override { return m_inner->isOpen(); }
    bool send(const QByteArray &frame) override
    {
        const bool ok = m_inner->send(frame);
        ++tx;
        if (!ok)
            ++txFail;
        if (tx <= 6) {
            const auto *u = reinterpret_cast<const unsigned char *>(frame.constData());
            std::fprintf(stderr, "  [tx#%lld] %d B ok=%d proto=%d %d.%d.%d.%d -> %d.%d.%d.%d\n", tx,
                         int(frame.size()), int(ok), u[14 + 9], u[14 + 12], u[14 + 13], u[14 + 14],
                         u[14 + 15], u[14 + 16], u[14 + 17], u[14 + 18], u[14 + 19]);
        }
        return ok;
    }
    QByteArray localMac() const override { return m_inner->localMac(); }
    int ifIndex() const override { return m_inner->ifIndex(); }
    int mtu() const override { return m_inner->mtu(); }

    qint64 rx = 0, rxBytes = 0, tx = 0, txFail = 0;

private:
    IL2Endpoint *m_inner;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString kTunIp = QStringLiteral("10.77.0.1"); // 我们（栈侧）在 TUN 上的地址
    const QString kAppIp = QStringLiteral("10.77.0.2"); // netns 里"应用"的地址
    const QString kTarget = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("10.99.0.2");
    const QString kPort = argc > 2 ? QString::fromLatin1(argv[2]) : QStringLiteral("8000");

    // netns 从零来一遍，别继承上一轮的残留。每条 ip 命令都查退出码——上一版驱动全靠猜，
    // 一条命令悄悄失败就会把「链路不通」和「测试台没搭起来」混为一谈。
    run("ip", {"netns", "delete", "app"}, true);
    run("ip", {"link", "del", "coast0"}, true);
    if (run("ip", {"netns", "add", "app"}) != 0) {
        std::fprintf(stderr, "建 netns app 失败\n");
        return 2;
    }

    IL2Endpoint *real = createTunEndpoint(nullptr);
    QString err;
    if (!real->open(QStringLiteral("coast0"), &err)) {
        std::fprintf(stderr, "TUN open 失败: %s\n", qUtf8Printable(err));
        return 2;
    }
    auto *ep = new CountingEndpoint(real);
    real->setParent(ep);

    // 把网卡挪进 netns app —— fd 留在本进程，包照样进我们的 fd。
    if (run("ip", {"link", "set", "coast0", "netns", "app"}) != 0) {
        std::fprintf(stderr, "!! coast0 移进 netns 失败 —— 后面 curl 必然 000\n");
        return 2;
    }
    run("ip", {"netns", "exec", "app", "ip", "addr", "add", kAppIp + "/24", "dev", "coast0"});
    run("ip", {"netns", "exec", "app", "ip", "link", "set", "coast0", "up"});
    run("ip", {"netns", "exec", "app", "ip", "link", "set", "lo", "up"});
    // TUN 是 NOARP 点对点设备，默认路由不需要 via。
    run("ip", {"netns", "exec", "app", "ip", "route", "add", "default", "dev", "coast0"});
    std::fprintf(stderr, "--- netns app 路由 ---\n%s",
                 qUtf8Printable(runOut("ip", {"netns", "exec", "app", "ip", "route", "show"})));

    NetStack net(0); // socksPort=0：本测试不用 SOCKS 回退（strict 下也用不到）
    if (!net.init(&err)) {
        std::fprintf(stderr, "NetStack init 失败: %s\n", qUtf8Printable(err));
        return 2;
    }
    if (!net.addNic(ep, ep->localMac(), kTunIp, QStringLiteral("255.255.255.0"), &err)) {
        std::fprintf(stderr, "addNic 失败: %s\n", qUtf8Printable(err));
        return 2;
    }
    net.addDevice(kAppIp, coastcore::tunPeerMac(), QStringLiteral("local"));

    // 出站：只装内建 DIRECT + 严格模式（任何回退核心都会当场失败 ⇒ PASS 即证明走的是进程内）
    auto store = std::make_shared<ProxyConfigStore>();
    QVector<ProxyNode> nodes;
    nodes.push_back(ProxyNode::direct());
    store->reload(std::make_shared<const ProxyConfig>(nodes, QStringLiteral("DIRECT"),
                                                      ProxyConfig::Mode::Direct));
    auto *factory = new CoreDialerFactory(store.get(), nullptr);
    factory->setStrict(true);
    factory->setRouter(coastcore::makeRouter(store, nullptr, true));
    net.setOutboundFactory(factory);

    QObject::connect(ep, &IL2Endpoint::frameReceived, &net,
                     [&net, ep](const QByteArray &f) { net.inputFrame(ep, f); });

    std::fprintf(stderr, "=== TUN=%s app=%s 目标=%s:%s，netns app 里发起请求 ===\n",
                 qUtf8Printable(kTunIp), qUtf8Printable(kAppIp), qUtf8Printable(kTarget),
                 qUtf8Printable(kPort));

    const bool blockMode = qEnvironmentVariableIsSet("COAST_TUNTEST_BLOCK");
    const int reqs = qEnvironmentVariableIntValue("COAST_TUNTEST_N") > 0
                         ? qEnvironmentVariableIntValue("COAST_TUNTEST_N")
                         : 1;

    QTimer::singleShot(500, [&] {
        QStringList codes;
        int okCount = 0;
        QString cerr;
        for (int i = 0; i < reqs; ++i) {
            QProcess p;
            p.start("ip", {"netns", "exec", "app", "curl", "-sS", "-o", "/dev/null", "-w",
                           "%{http_code}", "--max-time", "8",
                           "http://" + kTarget + ":" + kPort + "/"});
            if (blockMode) {
                // ★★ 这条分支是**故意留着的反面教材**，别删。
                //   本驱动第一版就是这么写的，结果 curl 恒 000、看起来像「TUN 这条路根本不通」。
                //   真因与 TUN 无关：QProcess::waitForFinished 只泵它自己那个子进程的 IO，
                //   **不跑主事件循环**；而 TUN fd 的 QSocketNotifier 和 lwIP 的 25ms 定时器泵
                //   全挂在主线程事件循环上 —— curl 跑的那 8 秒里，一帧都没被读走。
                //   实测对照：阻塞版 ep.rx=1 ep.tx=0 tcpAcc=0 → 000；非阻塞版 10/10 全 200。
                p.waitForFinished(12000);
            } else {
                while (p.state() != QProcess::NotRunning) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    p.waitForFinished(20);
                }
            }
            const QString c1 = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
            codes << c1;
            if (c1 == QStringLiteral("200"))
                ++okCount;
            const QString e1 = QString::fromUtf8(p.readAllStandardError()).trimmed();
            if (!e1.isEmpty())
                cerr = e1;
        }
        const QString code = codes.join(QLatin1Char(','));
        const bool ok = (okCount == reqs);
        std::fprintf(stderr, "\n--- coast0 (netns app) 计数 ---\n%s",
                     qUtf8Printable(runOut("ip", {"netns", "exec", "app", "ip", "-s", "link", "show",
                                                  "coast0"})));
        const auto &c = GatewayDiag::c;
        std::fprintf(stderr,
                     "\n统计: ep.rx=%lld(%lld B) ep.tx=%lld(fail=%lld)  tcpAcc=%lld close=%lld "
                     "abort=%lld socksFail=%lld\n"
                     "  cc=%lld/%lld/%lld/%lld/%lld/%lld "
                     "（进程内/无路由/节点缺/协议缺/UDP不支持/严格拒绝）\n",
                     ep->rx, ep->rxBytes, ep->tx, ep->txFail, c.tcpAccepted, c.tcpClosed,
                     c.tcpAborted, c.socksFailed, c.ccInProcess, c.fbNoRoute, c.fbNodeMissing,
                     c.fbProtoMissing, c.fbUdpUnsupported, c.ccStrictRefused);
        std::fprintf(stderr, "=== netns 里 curl(%d 次%s) 返回 \"%s\"%s%s → %s ===\n", reqs,
                     blockMode ? ", 阻塞事件循环模式" : "", qUtf8Printable(code),
                     cerr.isEmpty() ? "" : "  stderr=", qUtf8Printable(cerr),
                     ok ? "PASS（TUN→NetStack→进程内出站 整条通）" : "FAIL");
        run("ip", {"netns", "delete", "app"}, true);
        qApp->exit(ok ? 0 : 1);
    });
    return app.exec();
}
