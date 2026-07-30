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
#include <QTimer>

#include <cstdio>
#include <memory>

namespace {

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
            QObject::connect(sk, &QTcpSocket::readyRead, sk, [sk] {
                const QByteArray req = sk->readAll();
                const QByteArray line = req.left(qMax(0, req.indexOf('\r')));
                sk->write("HTTP/1.1 200 OK\r\nContent-Length: "
                          + QByteArray::number(line.size() + 8)
                          + "\r\nConnection: close\r\n\r\nCOASTOK:" + line);
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

    std::fprintf(stderr, "=== 结论: %s ===\n",
                 ok ? "PASS —— 本机流量已能全程走进程内引擎，未经 mihomo" : "FAIL");
    std::fflush(stderr);
    return ok ? 0 : 1;
}
