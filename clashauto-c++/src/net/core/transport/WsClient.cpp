#include "WsClient.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

// —— RFC6455 要点（本实现只覆盖「当字节流用」所需的最小子集）——
//  握手：GET + Upgrade: websocket + Sec-WebSocket-Key(随机16字节的 base64) + Version:13；
//        响应须 101，且 Sec-WebSocket-Accept == base64( SHA1( key + GUID ) )。
//  帧头：byte0 = FIN(1) RSV(3) opcode(4)；byte1 = MASK(1) payloadLen(7)；
//        payloadLen==126 → 随后 2 字节为真实长度；==127 → 随后 8 字节；
//        MASK=1 → 随后 4 字节 mask key，payload 逐字节 XOR mask[i%4]。
//  客户端→服务器帧**必须** MASK；服务器→客户端帧**必须不** MASK（我们对入方向宽容处理：带掩码也能解）。
//  opcode：0x0 续帧 / 0x1 文本 / 0x2 二进制 / 0x8 关闭 / 0x9 ping / 0xA pong。
//  ★ 字节流语义：我们不关心消息边界（FIN/分片）——把 0x0/0x1/0x2 的 payload 一律拼进入方向字节流。
//    控制帧(ping)要回 pong，close 要收口。

namespace coastcore {

namespace {
// RFC6455 固定 GUID，用于算 Sec-WebSocket-Accept。
const char *kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
} // namespace

WsClient::WsClient(QObject *parent) : QObject(parent) {}

WsClient::~WsClient() = default;

void WsClient::start(QAbstractSocket *sock, const QString &host, const QString &path, quint16 port)
{
    m_sock = sock;
    m_established = false;
    m_closed = false;
    m_httpResp.clear();
    m_inbuf.clear();

    if (!m_sock) {
        fail(QStringLiteral("ws: 传入的 socket 为空"));
        return;
    }

    connect(m_sock, &QAbstractSocket::readyRead, this, [this] { onReadyRead(); });
    connect(m_sock, &QAbstractSocket::disconnected, this, [this] {
        // 底层断开 = 会话结束（无论是否已握手）。
        if (!m_closed) {
            m_closed = true;
            emit closed();
        }
    });
    connect(m_sock, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { fail(m_sock->errorString()); });

    // —— 生成 Sec-WebSocket-Key（16 随机字节的 base64），并预算期望的 Accept ——
    QByteArray keyRaw(16, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(keyRaw.data()), 4);
    const QByteArray keyB64 = keyRaw.toBase64();
    m_acceptExpected =
        QCryptographicHash::hash(keyB64 + kWsGuid, QCryptographicHash::Sha1).toBase64();

    const QString reqPath = path.isEmpty() ? QStringLiteral("/") : path;
    // Host 头：非默认端口才附 :port（0/80/443 不附）。
    QString hostHdr = host;
    if (port != 0 && port != 80 && port != 443)
        hostHdr += QLatin1Char(':') + QString::number(port);

    QByteArray req;
    req += "GET " + reqPath.toUtf8() + " HTTP/1.1\r\n";
    req += "Host: " + hostHdr.toUtf8() + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + keyB64 + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "\r\n";
    m_sock->write(req);
}

void WsClient::onReadyRead()
{
    if (!m_sock || m_closed)
        return;
    if (!m_established) {
        onHandshakeBytes();
        // 握手可能刚在本次里完成；若响应之后还捎带了数据帧，onHandshakeBytes 已把余下字节转入 m_inbuf，
        // established 置位后这里继续解帧。
        if (m_established && !m_inbuf.isEmpty())
            parseFrames();
        return;
    }
    m_inbuf += m_sock->readAll();
    parseFrames();
}

void WsClient::onHandshakeBytes()
{
    m_httpResp += m_sock->readAll();
    const int headEnd = m_httpResp.indexOf("\r\n\r\n");
    if (headEnd < 0) {
        // 头还没收全。防滥用：HTTP 响应头异常巨大直接判错。
        if (m_httpResp.size() > 64 * 1024)
            fail(QStringLiteral("ws: 握手响应头过大"));
        return;
    }

    const QByteArray head = m_httpResp.left(headEnd);
    // 状态行须是 101。
    const int firstEol = head.indexOf("\r\n");
    const QByteArray statusLine = firstEol < 0 ? head : head.left(firstEol);
    if (!statusLine.contains(" 101")) {
        fail(QStringLiteral("ws: 握手未返回 101（%1）")
                 .arg(QString::fromLatin1(statusLine.trimmed())));
        return;
    }
    // 校验 Sec-WebSocket-Accept（大小写不敏感地找头字段）。
    QByteArray gotAccept;
    for (const QByteArray &line : head.split('\n')) {
        const QByteArray l = line.trimmed();
        const int colon = l.indexOf(':');
        if (colon < 0)
            continue;
        if (l.left(colon).trimmed().toLower() == "sec-websocket-accept") {
            gotAccept = l.mid(colon + 1).trimmed();
            break;
        }
    }
    if (gotAccept != m_acceptExpected) {
        fail(QStringLiteral("ws: Sec-WebSocket-Accept 不匹配"));
        return;
    }

    // 头之后的字节可能已经是服务器抢先发来的数据帧 —— 转入帧缓冲，别丢。
    m_inbuf = m_httpResp.mid(headEnd + 4);
    m_httpResp.clear();
    m_established = true;
    emit established();

    // established 前排队的出方向字节，此刻按帧补发。
    if (!m_pendingOut.isEmpty()) {
        const QByteArray p = m_pendingOut;
        m_pendingOut.clear();
        sendFrame(0x2, p);
    }
}

void WsClient::parseFrames()
{
    // 尽量多解：一次 readyRead 可能带来多个完整帧，也可能只带来半个（等下一拍）。
    for (;;) {
        if (m_inbuf.size() < 2)
            return; // 连基本帧头都不够
        const auto *p = reinterpret_cast<const unsigned char *>(m_inbuf.constData());
        const quint8 b0 = p[0];
        const quint8 b1 = p[1];
        const quint8 opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        quint64 payloadLen = b1 & 0x7F;

        int pos = 2;
        if (payloadLen == 126) {
            if (m_inbuf.size() < pos + 2)
                return;
            payloadLen = (quint64(p[2]) << 8) | quint64(p[3]);
            pos += 2;
        } else if (payloadLen == 127) {
            if (m_inbuf.size() < pos + 8)
                return;
            payloadLen = 0;
            for (int i = 0; i < 8; ++i)
                payloadLen = (payloadLen << 8) | quint64(p[pos + i]);
            pos += 8;
        }
        // 单帧上限保护（服务器正常不会给巨帧；防内存被拖爆）。
        if (payloadLen > quint64(64) * 1024 * 1024) {
            fail(QStringLiteral("ws: 帧过大"));
            return;
        }

        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (m_inbuf.size() < pos + 4)
                return;
            for (int i = 0; i < 4; ++i)
                mask[i] = p[pos + i];
            pos += 4;
        }
        if (quint64(m_inbuf.size()) < quint64(pos) + payloadLen)
            return; // payload 还没收全，等下一拍

        QByteArray payload = m_inbuf.mid(pos, int(payloadLen));
        if (masked) {
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = char(quint8(payload.at(i)) ^ mask[i % 4]);
        }
        m_inbuf.remove(0, pos + int(payloadLen)); // 消费掉这一整帧

        switch (opcode) {
        case 0x0: // 续帧
        case 0x1: // 文本
        case 0x2: // 二进制
            // 字节流语义：一律拼进入方向流（不理会 FIN/分片边界）。
            if (!payload.isEmpty())
                emit dataReceived(payload);
            break;
        case 0x8: // close
            if (!m_closed) {
                sendFrame(0x8, QByteArray()); // 回一个 close 帧（尽力，忽略失败）
                m_closed = true;
                emit closed();
            }
            return;
        case 0x9: // ping → 原样 payload 回 pong
            sendFrame(0xA, payload);
            break;
        case 0xA: // pong：忽略
            break;
        default:
            fail(QStringLiteral("ws: 未知 opcode 0x%1").arg(opcode, 0, 16));
            return;
        }
    }
}

qint64 WsClient::sendFrame(quint8 opcode, const QByteArray &payload)
{
    if (!m_sock)
        return -1;
    QByteArray frame;
    frame.append(char(0x80 | (opcode & 0x0F))); // FIN=1 + opcode

    const int n = payload.size();
    // MASK 位恒 1（客户端强制），长度按 7/7+16/7+64 编码。
    if (n < 126) {
        frame.append(char(0x80 | n));
    } else if (n <= 0xFFFF) {
        frame.append(char(0x80 | 126));
        frame.append(char((n >> 8) & 0xFF));
        frame.append(char(n & 0xFF));
    } else {
        frame.append(char(0x80 | 127));
        // 64-bit 大端长度（高 32 位对我们的用量恒 0，但按规范补全）。
        const quint64 len = quint64(n);
        for (int i = 7; i >= 0; --i)
            frame.append(char((len >> (i * 8)) & 0xFF));
    }

    // 每帧一把新的 4 字节随机 mask key。
    quint32 maskRaw = QRandomGenerator::system()->generate();
    unsigned char mask[4];
    mask[0] = (maskRaw >> 24) & 0xFF;
    mask[1] = (maskRaw >> 16) & 0xFF;
    mask[2] = (maskRaw >> 8) & 0xFF;
    mask[3] = maskRaw & 0xFF;
    frame.append(reinterpret_cast<const char *>(mask), 4);

    // 掩码 payload。
    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i)
        masked[i] = char(quint8(masked.at(i)) ^ mask[i % 4]);
    frame.append(masked);

    return m_sock->write(frame);
}

qint64 WsClient::write(const QByteArray &data)
{
    if (!m_established) {
        m_pendingOut += data; // 握手未完：缓冲，established 后一并发
        return data.size();
    }
    return sendFrame(0x2, data); // 每次 write 一个 binary 帧（FIN），字节流语义无需分片
}

qint64 WsClient::bytesToWrite() const
{
    return m_sock ? m_sock->bytesToWrite() : 0;
}

void WsClient::close()
{
    if (m_closed)
        return;
    if (m_established && m_sock)
        sendFrame(0x8, QByteArray());
    m_closed = true;
    emit closed();
}

bool WsClient::isEstablished() const
{
    return m_established;
}

void WsClient::fail(const QString &reason)
{
    if (m_closed)
        return;
    m_closed = true;
    emit errorOccurred(reason);
}

} // namespace coastcore
