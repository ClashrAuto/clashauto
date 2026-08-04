#include "TrojanOutbound.h"

#include "OutboundRegistry.h"
#include "../transport/TlsClient.h"

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QStringList>

#include <cstring>

// 说明：TCP 分支 = DirectOutbound/Socks5Tcp「把裸 socket 换成 TlsClient、握手换成 trojan 首包」后的
// 样子。凡与首包无关的东西（背压闸门、零拷贝上行的生命周期契约、established/closed 的信号时序、
// readPaused 的排队补读）都逐点照搬那两处，目的就是让 NetStack 侧对「出站是 SOCKS5 / 直连 / trojan」
// 完全无感。改动前请对照 DirectOutbound.cpp / Socks5Client.cpp 的对应注释。

namespace {

// 端口 / 长度：大端两字节
QByteArray be16(quint16 v)
{
    QByteArray b;
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
    return b;
}

// hex(SHA224(password)) —— 56 个小写 hex 字节。QCryptographicHash::toHex() 本就是小写。
QByteArray trojanPasswordHex(const QString &password)
{
    return QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha224).toHex();
}

// SOCKS5 地址编码（ATYP + ADDR + PORT），语义完全对齐 Socks5Tcp::sendConnect：
//   host 能解析为 IPv4 → 0x01 + 4 裸字节；解析为 IPv6 → 0x04 + 16 裸字节（网络序）；
//   两者皆非 → 域名 0x03 + 1 字节长度 + utf8。PORT 恒大端。
QByteArray encodeSocksAddr(const QString &host, quint16 port)
{
    QByteArray r;
    QHostAddress addr;
    if (addr.setAddress(host) && addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ip = addr.toIPv4Address();
        r.append(char(0x01));
        r.append(char((ip >> 24) & 0xFF));
        r.append(char((ip >> 16) & 0xFF));
        r.append(char((ip >> 8) & 0xFF));
        r.append(char(ip & 0xFF));
    } else if (addr.setAddress(host) && addr.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = addr.toIPv6Address(); // c[0..15] 已是网络序
        r.append(char(0x04));
        r.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        const QByteArray h = host.toUtf8();
        r.append(char(0x03));
        r.append(char(h.size() & 0xFF));
        r.append(h);
    }
    r.append(be16(port));
    return r;
}

// SOCKS5 地址编码的 QHostAddress 版本（UDP 每包的目标恒为 IP 字面量，不会是域名）。
QByteArray encodeSocksAddr(const QHostAddress &ip, quint16 port)
{
    QByteArray r;
    if (ip.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = ip.toIPv6Address();
        r.append(char(0x04));
        r.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        const quint32 v4 = ip.toIPv4Address();
        r.append(char(0x01));
        r.append(char((v4 >> 24) & 0xFF));
        r.append(char((v4 >> 16) & 0xFF));
        r.append(char((v4 >> 8) & 0xFF));
        r.append(char(v4 & 0xFF));
    }
    r.append(be16(port));
    return r;
}

// 每个 TLS 协议都要设的 ALPN；trojan 常用 {"h2","http/1.1"}（伪装成正常 HTTPS）。
QStringList trojanAlpn()
{
    return QStringList{QStringLiteral("h2"), QStringLiteral("http/1.1")};
}

} // namespace

// ============================ TrojanOutboundTcp ============================

class TrojanOutboundTcp::Priv
{
public:
    explicit Priv(TrojanOutboundTcp *owner) : q(owner) {}

    TrojanOutboundTcp *q = nullptr;
    coastcore::TlsClient *tls = nullptr;

    // —— 节点参数（构造时定死）——
    QString server;
    quint16 serverPort = 0;
    QByteArray passwordHex; // hex(SHA224(password))，56 字节
    QString sni;
    bool skipVerify = false;

    // —— 目标 & 会话状态 ——
    QString dstHost;
    quint16 dstPort = 0;
    QByteArray pending; // established 前缓冲的上行字节（TLS 握手 / 首包未发前）
    bool established = false;
    bool closedEmitted = false;
    bool readPaused = false;

    // TLS encrypted：写 trojan 首包（头 + 已缓冲的上行作初始 payload），随后进入裸字节管道。
    void onEncrypted()
    {
        if (!tls) {
            return;
        }
        QByteArray first;
        first += passwordHex;
        first += "\r\n";
        first += char(0x01); // CMD=CONNECT(TCP)
        first += encodeSocksAddr(dstHost, dstPort);
        first += "\r\n";
        first += pending; // 初始 payload（可能为空）
        pending.clear();
        tls->write(first);

        established = true;
        emit q->established();
        // 若 TLS 缓冲里已捎带下行（trojan 正常不会在收到首包前先发，但兜底一手），立即上抛。
        if (!readPaused && tls->bytesAvailable() > 0) {
            const QByteArray data = tls->readAll();
            if (!data.isEmpty()) {
                emit q->dataReceived(data);
            }
        }
    }

    void onReadyRead()
    {
        if (!tls) {
            return;
        }
        // 背压中一个字节都不读：数据留在 QSslSocket 读缓冲（setReadBufferSize 已限死）→ 满了内核收窗
        // 关闭 → 远端被压住。恢复时由 setReadPaused 排队补读。语义同 Socks5Tcp/DirectOutbound。
        if (readPaused) {
            return;
        }
        if (!established) {
            return; // 首包尚未发出（TLS 未 encrypted）：不应有下行，留着 onEncrypted 里再排空
        }
        const QByteArray data = tls->readAll();
        if (!data.isEmpty()) {
            emit q->dataReceived(data);
        }
    }

    void fail(const QString &reason)
    {
        if (closedEmitted) {
            return;
        }
        closedEmitted = true; // failed 即终态：抑制随后 abort 触发的 closed，避免双重信号
        emit q->failed(reason);
        if (tls) {
            tls->abort();
        }
    }

    void emitClosed()
    {
        if (closedEmitted) {
            return;
        }
        closedEmitted = true;
        emit q->closed();
    }
};

TrojanOutboundTcp::TrojanOutboundTcp(const ProxyNode &node, QObject *parent)
    : IOutboundTcp(parent), d(new Priv(this))
{
    d->server = node.server;
    d->serverPort = node.port;
    d->passwordHex = trojanPasswordHex(node.password);
    d->sni = node.sni.isEmpty() ? node.server : node.sni;
    d->skipVerify = node.skipCertVerify;
}

TrojanOutboundTcp::~TrojanOutboundTcp()
{
    delete d;
}

void TrojanOutboundTcp::connectTo(const QString &dstHost, quint16 dstPort, const QString & /*user*/)
{
    d->dstHost = dstHost;
    d->dstPort = dstPort;
    d->established = false;
    d->closedEmitted = false;
    d->readPaused = false;
    // ★ 不清 pending —— 拨号被 NetStack 推迟一拍，首个数据段可能已经 write() 进来了。
    //   契约见 IOutbound.h 的 connectTo 注释。

    d->tls = new coastcore::TlsClient(this);
    // 读缓冲上限 64 KiB —— 与 Socks5Tcp/DirectOutbound 一致（背压滞留上限）。
    d->tls->setReadBufferSize(64 * 1024);

    // 用 this 作 context：本对象析构时自动断开这些连接，回调里安全访问 d。
    connect(d->tls, &coastcore::TlsClient::connected, this, [this] { d->onEncrypted(); });
    connect(d->tls, &coastcore::TlsClient::readyRead, this, [this] { d->onReadyRead(); });
    connect(d->tls, &coastcore::TlsClient::errorOccurred, this, [this](const QString &reason) {
        // 建立后出错（对端 RST/关闭）视为正常关闭；建立前出错为握手/连接/TLS 失败。
        if (d->established) {
            d->emitClosed();
        } else {
            d->fail(reason);
        }
    });
    connect(d->tls, &coastcore::TlsClient::disconnected, this, [this] { d->emitClosed(); });

    // ALPN {"h2","http/1.1"}；SNI/skipVerify 见构造。
    d->tls->connectToHost(d->server, d->serverPort, d->sni, trojanAlpn(), d->skipVerify);

    // 上行排空进度：TlsClient 不直接暴露 bytesWritten，取底层 QSslSocket 的（明文 payload 计数，与
    // bytesToWrite 语义配套）转发给上层做「按量归还 lwIP 接收窗口」。首包头字节也会计入，但如同
    // Socks5Tcp 里握手包触发的那次，上层只是重新评估水位，无副作用。
    // connectToHost 之后 socket() 已非空：接底层明文 bytesWritten → upstreamBytesWritten。
    if (QAbstractSocket *raw = d->tls->socket()) {
        connect(raw, &QAbstractSocket::bytesWritten, this,
                [this](qint64 n) { emit upstreamBytesWritten(n); });
    }
}

void TrojanOutboundTcp::write(const QByteArray &data)
{
    if (d->established && d->tls) {
        d->tls->write(data);
    } else {
        d->pending += data; // 建立前先缓冲，onEncrypted 时随首包一起发
    }
    // 故意不转发给裸指针重载：pending 为空时 += QByteArray 会退化成隐式共享（零拷贝），转发反成 memcpy。
}

// 裸缓冲版本，生命周期契约同 Socks5Tcp（见 IOutbound.h）：data 只需活到本次调用返回。
//  · established：包成 QByteArray 交 TlsClient::write（QSslSocket::write 当场把明文拷进写缓冲），
//    QByteArray 构造已复制一份，指针不外泄。
//  · 未 established：字节要留到首包才发，指针必悬垂 → append(const char*,size) 深拷贝进 pending。
void TrojanOutboundTcp::write(const char *data, qsizetype size)
{
    if (!data || size <= 0) {
        return;
    }
    if (d->established && d->tls) {
        d->tls->write(QByteArray(data, size)); // TlsClient 无裸重载，构造即复制（满足生命周期契约）
    } else {
        d->pending.append(data, size); // 深拷贝，见上
    }
}

void TrojanOutboundTcp::closeTunnel()
{
    if (d->tls) {
        d->tls->abort();
    }
    d->emitClosed();
}

bool TrojanOutboundTcp::isEstablished() const
{
    return d->established;
}

qint64 TrojanOutboundTcp::bytesToWrite() const
{
    // pending 也要算进来（established 前 write() 进来还没交给 TLS 的字节），否则上层会误判「上行已
    // 排空」而把接收窗口开满 —— 正是要避免的无闸状态。
    return qint64(d->pending.size()) + (d->tls ? d->tls->bytesToWrite() : 0);
}

void TrojanOutboundTcp::setReadPaused(bool paused)
{
    if (d->readPaused == paused) {
        return;
    }
    d->readPaused = paused;
    if (paused || !d->tls || d->tls->bytesAvailable() <= 0) {
        return;
    }
    // 恢复且缓冲里有存货：必须主动补读一次（readyRead 只在有新数据到达时才发，远端此刻若已发完就永远
    // 卡住）。补读会同步 emit dataReceived，而本函数常从 lwIP 的 tcp_sent 回调里被调到，上层的槽可能
    // 当场把整条连接（含本对象）拆掉 → 排队到事件循环里做。被 deleteLater 后排队事件随析构丢弃。
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!d->readPaused) {
                d->onReadyRead();
            }
        },
        Qt::QueuedConnection);
}

bool TrojanOutboundTcp::isReadPaused() const
{
    return d->readPaused;
}

// ============================ TrojanOutboundUdp ============================

class TrojanOutboundUdp::Priv
{
public:
    explicit Priv(TrojanOutboundUdp *owner) : q(owner) {}

    TrojanOutboundUdp *q = nullptr;
    coastcore::TlsClient *tls = nullptr;

    // —— 节点参数 ——
    QString server;
    quint16 serverPort = 0;
    QByteArray passwordHex;
    QString sni;
    bool skipVerify = false;

    // —— 会话状态 ——
    bool ready = false;
    bool closedEmitted = false;
    QByteArray rx; // 下行 TLS 流的分帧累积缓冲（可能半包，跨 readyRead 保留）

    // TLS encrypted：写 UDP ASSOCIATE 首包头（CMD=0x03），之后每个 UDP 包各自成帧。
    void onEncrypted()
    {
        if (!tls) {
            return;
        }
        QByteArray hdr;
        hdr += passwordHex;
        hdr += "\r\n";
        hdr += char(0x03); // CMD=UDP ASSOCIATE
        // 首包请求头的目标地址在 UDP 模式下被服务器忽略（真实目标在每个 UDP 包头里），按 trojan 惯例
        // 填占位 0.0.0.0:0。★ 待真机验证：个别服务器实现可能对此地址挑剔（见报告 TODO）。
        hdr += encodeSocksAddr(QStringLiteral("0.0.0.0"), 0);
        hdr += "\r\n";
        tls->write(hdr);

        ready = true;
        emit q->ready(); // TLS 握手完成本就是异步事件，此刻上层的 ready 槽早已连上（先 createUdp 再 associate）
    }

    // 下行 TLS 流按 trojan UDP 帧解析：[ATYP+addr+port][len 2B 大端][CRLF][payload]，逐包上抛。
    void onReadyRead()
    {
        if (!tls) {
            return;
        }
        rx += tls->readAll();
        for (;;) {
            if (rx.size() < 1) {
                return;
            }
            const quint8 atyp = quint8(rx.at(0));
            int addrLen = 0;
            if (atyp == 0x01) {
                addrLen = 4;
            } else if (atyp == 0x04) {
                addrLen = 16;
            } else if (atyp == 0x03) {
                if (rx.size() < 2) {
                    return; // 还没读到域名长度字节
                }
                addrLen = 1 + quint8(rx.at(1));
            } else {
                fail(QStringLiteral("bad trojan udp atyp %1").arg(atyp));
                return;
            }
            const int portOff = 1 + addrLen;
            const int lenOff = portOff + 2;
            const int crlfOff = lenOff + 2;
            const int payloadOff = crlfOff + 2;
            if (rx.size() < payloadOff) {
                return; // 帧头还不完整，等下一拍
            }
            const quint16 payLen =
                quint16((quint8(rx.at(lenOff)) << 8) | quint8(rx.at(lenOff + 1)));
            const int total = payloadOff + int(payLen);
            if (rx.size() < total) {
                return; // 载荷未收全，等下一拍
            }

            QHostAddress src;
            bool haveAddr = false;
            if (atyp == 0x01) {
                const quint32 ip = (quint32(quint8(rx.at(1))) << 24)
                                 | (quint32(quint8(rx.at(2))) << 16)
                                 | (quint32(quint8(rx.at(3))) << 8)
                                 | quint32(quint8(rx.at(4)));
                src = QHostAddress(ip);
                haveAddr = true;
            } else if (atyp == 0x04) {
                Q_IPV6ADDR v6;
                memcpy(v6.c, rx.constData() + 1, 16); // 网络序，直接喂 QHostAddress
                src = QHostAddress(v6);
                haveAddr = true;
            } // 域名(0x03) 回程本客户端用不到：仍精确消费其字节以维持分帧同步，只是不上抛

            const quint16 port =
                quint16((quint8(rx.at(portOff)) << 8) | quint8(rx.at(portOff + 1)));
            const QByteArray payload = rx.mid(payloadOff, int(payLen));
            rx.remove(0, total);

            if (haveAddr) {
                emit q->datagramReceived(src, port, payload);
            }
            // 继续 while，一次 readyRead 可能带多帧
        }
    }

    void fail(const QString &reason)
    {
        if (closedEmitted) {
            return;
        }
        closedEmitted = true;
        emit q->failed(reason);
        if (tls) {
            tls->abort();
        }
    }

    void emitClosed()
    {
        if (closedEmitted) {
            return;
        }
        closedEmitted = true;
        emit q->closed();
    }
};

TrojanOutboundUdp::TrojanOutboundUdp(const ProxyNode &node, QObject *parent)
    : IOutboundUdp(parent), d(new Priv(this))
{
    d->server = node.server;
    d->serverPort = node.port;
    d->passwordHex = trojanPasswordHex(node.password);
    d->sni = node.sni.isEmpty() ? node.server : node.sni;
    d->skipVerify = node.skipCertVerify;
}

TrojanOutboundUdp::~TrojanOutboundUdp()
{
    delete d;
}

void TrojanOutboundUdp::associate(const QString & /*user*/)
{
    d->ready = false;
    d->closedEmitted = false;
    d->rx.clear();

    d->tls = new coastcore::TlsClient(this);
    connect(d->tls, &coastcore::TlsClient::connected, this, [this] { d->onEncrypted(); });
    connect(d->tls, &coastcore::TlsClient::readyRead, this, [this] { d->onReadyRead(); });
    connect(d->tls, &coastcore::TlsClient::errorOccurred, this, [this](const QString &reason) {
        // 就绪后出错视为会话结束；就绪前出错为建立/TLS 失败。
        if (d->ready) {
            d->emitClosed();
        } else {
            d->fail(reason);
        }
    });
    connect(d->tls, &coastcore::TlsClient::disconnected, this, [this] { d->emitClosed(); });

    // ★ ready() 靠 TLS encrypted 事件异步触发（见 onEncrypted）：NetStack 先 createUdp()、connect 好
    //   ready 槽、才 associate()，此刻信号必已连上，不会丢（语义同 DirectOutbound 的异步 ready）。
    d->tls->connectToHost(d->server, d->serverPort, d->sni, trojanAlpn(), d->skipVerify);
}

void TrojanOutboundUdp::sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload)
{
    if (!d->ready || !d->tls) {
        return; // 未就绪前丢弃（上层 NetStack 会缓冲 pending 并在 ready 后补发，同 Socks5Udp）
    }
    // trojan UDP 帧：[ATYP+addr+port][payloadLen 2B 大端][CRLF][payload]
    QByteArray pkt;
    pkt += encodeSocksAddr(dstIp, dstPort);
    pkt += be16(quint16(payload.size() & 0xFFFF));
    pkt += "\r\n";
    pkt += payload;
    d->tls->write(pkt);
}

void TrojanOutboundUdp::closeSession()
{
    if (d->tls) {
        d->tls->abort();
    }
    d->emitClosed();
}

bool TrojanOutboundUdp::isReady() const
{
    return d->ready;
}

// ============================ 注册 ============================

void registerTrojan(OutboundRegistry &reg)
{
    // 本单元只做 Trojan-over-TLS 裸 TCP，不看 network、没实现 ws/grpc 传输封装。带 `network: ws/grpc`
    // 的 trojan 节点若被静默当成裸 TLS 去拨，会失败并伪装成「网络不通」。故非裸 TCP 一律返回 nullptr
    // 回退核心。理由详见 OutboundRegistry.h 的传输守卫注释。
    reg.registerProto(
        QStringLiteral("trojan"),
        [](const ProxyNode &n, QObject *p) -> IOutboundTcp * {
            if (!coastTransportSupported(n.network, {})) {
                qInfo("[trojan] 节点「%s」传输 network=%s 未实现（本单元仅裸 TCP+TLS）→ 回退核心",
                      qUtf8Printable(n.name), qUtf8Printable(n.network));
                return nullptr;
            }
            return new TrojanOutboundTcp(n, p);
        },
        [](const ProxyNode &n, QObject *p) -> IOutboundUdp * {
            if (!coastTransportSupported(n.network, {}))
                return nullptr;
            return new TrojanOutboundUdp(n, p);
        });
}
