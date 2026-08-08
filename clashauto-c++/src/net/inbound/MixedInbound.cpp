#include "MixedInbound.h"

#include "../IOutbound.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <cstdio>
#include <tuple>
#include <utility>

namespace {

// 背压水位：与 NetStack 那边同量级。高于高水位就停读，回落到低水位再恢复。
constexpr qint64 kHighWater = 512 * 1024;
constexpr qint64 kLowWater = 128 * 1024;
// 握手期缓冲上限——没握完手就敢送这么多字节的，不是正常客户端。
constexpr int kMaxHandshakeBytes = 64 * 1024;

// 逐跳头（RFC 7230 §6.1）：代理转发时必须摘掉，不能原样透传。
bool isHopByHopHeader(const QByteArray &lowerName)
{
    return lowerName == "proxy-connection" || lowerName == "proxy-authorization"
        || lowerName == "connection" || lowerName == "keep-alive" || lowerName == "te"
        || lowerName == "trailer" || lowerName == "transfer-encoding" || lowerName == "upgrade";
}

// 往 SOCKS5 报文里追加 ATYP + 地址 + 端口（应答的 BND 和 UDP 头共用同一个编码）。
void appendSocksAddr(QByteArray &out, const QHostAddress &addr, quint16 port)
{
    if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        out.append('\x04');
        const Q_IPV6ADDR raw = addr.toIPv6Address();
        out.append(reinterpret_cast<const char *>(raw.c), 16);
    } else {
        out.append('\x01');
        const quint32 v4 = qToBigEndian<quint32>(addr.toIPv4Address());
        out.append(reinterpret_cast<const char *>(&v4), 4);
    }
    const quint16 p = qToBigEndian<quint16>(port);
    out.append(reinterpret_cast<const char *>(&p), 2);
}

} // namespace

struct MixedInbound::Session {
    QTcpSocket *client = nullptr;
    IOutboundTcp *out = nullptr;
    QByteArray inBuf;        // 握手期累积的客户端字节
    QByteArray successReply; // 隧道就绪后要回给客户端的握手应答（SOCKS5 / CONNECT 才有）
    QString target;
    bool dialed = false;
    bool established = false;
    bool gone = false;   // 已进入销毁流程，防重入
    int socksPhase = 0;  // 0=等 greeting，1=等 request
    bool upThrottled = false; // 已卡住客户端读（等出站排空）
    bool downPaused = false;  // 已暂停从出站读（等客户端排空）
    // —— SOCKS5 UDP ASSOCIATE ——（TCP 连接只用来维持关联生命周期）
    QUdpSocket *udp = nullptr;      // 收客户端数据报的中继口（只绑回环）
    IOutboundUdp *udpOut = nullptr; // 对应的出站 UDP 会话
    QHostAddress udpPeer;           // 客户端的源地址（第一份数据报里学到）
    quint16 udpPeerPort = 0;
    bool udpReady = false;
    // ★ associate() 之后 ready() 是**异步**才发的（DirectOutboundUdp.h 明写：NetStack 依赖这个时序）。
    //   就绪前直接 sendTo 会把数据报丢掉 —— DNS 查询这种「一来就发一个包」的场景 100% 命中。
    //   所以先排队，ready 到了再一次性发出去。
    QVector<std::tuple<QHostAddress, quint16, QByteArray>> udpPending;
};

MixedInbound::MixedInbound(OutboundFactory *factory, QObject *parent)
    : QObject(parent), m_factory(factory)
{
}

MixedInbound::~MixedInbound()
{
    stop();
}

bool MixedInbound::listen(quint16 port, const QHostAddress &addr)
{
    stop();
    m_server = new QTcpServer(this);
    if (!m_server->listen(addr, port)) {
        std::fprintf(stderr, "[INBOUND] listen %s:%u failed: %s\n",
                     qPrintable(addr.toString()), unsigned(port),
                     qPrintable(m_server->errorString()));
        std::fflush(stderr);
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QTcpServer::newConnection, this, &MixedInbound::onNewConnection);
    return true;
}

void MixedInbound::stop()
{
    const auto sessions = m_sessions; // closeSession 会改集合，先拷一份
    for (Session *s : sessions)
        closeSession(s, "stop");
    m_sessions.clear();
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

quint16 MixedInbound::boundPort() const { return m_server ? m_server->serverPort() : 0; }
bool MixedInbound::isListening() const { return m_server && m_server->isListening(); }

void MixedInbound::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *c = m_server->nextPendingConnection();
        auto *s = new Session;
        s->client = c;
        m_sessions.insert(s);
        connect(c, &QTcpSocket::readyRead, this, [this, s] { onClientReadable(s); });
        connect(c, &QTcpSocket::disconnected, this, [this, s] { closeSession(s, "client closed"); });
        connect(c, &QTcpSocket::bytesWritten, this, [this, s] {
            // 下行排空 → 恢复从出站读（背压回落）
            if (s->out && s->client && s->downPaused
                && s->client->bytesToWrite() <= kLowWater) {
                s->downPaused = false;
                s->out->setReadPaused(false);
            }
        });
        if (c->bytesAvailable() > 0)
            onClientReadable(s);
    }
}

void MixedInbound::onClientReadable(Session *s)
{
    if (s->gone || !s->client)
        return;
    if (!s->dialed) {
        s->inBuf.append(s->client->readAll());
        if (s->inBuf.size() > kMaxHandshakeBytes) {
            closeSession(s, "handshake too large");
            return;
        }
        handleHandshake(s);
        return;
    }
    pumpClientToOut(s);
}

// 隧道已建：把客户端字节直接灌给出站，并按出站积压做背压。
void MixedInbound::pumpClientToOut(Session *s)
{
    if (!s->out || !s->client)
        return;
    const QByteArray data = s->client->readAll();
    if (!data.isEmpty()) {
        m_bytesUp += quint64(data.size());
        s->out->write(data);
    }
    // 出站积压过高 → 暂停从客户端读（QTcpSocket 没有 setReadPaused，用 readBufferSize 卡住）
    if (s->out->bytesToWrite() > kHighWater) {
        if (!s->upThrottled) {
            s->upThrottled = true;
            ++m_upThrottleHits; // 只数进入节流态的沿
        }
        s->client->setReadBufferSize(1);
    } else if (s->upThrottled) {
        s->upThrottled = false;
        s->client->setReadBufferSize(0); // 0 = 不限
    }
}

void MixedInbound::handleHandshake(Session *s)
{
    const QByteArray &b = s->inBuf;
    if (b.isEmpty())
        return;

    const auto first = static_cast<unsigned char>(b.at(0));

    // ---- SOCKS4/4a：明确不支持，直接断（见头文件里的理由）----
    if (first == 0x04) {
        closeSession(s, "socks4 not supported");
        return;
    }

    // ---- SOCKS5 ----
    if (first == 0x05) {
        if (s->socksPhase == 0) {
            if (b.size() < 2)
                return;
            const int nmethods = static_cast<unsigned char>(b.at(1));
            if (b.size() < 2 + nmethods)
                return;
            s->inBuf.remove(0, 2 + nmethods);
            s->client->write(QByteArray::fromRawData("\x05\x00", 2)); // 无认证
            s->socksPhase = 1;
            if (s->inBuf.isEmpty())
                return;
        }
        // phase 1：请求
        const QByteArray &r = s->inBuf;
        if (r.size() < 4)
            return;
        const auto cmd = static_cast<unsigned char>(r.at(1));
        const auto atyp = static_cast<unsigned char>(r.at(3));
        QString host;
        quint16 port = 0;
        int consumed = 0;
        if (atyp == 0x01) { // IPv4
            if (r.size() < 4 + 4 + 2)
                return;
            host = QHostAddress(qFromBigEndian<quint32>(
                                    reinterpret_cast<const uchar *>(r.constData() + 4)))
                       .toString();
            port = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(r.constData() + 8));
            consumed = 10;
        } else if (atyp == 0x03) { // 域名
            const int len = static_cast<unsigned char>(r.at(4));
            if (r.size() < 5 + len + 2)
                return;
            host = QString::fromLatin1(r.constData() + 5, len);
            port = qFromBigEndian<quint16>(
                reinterpret_cast<const uchar *>(r.constData() + 5 + len));
            consumed = 5 + len + 2;
        } else if (atyp == 0x04) { // IPv6
            if (r.size() < 4 + 16 + 2)
                return;
            Q_IPV6ADDR raw;
            memcpy(raw.c, r.constData() + 4, 16);
            host = QHostAddress(raw).toString();
            port = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(r.constData() + 20));
            consumed = 22;
        } else {
            s->client->write(QByteArray::fromRawData("\x05\x08\x00\x01\x00\x00\x00\x00\x00\x00", 10));
            closeSession(s, "bad atyp");
            return;
        }
        if (cmd == 0x03) { // UDP ASSOCIATE
            s->inBuf.remove(0, consumed);
            startUdpAssociate(s);
            return;
        }
        if (cmd != 0x01) { // 其余命令（BIND 等）不支持
            s->client->write(QByteArray::fromRawData("\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00", 10));
            closeSession(s, "socks5 cmd not supported");
            return;
        }
        s->inBuf.remove(0, consumed);
        // 成功应答固定用 0.0.0.0:0（BND.ADDR 对客户端无意义，mihomo 也这么回）
        s->successReply = QByteArray::fromRawData("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);
        startDial(s, host, port, s->inBuf); // inBuf 里剩下的是「早到的」上行数据
        return;
    }

    // ---- HTTP ----
    const int hdrEnd = b.indexOf("\r\n\r\n");
    if (hdrEnd < 0)
        return; // 头还没收全
    const QByteArray head = b.left(hdrEnd);
    QByteArray body = b.mid(hdrEnd + 4);

    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty()) {
        closeSession(s, "bad http");
        return;
    }
    QByteArray reqLine = lines.first();
    if (reqLine.endsWith('\r'))
        reqLine.chop(1);
    const QList<QByteArray> parts = reqLine.split(' ');
    if (parts.size() < 3) {
        closeSession(s, "bad http request line");
        return;
    }
    const QByteArray method = parts.at(0);
    const QByteArray uri = parts.at(1);
    const QByteArray version = parts.at(2);

    if (method.compare("CONNECT", Qt::CaseInsensitive) == 0) {
        const int colon = uri.lastIndexOf(':');
        if (colon <= 0) {
            closeSession(s, "bad CONNECT target");
            return;
        }
        const QString host = QString::fromLatin1(uri.left(colon));
        const quint16 port = uri.mid(colon + 1).toUShort();
        s->successReply = "HTTP/1.1 200 Connection Established\r\n\r\n";
        startDial(s, host, port ? port : 443, body);
        return;
    }

    // 绝对形式：GET http://host[:port]/path HTTP/1.1 → 改写成源形式转发
    const QUrl url = QUrl::fromEncoded(uri);
    if (!url.isValid() || url.host().isEmpty()) {
        closeSession(s, "not an absolute-form http request");
        return;
    }
    QByteArray path = url.path(QUrl::FullyEncoded).toLatin1();
    if (path.isEmpty())
        path = "/";
    const QString q = url.query(QUrl::FullyEncoded);
    if (!q.isEmpty())
        path += '?' + q.toLatin1();

    QByteArray rewritten = method + ' ' + path + ' ' + version + "\r\n";
    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;
        const int c = line.indexOf(':');
        if (c > 0 && isHopByHopHeader(line.left(c).toLower().trimmed()))
            continue; // 逐跳头不转发
        rewritten += line + "\r\n";
    }
    // ★ 强制 Connection: close —— 本单元把「一条客户端连接」绑定到「一个上游目标」。
    //   若放任 keep-alive，客户端完全可能在同一条连接上发第二个请求指向**另一个 host**，
    //   而我们会把它错误地送进第一个目标的隧道。关掉复用是正确性优先的取舍（代价是每请求一次握手）。
    rewritten += "Connection: close\r\n\r\n";
    rewritten += body;

    // 端口缺省要跟着 scheme 走：绝对形式虽然绝大多数是 http://，但写死 80 会让 https:// 的
    // 绝对形式（少见但合法）被送到 80 端口，表现为「莫名其妙连不上」。
    const quint16 defPort =
        url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
    startDial(s, url.host(), static_cast<quint16>(url.port(defPort)), rewritten);
}

void MixedInbound::startDial(Session *s, const QString &host, quint16 port,
                             const QByteArray &initialUpstream)
{
    if (!m_factory) {
        closeSession(s, "no outbound factory");
        return;
    }
    // ★ 必须先**拷贝**再清空：SOCKS5 那条路径直接把 `s->inBuf` 当 initialUpstream 传进来，
    //   而 initialUpstream 是 const 引用 —— 下一行的 clear() 会把它一起清掉，
    //   于是「早到的」首个请求被静默吞掉：隧道建起来却一个字节不发，两端互等到超时。
    //   （与 IOutbound.h 里记的网关侧丢首段是同一类 bug，自检就是为了钉住这类静默丢数据。）
    const QByteArray early = initialUpstream;
    s->dialed = true;
    s->inBuf.clear();
    s->target = host + QLatin1Char(':') + QString::number(port);

    IOutboundTcp *out = m_factory->createTcp(this);
    if (!out) {
        closeSession(s, "factory returned null");
        return;
    }
    s->out = out;

    connect(out, &IOutboundTcp::established, this, [this, s] {
        s->established = true;
        if (!s->successReply.isEmpty() && s->client)
            s->client->write(s->successReply); // SOCKS5 / CONNECT 的成功应答只在此刻发
    });
    connect(out, &IOutboundTcp::dataReceived, this, [this, s](const QByteArray &d) {
        if (s->gone || !s->client)
            return;
        m_bytesDown += quint64(d.size());
        s->client->write(d);
        if (s->client->bytesToWrite() > kHighWater && s->out && !s->downPaused) {
            s->downPaused = true;
            ++m_downPauseHits;
            s->out->setReadPaused(true); // 客户端吃不下 → 停止从上游读
        }
    });
    connect(out, &IOutboundTcp::failed, this, [this, s](const QString &why) {
        // 握手还没回应答就失败 → 给客户端一个像样的拒绝，而不是静默断开
        if (s->client && !s->established && !s->successReply.isEmpty()) {
            if (s->successReply.startsWith("HTTP/"))
                s->client->write("HTTP/1.1 502 Bad Gateway\r\n\r\n");
            else
                s->client->write(
                    QByteArray::fromRawData("\x05\x01\x00\x01\x00\x00\x00\x00\x00\x00", 10));
        }
        closeSession(s, qPrintable(why));
    });
    connect(out, &IOutboundTcp::closed, this, [this, s] { closeSession(s, "upstream closed"); });
    // 上行排空 → 立刻重新评估「要不要解除对客户端的节流」。
    // ★ 不接这个信号的话，解除节流的唯一触发点是客户端的下一次 readyRead —— 而我们正把它的读缓冲
    //   卡在 1 字节，于是只能一字节一字节地爬回来（能恢复，但慢得莫名其妙）。这是典型的
    //   「节流容易、恢复难」的坑：不看信号就以为它自愈了。
    connect(out, &IOutboundTcp::upstreamBytesWritten, this, [this, s](qint64) {
        if (s->gone || !s->out || !s->client || !s->upThrottled)
            return;
        if (s->out->bytesToWrite() <= kLowWater) {
            s->upThrottled = false;
            s->client->setReadBufferSize(0);
        }
    });

    out->connectTo(host, port, m_user);
    // 契约允许（也要求）write() 早于/紧随 connectTo：早到的上行字节必须原样补发，
    // 绝不能丢——真机上「隧道建起来却一个字节不发」就是这么来的（见 IOutbound.h 的长注释）。
    if (!early.isEmpty()) {
        m_bytesUp += quint64(early.size());
        out->write(early);
    }
}

// —————————————— SOCKS5 UDP ASSOCIATE ——————————————
//
// 语义（RFC 1928 §7）：控制用的 TCP 连接**保持打开**代表关联存活，断开即结束；
// 客户端把数据报发到我们回给它的 BND 端口，每个报文前带
//   RSV(2) FRAG(1) ATYP(1) DST.ADDR DST.PORT
// 我们剥掉这个头把载荷交给出站，回程再原样套回去。FRAG≠0（分片）按 RFC 可以丢，这里就丢。
void MixedInbound::startUdpAssociate(Session *s)
{
    if (!m_factory) {
        closeSession(s, "no factory");
        return;
    }
    s->udp = new QUdpSocket(this);
    // 只在回环上收：客户端就是本机应用；绑 0.0.0.0 会把这个中继暴露给整个局域网。
    if (!s->udp->bind(QHostAddress::LocalHost, 0)) {
        s->client->write(QByteArray::fromRawData("\x05\x01\x00\x01\x00\x00\x00\x00\x00\x00", 10));
        closeSession(s, "udp bind failed");
        return;
    }
    const quint16 relayPort = s->udp->localPort();

    s->udpOut = m_factory->createUdp(this);
    if (!s->udpOut) { // 出站不支持 UDP（严格模式 / 协议没实现）→ 老实说不支持，别假装成功
        s->client->write(QByteArray::fromRawData("\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00", 10));
        closeSession(s, "outbound has no udp");
        return;
    }

    connect(s->udpOut, &IOutboundUdp::datagramReceived, this,
            [this, s](const QHostAddress &srcIp, quint16 srcPort, const QByteArray &payload) {
                if (s->gone || !s->udp || s->udpPeerPort == 0)
                    return; // 还没见过客户端的源地址，回不去
                QByteArray out;
                out.append('\0');
                out.append('\0');
                out.append('\0'); // RSV RSV FRAG
                appendSocksAddr(out, srcIp, srcPort);
                out.append(payload);
                m_bytesDown += quint64(payload.size());
                s->udp->writeDatagram(out, s->udpPeer, s->udpPeerPort);
            });
    connect(s->udpOut, &IOutboundUdp::failed, this,
            [this, s](const QString &why) { closeSession(s, qPrintable(why)); });
    connect(s->udpOut, &IOutboundUdp::closed, this, [this, s] { closeSession(s, "udp out closed"); });

    connect(s->udpOut, &IOutboundUdp::ready, this, [this, s] {
        s->udpReady = true;
        for (const auto &d : std::as_const(s->udpPending))
            s->udpOut->sendTo(std::get<0>(d), std::get<1>(d), std::get<2>(d));
        s->udpPending.clear();
    });
    connect(s->udp, &QUdpSocket::readyRead, this, [this, s] { onUdpReadable(s); });
    s->udpOut->associate(m_user);

    // 应答里的 BND 必须是**客户端够得着**的地址：回环 + 我们刚绑的端口。
    QByteArray reply;
    reply.append('\x05');
    reply.append('\0');
    reply.append('\0');
    appendSocksAddr(reply, QHostAddress(QHostAddress::LocalHost), relayPort);
    s->client->write(reply);
    s->dialed = true; // 之后 TCP 上不再有请求；它只用来维持关联的生命周期
}

void MixedInbound::onUdpReadable(Session *s)
{
    while (s->udp && s->udp->hasPendingDatagrams()) {
        QByteArray buf(int(s->udp->pendingDatagramSize()), Qt::Uninitialized);
        QHostAddress from;
        quint16 fromPort = 0;
        const qint64 n = s->udp->readDatagram(buf.data(), buf.size(), &from, &fromPort);
        if (n <= 0)
            continue;
        buf.truncate(int(n));
        // 记住客户端的源地址：回程要往这里送。只认第一个（一个关联对应一个客户端 socket）。
        if (s->udpPeerPort == 0) {
            s->udpPeer = from;
            s->udpPeerPort = fromPort;
        }
        if (buf.size() < 5 || buf.at(2) != '\0')
            continue; // 太短，或 FRAG≠0（分片）——按 RFC 可以直接丢
        const auto atyp = static_cast<unsigned char>(buf.at(3));
        QHostAddress dst;
        quint16 dport = 0;
        int off = 0;
        if (atyp == 0x01 && buf.size() >= 10) {
            dst = QHostAddress(qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(buf.constData() + 4)));
            dport = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(buf.constData() + 8));
            off = 10;
        } else if (atyp == 0x04 && buf.size() >= 22) {
            Q_IPV6ADDR raw;
            memcpy(raw.c, buf.constData() + 4, 16);
            dst = QHostAddress(raw);
            dport = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(buf.constData() + 20));
            off = 22;
        } else {
            continue; // ATYP=域名：见头文件里写明的局限，丢弃
        }
        const QByteArray payload = buf.mid(off);
        if (payload.isEmpty() || !s->udpOut)
            continue;
        m_bytesUp += quint64(payload.size());
        if (s->udpReady)
            s->udpOut->sendTo(dst, dport, payload);
        else
            s->udpPending.append({dst, dport, payload}); // 见 Session::udpPending 上方的时序说明
    }
}

void MixedInbound::closeSession(Session *s, const char *why)
{
    if (!s || s->gone)
        return;
    s->gone = true;
    // 会话收口原因通常无需刷屏；但排查「出站到底为什么不通」时它是唯一的真相来源
    // （curl 只给一个 000）。COAST_INBOUND_VERBOSE=1 时把每条会话的收口原因 + 目标打出来
    // —— 出站探针(outboundProbe)会自动开这个，好让 curl=000 背后的真实错误浮出来。
    static const bool verbose = qEnvironmentVariableIsSet("COAST_INBOUND_VERBOSE");
    if (verbose && why) {
        std::fprintf(stderr, "[INBOUND] 会话收口 target=%s established=%d 原因=%s\n",
                     qUtf8Printable(s->target), int(s->established), why);
        std::fflush(stderr);
    }
    m_sessions.remove(s);
    if (s->out) {
        s->out->disconnect(this);
        s->out->closeTunnel();
        s->out->deleteLater();
        s->out = nullptr;
    }
    // UDP 关联的两件东西也要收：TCP 控制连接一断，关联即结束（RFC 1928 §7）。
    if (s->udpOut) {
        s->udpOut->disconnect(this);
        s->udpOut->closeSession();
        s->udpOut->deleteLater();
        s->udpOut = nullptr;
    }
    if (s->udp) {
        s->udp->disconnect(this);
        s->udp->close();
        s->udp->deleteLater();
        s->udp = nullptr;
    }
    if (s->client) {
        s->client->disconnect(this);
        // ★ 先把还缓在写队列里的下行字节冲出去，再关。
        //   典型场景：上游(如 Hy2 代理流)把「响应体 + FIN」几乎同时给过来 —— dataReceived 刚把响应体
        //   write() 进客户端 socket 的写缓冲，紧接着 peerSendShutdown 就触发 closeSession。若此刻直接
        //   close()+deleteLater()，QAbstractSocket 析构**不等**写缓冲排空，那段响应体会连同对象一起被丢，
        //   客户端(curl)只看到连接被关、拿不到任何响应 → 表现为 curl=000。flush() 把写缓冲同步推进 OS
        //   发送缓冲，随后 close() 再发 FIN，客户端就能读到完整响应再收到 EOF。（真机 Hy2 实证：源站
        //   Connection: close 的 404 响应，修此点前 curl=000、修后 curl=404。）
        if (s->client->state() == QAbstractSocket::ConnectedState && s->client->bytesToWrite() > 0)
            s->client->flush();
        s->client->close();
        s->client->deleteLater();
        s->client = nullptr;
    }
    delete s;
}

// ===================== 自检 =====================
// 不需要任何节点/订阅/网络：靶服务器 + 直连出站桩全在本进程内。
namespace {

// 出站桩：忠实实现 IOutboundTcp 契约（含「write 可早于 connectTo」），底层就是一条 QTcpSocket。
class DirectStubTcp : public IOutboundTcp
{
public:
    explicit DirectStubTcp(QObject *parent) : IOutboundTcp(parent), m_sock(new QTcpSocket(this))
    {
        QObject::connect(m_sock, &QTcpSocket::connected, this, [this] {
            m_established = true;
            if (!m_pending.isEmpty()) {
                m_sock->write(m_pending);
                m_pending.clear();
            }
            emit established();
        });
        QObject::connect(m_sock, &QTcpSocket::readyRead, this,
                         [this] { emit dataReceived(m_sock->readAll()); });
        QObject::connect(m_sock, &QTcpSocket::disconnected, this, [this] { emit closed(); });
        QObject::connect(m_sock, &QTcpSocket::errorOccurred, this,
                         [this](QAbstractSocket::SocketError) { emit failed(m_sock->errorString()); });
    }
    void connectTo(const QString &host, quint16 port, const QString &) override
    {
        m_sock->connectToHost(host, port); // ★ 不碰 m_pending
    }
    void write(const QByteArray &d) override
    {
        if (m_established)
            m_sock->write(d);
        else
            m_pending.append(d);
    }
    void write(const char *d, qsizetype n) override { write(QByteArray(d, int(n))); }
    void closeTunnel() override { m_sock->close(); }
    bool isEstablished() const override { return m_established; }
    qint64 bytesToWrite() const override { return m_sock->bytesToWrite() + m_pending.size(); }
    void setReadPaused(bool p) override { m_sock->setReadBufferSize(p ? 1 : 0); }
    bool isReadPaused() const override { return m_sock->readBufferSize() == 1; }

private:
    QTcpSocket *m_sock;
    QByteArray m_pending;
    bool m_established = false;
};

class DirectStubFactory : public OutboundFactory
{
public:
    IOutboundTcp *createTcp(QObject *parent) override { return new DirectStubTcp(parent); }
    IOutboundUdp *createUdp(QObject *) override { return nullptr; }
};

// 跑一个客户端会话：发 request，等到收满 expectContains 或超时。
bool runCase(const char *name, quint16 port, const QByteArray &request,
             const QByteArray &expectContains, int timeoutMs = 4000)
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
        std::fprintf(stderr, "  [FAIL] %s: connect to inbound failed\n", name);
        return false;
    }
    c.write(request);
    to.start(timeoutMs);
    loop.exec();
    const bool ok = got.contains(expectContains);
    std::fprintf(stderr, "  [%s] %s (got %lld bytes)\n", ok ? "PASS" : "FAIL", name,
                 static_cast<long long>(got.size()));
    if (!ok)
        std::fprintf(stderr, "        expected to contain: %s\n", expectContains.constData());
    return ok;
}

} // namespace

bool MixedInbound::selfTest()
{
    std::fprintf(stderr, "=== MixedInbound self-test ===\n");

    // 靶服务器：收到任何请求就回一段可识别的 HTTP 应答
    QTcpServer target;
    if (!target.listen(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "  [FAIL] target listen: %s\n", qPrintable(target.errorString()));
        return false;
    }
    const quint16 tport = target.serverPort();
    QObject::connect(&target, &QTcpServer::newConnection, &target, [&target] {
        while (target.hasPendingConnections()) {
            QTcpSocket *sk = target.nextPendingConnection();
            QObject::connect(sk, &QTcpSocket::readyRead, sk, [sk] {
                const QByteArray req = sk->readAll();
                // 把收到的请求行回显出去，好验证「绝对形式被改写成了源形式」
                const QByteArray line = req.left(qMax(0, req.indexOf('\r')));
                sk->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(line.size() + 8)
                          + "\r\nConnection: close\r\n\r\nCOASTOK:" + line);
            });
            QObject::connect(sk, &QTcpSocket::disconnected, sk, &QObject::deleteLater);
        }
    });

    DirectStubFactory factory;
    MixedInbound inbound(&factory);
    if (!inbound.listen(0)) {
        std::fprintf(stderr, "  [FAIL] inbound listen\n");
        return false;
    }
    const quint16 iport = inbound.boundPort();
    std::fprintf(stderr, "  inbound=127.0.0.1:%u target=127.0.0.1:%u\n", unsigned(iport),
                 unsigned(tport));

    bool ok = true;

    // 1) SOCKS5 CONNECT（域名形式）
    {
        const QByteArray hostBytes = QByteArray("127.0.0.1");
        QByteArray req;
        req.append("\x05\x01\x00", 3); // greeting: VER=5, 1 method, NO-AUTH
        req.append("\x05\x01\x00\x03", 4);
        req.append(char(hostBytes.size()));
        req.append(hostBytes);
        req.append(char((tport >> 8) & 0xFF));
        req.append(char(tport & 0xFF));
        req.append("GET /s5 HTTP/1.1\r\nHost: x\r\n\r\n");
        ok &= runCase("socks5 CONNECT + relay", iport, req, "COASTOK:GET /s5");
    }

    // 2) HTTP CONNECT
    {
        QByteArray req = "CONNECT 127.0.0.1:" + QByteArray::number(tport)
            + " HTTP/1.1\r\nHost: x\r\n\r\nGET /cx HTTP/1.1\r\nHost: x\r\n\r\n";
        ok &= runCase("http CONNECT + relay", iport, req, "COASTOK:GET /cx");
    }

    // 3) HTTP 绝对形式 → 必须被改写成源形式（靶机回显请求行，能直接验证）
    {
        QByteArray req = "GET http://127.0.0.1:" + QByteArray::number(tport)
            + "/abs?q=1 HTTP/1.1\r\nHost: 127.0.0.1\r\nProxy-Connection: keep-alive\r\n\r\n";
        ok &= runCase("http absolute-form rewritten to origin-form", iport, req,
                      "COASTOK:GET /abs?q=1");
    }

    std::fprintf(stderr, "=== MixedInbound self-test: %s ===\n", ok ? "PASS" : "FAIL");
    std::fflush(stderr);
    return ok;
}
