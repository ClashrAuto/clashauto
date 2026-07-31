// 本机入站 → 进程内引擎 → 出站 的端到端验证台：**全程不经 mihomo**。
//
// MixedInbound 自带的 selfTest() 用的是「直连出站桩」，只验协议解析与转发；
// 这里换成**真正的生产路径**：MixedInbound → CoreDialerFactory（真拨号工厂，读 ProxyConfig
// 快照做分流）→ DirectOutbound（真进程内直连）。
//
// ★ 关键设置：`fallback = nullptr` + `setStrict(true)`。
//   CoreDialerFactory 平时判不了就静默回退 mihomo，那会把「还差多少」藏起来 —— 这里把回退这条路
//   彻底堵死，任何一次想回退核心的企图都会当场变成连接失败。所以只要本程序 PASS，
//   就证明这条路上**一个字节都没有经过 mihomo**。
//
// 用法: ./ibh            （自带靶服务器，不需要任何节点/订阅/外网）
#include "core/CoreDialerFactory.h"
#include "core/CoreRouter.h"
#include "IOutbound.h"
#include "core/ProxyConfig.h"
#include "core/ProxyConfigBuilder.h"
#include "inbound/MixedInbound.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>
#include <QElapsedTimer>
#include <QTimer>
#include <algorithm>

#include <cstdio>
#include <memory>

namespace {

// 确定性载荷 + 校验和：大流量用例要的是**逐字节没错没漏**，光看长度对不上不了帐
//（背压写错最典型的后果就是「丢了一段」或「重复了一段」，长度可能仍然凑巧对）。
constexpr int kBigSize = 8 * 1024 * 1024;

QByteArray makePayload(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    for (int i = 0; i < n; ++i)
        b[i] = char((i * 31 + 7) & 0xFF);
    return b;
}

quint32 checksum(const QByteArray &b)
{
    quint32 h = 2166136261u; // FNV-1a
    for (char c : b) {
        h ^= quint8(c);
        h *= 16777619u;
    }
    return h;
}

bool runCase(const char *name, quint16 port, const QByteArray &request,
             const QByteArray &expectContains, int timeoutMs = 5000)
{
    QTcpSocket c;
    QByteArray got;
    QEventLoop loop;
    QTimer to;
    to.setSingleShot(true);
    QObject::connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&c, &QTcpSocket::readyRead, &loop, [&] {
        got.append(c.readAll());
        if (got.contains(expectContains))
            loop.quit();
    });
    QObject::connect(&c, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    c.connectToHost(QHostAddress::LocalHost, port);
    if (!c.waitForConnected(2000)) {
        std::fprintf(stderr, "  [FAIL] %s: 连不上入站\n", name);
        return false;
    }
    c.write(request);
    to.start(timeoutMs);
    loop.exec();
    const bool ok = got.contains(expectContains);
    std::fprintf(stderr, "  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        std::fprintf(stderr, "        期望包含: %s\n        实际收到: %s\n",
                     expectContains.constData(), got.left(160).constData());
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::fprintf(stderr, "=== 本机入站 → CoastCore 进程内引擎 端到端（无 mihomo）===\n");

    // ---- 靶服务器：回显请求行，便于验证「绝对形式被改写成源形式」----
    QTcpServer target;
    if (!target.listen(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "靶服务器监听失败: %s\n", qPrintable(target.errorString()));
        return 2;
    }
    const quint16 tport = target.serverPort();
    QObject::connect(&target, &QTcpServer::newConnection, &target, [&target] {
        while (target.hasPendingConnections()) {
            QTcpSocket *sk = target.nextPendingConnection();
            // 每连接一份累积缓冲：上行大流量用例要把 8 MiB 收全了才能算校验和
            auto buf = std::make_shared<QByteArray>();
            auto want = std::make_shared<int>(-1); // >=0 表示正处于「收 N 字节」的上行模式
            QObject::connect(sk, &QTcpSocket::readyRead, sk, [sk, buf, want] {
                buf->append(sk->readAll());
                if (*want < 0) {
                    // 上行大流量协议：首行 "LEN <n>\n"，随后是 n 字节确定性载荷
                    if (buf->startsWith("LEN ")) {
                        const int nl = buf->indexOf('\n');
                        if (nl < 0)
                            return; // 首行还没收全
                        *want = buf->mid(4, nl - 4).trimmed().toInt();
                        buf->remove(0, nl + 1);
                        // ★ 故意当个「慢服务端」：先卡住 1.2 秒不读。
                        //   回环 + 快消费者的话出站一写就走，out->bytesToWrite() 永远顶不到高水位，
                        //   上行节流分支根本不会跑 —— 那样用例即使 PASS 也是空的（实测就是 0 次）。
                        sk->setReadBufferSize(1);
                        QTimer::singleShot(1200, sk, [sk] { sk->setReadBufferSize(0); });
                    } else {
                        // 普通 HTTP：回显请求行（用来验绝对形式被改写成了源形式）
                        const QByteArray req = *buf;
                        if (!req.contains("\r\n\r\n"))
                            return;
                        const QByteArray line = req.left(qMax(0, req.indexOf('\r')));
                        if (line.contains("/big")) { // 下行大流量用例
                            const QByteArray body = makePayload(kBigSize);
                            sk->write("HTTP/1.1 200 OK\r\nContent-Length: "
                                      + QByteArray::number(body.size())
                                      + "\r\nConnection: close\r\n\r\n");
                            sk->write(body);
                        } else {
                            sk->write("HTTP/1.1 200 OK\r\nContent-Length: "
                                      + QByteArray::number(line.size() + 8)
                                      + "\r\nConnection: close\r\n\r\nCOASTOK:" + line);
                        }
                        buf->clear();
                        return;
                    }
                }
                if (*want >= 0 && buf->size() >= *want) {
                    // 收齐 → 回校验和，客户端据此判断「逐字节没错没漏」
                    const QByteArray got = buf->left(*want);
                    sk->write("SUM:" + QByteArray::number(checksum(got))
                              + " LEN:" + QByteArray::number(got.size()) + "\n");
                    *want = -1;
                    buf->clear();
                }
            });
            QObject::connect(sk, &QTcpSocket::disconnected, sk, &QObject::deleteLater);
        }
    });

    // ---- 配置快照：只装内建 DIRECT，Direct 模式（本测试不需要任何真节点）----
    ProxyConfigStore store;
    QVector<ProxyNode> nodes;
    nodes.push_back(ProxyNode::direct());
    store.reload(std::make_shared<const ProxyConfig>(nodes, QStringLiteral("DIRECT"),
                                                     ProxyConfig::Mode::Direct));

    // ---- 真拨号工厂；堵死回退核心这条路 ----
    CoreDialerFactory factory(&store, /*fallback=*/nullptr);
    factory.setStrict(true);
    factory.setRouter([](const QString &, const QString &) { return QStringLiteral("DIRECT"); });

    MixedInbound inbound(&factory);
    inbound.setUser(QStringLiteral("local"));
    if (!inbound.listen(0)) {
        std::fprintf(stderr, "入站监听失败\n");
        return 2;
    }
    const quint16 iport = inbound.boundPort();
    QObject::connect(&inbound, &MixedInbound::connectionOpened, [](const QString &t) {
        std::fprintf(stderr, "        → 进程内出站已建立: %s\n", qPrintable(t));
    });
    std::fprintf(stderr, "  入站=127.0.0.1:%u  靶机=127.0.0.1:%u  (strict=on, fallback=none)\n",
                 unsigned(iport), unsigned(tport));

    // 先跑分流路由的判定自测（纯逻辑，不碰网络）。它钉住的是「什么时候必须回退核心」——
    // 这套判定松掉的后果是**误路由**（本该走节点的走了直连，或反过来），比崩溃难发现得多。
    bool ok = coastcore::routerSelfTest();
    // proxy-groups 解析向量（含 emoji 转义 / 嵌套 / 成环）——它是"核心不可用时还能不能分流"的基础。
    ok &= coastcore::proxyConfigSelfTest();

    {   // SOCKS5 CONNECT（域名形式）+ 紧随其后的早到数据
        const QByteArray host("127.0.0.1");
        QByteArray req;
        req.append("\x05\x01\x00", 3);
        req.append("\x05\x01\x00\x03", 4);
        req.append(char(host.size()));
        req.append(host);
        req.append(char((tport >> 8) & 0xFF));
        req.append(char(tport & 0xFF));
        req.append("GET /s5 HTTP/1.1\r\nHost: x\r\n\r\n");
        ok &= runCase("SOCKS5 CONNECT → DirectOutbound", iport, req, "COASTOK:GET /s5");
    }
    {   // HTTP CONNECT
        QByteArray req = "CONNECT 127.0.0.1:" + QByteArray::number(tport)
            + " HTTP/1.1\r\nHost: x\r\n\r\nGET /cx HTTP/1.1\r\nHost: x\r\n\r\n";
        ok &= runCase("HTTP CONNECT → DirectOutbound", iport, req, "COASTOK:GET /cx");
    }
    {   // HTTP 绝对形式 → 源形式改写
        QByteArray req = "GET http://127.0.0.1:" + QByteArray::number(tport)
            + "/abs?q=1 HTTP/1.1\r\nHost: 127.0.0.1\r\nProxy-Connection: keep-alive\r\n\r\n";
        ok &= runCase("HTTP 绝对形式 → 源形式 → DirectOutbound", iport, req, "COASTOK:GET /abs?q=1");
    }

    // ---- 大流量 · 上行：客户端 → 出站。压 pumpClientToOut 的节流路径 ----
    {
        const QByteArray payload = makePayload(kBigSize);
        const QByteArray expect =
            "SUM:" + QByteArray::number(checksum(payload)) + " LEN:" + QByteArray::number(kBigSize);
        QByteArray req = "CONNECT 127.0.0.1:" + QByteArray::number(tport) + " HTTP/1.1\r\n\r\n"
            + "LEN " + QByteArray::number(kBigSize) + "\n" + payload;
        ok &= runCase("上行 8 MiB（背压 + 逐字节校验）", iport, req, expect, 30000);
    }

    // ---- 大流量 · 下行：出站 → 客户端。**故意先卡住不读**，逼出 setReadPaused 那条路 ----
    {
        const QByteArray expectBody = makePayload(kBigSize);
        const quint32 wantSum = checksum(expectBody);
        QTcpSocket c;
        QByteArray got;
        QEventLoop loop;
        QTimer to;
        to.setSingleShot(true);
        QObject::connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
        c.connectToHost(QHostAddress::LocalHost, iport);
        bool connected = c.waitForConnected(2000);
        if (!connected) {
            std::fprintf(stderr, "  [FAIL] 下行 8 MiB: 连不上入站\n");
            ok = false;
        } else {
            // ★ 把客户端读缓冲卡到 1 字节：数据会堆在入站的 client->bytesToWrite() 上，
            //   越过高水位就该触发 out->setReadPaused(true)。1.5 秒后放开，验证它能**恢复**
            //   并把 8 MiB 一字不差地送完 —— 恢复漏了的话这里会超时或校验和不符。
            c.setReadBufferSize(1);
            QObject::connect(&c, &QTcpSocket::readyRead, &loop, [&] {
                got.append(c.readAll());
                const int hdr = got.indexOf("\r\n\r\n");
                if (hdr >= 0 && got.size() - (hdr + 4) >= kBigSize)
                    loop.quit();
            });
            QObject::connect(&c, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
            c.write("GET http://127.0.0.1:" + QByteArray::number(tport)
                    + "/big HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
            QTimer::singleShot(1500, &c, [&c] { c.setReadBufferSize(0); }); // 放开
            to.start(30000);
            loop.exec();
            const int hdr = got.indexOf("\r\n\r\n");
            const QByteArray body = hdr >= 0 ? got.mid(hdr + 4) : QByteArray();
            const bool good = body.size() == kBigSize && checksum(body) == wantSum;
            std::fprintf(stderr, "  [%s] 下行 8 MiB（先卡住 1.5s 逼出背压，再恢复）\n",
                         good ? "PASS" : "FAIL");
            if (!good)
                std::fprintf(stderr, "        收到 %d 字节（期望 %d），校验和 %s\n", body.size(),
                             kBigSize, checksum(body) == wantSum ? "对" : "不符");
            ok &= good;
        }
    }

    // ---- 延迟：进程内引擎给每条连接**加了多少**。直连靶机 vs 经入站，同一客户端同一靶机，
    //      只差「中间有没有 CoastCore」这一个变量。回答的是「把延迟降到最低」到底该从哪下手。
    {
        auto measure = [&](bool viaInbound, int n) -> QVector<double> {
            QVector<double> ms;
            const quint16 port = viaInbound ? iport : tport;
            for (int i = 0; i < n; ++i) {
                QElapsedTimer t;
                t.start();
                QTcpSocket c;
                QEventLoop loop;
                QTimer to;
                to.setSingleShot(true);
                QObject::connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
                QByteArray got;
                QObject::connect(&c, &QTcpSocket::readyRead, &loop, [&] {
                    got.append(c.readAll());
                    if (got.contains("COASTOK:"))
                        loop.quit();
                });
                QObject::connect(&c, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
                c.connectToHost(QHostAddress::LocalHost, port);
                if (!c.waitForConnected(2000))
                    continue;
                if (viaInbound)
                    c.write("GET http://127.0.0.1:" + QByteArray::number(tport)
                            + "/lat HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
                else
                    c.write("GET /lat HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
                to.start(3000);
                loop.exec();
                if (got.contains("COASTOK:"))
                    ms.push_back(t.nsecsElapsed() / 1e6);
            }
            std::sort(ms.begin(), ms.end());
            return ms;
        };
        auto pct = [](const QVector<double> &v, double p) {
            return v.isEmpty() ? 0.0 : v.at(qBound(0, int(v.size() * p), v.size() - 1));
        };
        const int N = 300;
        const QVector<double> dir = measure(false, N);
        const QVector<double> via = measure(true, N);
        std::fprintf(stderr,
                     "  [INFO] 每连接延迟（%d 次，connect→GET→拿到应答）\n"
                     "         直连靶机   : p50 %.3f ms  p90 %.3f ms  (n=%d)\n"
                     "         经进程内引擎: p50 %.3f ms  p90 %.3f ms  (n=%d)\n"
                     "         ⇒ 净增 p50 %.3f ms —— ★别把它全算作「引擎开销」：代理天然要**多建一条**\n"
                     "           到目标的 TCP 连接，本机直连一次就要 p50 %.3f ms，两次就接近这个净增值。\n"
                     "           要分离出引擎自身的开销，缺的对照是「同机 mihomo 走同一条路」，尚未做。\n",
                     N, pct(dir, 0.5), pct(dir, 0.9), dir.size(), pct(via, 0.5), pct(via, 0.9),
                     via.size(), pct(via, 0.5) - pct(dir, 0.5), pct(dir, 0.5));
    }

    // ---- 拨号器本身的开销：CoreDialerFactory→DirectOutbound 的 connectTo→established
    //      vs 裸 QTcpSocket 连同一个靶机。这条路上**没有 lwIP、没有入站解析**，量的就是
    //      「进程内拨号那一跳」自己。真机上 connMs 空载也大量落在 1~10ms 桶，可疑。
    {
        auto pct = [](QVector<double> v, double p) {
            std::sort(v.begin(), v.end());
            return v.isEmpty() ? 0.0 : v.at(qBound(0, int(v.size() * p), v.size() - 1));
        };
        const int N = 300;
        QVector<double> raw, viaDialer;

        for (int i = 0; i < N; ++i) { // 裸 socket 基线
            QTcpSocket sk;
            QEventLoop loop;
            QTimer to;
            to.setSingleShot(true);
            QObject::connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
            QObject::connect(&sk, &QTcpSocket::connected, &loop, &QEventLoop::quit);
            QElapsedTimer t;
            t.start();
            sk.connectToHost(QHostAddress::LocalHost, tport);
            to.start(3000);
            loop.exec();
            if (sk.state() == QAbstractSocket::ConnectedState)
                raw.push_back(t.nsecsElapsed() / 1e6);
        }
        for (int i = 0; i < N; ++i) { // 经进程内拨号工厂
            IOutboundTcp *out = factory.createTcp(nullptr);
            if (!out)
                break;
            QEventLoop loop;
            QTimer to;
            to.setSingleShot(true);
            QObject::connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
            QObject::connect(out, &IOutboundTcp::established, &loop, &QEventLoop::quit);
            QObject::connect(out, &IOutboundTcp::failed, &loop, &QEventLoop::quit);
            QElapsedTimer t;
            t.start();
            out->connectTo(QStringLiteral("127.0.0.1"), tport, QStringLiteral("local"));
            to.start(3000);
            loop.exec();
            if (out->isEstablished())
                viaDialer.push_back(t.nsecsElapsed() / 1e6);
            out->closeTunnel();
            delete out;
        }
        std::fprintf(stderr,
                     "  [INFO] 拨号一跳（connectTo→established，无 lwIP、无入站解析，%d 次）\n"
                     "         裸 QTcpSocket : p50 %.3f  p90 %.3f  p99 %.3f ms (n=%d)\n"
                     "         进程内拨号工厂: p50 %.3f  p90 %.3f  p99 %.3f ms (n=%d)\n"
                     "         ⇒ 拨号器自身开销 p50 %.3f ms\n",
                     N, pct(raw, 0.5), pct(raw, 0.9), pct(raw, 0.99), raw.size(),
                     pct(viaDialer, 0.5), pct(viaDialer, 0.9), pct(viaDialer, 0.99),
                     viaDialer.size(), pct(viaDialer, 0.5) - pct(raw, 0.5));
    }

    // ---- SOCKS5 UDP ASSOCIATE：真的把数据报送出去并收回来 ----
    //      系统代理改指本机入站后，macOS/Linux 的系统代理**含 SOCKS**，而 mihomo 支持 UDP。
    //      这条不通就等于切过去之后所有走 SOCKS 的 UDP 全坏 —— 必须端到端验，不能只看握手应答。
    {
        QUdpSocket echo; // UDP 回显靶机
        bool echoOk = echo.bind(QHostAddress::LocalHost, 0);
        QObject::connect(&echo, &QUdpSocket::readyRead, &echo, [&echo] {
            while (echo.hasPendingDatagrams()) {
                QByteArray d(int(echo.pendingDatagramSize()), Qt::Uninitialized);
                QHostAddress from;
                quint16 fp = 0;
                const qint64 n = echo.readDatagram(d.data(), d.size(), &from, &fp);
                if (n > 0)
                    echo.writeDatagram(QByteArray("ECHO:") + d.left(int(n)), from, fp);
            }
        });
        const quint16 eport = echo.localPort();

        // ★ 必须用事件循环，**不能**用 waitForReadyRead：入站服务端就在同一个线程里，
        //   阻塞式 waitFor* 期间事件循环不转，服务端压根没机会 accept/应答（第一版就栽在这，
        //   看起来像"UDP 没实现"，其实是测试自己把服务端饿死了）。
        QTcpSocket ctl;
        QByteArray ctlBuf;
        auto pump = [&](int wantBytes, int ms) {
            QEventLoop l;
            QTimer t;
            t.setSingleShot(true);
            QObject::connect(&t, &QTimer::timeout, &l, &QEventLoop::quit);
            auto c = QObject::connect(&ctl, &QTcpSocket::readyRead, &l, [&] {
                ctlBuf.append(ctl.readAll());
                if (ctlBuf.size() >= wantBytes)
                    l.quit();
            });
            if (ctlBuf.size() < wantBytes) {
                t.start(ms);
                l.exec();
            }
            QObject::disconnect(c);
            return ctlBuf.size() >= wantBytes;
        };
        ctl.connectToHost(QHostAddress::LocalHost, iport);
        bool good = echoOk && ctl.waitForConnected(2000);
        if (good) {
            ctl.write(QByteArray::fromRawData("\x05\x01\x00", 3)); // greeting
            good = pump(2, 2000) && ctlBuf.startsWith(QByteArray("\x05\x00", 2));
            ctlBuf.remove(0, 2);
        }
        quint16 relay = 0;
        if (good) { // UDP ASSOCIATE，DST 填 0.0.0.0:0（客户端惯例）
            ctl.write(QByteArray::fromRawData("\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00", 10));
            good = pump(10, 2000);
            if (good) {
                good = ctlBuf.at(1) == '\0';
                if (good)
                    relay = qFromBigEndian<quint16>(
                        reinterpret_cast<const uchar *>(ctlBuf.constData() + 8));
            }
        }
        QByteArray got;
        if (good && relay) {
            QUdpSocket cli;
            cli.bind(QHostAddress::LocalHost, 0);
            QByteArray dg;
            dg.append('\0'); dg.append('\0'); dg.append('\0'); // RSV RSV FRAG
            dg.append('\x01');
            const quint32 v4 = qToBigEndian<quint32>(QHostAddress(QStringLiteral("127.0.0.1")).toIPv4Address());
            dg.append(reinterpret_cast<const char *>(&v4), 4);
            const quint16 be = qToBigEndian<quint16>(eport);
            dg.append(reinterpret_cast<const char *>(&be), 2);
            dg.append("PING-UDP");
            cli.writeDatagram(dg, QHostAddress::LocalHost, relay);
            QEventLoop l;
            QTimer t;
            t.setSingleShot(true);
            QObject::connect(&t, &QTimer::timeout, &l, &QEventLoop::quit);
            QObject::connect(&cli, &QUdpSocket::readyRead, &l, [&] {
                QByteArray d(int(cli.pendingDatagramSize()), Qt::Uninitialized);
                cli.readDatagram(d.data(), d.size());
                got = d;
                l.quit();
            });
            t.start(4000);
            l.exec();
        }
        // 回程也带 SOCKS UDP 头（RSV/FRAG/ATYP/ADDR/PORT），载荷从第 10 字节起
        std::fprintf(stderr, "        [dbg] echoOk=%d relay=%u got=%d bytes\n", int(echoOk),
                     unsigned(relay), got.size());
        const bool ok3 = got.size() > 10 && got.mid(10).startsWith("ECHO:PING-UDP");
        std::fprintf(stderr, "  [%s] SOCKS5 UDP ASSOCIATE 端到端回显\n", ok3 ? "PASS" : "FAIL");
        if (!ok3)
            std::fprintf(stderr, "        收到 %d 字节: %s\n", got.size(),
                         got.mid(10).left(40).constData());
        ok &= ok3;
    }

    // ---- 流量计数：UI 上那行读数的数据源。必须**真的在数**，否则读数恒零、比没有还误导。
    //      上面已经跑过两个 8 MiB 用例，所以上下行都应当 ≥8 MiB，会话数 ≥5。
    {
        const quint64 up = inbound.bytesUp(), down = inbound.bytesDown();
        const quint64 sess = inbound.totalSessions();
        const bool ok2 = up >= quint64(kBigSize) && down >= quint64(kBigSize) && sess >= 5;
        std::fprintf(stderr,
                     "  [%s] 流量计数在数（会话 %llu，↑%.1f MiB ↓%.1f MiB）\n", ok2 ? "PASS" : "FAIL",
                     static_cast<unsigned long long>(sess), up / 1048576.0, down / 1048576.0);
        if (!ok2)
            std::fprintf(stderr, "        期望 上下行各 ≥8 MiB、会话 ≥5\n");
        ok &= ok2;
    }

    // ---- 证伪检查：上面两个大流量用例**必须真的把水位顶上去**，否则节流分支根本没跑，
    //      PASS 是空的。这一条不通过就说明用例设计失效（载荷太小 / 水位太高 / 卡住没生效）。
    {
        const bool up = inbound.upThrottleHits() > 0;
        const bool down = inbound.downPauseHits() > 0;
        std::fprintf(stderr, "  [%s] 背压路径确实被触发（上行节流 %llu 次 / 下行暂停 %llu 次）\n",
                     (up && down) ? "PASS" : "FAIL",
                     static_cast<unsigned long long>(inbound.upThrottleHits()),
                     static_cast<unsigned long long>(inbound.downPauseHits()));
        if (!(up && down))
            std::fprintf(stderr, "        ↑ 计数为 0 = 那个方向的节流代码没跑过，上面的 PASS 不算数\n");
        ok &= (up && down);
    }

    std::fprintf(stderr, "=== 结论: %s ===\n",
                 ok ? "PASS —— 本机流量已能全程走进程内引擎，未经 mihomo" : "FAIL");
    std::fflush(stderr);
    return ok ? 0 : 1;
}
