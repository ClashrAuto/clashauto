#include "DirectOutbound.h"

#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

// 说明：本实现是 Socks5Tcp/Socks5Udp「去掉 SOCKS5 握手」后的样子。凡是与握手无关的东西（背压闸门、
// 零拷贝上行的生命周期契约、established/closed 的信号时序、readPaused 的排队补读）都逐点照搬那边，
// 目的就是让 NetStack 侧无法区分「出站是直连还是拨 mihomo」。改动前请对照 Socks5Client.cpp 的对应注释。

// ============================ DirectOutboundTcp ============================

class DirectOutboundTcp::Priv
{
public:
    explicit Priv(DirectOutboundTcp *owner) : q(owner) {}

    DirectOutboundTcp *q = nullptr;
    QTcpSocket *sock = nullptr;

    QByteArray pending; // established 前缓冲的上行字节（连接尚在 TCP 握手中）
    bool established = false;
    bool closedEmitted = false;
    bool readPaused = false; // 下行背压：上层排不动了，暂时不从 socket 读

    void onReadyRead()
    {
        if (!sock) {
            return;
        }
        // 背压中一个字节都不读：数据留在 Qt 读缓冲（setReadBufferSize 已限死）→ 满了内核收窗关闭 →
        // 远端被压住。恢复时由 setReadPaused 排队补读，不会永久卡住。语义同 Socks5Tcp。
        if (readPaused) {
            return;
        }
        const QByteArray data = sock->readAll();
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

DirectOutboundTcp::DirectOutboundTcp(QObject *parent) : IOutboundTcp(parent), d(new Priv(this)) {}

DirectOutboundTcp::~DirectOutboundTcp()
{
    delete d;
}

void DirectOutboundTcp::connectTo(const QString &dstHost, quint16 dstPort, const QString & /*user*/)
{
    // user 直连用不到（无每设备分流/归属），忽略。
    d->established = false;
    d->closedEmitted = false;
    d->readPaused = false;
    d->pending.clear();

    d->sock = new QTcpSocket(this);
    // ★ 必须显式 NoProxy：否则本机开着系统代理时，Qt 默认 socket 会去读全局 ApplicationProxy，
    //   把这条「直连」又绕回代理里去。
    d->sock->setProxy(QNetworkProxy::NoProxy);
    // 读缓冲上限 64 KiB —— 与 Socks5Tcp 一致（理由见那边：非暂停态每拍读干净就够，暂停态即滞留上限）。
    d->sock->setReadBufferSize(64 * 1024);

    // 用 this 作 context：本对象析构时自动断开这些连接，回调里安全访问 d。
    connect(d->sock, &QTcpSocket::connected, this, [this] {
        // 关 Nagle：这条 socket 是纯转发管道，写进来的每段都是设备已决定要发的字节，攒一手只会平白
        // 叠延迟；设备侧那条 TCP 在 lwIP 里也是关 Nagle 的，两头对齐。必须放在 connected 里 ——
        // Qt6 setSocketOption 在 socketEngine 建起来前直接 return（同 Socks5Tcp 的注释）。
        d->sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        // 直连没有 SOCKS5 握手：TCP connected 就是隧道就绪。
        d->established = true;
        emit established();
        if (!d->pending.isEmpty()) { // 建立前缓冲的上行字节，此刻补发
            d->sock->write(d->pending);
            d->pending.clear();
        }
    });
    connect(d->sock, &QTcpSocket::readyRead, this, [this] { d->onReadyRead(); });
    // 上行排空进度：转发给上层做「按量归还 lwIP 接收窗口」。
    connect(d->sock, &QTcpSocket::bytesWritten, this,
            [this](qint64 n) { emit upstreamBytesWritten(n); });
    connect(d->sock, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        // 建立后出错（对端 RST/关闭）视为正常关闭；建立前出错为连接失败。
        if (d->established) {
            d->emitClosed();
        } else {
            d->fail(d->sock->errorString());
        }
    });
    connect(d->sock, &QTcpSocket::disconnected, this, [this] { d->emitClosed(); });

    // dstHost 可为 IPv4/IPv6 字面量或域名 —— connectToHost 会自行解析域名。
    d->sock->connectToHost(dstHost, dstPort);
}

void DirectOutboundTcp::write(const QByteArray &data)
{
    if (d->established && d->sock) {
        d->sock->write(data);
    } else {
        d->pending += data; // 建立前先缓冲，established 时统一补发
    }
    // 故意不转发给裸指针重载：pending 为空时 += QByteArray 会退化成隐式共享（零拷贝），转发反成 memcpy。
}

// 裸缓冲版本，生命周期契约同 Socks5Tcp（见 IOutbound.h）：data 只需活到本次调用返回。
//  · established：QIODevice::write(const char*,qint64) 当场同步拷进写缓冲，可零拷贝。
//  · 未 established：字节要留到 connected 后才发，指针必悬垂 → 必须深拷贝（append(const char*,size)
//    是真拷贝；绝不能写成 pending += QByteArray::fromRawData(...)，那会让 pending 指向调用方缓冲）。
void DirectOutboundTcp::write(const char *data, qsizetype size)
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

void DirectOutboundTcp::closeTunnel()
{
    if (d->sock) {
        d->sock->abort();
    }
    d->emitClosed();
}

bool DirectOutboundTcp::isEstablished() const
{
    return d->established;
}

qint64 DirectOutboundTcp::bytesToWrite() const
{
    // pending 也要算进来（established 前 write() 进来还没交给 socket 的字节），否则上层会误判「上行已
    // 排空」而把接收窗口开满 —— 正是要避免的无闸状态。
    return qint64(d->pending.size()) + (d->sock ? d->sock->bytesToWrite() : 0);
}

void DirectOutboundTcp::setReadPaused(bool paused)
{
    if (d->readPaused == paused) {
        return;
    }
    d->readPaused = paused;
    if (paused || !d->sock || d->sock->bytesAvailable() <= 0) {
        return;
    }
    // 恢复且缓冲里有存货：必须主动补读一次（readyRead 只在有新数据到达时才发，远端此刻若已发完就永远
    // 卡住）。但补读会同步 emit dataReceived，而本函数常从 lwIP 的 tcp_sent 回调里被调到，上层的槽
    // 可能当场把整条连接（含本对象）拆掉 → 排队到事件循环里做。被 deleteLater 后排队事件随析构丢弃。
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!d->readPaused) {
                d->onReadyRead();
            }
        },
        Qt::QueuedConnection);
}

bool DirectOutboundTcp::isReadPaused() const
{
    return d->readPaused;
}

// ============================ DirectOutboundUdp ============================

class DirectOutboundUdp::Priv
{
public:
    explicit Priv(DirectOutboundUdp *owner) : q(owner) {}

    DirectOutboundUdp *q = nullptr;
    QUdpSocket *udp = nullptr;
    bool ready = false;
    bool closedEmitted = false;

    void onDatagram()
    {
        while (udp && udp->hasPendingDatagrams()) {
            const QNetworkDatagram dg = udp->receiveDatagram();
            QHostAddress src = dg.senderAddress();
            // 双栈 socket 收 IPv4 回程时源地址会以 IPv4-mapped IPv6（::ffff:a.b.c.d）形式给出，
            // 归一成朴素 IPv4，对齐 NetStack 的 v4 回程封包路径。toIPv4Address 对「真 v4」和「v4-mapped
            // v6」都会置 ok=true 并给出 v4；对真正的 v6 则 ok=false，保持原样。
            bool isV4 = false;
            const quint32 v4 = src.toIPv4Address(&isV4);
            if (isV4) {
                src = QHostAddress(v4);
            }
            emit q->datagramReceived(src, dg.senderPort(), dg.data());
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

DirectOutboundUdp::DirectOutboundUdp(QObject *parent) : IOutboundUdp(parent), d(new Priv(this)) {}

DirectOutboundUdp::~DirectOutboundUdp()
{
    delete d;
}

void DirectOutboundUdp::associate(const QString & /*user*/)
{
    // user 直连用不到，忽略。直连无 ASSOCIATE 握手：绑一个本地临时端口即可收发。
    d->ready = false;
    d->closedEmitted = false;

    d->udp = new QUdpSocket(this);
    d->udp->setProxy(QNetworkProxy::NoProxy); // 同 TCP：别被系统代理绕回去
    connect(d->udp, &QUdpSocket::readyRead, this, [this] { d->onDatagram(); });
    connect(d->udp, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        // 就绪后出错视为会话结束；就绪前出错为建立失败。
        if (d->ready) {
            d->emitClosed();
        } else {
            d->fail(d->udp ? d->udp->errorString() : QStringLiteral("udp error"));
        }
    });

    // 双栈绑定（QHostAddress::Any = 同时监听 v4/v6），这样同一个会话既能发 v4 目标也能发 v6 目标。
    if (!d->udp->bind(QHostAddress(QHostAddress::Any), 0)) {
        // bind 失败：异步报 failed（与 ready 一样延到下一拍，保证上层此刻已 connect 上信号）。
        const QString reason = d->udp->errorString();
        QTimer::singleShot(0, this, [this, reason] { d->fail(reason); });
        return;
    }

    // ★ ready() 必须异步发：NetStack 是先 createUdp()、connect 好 ready 槽、才 associate() 的；
    //   若在此同步 emit ready()，那一下上层还没连上信号，就会丢。延到下一个事件循环 tick 再发。
    QTimer::singleShot(0, this, [this] {
        if (d->closedEmitted || !d->udp) { // 期间可能已被关掉
            return;
        }
        d->ready = true;
        emit ready();
    });
}

void DirectOutboundUdp::sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload)
{
    if (!d->ready || !d->udp) {
        return; // 未就绪前丢弃（上层 NetStack 会缓冲 pending 并在 ready 后补发，同 Socks5Udp）
    }
    d->udp->writeDatagram(payload, dstIp, dstPort); // 直连：不套 SOCKS UDP 头，裸载荷直发目标
}

void DirectOutboundUdp::closeSession()
{
    if (d->udp) {
        d->udp->close();
    }
    d->emitClosed();
}

bool DirectOutboundUdp::isReady() const
{
    return d->ready;
}
