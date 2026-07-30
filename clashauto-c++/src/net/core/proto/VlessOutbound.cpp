#include "VlessOutbound.h"

#include "OutboundRegistry.h"
#include "../transport/TlsClient.h"
#include "../transport/WsClient.h"

#include <QMetaObject>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>

// 说明：本单元只实现 VLESS，不改任何共享文件。承载流(选传输/发头/剥响应头/背压)集中在 VlessStream；
// TCP/UDP 两个出站各自在其字节管道上补协议语义。凡拿不准需真机验的点用 TODO 标出。
//
// 与 DirectOutbound/Socks5Client 的对齐点（务必保持）：
//  · 上行零拷贝的生命周期契约（裸缓冲 write 未就绪深拷贝、就绪同步写出），见 IOutbound.h。
//  · 背压：裸TCP/TLS 靠 setReadBufferSize + 暂停时不读（内核收窗关闭压住远端），恢复时补读一次。
//  · upstreamBytesWritten：直接透传底层 socket 的 bytesWritten —— 会把「请求头/帧头/TLS 记录」这些
//    协议开销也算进去（略微多算）。这与 Socks5Client 把 greeting/auth/connect 握手字节也计入是同一处理
//    （见那边注释「上层只是重新评估水位，无副作用」）：只会多归还窗口不会少，不会造成永久卡死。

namespace {

bool isHex(QChar c)
{
    const ushort u = c.unicode();
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

// 解析 8-4-4-4-12 hex uuid → 16 字节。成功返回 true 并填 out。
bool parseUuid(const QString &uuid, QByteArray &out)
{
    QString hex = uuid.trimmed();
    hex.remove(QLatin1Char('-'));
    if (hex.size() != 32)
        return false;
    for (const QChar c : hex) {
        if (!isHex(c))
            return false;
    }
    out = QByteArray::fromHex(hex.toLatin1());
    return out.size() == 16;
}

} // namespace

// ============================ VlessStream ============================

VlessStream::VlessStream(const ProxyNode &node, QObject *parent) : QObject(parent), m_node(node)
{
    m_uuidValid = parseUuid(m_node.uuid, m_uuid16);
}

QByteArray VlessStream::buildHeader() const
{
    QByteArray h;
    h.append(char(0x00));   // 版本
    h.append(m_uuid16);     // 16 字节 uuid
    h.append(char(0x00));   // addon 长度（本单元不发 addon）
    h.append(char(m_cmd));  // 0x01 tcp / 0x02 udp
    h.append(char((m_dstPort >> 8) & 0xFF)); // port 大端
    h.append(char(m_dstPort & 0xFF));

    QHostAddress addr;
    if (addr.setAddress(m_dstHost) && addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ip = addr.toIPv4Address();
        h.append(char(0x01)); // addrType=IPv4
        h.append(char((ip >> 24) & 0xFF));
        h.append(char((ip >> 16) & 0xFF));
        h.append(char((ip >> 8) & 0xFF));
        h.append(char(ip & 0xFF));
    } else if (addr.setAddress(m_dstHost) && addr.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = addr.toIPv6Address(); // c[0..15] 已是网络序（大端）
        h.append(char(0x03));                       // addrType=IPv6
        h.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        const QByteArray host = m_dstHost.toUtf8();
        h.append(char(0x02)); // addrType=域名
        h.append(char(host.size() & 0xFF));
        h.append(host);
    }
    return h;
}

void VlessStream::start(quint8 cmd, const QString &dstHost, quint16 dstPort)
{
    m_cmd = cmd;
    m_dstHost = dstHost;
    m_dstPort = dstPort;

    if (!m_uuidValid) {
        // 异步报错：调用方(工厂→outbound→NetStack)此刻可能还没接上 failed 槽，延到下一拍。
        QTimer::singleShot(0, this, [this] { finishFailed(QStringLiteral("vless: 无效 uuid")); });
        return;
    }

    m_useWs = (m_node.network == QStringLiteral("ws"));
    const bool useTls = m_node.tls;

    if (m_useWs) {
        if (useTls) {
            // wss：WsClient 跑在 TlsClient 的 QSslSocket 之上（读写经 socket() 透明加解密）。
            m_tls = new coastcore::TlsClient(this);
            connect(m_tls, &coastcore::TlsClient::connected, this, [this] { startWs(); });
            connect(m_tls, &coastcore::TlsClient::errorOccurred, this,
                    [this](const QString &r) { onTransportError(r); });
            connect(m_tls, &coastcore::TlsClient::disconnected, this, [this] { onTransportClosed(); });
            const QString sni = m_node.sni.isEmpty() ? m_node.server : m_node.sni;
            m_tls->connectToHost(m_node.server, m_node.port, sni, {}, m_node.skipCertVerify);
            m_bottom = m_tls->socket();
        } else {
            m_tcp = new QTcpSocket(this);
            m_tcp->setProxy(QNetworkProxy::NoProxy);
            connect(m_tcp, &QTcpSocket::connected, this, [this] {
                m_tcp->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                startWs();
            });
            connect(m_tcp, &QAbstractSocket::errorOccurred, this,
                    [this](QAbstractSocket::SocketError) { onTransportError(m_tcp->errorString()); });
            connect(m_tcp, &QTcpSocket::disconnected, this, [this] { onTransportClosed(); });
            m_bottom = m_tcp;
            m_tcp->connectToHost(m_node.server, m_node.port);
        }
    } else {
        if (useTls) {
            m_tls = new coastcore::TlsClient(this);
            connect(m_tls, &coastcore::TlsClient::connected, this, [this] { onStreamUp(); });
            connect(m_tls, &coastcore::TlsClient::readyRead, this, [this] { onTlsReadyRead(); });
            connect(m_tls, &coastcore::TlsClient::errorOccurred, this,
                    [this](const QString &r) { onTransportError(r); });
            connect(m_tls, &coastcore::TlsClient::disconnected, this, [this] { onTransportClosed(); });
            const QString sni = m_node.sni.isEmpty() ? m_node.server : m_node.sni;
            m_tls->connectToHost(m_node.server, m_node.port, sni, {}, m_node.skipCertVerify);
            m_bottom = m_tls->socket();
            m_tls->setReadBufferSize(64 * 1024); // 背压上限，对齐 Direct/Socks5（socket 已在 connectToHost 里建好）
        } else {
            m_tcp = new QTcpSocket(this);
            m_tcp->setProxy(QNetworkProxy::NoProxy);
            m_tcp->setReadBufferSize(64 * 1024);
            connect(m_tcp, &QTcpSocket::connected, this, [this] {
                m_tcp->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                onStreamUp();
            });
            connect(m_tcp, &QTcpSocket::readyRead, this, [this] { onRawReadyRead(); });
            connect(m_tcp, &QAbstractSocket::errorOccurred, this,
                    [this](QAbstractSocket::SocketError) { onTransportError(m_tcp->errorString()); });
            connect(m_tcp, &QTcpSocket::disconnected, this, [this] { onTransportClosed(); });
            m_bottom = m_tcp;
            m_tcp->connectToHost(m_node.server, m_node.port);
        }
    }

    // 底层 socket 排空进度 → 供上层归还接收窗口。对 TLS 是密文字节数、对 ws 是帧字节数（均 ≥ 明文），
    // 只会多算不会少算（见文件头注释），不会卡死。
    if (m_bottom) {
        connect(m_bottom, &QAbstractSocket::bytesWritten, this, [this](qint64 n) { emit wrote(n); });
    }
}

void VlessStream::startWs()
{
    m_ws = new coastcore::WsClient(this);
    connect(m_ws, &coastcore::WsClient::established, this, [this] { onStreamUp(); });
    connect(m_ws, &coastcore::WsClient::dataReceived, this,
            [this](const QByteArray &d) { onWsData(d); });
    connect(m_ws, &coastcore::WsClient::closed, this, [this] { onTransportClosed(); });
    connect(m_ws, &coastcore::WsClient::errorOccurred, this,
            [this](const QString &r) { onTransportError(r); });
    const QString wsHost = m_node.wsHost.isEmpty() ? m_node.server : m_node.wsHost;
    m_ws->start(m_bottom, wsHost, m_node.wsPath, m_node.port);
}

void VlessStream::onStreamUp()
{
    if (m_streamReady || m_finished)
        return;
    m_streamReady = true;
    // 请求头 + 流就绪前缓冲的初始上行，一并发出（头必须在最前）。
    QByteArray first = buildHeader();
    first += m_pendingUp;
    m_pendingUp.clear();
    writeCarrier(first);
    emit ready();
}

void VlessStream::writeCarrier(const QByteArray &b)
{
    if (m_ws)
        m_ws->write(b);
    else if (m_tls)
        m_tls->write(b);
    else if (m_tcp)
        m_tcp->write(b);
}

void VlessStream::write(const QByteArray &data)
{
    if (m_streamReady)
        writeCarrier(data);
    else
        m_pendingUp += data; // 未就绪缓冲（QByteArray += 在空时退化为隐式共享，零拷贝）
}

void VlessStream::write(const char *data, qsizetype size)
{
    if (!data || size <= 0)
        return;
    if (!m_streamReady) {
        m_pendingUp.append(data, size); // 深拷贝：字节要留到就绪后才发（同 Direct/Socks5）
        return;
    }
    if (m_ws)
        m_ws->write(QByteArray(data, int(size)));
    else if (m_tls)
        m_tls->write(QByteArray::fromRawData(data, size)); // write 同步拷进 socket 缓冲，data 只需活到返回
    else if (m_tcp)
        m_tcp->write(data, size);
}

qint64 VlessStream::bytesToWrite() const
{
    qint64 t = qint64(m_pendingUp.size());
    if (m_ws)
        t += m_ws->bytesToWrite();
    else if (m_tls)
        t += m_tls->bytesToWrite();
    else if (m_tcp)
        t += m_tcp->bytesToWrite();
    return t;
}

void VlessStream::onRawReadyRead()
{
    if (m_readPaused || !m_tcp)
        return; // 背压中一个字节都不读，留在内核/Qt 缓冲压住远端（恢复时补读）
    const QByteArray d = m_tcp->readAll();
    if (!d.isEmpty())
        feedDown(d);
}

void VlessStream::onTlsReadyRead()
{
    if (m_readPaused || !m_tls)
        return;
    const QByteArray d = m_tls->readAll();
    if (!d.isEmpty())
        feedDown(d);
}

void VlessStream::onWsData(const QByteArray &d)
{
    // ws 是「推」模型：WsClient 无条件读底层 socket 并解帧推给我们，无法在此真正暂停读。
    // 背压期间只能把已推来的字节缓进 m_downPending（软背压）。feedDown 内按 m_readPaused 决定缓冲/上抛。
    if (!d.isEmpty())
        feedDown(d);
}

void VlessStream::feedDown(QByteArray chunk)
{
    if (m_finished)
        return;
    if (!m_respHeaderDone) {
        m_respBuf += chunk;
        if (m_respBuf.size() < 2)
            return; // 连 [版本][addonLen] 都没凑齐
        const int addonLen = int(quint8(m_respBuf.at(1)));
        const int need = 2 + addonLen;
        if (m_respBuf.size() < need)
            return; // addon 还没收全（可能跨多次 readyRead）
        QByteArray rest = m_respBuf.mid(need);
        m_respBuf.clear();
        m_respHeaderDone = true;
        if (rest.isEmpty())
            return;
        chunk = rest; // 响应头之后紧跟的真实下行，继续下发
    }
    if (m_readPaused) {
        m_downPending += chunk;
        return;
    }
    if (!chunk.isEmpty())
        emit data(chunk);
}

void VlessStream::setReadPaused(bool paused)
{
    if (paused) {
        m_readPaused = true;
        return;
    }
    if (!m_readPaused || m_resumeScheduled)
        return;
    // 恢复延到事件循环下一拍：本函数常从 lwIP tcp_sent 回调里被调到，同步补读会 emit data 而上层槽
    // 可能当场把整条连接（含本对象）拆掉（同 Direct/Socks5 的排队补读）。
    // 另外故意**不**在此同步清 m_readPaused：ws 推模型下，若同步清标志，尚在事件队列里的旧 onWsData
    // 会抢在本次 flush 之前直接上抛，把顺序打乱；保持暂停到本闭包运行，期间到达的字节继续缓冲，
    // 最后一次性按序 flush。
    m_resumeScheduled = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_resumeScheduled = false;
            if (!m_readPaused || m_finished)
                return; // 期间又被暂停 / 已终结
            m_readPaused = false;
            // 先 flush 较旧的缓冲下行，再补读较新的 socket 存货，保证顺序。
            if (!m_downPending.isEmpty()) {
                const QByteArray p = m_downPending;
                m_downPending.clear();
                emit data(p);
            }
            if (m_finished)
                return;
            // 裸TCP/TLS：readyRead 只在有新数据到达时才发，远端此刻若已发完就永远卡住 → 主动补读一次。
            if (!m_useWs && m_tcp && m_tcp->bytesAvailable() > 0)
                feedDown(m_tcp->readAll());
            else if (!m_useWs && m_tls && m_tls->bytesAvailable() > 0)
                feedDown(m_tls->readAll());
        },
        Qt::QueuedConnection);
}

void VlessStream::onTransportError(const QString &reason)
{
    if (m_streamReady)
        finishClosed(); // 建立后出错视为正常关闭
    else
        finishFailed(reason);
}

void VlessStream::onTransportClosed()
{
    if (m_streamReady)
        finishClosed();
    else
        finishFailed(QStringLiteral("vless: 传输在流建立前关闭"));
}

void VlessStream::finishFailed(const QString &reason)
{
    if (m_finished)
        return;
    m_finished = true;
    abortTransports();
    emit failed(reason);
}

void VlessStream::finishClosed()
{
    if (m_finished)
        return;
    m_finished = true;
    emit closed();
}

void VlessStream::abortTransports()
{
    if (m_tcp)
        m_tcp->abort();
    if (m_tls)
        m_tls->abort();
    if (m_ws)
        m_ws->close();
}

void VlessStream::abort()
{
    abortTransports();
    finishClosed();
}

// ============================ VlessOutboundTcp ============================

VlessOutboundTcp::VlessOutboundTcp(const ProxyNode &node, QObject *parent)
    : IOutboundTcp(parent), m_node(node)
{
}

void VlessOutboundTcp::connectTo(const QString &dstHost, quint16 dstPort, const QString & /*user*/)
{
    // user 用于每设备策略/归属；VLESS 出站无每设备语义（凭 uuid 认证到服务器），忽略。
    m_established = false;
    m_closedEmitted = false;

    m_stream = new VlessStream(m_node, this);
    connect(m_stream, &VlessStream::ready, this, [this] {
        m_established = true;
        emit established();
    });
    connect(m_stream, &VlessStream::data, this,
            [this](const QByteArray &d) { emit dataReceived(d); });
    connect(m_stream, &VlessStream::wrote, this,
            [this](qint64 n) { emit upstreamBytesWritten(n); });
    connect(m_stream, &VlessStream::failed, this, [this](const QString &reason) {
        if (m_closedEmitted)
            return;
        m_closedEmitted = true; // failed 即终态：抑制随后的 closed，避免双重信号
        emit failed(reason);
    });
    connect(m_stream, &VlessStream::closed, this, [this] { emitClosed(); });

    m_stream->start(0x01, dstHost, dstPort); // cmd=0x01 TCP
}

void VlessOutboundTcp::write(const QByteArray &data)
{
    if (m_stream)
        m_stream->write(data);
}

void VlessOutboundTcp::write(const char *data, qsizetype size)
{
    if (m_stream)
        m_stream->write(data, size);
}

void VlessOutboundTcp::closeTunnel()
{
    if (m_stream)
        m_stream->abort(); // → VlessStream::closed → emitClosed()
    emitClosed();          // 兜底（stream 为空 / 已终结时无副作用，emitClosed 有幂等门）
}

bool VlessOutboundTcp::isEstablished() const
{
    return m_established;
}

qint64 VlessOutboundTcp::bytesToWrite() const
{
    return m_stream ? m_stream->bytesToWrite() : 0;
}

void VlessOutboundTcp::setReadPaused(bool paused)
{
    if (m_stream)
        m_stream->setReadPaused(paused);
}

bool VlessOutboundTcp::isReadPaused() const
{
    return m_stream ? m_stream->isReadPaused() : false;
}

void VlessOutboundTcp::emitClosed()
{
    if (m_closedEmitted)
        return;
    m_closedEmitted = true;
    emit closed();
}

// ============================ VlessOutboundUdp ============================

VlessOutboundUdp::VlessOutboundUdp(const ProxyNode &node, QObject *parent)
    : IOutboundUdp(parent), m_node(node)
{
}

void VlessOutboundUdp::associate(const QString & /*user*/)
{
    m_ready = false;
    m_closedEmitted = false;
    m_streamStarted = false;
    m_downAcc.clear();
    // 承载流要等首个 sendTo 拿到目标地址才能起（VLESS 的目标写在请求头里）。这里像 Direct 一样**异步**
    // 先 emit ready()，让 NetStack 开始投递 UDP 包；真正的流在首个 sendTo 惰性建立。
    QTimer::singleShot(0, this, [this] {
        if (m_closedEmitted)
            return;
        m_ready = true;
        emit ready();
    });
}

void VlessOutboundUdp::sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload)
{
    if (!m_ready || m_closedEmitted)
        return; // 未就绪前丢弃（DNS/UDP 会重试，无需缓冲，同 Socks5Udp）

    if (!m_streamStarted) {
        m_streamStarted = true;
        m_targetIp = dstIp;
        m_targetPort = dstPort;
        m_stream = new VlessStream(m_node, this);
        connect(m_stream, &VlessStream::data, this,
                [this](const QByteArray &chunk) { onStreamData(chunk); });
        connect(m_stream, &VlessStream::failed, this, [this](const QString &) { emitClosed(); });
        connect(m_stream, &VlessStream::closed, this, [this] { emitClosed(); });
        // 目标以 IP 字面量写进请求头（dstIp 已是解析后的 v4/v6）。
        m_stream->start(0x02, dstIp.toString(), dstPort); // cmd=0x02 UDP
    }

    // TODO(真机验证/单目标局限)：基础 VLESS-over-流按请求头里的单一目标转发，本会话后续发往**其他**
    // 目标的包也会走这条流、落到首个目标。多目标(full-cone)需 XUDP/mux(mux.cool)，本单元未实现。
    // 对 DNS(单一上游) / 单流 UDP 足够。

    // 每个 UDP 包封成 [len 2B 大端][payload]。
    QByteArray frame;
    frame.append(char((payload.size() >> 8) & 0xFF));
    frame.append(char(payload.size() & 0xFF));
    frame.append(payload);
    m_stream->write(frame); // 流未就绪则缓冲，就绪时连同请求头补发
}

void VlessOutboundUdp::onStreamData(const QByteArray &chunk)
{
    // 下行同样按 [len 2B 大端][payload] 分帧；源地址无逐包信息 → 用请求头里的目标回填（单目标假设）。
    m_downAcc += chunk;
    for (;;) {
        if (m_downAcc.size() < 2)
            return;
        const int len =
            (int(quint8(m_downAcc.at(0))) << 8) | int(quint8(m_downAcc.at(1)));
        if (m_downAcc.size() < 2 + len)
            return; // 本帧还没收全
        const QByteArray p = m_downAcc.mid(2, len);
        m_downAcc.remove(0, 2 + len);
        emit datagramReceived(m_targetIp, m_targetPort, p);
    }
}

void VlessOutboundUdp::closeSession()
{
    if (m_stream)
        m_stream->abort();
    emitClosed();
}

bool VlessOutboundUdp::isReady() const
{
    return m_ready;
}

void VlessOutboundUdp::emitClosed()
{
    if (m_closedEmitted)
        return;
    m_closedEmitted = true;
    emit closed();
}

// ============================ 注册 ============================

void registerVless(OutboundRegistry &reg)
{
    reg.registerProto(
        QStringLiteral("vless"),
        [](const ProxyNode &n, QObject *p) -> IOutboundTcp * { return new VlessOutboundTcp(n, p); },
        [](const ProxyNode &n, QObject *p) -> IOutboundUdp * { return new VlessOutboundUdp(n, p); });
}
