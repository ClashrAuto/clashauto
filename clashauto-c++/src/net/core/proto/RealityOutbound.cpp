#include "RealityOutbound.h"

#include "OutboundRegistry.h"
#include "../transport/UtlsClient.h"

#include <QMetaObject>
#include <QTimer>

// 说明：本单元 = 「VLESS 内层 + UtlsClient(REALITY TLS1.3) 传输」。承载流逻辑刻意对齐 VlessOutbound 的
// TLS 路径（发头/剥响应头/背压/信号时序），只把底层 TlsClient 换成 UtlsClient 并强制 REALITY 认证。
//
// ★ type 归一：主控把订阅里的 `type:vless + reality-opts` 映射进 ProxyNode 时应把 type 设为 "reality"
//   （见文件尾 registerReality 与本仓报告）。
//
// ★★ REALITY 所需字段 ★★：ProxyConfig.h 现已有 realityPublicKey / realityShortId / fingerprint / flow，
//   parseRealityParams 直接读取并解码：
//     · realityPublicKey —— REALITY 服务端 X25519 公钥（配置里是 base64 或 hex；解码成 32B 裸公钥）
//     · realityShortId   —— REALITY shortId（配置里是 hex；解码成 0..8B 裸字节，可空）
//     · fingerprint      —— uTLS 指纹（"chrome"/…；空=chrome）
//     · flow             —— "xtls-rprx-vision"（本单元未实现 vision 流量整形，见 TODO）
//   publicKey 缺失/解码后非 32B 时 parseRealityParams().valid==false，start() 会诚实失败并给出明确原因
//   （绝不发无效认证）。握手末尾还会核验服务端证书 AuthKey（见 UtlsClient::verifyRealityCertificate）。

namespace {

bool isHex(QChar c)
{
    const ushort u = c.unicode();
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

bool parseUuid(const QString &uuid, QByteArray &out)
{
    QString hex = uuid.trimmed();
    hex.remove(QLatin1Char('-'));
    if (hex.size() != 32)
        return false;
    for (const QChar c : hex)
        if (!isHex(c))
            return false;
    out = QByteArray::fromHex(hex.toLatin1());
    return out.size() == 16;
}

// 从 ProxyNode 抽 REALITY 参数（realityPublicKey/realityShortId/fingerprint/flow 均已在 ProxyConfig.h）。
RealityParams parseRealityParams(const ProxyNode &n)
{
    RealityParams rp;
    rp.server = n.server;
    rp.port = n.port;
    rp.uuid = n.uuid;
    rp.sni = n.sni.isEmpty() ? n.server : n.sni; // dest / ServerName
    rp.fingerprint = n.fingerprint.isEmpty() ? QStringLiteral("chrome") : n.fingerprint;
    rp.flow = n.flow;

    // REALITY 公钥（x25519,32B）：配置里常见 base64,也有 hex。先按 base64 解,长度不对再退回 hex。
    const QByteArray pkRaw = n.realityPublicKey.trimmed().toLatin1();
    if (!pkRaw.isEmpty()) {
        QByteArray pk = QByteArray::fromBase64(pkRaw, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        if (pk.size() != 32)
            pk = QByteArray::fromBase64(pkRaw); // 标准 base64
        if (pk.size() != 32)
            pk = QByteArray::fromHex(pkRaw);    // 退回 hex
        rp.publicKey = pk;
    }
    // short-id：hex,0..8 字节(可空)。
    rp.shortId = QByteArray::fromHex(n.realityShortId.trimmed().toLatin1());

    rp.valid = (rp.publicKey.size() == 32); // 公钥就绪才能真正做 REALITY 认证
    return rp;
}

} // namespace

// ============================ RealityStream ============================

RealityStream::RealityStream(const ProxyNode &node, QObject *parent) : QObject(parent), m_node(node)
{
    m_uuidValid = parseUuid(m_node.uuid, m_uuid16);
    m_rp = parseRealityParams(m_node);
}

QByteArray RealityStream::buildVlessHeader() const
{
    // 与 VlessOutbound::buildHeader 逐字节一致（VLESS 内层格式）。
    QByteArray h;
    h.append(char(0x00)); // 版本
    h.append(m_uuid16);   // 16B uuid
    h.append(char(0x00)); // addonLen（不发 addon；vision flow 需在此协商，未实现）
    h.append(char(m_cmd));
    h.append(char((m_dstPort >> 8) & 0xFF));
    h.append(char(m_dstPort & 0xFF));

    QHostAddress addr;
    if (addr.setAddress(m_dstHost) && addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ip = addr.toIPv4Address();
        h.append(char(0x01));
        h.append(char((ip >> 24) & 0xFF));
        h.append(char((ip >> 16) & 0xFF));
        h.append(char((ip >> 8) & 0xFF));
        h.append(char(ip & 0xFF));
    } else if (addr.setAddress(m_dstHost) && addr.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = addr.toIPv6Address();
        h.append(char(0x03));
        h.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        const QByteArray host = m_dstHost.toUtf8();
        h.append(char(0x02));
        h.append(char(host.size() & 0xFF));
        h.append(host);
    }
    return h;
}

void RealityStream::start(quint8 cmd, const QString &dstHost, quint16 dstPort)
{
    m_cmd = cmd;
    m_dstHost = dstHost;
    m_dstPort = dstPort;

    if (!m_uuidValid) {
        QTimer::singleShot(0, this, [this] { finishFailed(QStringLiteral("reality: 无效 uuid")); });
        return;
    }
    if (!m_rp.valid) {
        // publicKey 缺失或解码后非 32B——诚实失败，绝不发无效认证。
        QTimer::singleShot(0, this, [this] {
            finishFailed(QStringLiteral("reality: publicKey 无效（realityPublicKey 缺失或非 32B x25519 公钥）"));
        });
        return;
    }
    if (!m_rp.flow.isEmpty()) {
        // xtls-rprx-vision：内层需在裸 TLS 上做额外的流量整形/直通判定，本单元未实现。
        // 不阻断（仍按普通 VLESS-over-REALITY 走），但记为 TODO —— 与真机 vision 服务端可能不兼容。
    }

    m_tls = new coastcore::UtlsClient(this);
    m_tls->setFingerprint(coastcore::UtlsClient::Fingerprint::Chrome);
    m_tls->setReality(m_rp.publicKey, m_rp.shortId);
    connect(m_tls, &coastcore::UtlsClient::connected, this, [this] { onTlsUp(); });
    connect(m_tls, &coastcore::UtlsClient::readyRead, this, [this] { onTlsReadyRead(); });
    connect(m_tls, &coastcore::UtlsClient::errorOccurred, this,
            [this](const QString &r) { onTransportError(r); });
    connect(m_tls, &coastcore::UtlsClient::disconnected, this, [this] { onTransportClosed(); });

    // SNI = dest（伪装目标站）；ALPN 留空（REALITY+VLESS 常态）。
    m_tls->connectToHost(m_rp.server, m_rp.port, m_rp.sni, {});
    m_tls->setReadBufferSize(64 * 1024); // 背压上限，对齐 Vless/Direct

    // 底层 socket 排空进度 → 供上层归还接收窗口。
    if (QAbstractSocket *s = m_tls->socket()) {
        connect(s, &QAbstractSocket::bytesWritten, this, [this](qint64 n) { emit wrote(n); });
    }
}

void RealityStream::onTlsUp()
{
    if (m_streamReady || m_finished)
        return;
    m_streamReady = true;
    QByteArray first = buildVlessHeader();
    first += m_pendingUp;
    m_pendingUp.clear();
    writeCarrier(first);
    emit ready();
}

void RealityStream::writeCarrier(const QByteArray &b)
{
    if (m_tls)
        m_tls->write(b);
}

void RealityStream::write(const QByteArray &data)
{
    if (m_streamReady)
        writeCarrier(data);
    else
        m_pendingUp += data;
}

void RealityStream::write(const char *data, qsizetype size)
{
    if (!data || size <= 0)
        return;
    if (!m_streamReady) {
        m_pendingUp.append(data, size); // 深拷贝（同 Vless/Direct 的生命周期契约）
        return;
    }
    if (m_tls)
        m_tls->write(QByteArray::fromRawData(data, size)); // UtlsClient::write 同步拷入，data 只需活到返回
}

qint64 RealityStream::bytesToWrite() const
{
    qint64 t = qint64(m_pendingUp.size());
    if (m_tls)
        t += m_tls->bytesToWrite();
    return t;
}

void RealityStream::onTlsReadyRead()
{
    if (m_readPaused || !m_tls)
        return;
    const QByteArray d = m_tls->readAll();
    if (!d.isEmpty())
        feedDown(d);
}

void RealityStream::feedDown(QByteArray chunk)
{
    if (m_finished)
        return;
    if (!m_respHeaderDone) {
        m_respBuf += chunk;
        if (m_respBuf.size() < 2)
            return;
        const int addonLen = int(quint8(m_respBuf.at(1)));
        const int need = 2 + addonLen;
        if (m_respBuf.size() < need)
            return;
        QByteArray rest = m_respBuf.mid(need);
        m_respBuf.clear();
        m_respHeaderDone = true;
        if (rest.isEmpty())
            return;
        chunk = rest;
    }
    if (m_readPaused) {
        m_downPending += chunk;
        return;
    }
    if (!chunk.isEmpty())
        emit data(chunk);
}

void RealityStream::setReadPaused(bool paused)
{
    if (paused) {
        m_readPaused = true;
        // ★ 透传到底层：让 UtlsClient 真正停止从 socket 抽字节（TCP 窗口自然关），否则它会继续解密
        //   并把明文堆进 m_appReadBuf → 慢消费者下无界增长。这才是真背压（对齐 TlsClient/Socks5）。
        if (m_tls)
            m_tls->setReadPaused(true);
        return;
    }
    if (!m_readPaused || m_resumeScheduled)
        return;
    // 恢复延到下一拍（同 Vless/Direct：补读会 emit data，上层槽可能当场拆连接）。
    m_resumeScheduled = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_resumeScheduled = false;
            if (!m_readPaused || m_finished)
                return;
            m_readPaused = false;
            // 先 flush 暂停期间缓存的较旧下行，保证顺序。
            if (!m_downPending.isEmpty()) {
                const QByteArray p = m_downPending;
                m_downPending.clear();
                emit data(p);
            }
            if (m_finished)
                return;
            // 再解除底层读暂停：UtlsClient 会补抽 socket、解密并（经 readyRead）按序下发较新的字节。
            if (m_tls)
                m_tls->setReadPaused(false);
            if (m_finished)
                return;
            // 兜底：UtlsClient 是「解密即缓存」的推模型，缓冲里可能仍有已解密存货，再补读一次。
            if (m_tls && m_tls->bytesAvailable() > 0)
                feedDown(m_tls->readAll());
        },
        Qt::QueuedConnection);
}

void RealityStream::onTransportError(const QString &reason)
{
    if (m_streamReady)
        finishClosed();
    else
        finishFailed(reason);
}

void RealityStream::onTransportClosed()
{
    if (m_streamReady)
        finishClosed();
    else
        finishFailed(QStringLiteral("reality: 传输在流建立前关闭"));
}

void RealityStream::finishFailed(const QString &reason)
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_tls)
        m_tls->abort();
    emit failed(reason);
}

void RealityStream::finishClosed()
{
    if (m_finished)
        return;
    m_finished = true;
    emit closed();
}

void RealityStream::abort()
{
    if (m_tls)
        m_tls->abort();
    finishClosed();
}

// ============================ RealityOutboundTcp ============================

RealityOutboundTcp::RealityOutboundTcp(const ProxyNode &node, QObject *parent)
    : IOutboundTcp(parent), m_node(node)
{
}

void RealityOutboundTcp::connectTo(const QString &dstHost, quint16 dstPort, const QString & /*user*/)
{
    m_established = false;
    m_closedEmitted = false;

    m_stream = new RealityStream(m_node, this);
    connect(m_stream, &RealityStream::ready, this, [this] {
        m_established = true;
        emit established();
    });
    connect(m_stream, &RealityStream::data, this,
            [this](const QByteArray &d) { emit dataReceived(d); });
    connect(m_stream, &RealityStream::wrote, this,
            [this](qint64 n) { emit upstreamBytesWritten(n); });
    connect(m_stream, &RealityStream::failed, this, [this](const QString &reason) {
        if (m_closedEmitted)
            return;
        m_closedEmitted = true;
        emit failed(reason);
    });
    connect(m_stream, &RealityStream::closed, this, [this] { emitClosed(); });

    // 拨号前就 write() 进来的字节（NetStack 推迟一拍拨号导致，契约见 IOutbound.h）：承载流
    // 未就绪时它自己会再缓冲一层，这里只负责把顺序原样交接过去，不能丢。
    if (!m_preDial.isEmpty()) {
        m_stream->write(m_preDial);
        m_preDial.clear();
    }

    m_stream->start(0x01, dstHost, dstPort);
}

void RealityOutboundTcp::write(const QByteArray &data)
{
    if (m_stream)
        m_stream->write(data);
    else
        m_preDial += data; // 还没 connectTo：先存着，connectTo 里交接（早先是直接丢弃）
}

void RealityOutboundTcp::write(const char *data, qsizetype size)
{
    if (!data || size <= 0)
        return;
    if (m_stream)
        m_stream->write(data, size);
    else
        m_preDial.append(data, size); // 深拷贝：字节要留到 connectTo 之后（生命周期契约见 IOutbound.h）
}

void RealityOutboundTcp::closeTunnel()
{
    if (m_stream)
        m_stream->abort();
    emitClosed();
}

bool RealityOutboundTcp::isEstablished() const
{
    return m_established;
}

qint64 RealityOutboundTcp::bytesToWrite() const
{
    // 拨号前缓冲的字节也要算进水位，否则上层会误以为上行已排空而把 lwIP 接收窗口开满。
    return qint64(m_preDial.size()) + (m_stream ? m_stream->bytesToWrite() : 0);
}

void RealityOutboundTcp::setReadPaused(bool paused)
{
    if (m_stream)
        m_stream->setReadPaused(paused);
}

bool RealityOutboundTcp::isReadPaused() const
{
    return m_stream ? m_stream->isReadPaused() : false;
}

void RealityOutboundTcp::emitClosed()
{
    if (m_closedEmitted)
        return;
    m_closedEmitted = true;
    emit closed();
}

// ============================ RealityOutboundUdp ============================

RealityOutboundUdp::RealityOutboundUdp(const ProxyNode &node, QObject *parent)
    : IOutboundUdp(parent), m_node(node)
{
}

void RealityOutboundUdp::associate(const QString & /*user*/)
{
    m_ready = false;
    m_closedEmitted = false;
    m_streamStarted = false;
    m_downAcc.clear();
    QTimer::singleShot(0, this, [this] {
        if (m_closedEmitted)
            return;
        m_ready = true;
        emit ready();
    });
}

void RealityOutboundUdp::sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload)
{
    if (!m_ready || m_closedEmitted)
        return;

    if (!m_streamStarted) {
        m_streamStarted = true;
        m_targetIp = dstIp;
        m_targetPort = dstPort;
        m_stream = new RealityStream(m_node, this);
        connect(m_stream, &RealityStream::data, this,
                [this](const QByteArray &chunk) { onStreamData(chunk); });
        connect(m_stream, &RealityStream::failed, this, [this](const QString &) { emitClosed(); });
        connect(m_stream, &RealityStream::closed, this, [this] { emitClosed(); });
        m_stream->start(0x02, dstIp.toString(), dstPort);
    }

    // TODO(单目标局限，同 VLESS)：同一会话发往其它目标的包会落到首个目标。多目标需 XUDP，未实现。
    QByteArray frame;
    frame.append(char((payload.size() >> 8) & 0xFF));
    frame.append(char(payload.size() & 0xFF));
    frame.append(payload);
    m_stream->write(frame);
}

void RealityOutboundUdp::onStreamData(const QByteArray &chunk)
{
    m_downAcc += chunk;
    for (;;) {
        if (m_downAcc.size() < 2)
            return;
        const int len = (int(quint8(m_downAcc.at(0))) << 8) | int(quint8(m_downAcc.at(1)));
        if (m_downAcc.size() < 2 + len)
            return;
        const QByteArray p = m_downAcc.mid(2, len);
        m_downAcc.remove(0, 2 + len);
        emit datagramReceived(m_targetIp, m_targetPort, p);
    }
}

void RealityOutboundUdp::closeSession()
{
    if (m_stream)
        m_stream->abort();
    emitClosed();
}

bool RealityOutboundUdp::isReady() const
{
    return m_ready;
}

void RealityOutboundUdp::emitClosed()
{
    if (m_closedEmitted)
        return;
    m_closedEmitted = true;
    emit closed();
}

// ============================ 注册 ============================

void registerReality(OutboundRegistry &reg)
{
    // RealityStream::start 只做裸 TCP 上的 REALITY(uTLS)+VLESS，**根本不看 network** —— 没实现 ws/grpc
    // 等任何传输封装。真机上 vless(reality) 常带 `network: grpc`（本项目实测 HK-1 节点即是），早先它被
    // 静默当成裸 TCP 去拨一个期待 gRPC 分帧的服务端 → 必然失败、且伪装成「网络不通」。现在：非裸 TCP
    // 一律返回 nullptr 回退核心。理由详见 OutboundRegistry.h 的传输守卫注释。
    reg.registerProto(
        QStringLiteral("reality"),
        [](const ProxyNode &n, QObject *p) -> IOutboundTcp * {
            if (!coastTransportSupported(n.network, {})) {
                qInfo("[reality] 节点「%s」传输 network=%s 未实现（本单元仅裸 TCP）→ 回退核心",
                      qUtf8Printable(n.name), qUtf8Printable(n.network));
                return nullptr;
            }
            return new RealityOutboundTcp(n, p);
        },
        [](const ProxyNode &n, QObject *p) -> IOutboundUdp * {
            if (!coastTransportSupported(n.network, {}))
                return nullptr;
            return new RealityOutboundUdp(n, p);
        });
}
