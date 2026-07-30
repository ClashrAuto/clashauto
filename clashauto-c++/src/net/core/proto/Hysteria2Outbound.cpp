#include "Hysteria2Outbound.h"

#include "OutboundRegistry.h"
#include "../transport/QuicTransport.h"

#include <QHash>
#include <QRandomGenerator>

// 说明：本文件用 coastcore::QuicTransport 实现 Hysteria2。协议线格式见头文件与下方注释。
// 与 Trojan/Direct 一样, 对 NetStack 侧无感:established 后就是背压化的双向字节管道。
//
// ★ 每个出站对象各自起一条 QUIC 连接并单独做 HTTP/3 认证。生产级 Hy2 会把多条流复用到**一条**
//   QUIC 连接上(省握手/认证), 那需要一个跨连接的共享连接池——属未来优化, 且要动共享的拨号侧,
//   本单元受「不改已有文件」约束故未做(见报告 TODO:共享连接复用)。

namespace {

// —— QUIC 变长整数(RFC 9000 §16)：首字节高 2 位表示总长 1/2/4/8, 其余为大端数值。——
QByteArray quicVarint(quint64 v)
{
    QByteArray b;
    if (v <= 0x3F) {
        b.append(char(v));
    } else if (v <= 0x3FFF) {
        b.append(char(0x40 | ((v >> 8) & 0x3F)));
        b.append(char(v & 0xFF));
    } else if (v <= 0x3FFFFFFF) {
        b.append(char(0x80 | ((v >> 24) & 0x3F)));
        b.append(char((v >> 16) & 0xFF));
        b.append(char((v >> 8) & 0xFF));
        b.append(char(v & 0xFF));
    } else {
        b.append(char(0xC0 | ((v >> 56) & 0x3F)));
        b.append(char((v >> 48) & 0xFF));
        b.append(char((v >> 40) & 0xFF));
        b.append(char((v >> 32) & 0xFF));
        b.append(char((v >> 24) & 0xFF));
        b.append(char((v >> 16) & 0xFF));
        b.append(char((v >> 8) & 0xFF));
        b.append(char(v & 0xFF));
    }
    return b;
}

// 从 buf[pos] 解一个 QUIC 变长整数。成功返回 true 并前移 pos;字节不足返回 false(pos 不动)。
bool readVarint(const QByteArray &buf, int &pos, quint64 *out)
{
    if (pos >= buf.size())
        return false;
    const quint8 first = quint8(buf.at(pos));
    const int len = 1 << (first >> 6); // 1,2,4,8
    if (pos + len > buf.size())
        return false;
    quint64 v = first & 0x3F;
    for (int i = 1; i < len; ++i)
        v = (v << 8) | quint8(buf.at(pos + i));
    pos += len;
    *out = v;
    return true;
}

// 目标地址 → Hysteria2 的 "host:port" 字符串(IPv6 用方括号)。
QByteArray hostPortString(const QString &host, quint16 port)
{
    QHostAddress a;
    if (a.setAddress(host) && a.protocol() == QAbstractSocket::IPv6Protocol)
        return QStringLiteral("[%1]:%2").arg(host).arg(port).toUtf8();
    return QStringLiteral("%1:%2").arg(host).arg(port).toUtf8();
}
QByteArray hostPortString(const QHostAddress &ip, quint16 port)
{
    if (ip.protocol() == QAbstractSocket::IPv6Protocol)
        return QStringLiteral("[%1]:%2").arg(ip.toString()).arg(port).toUtf8();
    return QStringLiteral("%1:%2").arg(ip.toString()).arg(port).toUtf8();
}

// 解析 "host:port"(可能是 "[v6]:port") → (QHostAddress, port)。失败返回 false。
bool parseHostPort(const QByteArray &s, QHostAddress *ip, quint16 *port)
{
    const QString str = QString::fromUtf8(s);
    int colon = str.lastIndexOf(':');
    if (colon < 0)
        return false;
    QString h = str.left(colon);
    const QString p = str.mid(colon + 1);
    if (h.startsWith('[') && h.endsWith(']'))
        h = h.mid(1, h.size() - 2);
    bool okp = false;
    const uint pv = p.toUInt(&okp);
    if (!okp || pv > 0xFFFF)
        return false;
    QHostAddress a;
    if (!a.setAddress(h))
        return false; // 下行地址应为 IP 字面量;域名回程本客户端用不到
    *ip = a;
    *port = quint16(pv);
    return true;
}

// —— 最小 HPACK/QPACK 前缀整数编码(RFC 7541 §5.1)——
void appendPrefixedInt(QByteArray &out, quint64 value, int prefixBits, quint8 highBits)
{
    const quint64 maxPrefix = (1u << prefixBits) - 1;
    if (value < maxPrefix) {
        out.append(char(highBits | quint8(value)));
        return;
    }
    out.append(char(highBits | quint8(maxPrefix)));
    value -= maxPrefix;
    while (value >= 128) {
        out.append(char((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.append(char(value & 0x7F));
}

// QPACK「Literal Field Line With Literal Name」(无 Huffman, 不索引):见 RFC 9204 §4.5.6。
// 首字节 001NHxxx:001 前缀 + N=0 + H=0 + 3 位名字长度前缀整数;名字;值(H=0 + 7 位长度前缀整数);值。
void appendQpackLiteral(QByteArray &out, const QByteArray &name, const QByteArray &value)
{
    appendPrefixedInt(out, quint64(name.size()), 3, 0x20); // 001 0 0 <len(3)>
    out.append(name);
    appendPrefixedInt(out, quint64(value.size()), 7, 0x00); // H=0 + <len(7)>
    out.append(value);
}

// 构造发往服务器的 HTTP/3 认证请求(HEADERS 帧的 QPACK 字段块 → 包成 HEADERS 帧)。
// 头:POST https://hysteria/auth, Hysteria-Auth=<password>, Hysteria-CC-RX=0, Hysteria-Padding=随机。
QByteArray buildAuthHeadersFrame(const QByteArray &password)
{
    QByteArray fields;
    fields.append(char(0x00)); // QPACK 字段段前缀:Required Insert Count = 0
    fields.append(char(0x00)); // Delta Base = 0(S=0)
    appendQpackLiteral(fields, ":method", "POST");
    appendQpackLiteral(fields, ":scheme", "https");
    appendQpackLiteral(fields, ":authority", "hysteria");
    appendQpackLiteral(fields, ":path", "/auth");
    appendQpackLiteral(fields, "hysteria-auth", password);
    appendQpackLiteral(fields, "hysteria-cc-rx", "0");
    // 随机 padding(长度 8..255)防流量指纹。
    const int padLen = 8 + int(QRandomGenerator::global()->bounded(248));
    QByteArray pad(padLen, 'x');
    appendQpackLiteral(fields, "hysteria-padding", pad);

    QByteArray frame;
    frame += quicVarint(0x01);                 // HTTP/3 HEADERS 帧类型
    frame += quicVarint(quint64(fields.size()));
    frame += fields;
    return frame;
}

// HTTP/3 控制流首发内容:流类型(0x00) + SETTINGS 帧(声明 QPACK 动态表容量=0, 强制服务器只用静态表+字面量)。
QByteArray buildControlStreamPreamble()
{
    QByteArray settingsPayload;
    settingsPayload += quicVarint(0x01); // SETTINGS_QPACK_MAX_TABLE_CAPACITY
    settingsPayload += quicVarint(0x00);
    settingsPayload += quicVarint(0x07); // SETTINGS_QPACK_BLOCKED_STREAMS
    settingsPayload += quicVarint(0x00);

    QByteArray out;
    out += quicVarint(0x00); // 单向流类型 = Control Stream
    out += quicVarint(0x04); // SETTINGS 帧类型
    out += quicVarint(quint64(settingsPayload.size()));
    out += settingsPayload;
    return out;
}

const quint64 kTcpRequestId = 0x401; // Hysteria2 TCPRequest 帧 ID
constexpr int kUdpAddrMaxGuess = 512; // 估算 UDP 头开销上限, 用于分片阈值

} // namespace

// ============================ Hysteria2OutboundTcp ============================

class Hysteria2OutboundTcp::Priv
{
public:
    explicit Priv(Hysteria2OutboundTcp *owner) : q(owner) {}

    Hysteria2OutboundTcp *q = nullptr;

    // 节点参数
    QString server;
    quint16 serverPort = 0;
    QByteArray password;
    QString sni;
    bool skipVerify = false;

    // 目标
    QString dstHost;
    quint16 dstPort = 0;

    coastcore::QuicTransport *quic = nullptr;
    coastcore::QuicStream *ctrl = nullptr;    // HTTP/3 控制流
    coastcore::QuicStream *authS = nullptr;   // HTTP/3 认证请求流
    coastcore::QuicStream *proxy = nullptr;   // TCP 代理流
    QByteArray authRx;                        // 认证响应累积(HTTP/3 帧)
    QByteArray respRx;                        // 代理流 TCPResponse 累积
    QByteArray pending;                       // established 前的上行字节
    bool authed = false;
    bool established = false;
    bool closedEmitted = false;
    bool readPaused = false;

    void fail(const QString &reason)
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->failed(reason);
        if (quic)
            quic->close();
    }
    void emitClosed()
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->closed();
    }

    // 认证成功 → 开代理流并发 TCPRequest。
    void startProxyStream()
    {
        proxy = quic->openBidiStream();
        QObject::connect(proxy, &coastcore::QuicStream::dataReceived, q,
                         [this](const QByteArray &b) { onProxyData(b); });
        QObject::connect(proxy, &coastcore::QuicStream::sendCompleted, q,
                         [this](qint64 n) { emit q->upstreamBytesWritten(n); });
        QObject::connect(proxy, &coastcore::QuicStream::peerSendShutdown, q,
                         [this] { emitClosed(); });
        QObject::connect(proxy, &coastcore::QuicStream::closed, q, [this] { emitClosed(); });
        QObject::connect(proxy, &coastcore::QuicStream::failed, q,
                         [this](const QString &r) {
                             if (established)
                                 emitClosed();
                             else
                                 fail(r);
                         });

        // TCPRequest = varint(0x401) | varint(addrLen) | addr | varint(padLen) | pad
        const QByteArray addr = hostPortString(dstHost, dstPort);
        const int padLen = int(QRandomGenerator::global()->bounded(256));
        QByteArray req;
        req += quicVarint(kTcpRequestId);
        req += quicVarint(quint64(addr.size()));
        req += addr;
        req += quicVarint(quint64(padLen));
        req += QByteArray(padLen, 'x');
        proxy->send(req);
    }

    // 代理流下行:先吃掉 TCPResponse, 之后为裸下行字节。
    void onProxyData(const QByteArray &b)
    {
        if (!established) {
            respRx += b;
            // TCPResponse = status(u8) | varint msgLen | msg | varint padLen | pad
            int pos = 0;
            if (respRx.size() < 1)
                return;
            const quint8 status = quint8(respRx.at(0));
            pos = 1;
            quint64 msgLen = 0;
            if (!readVarint(respRx, pos, &msgLen))
                return;
            if (respRx.size() < pos + int(msgLen))
                return;
            pos += int(msgLen);
            quint64 padLen = 0;
            if (!readVarint(respRx, pos, &padLen))
                return;
            if (respRx.size() < pos + int(padLen))
                return;
            pos += int(padLen);
            if (status != 0x00) {
                fail(QStringLiteral("hysteria2 TCPResponse status=%1").arg(status));
                return;
            }
            // TCPResponse 消费完毕:进入 established, 余下字节即下行 payload。
            const QByteArray rest = respRx.mid(pos);
            respRx.clear();
            established = true;
            emit q->established();
            if (!pending.isEmpty()) {
                proxy->send(pending);
                pending.clear();
            }
            if (!readPaused && !rest.isEmpty())
                emit q->dataReceived(rest);
            return;
        }
        if (!readPaused && !b.isEmpty())
            emit q->dataReceived(b);
    }

    // —— HTTP/3 认证 ——
    void beginAuth()
    {
        ctrl = quic->openUniStream();
        ctrl->send(buildControlStreamPreamble()); // 控制流保持打开(不 fin)

        authS = quic->openBidiStream();
        QObject::connect(authS, &coastcore::QuicStream::dataReceived, q,
                         [this](const QByteArray &b) { onAuthData(b); });
        QObject::connect(authS, &coastcore::QuicStream::failed, q,
                         [this](const QString &r) { fail(QStringLiteral("hy2 auth stream: ") + r); });
        // 发认证请求头并 fin(无请求体)。
        authS->send(buildAuthHeadersFrame(password), /*fin=*/true);
    }

    // ★需真机验证:此处为最小 HTTP/3 响应处理 —— 只确认收到一个 HEADERS 帧(即服务器已回认证响应)
    //   即视为认证通过, 未严格 QPACK 解出 :status 是否 == 233。完整实现需 QPACK 解码器(含 Huffman)
    //   来区分 233 HyOK 与失败状态(见报告 TODO)。认证真失败时会在后续代理流阶段暴露。
    void onAuthData(const QByteArray &b)
    {
        if (authed)
            return;
        authRx += b;
        int pos = 0;
        quint64 ftype = 0, flen = 0;
        while (readVarint(authRx, pos, &ftype)) {
            int save = pos;
            if (!readVarint(authRx, pos, &flen)) {
                pos = save;
                break;
            }
            if (authRx.size() < pos + int(flen))
                break; // 帧体未收全
            if (ftype == 0x01) { // HEADERS
                authed = true;
                startProxyStream();
                return;
            }
            pos += int(flen); // 跳过非 HEADERS 帧(如 SETTINGS)
        }
    }
};

Hysteria2OutboundTcp::Hysteria2OutboundTcp(const ProxyNode &node, QObject *parent)
    : IOutboundTcp(parent), d(new Priv(this))
{
    d->server = node.server;
    d->serverPort = node.port;
    d->password = node.password.toUtf8();
    d->sni = node.sni.isEmpty() ? node.server : node.sni;
    d->skipVerify = node.skipCertVerify;
}

Hysteria2OutboundTcp::~Hysteria2OutboundTcp()
{
    delete d;
}

void Hysteria2OutboundTcp::connectTo(const QString &dstHost, quint16 dstPort, const QString &)
{
    d->dstHost = dstHost;
    d->dstPort = dstPort;
    d->established = false;
    d->authed = false;
    d->closedEmitted = false;
    d->readPaused = false;
    d->pending.clear();
    d->authRx.clear();
    d->respRx.clear();

    d->quic = new coastcore::QuicTransport(this);
    connect(d->quic, &coastcore::QuicTransport::connected, this, [this] { d->beginAuth(); });
    connect(d->quic, &coastcore::QuicTransport::connectionFailed, this,
            [this](const QString &r) { d->fail(r); });
    connect(d->quic, &coastcore::QuicTransport::connectionClosed, this, [this] {
        if (d->established)
            d->emitClosed();
        else
            d->fail(QStringLiteral("hysteria2 connection closed before established"));
    });
    d->quic->openConnection(d->server, d->serverPort, QByteArrayLiteral("h3"), d->sni, d->skipVerify);
}

void Hysteria2OutboundTcp::write(const QByteArray &data)
{
    if (d->established && d->proxy)
        d->proxy->send(data);
    else
        d->pending += data;
}

void Hysteria2OutboundTcp::write(const char *data, qsizetype size)
{
    if (!data || size <= 0)
        return;
    if (d->established && d->proxy)
        d->proxy->send(QByteArray(data, size));
    else
        d->pending.append(data, size); // 深拷贝, 生命周期契约同 Trojan/Direct
}

void Hysteria2OutboundTcp::closeTunnel()
{
    if (d->quic)
        d->quic->close();
    d->emitClosed();
}

bool Hysteria2OutboundTcp::isEstablished() const { return d->established; }

qint64 Hysteria2OutboundTcp::bytesToWrite() const
{
    return qint64(d->pending.size()) + (d->proxy ? d->proxy->bytesInFlight() : 0);
}

void Hysteria2OutboundTcp::setReadPaused(bool paused)
{
    if (d->readPaused == paused)
        return;
    d->readPaused = paused;
    if (d->proxy)
        d->proxy->setReceiveEnabled(!paused); // QUIC 原生流控:关=远端被压住
}

bool Hysteria2OutboundTcp::isReadPaused() const { return d->readPaused; }

// ============================ Hysteria2OutboundUdp ============================

class Hysteria2OutboundUdp::Priv
{
public:
    explicit Priv(Hysteria2OutboundUdp *owner) : q(owner) {}

    Hysteria2OutboundUdp *q = nullptr;

    QString server;
    quint16 serverPort = 0;
    QByteArray password;
    QString sni;
    bool skipVerify = false;

    coastcore::QuicTransport *quic = nullptr;
    coastcore::QuicStream *ctrl = nullptr;
    coastcore::QuicStream *authS = nullptr;
    QByteArray authRx;
    bool authed = false;
    bool ready = false;
    bool closedEmitted = false;

    quint32 sessionId = 0;
    quint16 nextPktId = 1;

    // 下行分片重组:key = pktId, 值 = (fragCount, 已收 fragId → payload)。
    struct Reasm {
        quint8 fragCount = 0;
        QByteArray addr;
        QHash<quint8, QByteArray> frags;
    };
    QHash<quint16, Reasm> reasm;

    void fail(const QString &reason)
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->failed(reason);
        if (quic)
            quic->close();
    }
    void emitClosed()
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->closed();
    }

    void beginAuth()
    {
        ctrl = quic->openUniStream();
        ctrl->send(buildControlStreamPreamble());
        authS = quic->openBidiStream();
        QObject::connect(authS, &coastcore::QuicStream::dataReceived, q,
                         [this](const QByteArray &b) { onAuthData(b); });
        QObject::connect(authS, &coastcore::QuicStream::failed, q,
                         [this](const QString &r) { fail(QStringLiteral("hy2 auth stream: ") + r); });
        authS->send(buildAuthHeadersFrame(password), true);
    }

    void onAuthData(const QByteArray &b)
    {
        if (authed)
            return;
        authRx += b;
        int pos = 0;
        quint64 ftype = 0, flen = 0;
        while (readVarint(authRx, pos, &ftype)) {
            int save = pos;
            if (!readVarint(authRx, pos, &flen)) {
                pos = save;
                break;
            }
            if (authRx.size() < pos + int(flen))
                break;
            if (ftype == 0x01) { // HEADERS → 认证响应(见 TCP 侧同名 TODO)
                authed = true;
                ready = true;
                emit q->ready();
                return;
            }
            pos += int(flen);
        }
    }

    // 下行数据报 → 解 UDPMessage, 重组分片, 完整后 emit datagramReceived。
    void onDatagram(const QByteArray &dg)
    {
        // u32 sessionId | u16 pktId | u8 fragId | u8 fragCount | varint addrLen | addr | payload
        if (dg.size() < 8)
            return;
        int pos = 0;
        // sessionId(4, 大端) —— 本客户端只有一个会话, 不校验。
        pos += 4;
        const quint16 pktId = quint16((quint8(dg.at(4)) << 8) | quint8(dg.at(5)));
        const quint8 fragId = quint8(dg.at(6));
        const quint8 fragCount = quint8(dg.at(7));
        pos = 8;
        quint64 addrLen = 0;
        if (!readVarint(dg, pos, &addrLen))
            return;
        if (dg.size() < pos + int(addrLen))
            return;
        const QByteArray addr = dg.mid(pos, int(addrLen));
        pos += int(addrLen);
        const QByteArray payload = dg.mid(pos);

        if (fragCount <= 1) {
            deliver(addr, payload);
            return;
        }
        Reasm &r = reasm[pktId];
        r.fragCount = fragCount;
        if (r.addr.isEmpty())
            r.addr = addr;
        r.frags.insert(fragId, payload);
        if (r.frags.size() >= int(fragCount)) {
            QByteArray full;
            for (quint8 i = 0; i < fragCount; ++i)
                full += r.frags.value(i);
            const QByteArray a = r.addr;
            reasm.remove(pktId);
            deliver(a, full);
        }
    }

    void deliver(const QByteArray &addr, const QByteArray &payload)
    {
        QHostAddress ip;
        quint16 port = 0;
        if (parseHostPort(addr, &ip, &port))
            emit q->datagramReceived(ip, port, payload);
    }
};

Hysteria2OutboundUdp::Hysteria2OutboundUdp(const ProxyNode &node, QObject *parent)
    : IOutboundUdp(parent), d(new Priv(this))
{
    d->server = node.server;
    d->serverPort = node.port;
    d->password = node.password.toUtf8();
    d->sni = node.sni.isEmpty() ? node.server : node.sni;
    d->skipVerify = node.skipCertVerify;
}

Hysteria2OutboundUdp::~Hysteria2OutboundUdp()
{
    delete d;
}

void Hysteria2OutboundUdp::associate(const QString &)
{
    d->ready = false;
    d->authed = false;
    d->closedEmitted = false;
    d->authRx.clear();
    d->sessionId = QRandomGenerator::global()->generate();
    d->nextPktId = 1;

    d->quic = new coastcore::QuicTransport(this);
    connect(d->quic, &coastcore::QuicTransport::connected, this, [this] { d->beginAuth(); });
    connect(d->quic, &coastcore::QuicTransport::connectionFailed, this,
            [this](const QString &r) { d->fail(r); });
    connect(d->quic, &coastcore::QuicTransport::connectionClosed, this, [this] {
        if (d->ready)
            d->emitClosed();
        else
            d->fail(QStringLiteral("hysteria2 udp connection closed before ready"));
    });
    connect(d->quic, &coastcore::QuicTransport::datagramReceived, this,
            [this](const QByteArray &dg) { d->onDatagram(dg); });
    d->quic->openConnection(d->server, d->serverPort, QByteArrayLiteral("h3"), d->sni, d->skipVerify);
}

void Hysteria2OutboundUdp::sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload)
{
    if (!d->ready || !d->quic)
        return;
    const QByteArray addr = hostPortString(dstIp, dstPort);
    const quint16 pktId = d->nextPktId++;

    // 头部固定开销:sessionId(4)+pktId(2)+fragId(1)+fragCount(1)+varint(addrLen)+addr。
    const QByteArray addrLenVi = quicVarint(quint64(addr.size()));
    const int headLen = 8 + addrLenVi.size() + addr.size();
    const int maxDg = d->quic->datagramMaxSendLength();
    const int budget = (maxDg > headLen + 1) ? (maxDg - headLen) : kUdpAddrMaxGuess;

    auto buildAndSend = [&](quint8 fragId, quint8 fragCount, const QByteArray &part) {
        QByteArray dg;
        dg.append(char((d->sessionId >> 24) & 0xFF));
        dg.append(char((d->sessionId >> 16) & 0xFF));
        dg.append(char((d->sessionId >> 8) & 0xFF));
        dg.append(char(d->sessionId & 0xFF));
        dg.append(char((pktId >> 8) & 0xFF));
        dg.append(char(pktId & 0xFF));
        dg.append(char(fragId));
        dg.append(char(fragCount));
        dg += addrLenVi;
        dg += addr;
        dg += part;
        d->quic->sendDatagram(dg);
    };

    if (payload.size() <= budget) {
        buildAndSend(0, 1, payload);
        return;
    }
    // 分片:每片 budget 字节, fragCount 片, fragId 从 0 递增(线格式见头文件)。
    const int fragCount = (payload.size() + budget - 1) / budget;
    if (fragCount > 255) // 单包超 255 片:放弃(极大 UDP 载荷, 实际不会出现)
        return;
    for (int i = 0; i < fragCount; ++i)
        buildAndSend(quint8(i), quint8(fragCount), payload.mid(i * budget, budget));
}

void Hysteria2OutboundUdp::closeSession()
{
    if (d->quic)
        d->quic->close();
    d->emitClosed();
}

bool Hysteria2OutboundUdp::isReady() const { return d->ready; }

// ============================ 注册 ============================

void registerHysteria2(OutboundRegistry &reg)
{
    auto tcp = [](const ProxyNode &n, QObject *p) -> IOutboundTcp * {
        return new Hysteria2OutboundTcp(n, p);
    };
    auto udp = [](const ProxyNode &n, QObject *p) -> IOutboundUdp * {
        return new Hysteria2OutboundUdp(n, p);
    };
    reg.registerProto(QStringLiteral("hysteria2"), tcp, udp);
    reg.registerProto(QStringLiteral("hy2"), tcp, udp); // 别名
}
