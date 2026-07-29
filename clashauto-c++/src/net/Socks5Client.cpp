#include "Socks5Client.h"

#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QUdpSocket>

#include <cstring>

// 说明：这里手写 SOCKS5（RFC1928 + 用户名/密码认证 RFC1929）。之所以不用 QNetworkProxy，是因为
// 我们要「每设备唯一用户名」认证（user=dev-<mac>，密码恒 "coast"），让 mihomo 侧靠 IN-USER 规则
// 做每设备分流/流量归属——QNetworkProxy 的凭据模型套不进这套自定义握手，且我们要拿到裸字节管道
// （TCP CONNECT）与 UDP ASSOCIATE 中继端点，直接对接用户态网络栈。
//
// 关键约束（务必保持）：
//  - readyRead 可能是「部分读」：握手每一步都要先把字节累积进 inbuf，凑够长度才推进状态机；
//    否则会读到半个回复而错判。
//  - 端口一律大端；host 恒 127.0.0.1（LocalHost，避免 DNS）。

namespace {

// 问候包：user 为空只报「无认证 0x00」；非空则同时报「无认证 0x00 + 用户名/密码 0x02」，
// 让 mihomo 二选一（无认证 inbound 时也能连上，认证 inbound 时走 0x02）。
QByteArray buildGreeting(const QByteArray &user)
{
    QByteArray g;
    if (user.isEmpty()) {
        g.append(char(0x05));
        g.append(char(0x01));
        g.append(char(0x00));
    } else {
        g.append(char(0x05));
        g.append(char(0x02));
        g.append(char(0x00));
        g.append(char(0x02));
    }
    return g;
}

// RFC1929 认证包：ver=0x01, ulen, uname, plen, passwd。密码恒 "coast"（mihomo 只认 user 做 IN-USER）。
QByteArray buildAuth(const QByteArray &user)
{
    static const QByteArray kPass = QByteArrayLiteral("coast");
    QByteArray a;
    a.append(char(0x01));
    a.append(char(user.size() & 0xFF)); // dev-<mac> 远短于 255，不会截断
    a.append(user);
    a.append(char(kPass.size() & 0xFF));
    a.append(kPass);
    return a;
}

// 端口大端两字节
QByteArray be16(quint16 v)
{
    QByteArray b;
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
    return b;
}

// SOCKS5 应答码 → 可读原因（RFC1928 §6）
QString decodeRep(quint8 rep)
{
    switch (rep) {
    case 0x00: return QStringLiteral("succeeded");
    case 0x01: return QStringLiteral("general SOCKS server failure");
    case 0x02: return QStringLiteral("connection not allowed by ruleset");
    case 0x03: return QStringLiteral("network unreachable");
    case 0x04: return QStringLiteral("host unreachable");
    case 0x05: return QStringLiteral("connection refused");
    case 0x06: return QStringLiteral("TTL expired");
    case 0x07: return QStringLiteral("command not supported");
    case 0x08: return QStringLiteral("address type not supported");
    default:   return QStringLiteral("unknown SOCKS error %1").arg(rep);
    }
}

// 计算一条 SOCKS5 应答（[ver,rep,rsv,atyp, bnd.addr, bnd.port]）的完整长度；字节不足返回 -1。
// 域名(0x03) 的地址段 = 1 字节长度 + 长度字节个字符。
int socksReplyLength(const QByteArray &buf)
{
    if (buf.size() < 4) {
        return -1;
    }
    const quint8 atyp = quint8(buf.at(3));
    int addrLen = 0;
    if (atyp == 0x01) {
        addrLen = 4;
    } else if (atyp == 0x04) {
        addrLen = 16;
    } else if (atyp == 0x03) {
        if (buf.size() < 5) {
            return -1;
        }
        addrLen = 1 + quint8(buf.at(4));
    } else {
        return -2; // 未知地址类型：调用方判负数即报错
    }
    const int total = 4 + addrLen + 2;
    return buf.size() < total ? -1 : total;
}

} // namespace

// ============================ Socks5Tcp ============================

class Socks5Tcp::Priv
{
public:
    explicit Priv(Socks5Tcp *owner) : q(owner) {}

    Socks5Tcp *q = nullptr;
    QTcpSocket *sock = nullptr;

    enum class Phase { Idle, Greeting, Auth, Connect, Established, Closed };
    Phase phase = Phase::Idle;

    QByteArray user;    // 已转好的 utf8
    QString dstHost;
    quint16 dstPort = 0;

    QByteArray inbuf;   // 握手期累积的下行字节（凑够再解析）
    QByteArray pending; // established 前缓冲的上行字节
    bool established = false;
    bool closedEmitted = false;
    bool readPaused = false; // 下行背压：上层排不动了，暂时不从 socket 读

    void onConnected()
    {
        phase = Phase::Greeting;
        sock->write(buildGreeting(user));
    }

    void onReadyRead()
    {
        if (!sock) {
            return;
        }
        // 背压中：**一个字节都不读**。数据留在 Qt 读缓冲（setReadBufferSize 已限死）里，
        // 满了之后 Qt 就不再从内核取，内核收窗关闭 → 远端被压住。恢复时由 setReadPaused
        // 排队补读，所以「不读」不会把数据永久卡住。
        if (readPaused) {
            return;
        }
        // 建立后就是纯字节管道：读到什么直接上抛为下行数据。
        if (phase == Phase::Established) {
            const QByteArray data = sock->readAll();
            if (!data.isEmpty()) {
                emit q->dataReceived(data);
            }
            return;
        }
        inbuf += sock->readAll();

        // 一次 readyRead 可能同时带来多个握手阶段的回复，循环推进直到数据不够或完成/出错。
        for (;;) {
            switch (phase) {
            case Phase::Greeting: {
                if (inbuf.size() < 2) {
                    return;
                }
                const quint8 method = quint8(inbuf.at(1));
                inbuf.remove(0, 2);
                if (method == 0x00) {
                    phase = Phase::Connect;
                    sendConnect();
                } else if (method == 0x02) {
                    phase = Phase::Auth;
                    sock->write(buildAuth(user));
                } else if (method == 0xFF) {
                    fail(QStringLiteral("no acceptable auth"));
                    return;
                } else {
                    fail(QStringLiteral("unexpected auth method %1").arg(method));
                    return;
                }
                continue;
            }
            case Phase::Auth: {
                if (inbuf.size() < 2) {
                    return;
                }
                const quint8 status = quint8(inbuf.at(1));
                inbuf.remove(0, 2);
                if (status != 0x00) {
                    fail(QStringLiteral("username/password auth rejected"));
                    return;
                }
                phase = Phase::Connect;
                sendConnect();
                continue;
            }
            case Phase::Connect: {
                const int len = socksReplyLength(inbuf);
                if (len == -1) {
                    return; // 字节不足，等下一拍
                }
                if (len < 0) {
                    fail(QStringLiteral("unsupported reply address type"));
                    return;
                }
                const quint8 rep = quint8(inbuf.at(1));
                inbuf.remove(0, len); // 连同变长 BND.ADDR/PORT 一起消费掉
                if (rep != 0x00) {
                    fail(QStringLiteral("CONNECT failed: %1").arg(decodeRep(rep)));
                    return;
                }
                phase = Phase::Established;
                established = true;
                emit q->established();
                if (!pending.isEmpty()) { // 建立前缓冲的上行字节，此刻补发
                    sock->write(pending);
                    pending.clear();
                }
                if (!inbuf.isEmpty()) { // 回复之后若已捎带下行数据，直接上抛
                    const QByteArray data = inbuf;
                    inbuf.clear();
                    emit q->dataReceived(data);
                }
                return;
            }
            default:
                return; // Idle / Established / Closed：不在此循环处理
            }
        }
    }

    // 组装 CONNECT 请求：dstHost 能解析为 IPv4 → ATYP 0x01 + 4 裸字节；解析为 IPv6 → ATYP 0x04 +
    // 16 裸字节（透明网关终结出的 v6 连接走这条：lwIP accept 里把设备想访问的 v6 服务器地址
    // ipaddr_ntoa 成串传进来，形如 "2606:...."，必须按 v6 字面量编码，否则会被当域名交给 mihomo
    // 去做一次没有意义的 DNS）；两者皆非 → 按域名 ATYP 0x03。
    void sendConnect()
    {
        QByteArray req;
        req.append(char(0x05));
        req.append(char(0x01)); // CMD=CONNECT
        req.append(char(0x00)); // RSV
        QHostAddress addr;
        if (addr.setAddress(dstHost) && addr.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 ip = addr.toIPv4Address();
            req.append(char(0x01));
            req.append(char((ip >> 24) & 0xFF));
            req.append(char((ip >> 16) & 0xFF));
            req.append(char((ip >> 8) & 0xFF));
            req.append(char(ip & 0xFF));
        } else if (addr.setAddress(dstHost) && addr.protocol() == QAbstractSocket::IPv6Protocol) {
            const Q_IPV6ADDR v6 = addr.toIPv6Address(); // c[0..15] 已是网络序（大端）
            req.append(char(0x04));
            req.append(reinterpret_cast<const char *>(v6.c), 16);
        } else {
            const QByteArray host = dstHost.toUtf8();
            req.append(char(0x03));
            req.append(char(host.size() & 0xFF));
            req.append(host);
        }
        req.append(be16(dstPort));
        sock->write(req);
    }

    void fail(const QString &reason)
    {
        if (phase == Phase::Closed) {
            return;
        }
        phase = Phase::Closed;
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
        phase = Phase::Closed;
        emit q->closed();
    }
};

Socks5Tcp::Socks5Tcp(quint16 socksPort, QObject *parent)
    : IOutboundTcp(parent), m_socksPort(socksPort), d(new Priv(this))
{
}

Socks5Tcp::~Socks5Tcp()
{
    delete d;
}

void Socks5Tcp::connectTo(const QString &dstHost, quint16 dstPort, const QString &user)
{
    d->user = user.toUtf8();
    d->dstHost = dstHost;
    d->dstPort = dstPort;
    d->established = false;
    d->closedEmitted = false;
    d->readPaused = false;
    d->inbuf.clear();
    d->pending.clear();

    d->sock = new QTcpSocket(this);
    // 读缓冲设上限（默认是 0 = 无上限，Qt 会一直往上读，暂停也就无从谈起）。
    // 取 64 KiB 与 NetStack 的下行水位 kToLwipHighWater 一致（那里有为什么不跟着 lwIP 的
    // TCP_WND/TCP_SND_BUF = 128 KiB 一起上调的理由）：非暂停态下每次 readyRead
    // 我们都当场读干净，64 KiB 远够一个事件循环周期内的回环吞吐；暂停态下它就是每条连接
    // 在本进程里额外滞留的上限。
    d->sock->setReadBufferSize(64 * 1024);
    // 用 this 作 context：本对象析构时自动断开这些连接，回调里安全访问 d。
    connect(d->sock, &QTcpSocket::connected, this, [this] {
        // 关 Nagle。这条 socket 是**纯转发管道**：写进来的每一段都是设备那边已经决定要发出去
        // 的字节（TLS 记录、HTTP 请求头…），再攒一手毫无意义 —— 攒的判据（「有未确认数据且这
        // 次不足一个 MSS」）在握手往返里真的会命中，白白给每次请求叠一次延迟。设备侧那条 TCP
        // 在 lwIP 里也是关 Nagle 的（NetStack 的 tcp_nagle_disable），两头对齐。
        // ★ 必须放在 connected 里：Qt6 的 QAbstractSocket::setSocketOption 在 socketEngine 还
        //   没建起来时**直接 return**，connectToHost 之前调等于没调。
        d->sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        d->onConnected();
    });
    connect(d->sock, &QTcpSocket::readyRead, this, [this] { d->onReadyRead(); });
    // 上行排空进度：转发出去给上层做「按量归还 lwIP 接收窗口」。握手包(greeting/auth/connect)
    // 也会触发一次，上层只是重新评估一下水位，无副作用。
    connect(d->sock, &QTcpSocket::bytesWritten, this,
            [this](qint64 n) { emit upstreamBytesWritten(n); });
    connect(d->sock, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        // 建立后出错（如对端 RST/关闭）视为正常关闭；建立前出错为握手/连接失败。
        if (d->established) {
            d->emitClosed();
        } else {
            d->fail(d->sock->errorString());
        }
    });
    connect(d->sock, &QTcpSocket::disconnected, this, [this] { d->emitClosed(); });

    d->phase = Priv::Phase::Idle;
    d->sock->connectToHost(QHostAddress(QHostAddress::LocalHost), m_socksPort);
}

void Socks5Tcp::write(const QByteArray &data)
{
    if (d->established && d->sock) {
        d->sock->write(data);
    } else {
        d->pending += data; // 建立前先缓冲，established 时统一补发
    }
    // 注意：**故意不**转发给下面的裸指针重载。QByteArray += QByteArray 在 pending 为空时
    // 会退化成一次隐式共享赋值（零拷贝），转发过去反而会变成实打实的 memcpy。
}

// 裸缓冲版本。两条分支的性质完全不同，改动时必须分清（头文件里的生命周期契约就靠这个成立）：
//  · established：落到 QIODevice::write(const char*, qint64) —— QAbstractSocket::writeData
//    当场 memcpy 进自己的写缓冲（或直接交给 socket engine），**同步**消费完才返回。
//    所以调用方的缓冲只需活到本次调用返回，可以零拷贝。
//    注意它同样可能在写入途中同步 emit errorOccurred（对端已 RST）→ failed → 上层可能当场
//    把整条连接拆掉；但那与 data 指针无关，data 的所有权始终在调用方手上。
//  · 未 established：字节要**留到**握手完成后才发，指针必然悬垂 → 只能深拷贝。
//    这里用 QByteArray::append(const char*, qsizetype)，它是真拷贝；绝不能写成
//    `pending += QByteArray::fromRawData(data, size)` —— pending 为空时 append(QByteArray)
//    的快路径是隐式共享赋值，pending 会直接指向调用方的缓冲，返回后立刻变野指针。
void Socks5Tcp::write(const char *data, qsizetype size)
{
    if (!data || size <= 0) {
        return;
    }
    if (d->established && d->sock) {
        d->sock->write(data, size);
    } else {
        d->pending.append(data, size); // 深拷贝，见上
    }
}

void Socks5Tcp::closeTunnel()
{
    if (d->sock) {
        d->sock->abort();
    }
    d->emitClosed();
}

bool Socks5Tcp::isEstablished() const
{
    return d->established;
}

qint64 Socks5Tcp::bytesToWrite() const
{
    // pending 也要算进来：established 之前 write() 进来的字节还没交给 socket，
    // 漏算的话上层会误以为「上行已排空」而把接收窗口开满，正是要避免的那种无闸状态。
    return qint64(d->pending.size()) + (d->sock ? d->sock->bytesToWrite() : 0);
}

void Socks5Tcp::setReadPaused(bool paused)
{
    if (d->readPaused == paused) {
        return;
    }
    d->readPaused = paused;
    if (paused || !d->sock || d->sock->bytesAvailable() <= 0) {
        return;
    }
    // 恢复且缓冲里有存货：**必须**主动补读一次 —— readyRead 只在「有新数据到达」时才发，
    // 若远端此刻正好不再发（例如响应已发完），不补读就永远卡在这里（真死锁）。
    // 但补读会同步 emit dataReceived，而本函数的调用点在 lwIP 的 tcp_sent 回调里，
    // 上层的槽有可能当场把整条连接（含本对象）拆掉 → 排队到事件循环里做。
    // 本对象被 deleteLater 后，投递给它的排队事件会随析构一并丢弃，不会打到野指针上。
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!d->readPaused) {
                d->onReadyRead();
            }
        },
        Qt::QueuedConnection);
}

bool Socks5Tcp::isReadPaused() const
{
    return d->readPaused;
}

// ============================ Socks5Udp ============================

class Socks5Udp::Priv
{
public:
    explicit Priv(Socks5Udp *owner) : q(owner) {}

    Socks5Udp *q = nullptr;
    QTcpSocket *ctrl = nullptr; // 控制连接：ASSOCIATE 的 TCP，须为会话生命周期保持打开
    QUdpSocket *udp = nullptr;

    enum class Phase { Idle, Greeting, Auth, Associate, Ready, Closed };
    Phase phase = Phase::Idle;

    QByteArray user;
    QByteArray inbuf;

    QHostAddress relayAddr;  // BND.ADDR:BND.PORT —— 发 UDP 的中继端点
    quint16 relayPort = 0;
    bool ready = false;
    bool closedEmitted = false;

    void onConnected()
    {
        phase = Phase::Greeting;
        ctrl->write(buildGreeting(user));
    }

    void onCtrlReadyRead()
    {
        if (!ctrl) {
            return;
        }
        if (phase == Phase::Ready || phase == Phase::Closed) {
            ctrl->readAll(); // 会话建立后控制连接不应再有数据；读掉丢弃即可
            return;
        }
        inbuf += ctrl->readAll();
        for (;;) {
            switch (phase) {
            case Phase::Greeting: {
                if (inbuf.size() < 2) {
                    return;
                }
                const quint8 method = quint8(inbuf.at(1));
                inbuf.remove(0, 2);
                if (method == 0x00) {
                    sendAssociate();
                } else if (method == 0x02) {
                    phase = Phase::Auth;
                    ctrl->write(buildAuth(user));
                } else if (method == 0xFF) {
                    fail(QStringLiteral("no acceptable auth"));
                    return;
                } else {
                    fail(QStringLiteral("unexpected auth method %1").arg(method));
                    return;
                }
                continue;
            }
            case Phase::Auth: {
                if (inbuf.size() < 2) {
                    return;
                }
                const quint8 status = quint8(inbuf.at(1));
                inbuf.remove(0, 2);
                if (status != 0x00) {
                    fail(QStringLiteral("username/password auth rejected"));
                    return;
                }
                sendAssociate();
                continue;
            }
            case Phase::Associate: {
                const int len = socksReplyLength(inbuf);
                if (len == -1) {
                    return;
                }
                if (len < 0) {
                    fail(QStringLiteral("unsupported reply address type"));
                    return;
                }
                const quint8 rep = quint8(inbuf.at(1));
                const quint8 atyp = quint8(inbuf.at(3));
                if (rep != 0x00) {
                    inbuf.remove(0, len);
                    fail(QStringLiteral("UDP ASSOCIATE failed: %1").arg(decodeRep(rep)));
                    return;
                }
                if (atyp != 0x01) {
                    inbuf.remove(0, len);
                    fail(QStringLiteral("relay endpoint not IPv4"));
                    return;
                }
                // BND.ADDR(4) 在偏移 4，BND.PORT(2) 紧随其后
                const quint32 ip = (quint32(quint8(inbuf.at(4))) << 24)
                                 | (quint32(quint8(inbuf.at(5))) << 16)
                                 | (quint32(quint8(inbuf.at(6))) << 8)
                                 | quint32(quint8(inbuf.at(7)));
                relayPort = quint16((quint8(inbuf.at(8)) << 8) | quint8(inbuf.at(9)));
                inbuf.remove(0, len);
                // BND.ADDR 常为 0.0.0.0（表示「用你连控制连接的那个地址」）→ 回落 127.0.0.1
                relayAddr = (ip == 0) ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(ip);

                udp = new QUdpSocket(q);
                connect(udp, &QUdpSocket::readyRead, q, [this] { onDatagram(); });
                udp->bind(QHostAddress(QHostAddress::LocalHost), 0); // 绑本地临时端口以收回程
                phase = Phase::Ready;
                ready = true;
                emit q->ready();
                return;
            }
            default:
                return;
            }
        }
    }

    void sendAssociate()
    {
        phase = Phase::Associate;
        QByteArray req;
        req.append(char(0x05));
        req.append(char(0x03)); // CMD=UDP ASSOCIATE
        req.append(char(0x00)); // RSV
        req.append(char(0x01)); // ATYP=IPv4
        req.append(char(0x00)); // 0.0.0.0
        req.append(char(0x00));
        req.append(char(0x00));
        req.append(char(0x00));
        req.append(char(0x00)); // 端口 0
        req.append(char(0x00));
        ctrl->write(req);
    }

    void onDatagram()
    {
        while (udp && udp->hasPendingDatagrams()) {
            const QNetworkDatagram dg = udp->receiveDatagram();
            const QByteArray buf = dg.data();
            // SOCKS UDP 头：RSV(2) FRAG(1) ATYP(1) ADDR PORT(2) 之后才是载荷。
            // 支持 IPv4(0x01,4B) 与 IPv6(0x04,16B)；域名(0x03) 回程本客户端用不到，跳过。
            if (buf.size() < 4) {
                continue;
            }
            const quint8 atyp = quint8(buf.at(3));
            QHostAddress srcAddr;
            int portOff = 0;
            if (atyp == 0x01) {
                if (buf.size() < 10) {
                    continue;
                }
                const quint32 ip = (quint32(quint8(buf.at(4))) << 24)
                                 | (quint32(quint8(buf.at(5))) << 16)
                                 | (quint32(quint8(buf.at(6))) << 8)
                                 | quint32(quint8(buf.at(7)));
                srcAddr = QHostAddress(ip);
                portOff = 8;
            } else if (atyp == 0x04) {
                if (buf.size() < 22) {
                    continue;
                }
                Q_IPV6ADDR v6;
                memcpy(v6.c, buf.constData() + 4, 16); // 网络序，直接喂 QHostAddress
                srcAddr = QHostAddress(v6);
                portOff = 20;
            } else {
                continue; // 非 IPv4/IPv6 回程跳过
            }
            const quint16 port =
                quint16((quint8(buf.at(portOff)) << 8) | quint8(buf.at(portOff + 1)));
            const QByteArray payload = buf.mid(portOff + 2);
            emit q->datagramReceived(srcAddr, port, payload);
        }
    }

    void fail(const QString &reason)
    {
        if (phase == Phase::Closed) {
            return;
        }
        phase = Phase::Closed;
        closedEmitted = true;
        emit q->failed(reason);
        if (ctrl) {
            ctrl->abort();
        }
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
        phase = Phase::Closed;
        emit q->closed();
    }
};

Socks5Udp::Socks5Udp(quint16 socksPort, QObject *parent)
    : IOutboundUdp(parent), m_socksPort(socksPort), d(new Priv(this))
{
}

Socks5Udp::~Socks5Udp()
{
    delete d;
}

void Socks5Udp::associate(const QString &user)
{
    d->user = user.toUtf8();
    d->ready = false;
    d->closedEmitted = false;
    d->inbuf.clear();

    d->ctrl = new QTcpSocket(this);
    connect(d->ctrl, &QTcpSocket::connected, this, [this] { d->onConnected(); });
    connect(d->ctrl, &QTcpSocket::readyRead, this, [this] { d->onCtrlReadyRead(); });
    connect(d->ctrl, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        // 会话就绪后控制连接掉线 = 会话结束；就绪前出错 = 建立失败。
        if (d->ready) {
            d->emitClosed();
        } else {
            d->fail(d->ctrl->errorString());
        }
    });
    connect(d->ctrl, &QTcpSocket::disconnected, this, [this] { d->emitClosed(); });

    d->phase = Priv::Phase::Idle;
    d->ctrl->connectToHost(QHostAddress(QHostAddress::LocalHost), m_socksPort);
}

void Socks5Udp::sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload)
{
    if (!d->ready || !d->udp) {
        return; // 未就绪前丢弃（DNS/UDP 会重试，无需缓冲）
    }
    QByteArray dg;
    dg.append(char(0x00)); // RSV
    dg.append(char(0x00));
    dg.append(char(0x00)); // FRAG=0（不分片）
    if (dstIp.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = dstIp.toIPv6Address(); // 网络序
        dg.append(char(0x04)); // ATYP=IPv6
        dg.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        const quint32 ip = dstIp.toIPv4Address();
        dg.append(char(0x01)); // ATYP=IPv4
        dg.append(char((ip >> 24) & 0xFF));
        dg.append(char((ip >> 16) & 0xFF));
        dg.append(char((ip >> 8) & 0xFF));
        dg.append(char(ip & 0xFF));
    }
    dg.append(be16(dstPort));
    dg.append(payload);
    d->udp->writeDatagram(dg, d->relayAddr, d->relayPort);
}

void Socks5Udp::closeSession()
{
    if (d->ctrl) {
        d->ctrl->abort();
    }
    if (d->udp) {
        d->udp->close();
    }
    d->emitClosed();
}

bool Socks5Udp::isReady() const
{
    return d->ready;
}
