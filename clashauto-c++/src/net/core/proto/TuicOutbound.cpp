#include "TuicOutbound.h"

#include "OutboundRegistry.h"
#include "../transport/QuicTransport.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QHash>
#include <QRandomGenerator>

// 说明：TUIC v5 线格式见头文件。TCP=双向流(Connect 头 + 裸管道), UDP=数据报(Packet, 分片重组)。
// 每个出站对象各自起一条 QUIC 连接并单独认证(同 Hy2 的取舍, 共享连接复用属未来优化)。

namespace {

constexpr quint8 kVer = 0x05;
constexpr quint8 kCmdAuthenticate = 0x00;
constexpr quint8 kCmdConnect = 0x01;
constexpr quint8 kCmdPacket = 0x02;
constexpr int kUdpHeadGuess = 512;

// —— 下行分片重组表的淘汰参数(防丢包长连接内存单调涨 + pktId 回绕跨包混帧)——
constexpr qint64 kReasmTtlMs = 10000; // 分片集 10s 内未集齐 → 丢弃
constexpr int kReasmMaxEntries = 256; // 在途重组项硬上限

// 端口大端 2 字节
void appendBe16(QByteArray &b, quint16 v)
{
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}

// TUIC 地址编码:TYPE(1) | ADDR | PORT(2 大端)。域名 0x00 = 1 字节长度 + utf8;v4 0x01=4;v6 0x02=16。
QByteArray encodeTuicAddr(const QString &host, quint16 port)
{
    QByteArray r;
    QHostAddress a;
    if (a.setAddress(host) && a.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ip = a.toIPv4Address();
        r.append(char(0x01));
        r.append(char((ip >> 24) & 0xFF));
        r.append(char((ip >> 16) & 0xFF));
        r.append(char((ip >> 8) & 0xFF));
        r.append(char(ip & 0xFF));
    } else if (a.setAddress(host) && a.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = a.toIPv6Address();
        r.append(char(0x02));
        r.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        const QByteArray h = host.toUtf8();
        r.append(char(0x00));
        r.append(char(h.size() & 0xFF));
        r.append(h);
    }
    appendBe16(r, port);
    return r;
}

QByteArray encodeTuicAddr(const QHostAddress &ip, quint16 port)
{
    QByteArray r;
    if (ip.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = ip.toIPv6Address();
        r.append(char(0x02));
        r.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        const quint32 v4 = ip.toIPv4Address();
        r.append(char(0x01));
        r.append(char((v4 >> 24) & 0xFF));
        r.append(char((v4 >> 16) & 0xFF));
        r.append(char((v4 >> 8) & 0xFF));
        r.append(char(v4 & 0xFF));
    }
    appendBe16(r, port);
    return r;
}

// 从 buf[pos] 解 TUIC 地址。成功前移 pos, 写出 ip/port, 返回 true;不完整/不支持返回 false。
// 仅解出 v4/v6(下行回程恒为 IP);域名类型精确消费其字节但不产出 ip(haveIp=false)。
bool decodeTuicAddr(const QByteArray &buf, int &pos, QHostAddress *ip, quint16 *port, bool *haveIp)
{
    *haveIp = false;
    if (pos + 1 > buf.size())
        return false;
    const quint8 type = quint8(buf.at(pos));
    int p = pos + 1;
    if (type == 0xff) { // None: 无地址无端口
        pos = p;
        *port = 0;
        return true;
    }
    if (type == 0x00) { // domain
        if (p + 1 > buf.size())
            return false;
        const int len = quint8(buf.at(p));
        p += 1;
        if (p + len + 2 > buf.size())
            return false;
        p += len; // 跳过域名(回程用不到)
        *port = quint16((quint8(buf.at(p)) << 8) | quint8(buf.at(p + 1)));
        p += 2;
        pos = p;
        return true;
    }
    if (type == 0x01) { // ipv4
        if (p + 4 + 2 > buf.size())
            return false;
        const quint32 v4 = (quint32(quint8(buf.at(p))) << 24) | (quint32(quint8(buf.at(p + 1))) << 16)
                         | (quint32(quint8(buf.at(p + 2))) << 8) | quint32(quint8(buf.at(p + 3)));
        *ip = QHostAddress(v4);
        *haveIp = true;
        p += 4;
        *port = quint16((quint8(buf.at(p)) << 8) | quint8(buf.at(p + 1)));
        p += 2;
        pos = p;
        return true;
    }
    if (type == 0x02) { // ipv6
        if (p + 16 + 2 > buf.size())
            return false;
        Q_IPV6ADDR v6;
        memcpy(v6.c, buf.constData() + p, 16);
        *ip = QHostAddress(v6);
        *haveIp = true;
        p += 16;
        *port = quint16((quint8(buf.at(p)) << 8) | quint8(buf.at(p + 1)));
        p += 2;
        pos = p;
        return true;
    }
    return false; // 未知地址类型
}

} // namespace

// ============================ TuicOutboundTcp ============================

class TuicOutboundTcp::Priv
{
public:
    explicit Priv(TuicOutboundTcp *owner) : q(owner) {}

    TuicOutboundTcp *q = nullptr;

    QString server;
    quint16 serverPort = 0;
    QByteArray uuid;     // 16 字节
    QByteArray password; // 明文
    QString sni;
    QByteArray alpn;
    bool skipVerify = false;

    QString dstHost;
    quint16 dstPort = 0;

    coastcore::QuicTransport *quic = nullptr;
    coastcore::QuicStream *authS = nullptr;
    coastcore::QuicStream *proxy = nullptr;
    QByteArray pending;
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

    void onConnected()
    {
        // 1) 认证:导出 token 并经单向流发 Authenticate。
        if (uuid.size() != 16) { // fromHex 解析失败/UUID 非法 → 命令头长度会错, 提前失败
            fail(QStringLiteral("tuic: invalid uuid (need 16 bytes)"));
            return;
        }
        if (!coastcore::QuicTransport::keyingMaterialSupported()) {
            fail(QStringLiteral("tuic: msquic keying material export unavailable (need preview/v2.6+)"));
            return;
        }
        const QByteArray token = quic->exportKeyingMaterial(uuid, password, 32);
        if (token.size() != 32) {
            fail(QStringLiteral("tuic: keying material export failed"));
            return;
        }
        QByteArray auth;
        auth.append(char(kVer));
        auth.append(char(kCmdAuthenticate));
        auth += uuid;
        auth += token;
        authS = quic->openUniStream();
        authS->send(auth, /*fin=*/true); // 一次性命令, 发完即关发送方向

        // 2) TCP:开双向流, 发 Connect 头, 即视为 established。
        proxy = quic->openBidiStream();
        QObject::connect(proxy, &coastcore::QuicStream::dataReceived, q,
                         [this](const QByteArray &b) {
                             if (!readPaused && !b.isEmpty())
                                 emit q->dataReceived(b);
                         });
        QObject::connect(proxy, &coastcore::QuicStream::sendCompleted, q,
                         [this](qint64 n) { emit q->upstreamBytesWritten(n); });
        QObject::connect(proxy, &coastcore::QuicStream::peerSendShutdown, q,
                         [this] { emitClosed(); });
        QObject::connect(proxy, &coastcore::QuicStream::closed, q, [this] { emitClosed(); });
        QObject::connect(proxy, &coastcore::QuicStream::failed, q, [this](const QString &r) {
            if (established)
                emitClosed();
            else
                fail(r);
        });

        QByteArray connectCmd;
        connectCmd.append(char(kVer));
        connectCmd.append(char(kCmdConnect));
        connectCmd += encodeTuicAddr(dstHost, dstPort);
        connectCmd += pending; // 初始上行捎带
        pending.clear();
        proxy->send(connectCmd);

        established = true;
        emit q->established();
    }
};

TuicOutboundTcp::TuicOutboundTcp(const ProxyNode &node, QObject *parent)
    : IOutboundTcp(parent), d(new Priv(this))
{
    d->server = node.server;
    d->serverPort = node.port;
    d->uuid = QByteArray::fromHex(QByteArray(node.uuid.toUtf8()).replace('-', "")); // 16 字节
    d->password = node.password.toUtf8();
    d->sni = node.sni.isEmpty() ? node.server : node.sni;
    d->alpn = QByteArrayLiteral("h3"); // ★缺省;应由配置(alpn)提供
    d->skipVerify = node.skipCertVerify;
}

TuicOutboundTcp::~TuicOutboundTcp()
{
    delete d;
}

void TuicOutboundTcp::connectTo(const QString &dstHost, quint16 dstPort, const QString &)
{
    d->dstHost = dstHost;
    d->dstPort = dstPort;
    d->established = false;
    d->closedEmitted = false;
    d->readPaused = false;
    // ★ 不清 pending —— 拨号被 NetStack 推迟一拍，首个数据段可能已经 write() 进来了。
    //   契约见 IOutbound.h 的 connectTo 注释。

    d->quic = new coastcore::QuicTransport(this);
    connect(d->quic, &coastcore::QuicTransport::connected, this, [this] { d->onConnected(); });
    connect(d->quic, &coastcore::QuicTransport::connectionFailed, this,
            [this](const QString &r) { d->fail(r); });
    connect(d->quic, &coastcore::QuicTransport::connectionClosed, this, [this] {
        if (d->established)
            d->emitClosed();
        else
            d->fail(QStringLiteral("tuic 未建立即被关闭: %1")
                        .arg(d->quic->lastCloseReason()));
    });
    d->quic->openConnection(d->server, d->serverPort, d->alpn, d->sni, d->skipVerify);
}

void TuicOutboundTcp::write(const QByteArray &data)
{
    if (d->established && d->proxy)
        d->proxy->send(data);
    else
        d->pending += data;
}

void TuicOutboundTcp::write(const char *data, qsizetype size)
{
    if (!data || size <= 0)
        return;
    if (d->established && d->proxy)
        d->proxy->send(QByteArray(data, size));
    else
        d->pending.append(data, size);
}

void TuicOutboundTcp::closeTunnel()
{
    if (d->quic)
        d->quic->close();
    d->emitClosed();
}

bool TuicOutboundTcp::isEstablished() const { return d->established; }

qint64 TuicOutboundTcp::bytesToWrite() const
{
    return qint64(d->pending.size()) + (d->proxy ? d->proxy->bytesInFlight() : 0);
}

void TuicOutboundTcp::setReadPaused(bool paused)
{
    if (d->readPaused == paused)
        return;
    d->readPaused = paused;
    if (d->proxy)
        d->proxy->setReceiveEnabled(!paused);
}

bool TuicOutboundTcp::isReadPaused() const { return d->readPaused; }

// ============================ TuicOutboundUdp ============================

class TuicOutboundUdp::Priv
{
public:
    explicit Priv(TuicOutboundUdp *owner) : q(owner) {}

    TuicOutboundUdp *q = nullptr;

    QString server;
    quint16 serverPort = 0;
    QByteArray uuid;
    QByteArray password;
    QString sni;
    QByteArray alpn;
    bool skipVerify = false;

    coastcore::QuicTransport *quic = nullptr;
    coastcore::QuicStream *authS = nullptr;
    bool ready = false;
    bool closedEmitted = false;

    quint16 assocId = 0;
    quint16 nextPktId = 1;

    struct Reasm {
        quint8 fragTotal = 0;
        QHostAddress ip;
        quint16 port = 0;
        bool haveAddr = false;
        QHash<quint8, QByteArray> frags;
        qint64 ts = 0; // 建项时刻, 用于超时淘汰
    };
    QHash<quint16, Reasm> reasm; // key = pktId

    // 淘汰过期(超 TTL 未集齐)与超上限的重组项。每次收到分片时调用。
    void evictStaleReasm(qint64 nowMs)
    {
        for (auto it = reasm.begin(); it != reasm.end();) {
            if (nowMs - it->ts > kReasmTtlMs)
                it = reasm.erase(it);
            else
                ++it;
        }
        while (reasm.size() > kReasmMaxEntries) {
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

    void onConnected()
    {
        if (uuid.size() != 16) {
            fail(QStringLiteral("tuic udp: invalid uuid (need 16 bytes)"));
            return;
        }
        if (!coastcore::QuicTransport::keyingMaterialSupported()) {
            fail(QStringLiteral("tuic udp: msquic keying material export unavailable"));
            return;
        }
        const QByteArray token = quic->exportKeyingMaterial(uuid, password, 32);
        if (token.size() != 32) {
            fail(QStringLiteral("tuic udp: keying material export failed"));
            return;
        }
        QByteArray auth;
        auth.append(char(kVer));
        auth.append(char(kCmdAuthenticate));
        auth += uuid;
        auth += token;
        authS = quic->openUniStream();
        authS->send(auth, true);

        ready = true;
        emit q->ready();
    }

    // 下行 Packet 数据报 → 解析 + 重组。
    void onDatagram(const QByteArray &dg)
    {
        // VER|TYPE|ASSOC_ID(2)|PKT_ID(2)|FRAG_TOTAL(1)|FRAG_ID(1)|SIZE(2)|ADDR||payload
        if (dg.size() < 2 || quint8(dg.at(0)) != kVer || quint8(dg.at(1)) != kCmdPacket)
            return;
        int pos = 2;
        if (pos + 8 > dg.size())
            return;
        // assoc_id(2) —— 本会话唯一, 不强校验
        pos += 2;
        const quint16 pktId = quint16((quint8(dg.at(pos)) << 8) | quint8(dg.at(pos + 1)));
        pos += 2;
        const quint8 fragTotal = quint8(dg.at(pos++));
        const quint8 fragId = quint8(dg.at(pos++));
        const quint16 size = quint16((quint8(dg.at(pos)) << 8) | quint8(dg.at(pos + 1)));
        pos += 2;
        // 地址只在 fragId==0 时出现(TUIC:分片仅首片带地址)。
        QHostAddress ip;
        quint16 port = 0;
        bool haveIp = false;
        if (fragId == 0) {
            if (!decodeTuicAddr(dg, pos, &ip, &port, &haveIp))
                return;
        }
        if (pos + int(size) > dg.size())
            return;
        const QByteArray payload = dg.mid(pos, int(size));

        if (fragTotal <= 1) {
            if (haveIp)
                emit q->datagramReceived(ip, port, payload);
            return;
        }
        if (fragId >= fragTotal) // 越界 fragId:拒绝(否则污染重组 / 让 size()>=fragTotal 误判集齐)
            return;

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        evictStaleReasm(nowMs);

        Reasm &r = reasm[pktId];
        // 新建 / 陈旧残片 / fragTotal 不一致 → 先清空重来。同时消除 pktId(quint16)回绕撞旧残片的跨包混帧。
        if (r.fragTotal != fragTotal || (nowMs - r.ts) > kReasmTtlMs) {
            r.frags.clear();
            r.haveAddr = false;
            r.fragTotal = fragTotal;
            r.ts = nowMs;
        }
        if (fragId == 0) {
            r.ip = ip;
            r.port = port;
            r.haveAddr = haveIp;
        }
        r.frags.insert(fragId, payload);
        if (r.frags.size() >= int(fragTotal)) { // fragId<fragTotal 已保证:集齐即全片到达
            QByteArray full;
            for (quint8 i = 0; i < fragTotal; ++i)
                full += r.frags.value(i);
            const bool ha = r.haveAddr;
            const QHostAddress a = r.ip;
            const quint16 pr = r.port;
            reasm.remove(pktId);
            if (ha)
                emit q->datagramReceived(a, pr, full);
        }
    }
};

TuicOutboundUdp::TuicOutboundUdp(const ProxyNode &node, QObject *parent)
    : IOutboundUdp(parent), d(new Priv(this))
{
    d->server = node.server;
    d->serverPort = node.port;
    d->uuid = QByteArray::fromHex(QByteArray(node.uuid.toUtf8()).replace('-', ""));
    d->password = node.password.toUtf8();
    d->sni = node.sni.isEmpty() ? node.server : node.sni;
    d->alpn = QByteArrayLiteral("h3");
    d->skipVerify = node.skipCertVerify;
}

TuicOutboundUdp::~TuicOutboundUdp()
{
    delete d;
}

void TuicOutboundUdp::associate(const QString &)
{
    d->ready = false;
    d->closedEmitted = false;
    d->assocId = quint16(QRandomGenerator::global()->bounded(0x10000));
    d->nextPktId = 1;

    d->quic = new coastcore::QuicTransport(this);
    connect(d->quic, &coastcore::QuicTransport::connected, this, [this] { d->onConnected(); });
    connect(d->quic, &coastcore::QuicTransport::connectionFailed, this,
            [this](const QString &r) { d->fail(r); });
    connect(d->quic, &coastcore::QuicTransport::connectionClosed, this, [this] {
        if (d->ready)
            d->emitClosed();
        else
            d->fail(QStringLiteral("tuic udp connection closed before ready"));
    });
    connect(d->quic, &coastcore::QuicTransport::datagramReceived, this,
            [this](const QByteArray &dg) { d->onDatagram(dg); });
    d->quic->openConnection(d->server, d->serverPort, d->alpn, d->sni, d->skipVerify);
}

void TuicOutboundUdp::sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload)
{
    if (!d->ready || !d->quic)
        return;
    const QByteArray addr = encodeTuicAddr(dstIp, dstPort);
    const quint16 pktId = d->nextPktId++;

    // 首片头开销:VER(1)+TYPE(1)+ASSOC(2)+PKT(2)+FTOTAL(1)+FID(1)+SIZE(2)+ADDR;后续片不带地址(ADDR=None 0xff)。
    const int head0 = 10 + addr.size();
    const int headN = 10 + 1; // 后续片地址用 0xff(None), 1 字节
    const int maxDg = d->quic->datagramMaxSendLength();
    const int budget0 = (maxDg > head0 + 1) ? (maxDg - head0) : kUdpHeadGuess;
    const int budgetN = (maxDg > headN + 1) ? (maxDg - headN) : kUdpHeadGuess;

    auto header = [&](quint8 fragTotal, quint8 fragId, quint16 size, bool withAddr) {
        QByteArray dg;
        dg.append(char(kVer));
        dg.append(char(kCmdPacket));
        appendBe16(dg, d->assocId);
        appendBe16(dg, pktId);
        dg.append(char(fragTotal));
        dg.append(char(fragId));
        appendBe16(dg, size);
        if (withAddr)
            dg += addr;
        else
            dg.append(char(0xff)); // None 地址
        return dg;
    };

    if (payload.size() <= budget0) {
        QByteArray dg = header(1, 0, quint16(payload.size()), true);
        dg += payload;
        d->quic->sendDatagram(dg);
        return;
    }
    // 分片:首片 budget0, 其余 budgetN。
    QList<QByteArray> parts;
    int off = 0;
    parts.append(payload.mid(0, budget0));
    off = budget0;
    while (off < payload.size()) {
        parts.append(payload.mid(off, budgetN));
        off += budgetN;
    }
    if (parts.size() > 255)
        return;
    const quint8 total = quint8(parts.size());
    for (int i = 0; i < parts.size(); ++i) {
        QByteArray dg = header(total, quint8(i), quint16(parts[i].size()), i == 0);
        dg += parts[i];
        d->quic->sendDatagram(dg);
    }
}

void TuicOutboundUdp::closeSession()
{
    if (d->quic)
        d->quic->close();
    d->emitClosed();
}

bool TuicOutboundUdp::isReady() const { return d->ready; }

// ============================ 注册 ============================

void registerTuic(OutboundRegistry &reg)
{
    reg.registerProto(
        QStringLiteral("tuic"),
        [](const ProxyNode &n, QObject *p) -> IOutboundTcp * { return new TuicOutboundTcp(n, p); },
        [](const ProxyNode &n, QObject *p) -> IOutboundUdp * { return new TuicOutboundUdp(n, p); });
}
