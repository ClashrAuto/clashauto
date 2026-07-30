#include "ShadowsocksOutbound.h"

#include "../ProxyConfig.h"
#include "../crypto/Aead.h"
#include "../crypto/Kdf.h"
#include "OutboundRegistry.h"

#include <QHostInfo>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include <cstring>

// 说明：本实现是「Socks5Tcp/DirectOutboundTcp 的字节管道」外面再套一层 Shadowsocks-AEAD（SIP004）
// 加解密。凡与加解密无关的东西（背压闸门、零拷贝上行的生命周期契约、established/closed 的信号时序、
// readPaused 的排队补读）都逐点照搬 DirectOutbound，目的就是让 NetStack 侧无法区分出站是谁。
//
// Shadowsocks-AEAD（SIP004）线格式，务必逐字节一致（错了是静默的：能加密但对端解不开）：
//   密钥两级派生：masterKey = Kdf::ssMasterKey(password, keySize)；每方向一条流各有一个随机 salt，
//     subkey = Kdf::hkdfSha1(masterKey, salt, "ss-subkey", keySize)。subkey 才是喂给 Aead 的 key。
//   nonce：12 字节小端计数器，每 seal/open 一次自增 1；收发各自独立、都从全 0 起。
//   TCP 流：  [salt(keySize, 明文)] 之后是若干 chunk；每个 chunk =
//              seal(subkey, nonce++, 2 字节大端 payloadLen) || seal(subkey, nonce++, payload)
//              单 chunk payload ≤ 0x3FFF，超出拆块。**首段上行明文 = 目标地址(SOCKS5 ATYP 格式) || 真实载荷**。
//   UDP 包：  [salt(keySize)] || seal(subkey, nonce=全0, [SOCKS5 目标地址] || payload)，每个 datagram 独立。

namespace {

constexpr int kChunkMax = 0x3FFF;                            // SIP004 单 chunk payload 上限
const QByteArray kSubkeyInfo = QByteArrayLiteral("ss-subkey"); // HKDF info

// 12 字节小端 nonce 计数器 +1（低位在前）。到达 0xFF 进位到下一字节。
void incNonce(QByteArray &n)
{
    for (int i = 0; i < n.size(); ++i) {
        const quint8 v = quint8(quint8(n.at(i)) + 1);
        n[i] = char(v);
        if (v != 0) {
            break; // 未进位，结束
        }
    }
}

// n 个密码学随机字节（salt 用）。逐字节取 CSPRNG，避免对 QByteArray::data() 做 quint32* 重解释的对齐假设。
QByteArray randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    for (int i = 0; i < n; ++i) {
        b[i] = char(QRandomGenerator::system()->generate() & 0xFF);
    }
    return b;
}

// 端口大端两字节
QByteArray be16(quint16 v)
{
    QByteArray b;
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
    return b;
}

// 把「主机名/字面量 + 端口」编码成 SOCKS5 地址（ATYP 1=IPv4 / 4=IPv6 / 3=域名）。SS 的目标地址就用这套。
QByteArray encodeSocksAddrHost(const QString &host, quint16 port)
{
    QByteArray out;
    QHostAddress addr;
    if (addr.setAddress(host) && addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ip = addr.toIPv4Address();
        out.append(char(0x01));
        out.append(char((ip >> 24) & 0xFF));
        out.append(char((ip >> 16) & 0xFF));
        out.append(char((ip >> 8) & 0xFF));
        out.append(char(ip & 0xFF));
    } else if (addr.setAddress(host) && addr.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = addr.toIPv6Address(); // c[0..15] 已是网络序（大端）
        out.append(char(0x04));
        out.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        const QByteArray h = host.toUtf8();
        out.append(char(0x03));
        out.append(char(h.size() & 0xFF));
        out.append(h);
    }
    out.append(be16(port));
    return out;
}

// 把 IP 字面量 + 端口编码成 SOCKS5 地址（UDP 上行目标恒是 IP，不含域名）。
QByteArray encodeSocksAddrIp(const QHostAddress &ip, quint16 port)
{
    QByteArray out;
    if (ip.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = ip.toIPv6Address();
        out.append(char(0x04));
        out.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        const quint32 v4 = ip.toIPv4Address();
        out.append(char(0x01));
        out.append(char((v4 >> 24) & 0xFF));
        out.append(char((v4 >> 16) & 0xFF));
        out.append(char((v4 >> 8) & 0xFF));
        out.append(char(v4 & 0xFF));
    }
    out.append(be16(port));
    return out;
}

// 从明文缓冲头部解出一条 SOCKS5 地址（UDP 回程用）。成功返回「地址+端口」占用的字节数并写出 addr/port；
// 字节不足 / 不支持的 ATYP 返回 -1。只支持 IPv4(0x01)/IPv6(0x04)；域名(0x03) 回程本客户端用不到（上行发的
// 是 IP，服务器原样回填），遇到即视为无法定位源，返回 -1 丢弃该包。
int parseSocksAddr(const QByteArray &buf, QHostAddress *addr, quint16 *port)
{
    if (buf.size() < 1) {
        return -1;
    }
    const quint8 atyp = quint8(buf.at(0));
    if (atyp == 0x01) {
        if (buf.size() < 1 + 4 + 2) {
            return -1;
        }
        const quint32 ip = (quint32(quint8(buf.at(1))) << 24) | (quint32(quint8(buf.at(2))) << 16)
                         | (quint32(quint8(buf.at(3))) << 8) | quint32(quint8(buf.at(4)));
        *addr = QHostAddress(ip);
        *port = quint16((quint8(buf.at(5)) << 8) | quint8(buf.at(6)));
        return 7;
    }
    if (atyp == 0x04) {
        if (buf.size() < 1 + 16 + 2) {
            return -1;
        }
        Q_IPV6ADDR v6;
        std::memcpy(v6.c, buf.constData() + 1, 16); // 网络序，直接喂 QHostAddress
        *addr = QHostAddress(v6);
        *port = quint16((quint8(buf.at(17)) << 8) | quint8(buf.at(18)));
        return 19;
    }
    return -1; // 0x03 域名回程或未知 ATYP：丢弃
}

} // namespace

// ============================ ShadowsocksOutboundTcp ============================

class ShadowsocksOutboundTcp::Priv
{
public:
    explicit Priv(ShadowsocksOutboundTcp *owner) : q(owner) {}

    ShadowsocksOutboundTcp *q = nullptr;
    QTcpSocket *sock = nullptr;

    // —— 从节点解析出的静态参数（构造时定）——
    coastcore::Aead::Method method = coastcore::Aead::Method::Invalid;
    int keySize = 0;
    QByteArray masterKey;
    QString server;
    quint16 port = 0;

    QString dstHost; // 最终目标（编进加密流首段）
    quint16 dstPort = 0;

    // —— 上行加密流状态 ——
    QByteArray sendSubkey;
    QByteArray sendNonce; // 12B 小端计数器
    QByteArray pending;   // established 前缓冲的上行**明文**（尚未加密下发）
    bool targetSent = false;

    // —— 下行解密流状态 ——
    QByteArray recvBuf;    // 累积的下行密文（凑够一个 chunk 再解）
    QByteArray recvSubkey;
    QByteArray recvNonce;  // 12B 小端计数器
    bool haveSalt = false; // 是否已收齐并消费掉下行流首的 salt
    int wantLen = -1;      // >=0：已解出长度、正等这么多字节的 payload chunk；-1：下一步该解长度 chunk

    bool established = false;
    bool closedEmitted = false;
    bool readPaused = false;

    // TCP connected：发流首明文 salt，派生上行子密钥，established；随后把「目标地址 || 已缓冲明文」冲进加密流。
    void onConnected()
    {
        sock->setSocketOption(QAbstractSocket::LowDelayOption, 1); // 关 Nagle，同 Direct/Socks5（须在 connected 里设）

        const int ks = keySize;
        const QByteArray salt = randomBytes(ks);
        sendSubkey = coastcore::Kdf::hkdfSha1(masterKey, salt, kSubkeyInfo, ks);
        if (sendSubkey.size() != ks) {
            fail(QStringLiteral("ss: derive send subkey failed"));
            return;
        }
        sendNonce = QByteArray(coastcore::Aead::nonceSize(), '\0');
        sock->write(salt); // 明文 salt 置于加密流之首

        // ★ 必须先把「目标地址(SOCKS5 格式) || established 前缓冲的明文」写进加密流,**再** emit established：
        //   established 的槽可能**同步**回写上行数据(write→encryptAndSend)。若先 emit，那份数据会抢在
        //   目标地址 chunk 之前进流(nonce 0)，服务端把它当 SOCKS5 地址解析 → 整条流错乱。
        //   真机实测(mihomo ss-in)正因此拨不通:GET 落 nonce0、目标地址落 nonce2、服务端只回 0 字节。
        QByteArray head = encodeSocksAddrHost(dstHost, dstPort);
        head += pending;
        pending.clear();
        targetSent = true;
        encryptAndSend(head); // head 至少含目标地址，非空(此刻 established 仍 false，无人能抢先写)

        established = true;
        emit q->established();
    }

    // 把一段明文按 SIP004 chunk 加密后写进 socket：每 chunk = seal(2B 大端长度) || seal(payload)。
    void encryptAndSend(const QByteArray &plain)
    {
        if (plain.isEmpty() || !sock) {
            return;
        }
        qsizetype off = 0;
        while (off < plain.size()) {
            const int n = int(qMin<qsizetype>(kChunkMax, plain.size() - off));
            QByteArray lenBytes;
            lenBytes.append(char((n >> 8) & 0xFF));
            lenBytes.append(char(n & 0xFF));

            const QByteArray lenCipher =
                coastcore::Aead::seal(method, sendSubkey, sendNonce, QByteArray(), lenBytes);
            incNonce(sendNonce);
            const QByteArray payCipher = coastcore::Aead::seal(
                method, sendSubkey, sendNonce, QByteArray(), plain.mid(off, n));
            incNonce(sendNonce);

            if (lenCipher.isEmpty() || payCipher.isEmpty()) {
                fail(QStringLiteral("ss: seal chunk failed"));
                return;
            }
            sock->write(lenCipher);
            sock->write(payCipher);
            off += n;
        }
    }

    void onReadyRead()
    {
        if (!sock) {
            return;
        }
        // 背压中一个字节都不读：数据留在 Qt 读缓冲（上限已设）→ 满了内核收窗关闭 → 远端被压住。
        // 恢复时由 setReadPaused 排队补读。语义同 Direct/Socks5。
        if (readPaused) {
            return;
        }
        recvBuf += sock->readAll();

        // 1) 先消费下行流首的 salt（keySize 字节），派生下行子密钥。
        if (!haveSalt) {
            if (recvBuf.size() < keySize) {
                return;
            }
            const QByteArray salt = recvBuf.left(keySize);
            recvBuf.remove(0, keySize);
            recvSubkey = coastcore::Kdf::hkdfSha1(masterKey, salt, kSubkeyInfo, keySize);
            if (recvSubkey.size() != keySize) {
                fail(QStringLiteral("ss: derive recv subkey failed"));
                return;
            }
            recvNonce = QByteArray(coastcore::Aead::nonceSize(), '\0');
            haveSalt = true;
        }

        // 2) 循环解 chunk：先长度 chunk(2+tag)，再 payload chunk(len+tag)；字节不够就等下一拍。
        const int tag = coastcore::Aead::tagSize();
        for (;;) {
            if (wantLen < 0) {
                if (recvBuf.size() < 2 + tag) {
                    return;
                }
                QByteArray lenPlain;
                if (!coastcore::Aead::open(method, recvSubkey, recvNonce, QByteArray(),
                                           recvBuf.left(2 + tag), &lenPlain)
                    || lenPlain.size() != 2) {
                    fail(QStringLiteral("ss: open length chunk failed"));
                    return;
                }
                incNonce(recvNonce);
                recvBuf.remove(0, 2 + tag);
                wantLen = (quint8(lenPlain.at(0)) << 8) | quint8(lenPlain.at(1));
                if (wantLen > kChunkMax) { // 长度非法（>0x3FFF）：流已错乱，收口
                    fail(QStringLiteral("ss: chunk length too large"));
                    return;
                }
            }
            if (recvBuf.size() < wantLen + tag) {
                return; // payload chunk 未到齐
            }
            QByteArray payload;
            if (!coastcore::Aead::open(method, recvSubkey, recvNonce, QByteArray(),
                                       recvBuf.left(wantLen + tag), &payload)) {
                fail(QStringLiteral("ss: open payload chunk failed"));
                return;
            }
            incNonce(recvNonce);
            recvBuf.remove(0, wantLen + tag);
            wantLen = -1;
            if (!payload.isEmpty()) {
                emit q->dataReceived(payload);
            }
        }
    }

    void fail(const QString &reason)
    {
        if (closedEmitted) {
            return;
        }
        closedEmitted = true; // failed 即终态：抑制随后 abort 触发的 closed，避免双重信号
        emit q->failed(reason);
        if (sock) {
            sock->abort();
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

ShadowsocksOutboundTcp::ShadowsocksOutboundTcp(const ProxyNode &node, QObject *parent)
    : IOutboundTcp(parent), d(new Priv(this))
{
    d->method = coastcore::Aead::methodFromName(node.cipher);
    d->keySize = coastcore::Aead::keySize(d->method);
    d->server = node.server;
    d->port = node.port;
    if (d->method != coastcore::Aead::Method::Invalid) {
        d->masterKey = coastcore::Kdf::ssMasterKey(node.password.toUtf8(), d->keySize);
    }
}

ShadowsocksOutboundTcp::~ShadowsocksOutboundTcp()
{
    delete d;
}

void ShadowsocksOutboundTcp::connectTo(const QString &dstHost, quint16 dstPort, const QString & /*user*/)
{
    // cipher 非法：无法加密，本次连接直接失败。closeConn 里 socks 用 deleteLater，故此处同步 emit failed
    // 安全（上层 failed 槽会 delete 连接对象 c，但本对象随包装器 deleteLater，栈返回后仍在）。
    if (d->method == coastcore::Aead::Method::Invalid) {
        d->closedEmitted = true;
        emit failed(QStringLiteral("ss: unsupported cipher"));
        return;
    }

    d->dstHost = dstHost;
    d->dstPort = dstPort;
    d->established = false;
    d->closedEmitted = false;
    d->readPaused = false;
    d->targetSent = false;
    d->haveSalt = false;
    d->wantLen = -1;
    d->pending.clear();
    d->recvBuf.clear();

    d->sock = new QTcpSocket(this);
    // ★ 必须显式 NoProxy：本 App 会开系统代理，否则「直连 ss 服务器」又被 Qt 默认 ApplicationProxy 绕回。
    d->sock->setProxy(QNetworkProxy::NoProxy);
    d->sock->setReadBufferSize(64 * 1024); // 与 Direct/Socks5 一致的下行读缓冲上限

    connect(d->sock, &QTcpSocket::connected, this, [this] { d->onConnected(); });
    connect(d->sock, &QTcpSocket::readyRead, this, [this] { d->onReadyRead(); });
    // 上行排空进度：转发给上层做「按量归还 lwIP 接收窗口」。注意这里的字节数是**密文**字节（含 salt/tag/
    // 长度块），比明文多；上层只用它做水位评估（是否已排空），量级偏大不影响正确性，只会略保守。
    connect(d->sock, &QTcpSocket::bytesWritten, this,
            [this](qint64 n) { emit upstreamBytesWritten(n); });
    connect(d->sock, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (d->established) {
            d->emitClosed(); // 建立后出错（对端 RST/关闭）视为正常关闭
        } else {
            d->fail(d->sock->errorString()); // 建立前出错为连接失败
        }
    });
    connect(d->sock, &QTcpSocket::disconnected, this, [this] { d->emitClosed(); });

    d->sock->connectToHost(d->server, d->port); // 域名交给 QTcpSocket 解析
}

void ShadowsocksOutboundTcp::write(const QByteArray &data)
{
    if (d->established && d->sock) {
        d->encryptAndSend(data);
    } else {
        d->pending += data; // 建立前缓冲明文（隐式共享，零拷贝），established 时随目标地址一并加密下发
    }
}

// 裸缓冲版本，生命周期契约同 Socks5Tcp（见 IOutbound.h）：data 只需活到本次调用返回。
//  · established：encryptAndSend 内的 seal 当场同步读完 data 生成密文，可用 fromRawData 免拷贝。
//  · 未 established：字节要留到 connected 后才发，指针必悬垂 → append(const char*,size) 深拷贝进 pending。
void ShadowsocksOutboundTcp::write(const char *data, qsizetype size)
{
    if (!data || size <= 0) {
        return;
    }
    if (d->established && d->sock) {
        d->encryptAndSend(QByteArray::fromRawData(data, size)); // 同步消费，安全
    } else {
        d->pending.append(data, size); // 深拷贝
    }
}

void ShadowsocksOutboundTcp::closeTunnel()
{
    if (d->sock) {
        d->sock->abort();
    }
    d->emitClosed();
}

bool ShadowsocksOutboundTcp::isEstablished() const
{
    return d->established;
}

qint64 ShadowsocksOutboundTcp::bytesToWrite() const
{
    // pending（established 前缓冲的明文）也算进来，否则上层会误判「上行已排空」而开满接收窗口。
    return qint64(d->pending.size()) + (d->sock ? d->sock->bytesToWrite() : 0);
}

void ShadowsocksOutboundTcp::setReadPaused(bool paused)
{
    if (d->readPaused == paused) {
        return;
    }
    d->readPaused = paused;
    if (paused || !d->sock || d->sock->bytesAvailable() <= 0) {
        return;
    }
    // 恢复且 socket 缓冲里有存货：主动补读一次（readyRead 只在有新数据到达时才发）。补读会同步 emit
    // dataReceived，可能当场把整条连接（含本对象）拆掉 → 排队到事件循环里做，被 deleteLater 后随析构丢弃。
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!d->readPaused) {
                d->onReadyRead();
            }
        },
        Qt::QueuedConnection);
}

bool ShadowsocksOutboundTcp::isReadPaused() const
{
    return d->readPaused;
}

// ============================ ShadowsocksOutboundUdp ============================

class ShadowsocksOutboundUdp::Priv
{
public:
    explicit Priv(ShadowsocksOutboundUdp *owner) : q(owner) {}

    ShadowsocksOutboundUdp *q = nullptr;
    QUdpSocket *udp = nullptr;

    coastcore::Aead::Method method = coastcore::Aead::Method::Invalid;
    int keySize = 0;
    QByteArray masterKey;
    QString server;
    quint16 port = 0;

    QHostAddress serverAddr; // 解析后的 ss 服务器地址（域名经 QHostInfo 解析后填）
    bool serverResolved = false;
    bool ready = false;
    bool closedEmitted = false;

    void onDatagram()
    {
        while (udp && udp->hasPendingDatagrams()) {
            const QNetworkDatagram dg = udp->receiveDatagram();
            const QByteArray buf = dg.data();
            // 回程包：[salt(keySize)] || seal(subkey, nonce=0, [SOCKS5 源地址] || payload)。
            if (buf.size() < keySize + coastcore::Aead::tagSize()) {
                continue; // 连 salt+tag 都不够，丢弃
            }
            const QByteArray salt = buf.left(keySize);
            const QByteArray subkey =
                coastcore::Kdf::hkdfSha1(masterKey, salt, kSubkeyInfo, keySize);
            if (subkey.size() != keySize) {
                continue;
            }
            const QByteArray zeroNonce(coastcore::Aead::nonceSize(), '\0');
            QByteArray plain;
            if (!coastcore::Aead::open(method, subkey, zeroNonce, QByteArray(), buf.mid(keySize),
                                       &plain)) {
                continue; // 验签失败：丢弃（AEAD 绝不外泄未认证数据）
            }
            QHostAddress src;
            quint16 srcPort = 0;
            const int off = parseSocksAddr(plain, &src, &srcPort);
            if (off < 0) {
                continue; // 源地址无法解析（含域名回程）：丢弃
            }
            emit q->datagramReceived(src, srcPort, plain.mid(off));
        }
    }

    void fail(const QString &reason)
    {
        if (closedEmitted) {
            return;
        }
        closedEmitted = true;
        emit q->failed(reason);
        if (udp) {
            udp->close();
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

ShadowsocksOutboundUdp::ShadowsocksOutboundUdp(const ProxyNode &node, QObject *parent)
    : IOutboundUdp(parent), d(new Priv(this))
{
    d->method = coastcore::Aead::methodFromName(node.cipher);
    d->keySize = coastcore::Aead::keySize(d->method);
    d->server = node.server;
    d->port = node.port;
    if (d->method != coastcore::Aead::Method::Invalid) {
        d->masterKey = coastcore::Kdf::ssMasterKey(node.password.toUtf8(), d->keySize);
    }
}

ShadowsocksOutboundUdp::~ShadowsocksOutboundUdp()
{
    delete d;
}

void ShadowsocksOutboundUdp::associate(const QString & /*user*/)
{
    d->ready = false;
    d->closedEmitted = false;
    d->serverResolved = false;

    if (d->method == coastcore::Aead::Method::Invalid) {
        QTimer::singleShot(0, this, [this] { d->fail(QStringLiteral("ss: unsupported cipher")); });
        return;
    }

    d->udp = new QUdpSocket(this);
    d->udp->setProxy(QNetworkProxy::NoProxy); // 同 TCP：别被系统代理绕回去
    connect(d->udp, &QUdpSocket::readyRead, this, [this] { d->onDatagram(); });
    connect(d->udp, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (d->ready) {
            d->emitClosed();
        } else {
            d->fail(d->udp ? d->udp->errorString() : QStringLiteral("ss udp error"));
        }
    });

    // 双栈绑定（Any = 同时收 v4/v6 回程）。
    if (!d->udp->bind(QHostAddress(QHostAddress::Any), 0)) {
        const QString reason = d->udp->errorString();
        QTimer::singleShot(0, this, [this, reason] { d->fail(reason); });
        return;
    }

    // ss 服务器地址：字面量直接用；域名经 QHostInfo 异步解析。ready() 延到地址就绪后再发（且必须异步，
    // 同 DirectOutboundUdp：上层先 connect ready 槽再 associate，同步 emit 会丢）。
    QHostAddress lit;
    if (lit.setAddress(d->server)) {
        d->serverAddr = lit;
        d->serverResolved = true;
        QTimer::singleShot(0, this, [this] {
            if (d->closedEmitted || !d->udp) {
                return;
            }
            d->ready = true;
            emit ready();
        });
    } else {
        QHostInfo::lookupHost(d->server, this, [this](const QHostInfo &info) {
            if (d->closedEmitted || !d->udp) {
                return;
            }
            if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
                d->fail(QStringLiteral("ss: resolve server failed: %1").arg(info.errorString()));
                return;
            }
            d->serverAddr = info.addresses().first();
            d->serverResolved = true;
            d->ready = true;
            emit ready();
        });
    }
}

void ShadowsocksOutboundUdp::sendTo(const QHostAddress &dstIp, quint16 dstPort,
                                    const QByteArray &payload)
{
    if (!d->ready || !d->serverResolved || !d->udp) {
        return; // 未就绪/服务器地址未定前丢弃（UDP/DNS 会重试，上层也会缓冲 pending 在 ready 后补发）
    }
    // 每个上行 datagram 独立：随机 salt → subkey → 单次 seal(nonce=0, [SOCKS5 目标地址] || payload)。
    const QByteArray salt = randomBytes(d->keySize);
    const QByteArray subkey =
        coastcore::Kdf::hkdfSha1(d->masterKey, salt, kSubkeyInfo, d->keySize);
    if (subkey.size() != d->keySize) {
        return;
    }
    QByteArray plain = encodeSocksAddrIp(dstIp, dstPort);
    plain += payload;
    const QByteArray zeroNonce(coastcore::Aead::nonceSize(), '\0');
    const QByteArray cipher =
        coastcore::Aead::seal(d->method, subkey, zeroNonce, QByteArray(), plain);
    if (cipher.isEmpty()) {
        return;
    }
    QByteArray pkt = salt;
    pkt += cipher;
    d->udp->writeDatagram(pkt, d->serverAddr, d->port);
}

void ShadowsocksOutboundUdp::closeSession()
{
    if (d->udp) {
        d->udp->close();
    }
    d->emitClosed();
}

bool ShadowsocksOutboundUdp::isReady() const
{
    return d->ready;
}

// ============================ 注册 ============================

void registerShadowsocks(OutboundRegistry &reg)
{
    reg.registerProto(
        QStringLiteral("ss"),
        [](const ProxyNode &n, QObject *p) -> IOutboundTcp * {
            return new ShadowsocksOutboundTcp(n, p);
        },
        [](const ProxyNode &n, QObject *p) -> IOutboundUdp * {
            return new ShadowsocksOutboundUdp(n, p);
        });
}
