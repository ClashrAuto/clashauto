#pragma once

// 到 mihomo 混合端口(127.0.0.1:7890)的 SOCKS5 客户端 —— 把用户态栈终结出的每条连接接进 mihomo，
// 从而复用 mihomo 的规则/分流。**每设备唯一用户名**认证（user=dev-<mac>），让 mihomo 侧靠
// `IN-USER` 规则做每设备策略、靠 /connections 的 inboundUser 做每设备流量归属。
//
// 两个类：
//  - Socks5Tcp：一条 TCP 连接的 CONNECT 隧道。握手成功后就是一根双向字节管道
//    （established() → 之后 write()/dataReceived()，closed() 表示对端关闭）。
//  - Socks5Udp：一次 UDP ASSOCIATE 会话，用于承载设备的 UDP（含 DNS）。sendTo() 发一个
//    UDP 载荷到目标；datagramReceived() 收到回程 UDP。一个会话可复用于该设备的多目标。
//
// 认证：user 非空则走「用户名/密码」认证(RFC1929，密码任意，mihomo 只认 user 做 IN-USER)；
// user 为空则「无认证」。host 恒 127.0.0.1。
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

class QTcpSocket;
class QUdpSocket;

class Socks5Tcp : public QObject
{
    Q_OBJECT
public:
    explicit Socks5Tcp(QObject *parent = nullptr);
    ~Socks5Tcp() override;

    // 经 127.0.0.1:socksPort 向 mihomo 发起 CONNECT 到 dstHost:dstPort（dstHost 可为 IP 或域名）。
    void connectTo(quint16 socksPort, const QString &dstHost, quint16 dstPort,
                   const QString &user);
    void write(const QByteArray &data); // 隧道建立后写入上行字节
    void closeTunnel();                 // 主动关闭
    bool isEstablished() const;

    // —————————————————— 背压（流控）接口 ——————————————————
    // 上层（NetStack）两个方向都要限流，否则慢的一头会让快的一头把内存吃光。
    //
    // 上行水位：还没真正交给内核的字节数 = established 前的缓冲 + QTcpSocket 的写队列。
    // NetStack 用它决定「要不要延后归还 lwIP 的接收窗口」。
    qint64 bytesToWrite() const;
    // 下行闸门：暂停后不再从 socket 读，字节先积在 Qt 读缓冲（已设上限）再积在内核缓冲里，
    // 填满后本端 TCP 窗口自然关闭 —— 背压就这样传回远端，而不是在我们进程里堆成无界队列。
    // 恢复时若缓冲里还有存货，会**排队(queued)**补发一次 dataReceived：本函数常常是从
    // lwIP 的回调里被调到的，同步重入会在调用方脚下把连接对象析构掉。
    void setReadPaused(bool paused);
    bool isReadPaused() const;

signals:
    void established();                    // CONNECT 成功，双向管道就绪
    void dataReceived(const QByteArray &data); // 下行字节
    void failed(const QString &reason);    // 握手/连接失败
    void closed();                         // 隧道关闭（对端 FIN / 出错）
    // 上行字节真正被交给内核（转发自 QTcpSocket::bytesWritten）。
    // 这是「上行排空了多少」的唯一可靠信号，NetStack 据此按量归还 lwIP 接收窗口。
    void upstreamBytesWritten(qint64 n);

private:
    class Priv;
    Priv *d;
};

class Socks5Udp : public QObject
{
    Q_OBJECT
public:
    explicit Socks5Udp(QObject *parent = nullptr);
    ~Socks5Udp() override;

    // 建立 UDP ASSOCIATE 会话（经 127.0.0.1:socksPort）。ready() 后可 sendTo()。
    void associate(quint16 socksPort, const QString &user);
    // 发一个 UDP 载荷到 dstIp:dstPort（IPv4）。
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload);
    void closeSession();
    bool isReady() const;

signals:
    void ready();
    void datagramReceived(const QHostAddress &srcIp, quint16 srcPort, const QByteArray &payload);
    void failed(const QString &reason);
    void closed();

private:
    class Priv;
    Priv *d;
};
