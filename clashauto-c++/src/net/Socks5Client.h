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

signals:
    void established();                    // CONNECT 成功，双向管道就绪
    void dataReceived(const QByteArray &data); // 下行字节
    void failed(const QString &reason);    // 握手/连接失败
    void closed();                         // 隧道关闭（对端 FIN / 出错）

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
