#include "Hysteria2Outbound.h"

#include "OutboundRegistry.h"
#include "../transport/QuicTransport.h"

#include <QDateTime>
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

// ————————————————————— 最小 QPACK 解码(只为解认证响应的 :status)—————————————————————
// 客户端在 SETTINGS 里把 QPACK 动态表容量声明为 0(见 buildControlStreamPreamble), 故服务器编码响应时
// **不得** 使用动态表 → 我们只需处理:静态表索引、带静态名引用的字面量、纯字面量。够解 :status。

// 解 HPACK/QPACK 前缀整数(RFC 7541 §5.1)。prefixBits=首字节低位可用位数(高位由调用方判读, 此处按掩码取)。
// 成功前移 pos 并写 *out;字节不足/溢出返回 false。
bool readPrefixedInt(const QByteArray &buf, int &pos, int prefixBits, quint64 *out)
{
    if (pos >= buf.size())
        return false;
    const quint64 maxPrefix = (quint64(1) << prefixBits) - 1;
    quint64 value = quint8(buf.at(pos)) & maxPrefix;
    ++pos;
    if (value < maxPrefix) {
        *out = value;
        return true;
    }
    int m = 0;
    while (true) {
        if (pos >= buf.size())
            return false;
        const quint8 b = quint8(buf.at(pos++));
        value += quint64(b & 0x7F) << m;
        m += 7;
        if (!(b & 0x80))
            break;
        if (m > 63)
            return false; // 防溢出
    }
    *out = value;
    return true;
}

// HPACK Huffman 解码 —— 仅支持「全数字」内容(HPACK 中 '0'..'9' 均是 5 位码, 5 位值即数字)。
// :status 恒为 3 位数字, 服务器常把它 Huffman 压成 2 字节, 故必须能解。非纯数字内容返回 false。
bool huffmanDecodeDigits(const QByteArray &raw, QByteArray *out)
{
    const int totalBits = raw.size() * 8;
    auto bitAt = [&](int i) -> int {
        return (quint8(raw.at(i >> 3)) >> (7 - (i & 7))) & 1;
    };
    QByteArray res;
    int bitPos = 0;
    while (bitPos < totalBits) {
        const int remaining = totalBits - bitPos;
        if (remaining < 8) { // 可能是结尾 EOS 填充(全 1, <8 位)
            bool allOnes = true;
            for (int i = bitPos; i < totalBits; ++i)
                if (bitAt(i) != 1) {
                    allOnes = false;
                    break;
                }
            if (allOnes)
                break;
        }
        if (bitPos + 5 > totalBits)
            return false; // 不足一个数字码又不是填充 → 非纯数字, 放弃
        int v = 0;
        for (int i = 0; i < 5; ++i)
            v = (v << 1) | bitAt(bitPos + i);
        if (v > 9)
            return false; // 不是数字码
        res.append(char('0' + v));
        bitPos += 5;
    }
    *out = res;
    return true;
}

// 解一个 QPACK 字符串:首字节含 H 标志(位 hMask)+ 长度前缀整数(prefixBits)。H=1→Huffman(仅数字), H=0→原样。
bool readQpackString(const QByteArray &buf, int &pos, int prefixBits, quint8 hMask, QByteArray *out)
{
    if (pos >= buf.size())
        return false;
    const bool huffman = (quint8(buf.at(pos)) & hMask) != 0;
    quint64 len = 0;
    if (!readPrefixedInt(buf, pos, prefixBits, &len))
        return false;
    if (len > quint64(buf.size() - pos)) // 防越界/超大
        return false;
    const QByteArray rawv = buf.mid(pos, int(len));
    pos += int(len);
    if (!huffman) {
        *out = rawv;
        return true;
    }
    return huffmanDecodeDigits(rawv, out);
}

// QPACK 静态表里 :status 值确定的项 → 其状态码(RFC 9204 附录 A);其余返回 -1。
int qpackStaticStatusValue(int idx)
{
    switch (idx) {
    case 24: return 103;
    case 25: return 200;
    case 26: return 304;
    case 27: return 404;
    case 28: return 503;
    case 63: return 100;
    case 64: return 204;
    case 65: return 206;
    case 66: return 302;
    case 67: return 400;
    case 68: return 403;
    case 69: return 421;
    case 70: return 425;
    case 71: return 500;
    default: return -1;
    }
}

// QPACK 静态表里 name==":status" 的项索引(用于「带名引用的字面量」判定名字是不是 :status)。
bool qpackStaticNameIsStatus(int idx)
{
    return (idx >= 24 && idx <= 28) || (idx >= 63 && idx <= 71);
}

// 从 HTTP/3 HEADERS 帧的 QPACK 字段块解出 :status。解不出返回 -1。
// Hysteria2 认证成功恒回 :status 233(HyOK);任何其它值(或解不出)都应视为认证失败。
int decodeH3AuthStatus(const QByteArray &block)
{
    int pos = 0;
    quint64 tmp = 0;
    if (!readPrefixedInt(block, pos, 8, &tmp)) // 字段段前缀: Required Insert Count(8 位前缀)
        return -1;
    if (!readPrefixedInt(block, pos, 7, &tmp)) // Base(最高位 S + 7 位前缀), 动态表禁用故忽略其值
        return -1;

    int status = -1;
    while (pos < block.size()) {
        const quint8 first = quint8(block.at(pos));
        if (first & 0x80) { // Indexed Field Line: 1 T <index(6)>
            const bool staticTable = (first & 0x40) != 0;
            quint64 idx = 0;
            if (!readPrefixedInt(block, pos, 6, &idx))
                return status;
            if (staticTable) {
                const int s = qpackStaticStatusValue(int(idx));
                if (s > 0)
                    status = s;
            }
        } else if (first & 0x40) { // Literal With Name Reference: 0 1 N T <nameIdx(4)> | value
            const bool staticTable = (first & 0x10) != 0;
            quint64 nameIdx = 0;
            if (!readPrefixedInt(block, pos, 4, &nameIdx))
                return status;
            QByteArray value;
            if (!readQpackString(block, pos, 7, 0x80, &value))
                return status;
            if (staticTable && qpackStaticNameIsStatus(int(nameIdx))) {
                bool ok = false;
                const int s = value.toInt(&ok);
                if (ok)
                    status = s;
            }
        } else if (first & 0x20) { // Literal With Literal Name: 0 0 1 N H <nameLen(3)> | name | value
            QByteArray name, value;
            if (!readQpackString(block, pos, 3, 0x08, &name))
                return status;
            if (!readQpackString(block, pos, 7, 0x80, &value))
                return status;
            if (name == ":status") {
                bool ok = false;
                const int s = value.toInt(&ok);
                if (ok)
                    status = s;
            }
        } else {
            // 0001/0000 = Post-Base(动态表)项 —— 声明容量 0 时不应出现, 无法安全跳过, 中止。
            return status;
        }
    }
    return status;
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

// —— 下行分片重组表的淘汰参数(防丢包长连接内存单调涨 + pktId 回绕跨包混帧)——
constexpr qint64 kReasmTtlMs = 10000;   // 单个 pktId 的分片集若 10s 内未集齐 → 丢弃
constexpr int kReasmMaxEntries = 256;   // 在途重组项硬上限

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
            if (msgLen > 0xFFFF) { // 防 int() 溢出/异常长度:TCPResponse 的 msg 不会有这么长
                fail(QStringLiteral("hysteria2 TCPResponse msg too long"));
                return;
            }
            if (respRx.size() < pos + int(msgLen))
                return;
            pos += int(msgLen);
            quint64 padLen = 0;
            if (!readVarint(respRx, pos, &padLen))
                return;
            if (padLen > 0xFFFF) {
                fail(QStringLiteral("hysteria2 TCPResponse pad too long"));
                return;
            }
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

    // HTTP/3 认证响应处理:解出 HEADERS 帧的 :status, 仅 233(HyOK)才算认证通过;否则(密码错等)
    //   立刻 fail —— 不再「见到 HEADERS 就当成功」而去开代理流等一个永不到来的 TCPResponse(旧 bug:挂到超时)。
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
            if (flen > quint64(authRx.size() - pos))
                break; // 帧体未收全(且防越界)
            if (ftype == 0x01) { // HEADERS = 认证响应
                const int status = decodeH3AuthStatus(authRx.mid(pos, int(flen)));
                if (status == 233) {
                    authed = true;
                    startProxyStream();
                } else {
                    fail(QStringLiteral("hysteria2 auth rejected (status=%1)").arg(status));
                }
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

    // 下行分片重组:key = pktId, 值 = (fragCount, 已收 fragId → payload)。ts=建项时刻(用于超时淘汰)。
    struct Reasm {
        quint8 fragCount = 0;
        QByteArray addr;
        QHash<quint8, QByteArray> frags;
        qint64 ts = 0;
    };
    QHash<quint16, Reasm> reasm;

    // 淘汰过期(超 TTL 未集齐)与超上限的重组项。每次收到分片时调用。
    void evictStaleReasm(qint64 nowMs)
    {
        for (auto it = reasm.begin(); it != reasm.end();) {
            if (nowMs - it->ts > kReasmTtlMs)
                it = reasm.erase(it);
            else
                ++it;
        }
        while (reasm.size() > kReasmMaxEntries) { // 仍超上限 → 删最旧
            auto oldest = reasm.begin();
            for (auto it = reasm.begin(); it != reasm.end(); ++it)
                if (it->ts < oldest->ts)
                    oldest = it;
            reasm.erase(oldest);
        }
    }

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
            if (flen > quint64(authRx.size() - pos))
                break; // 帧体未收全(且防越界)
            if (ftype == 0x01) { // HEADERS = 认证响应:必须 :status==233 才通过
                const int status = decodeH3AuthStatus(authRx.mid(pos, int(flen)));
                if (status == 233) {
                    authed = true;
                    ready = true;
                    emit q->ready();
                } else {
                    fail(QStringLiteral("hysteria2 udp auth rejected (status=%1)").arg(status));
                }
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
        if (addrLen > quint64(dg.size() - pos)) // 防 int() 溢出/越界
            return;
        const QByteArray addr = dg.mid(pos, int(addrLen));
        pos += int(addrLen);
        const QByteArray payload = dg.mid(pos);

        if (fragCount <= 1) {
            deliver(addr, payload);
            return;
        }
        if (fragId >= fragCount) // 越界 fragId:拒绝(否则会污染重组、或让 size()>=fragCount 误判集齐)
            return;

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        evictStaleReasm(nowMs);

        Reasm &r = reasm[pktId];
        // 新建项 / 陈旧残片 / fragCount 不一致 → 先清空重来。这一步同时消除 pktId(quint16)回绕后
        //   撞到旧残片导致的跨包混帧:回绕的新包 fragCount 或时序对不上, 旧残片被覆盖而非混入。
        if (r.fragCount != fragCount || (nowMs - r.ts) > kReasmTtlMs) {
            r.frags.clear();
            r.addr.clear();
            r.fragCount = fragCount;
            r.ts = nowMs;
        }
        if (r.addr.isEmpty())
            r.addr = addr;
        r.frags.insert(fragId, payload);
        if (r.frags.size() >= int(fragCount)) { // fragId<fragCount 已保证:集齐即 0..fragCount-1 全到
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
