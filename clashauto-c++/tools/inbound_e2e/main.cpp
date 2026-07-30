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
#include "core/ProxyConfig.h"
#include "inbound/MixedInbound.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
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

    bool ok = true;

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
