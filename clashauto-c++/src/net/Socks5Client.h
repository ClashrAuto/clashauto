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
//
// Socks5Tcp/Udp 实现 IOutbound* 接口（见 IOutbound.h）：socksPort 在**构造时**绑定，之后调用方
// 走接口的 connectTo(dstHost,dstPort,user) / associate(user)，与出站究竟是不是 SOCKS5 无关。
#include "IOutbound.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

class QTcpSocket;
class QUdpSocket;

class Socks5Tcp : public IOutboundTcp
{
    Q_OBJECT
public:
    // socksPort = mihomo 混合端口（通常 config.mixedPort，7890）。恒连 127.0.0.1:socksPort。
    explicit Socks5Tcp(quint16 socksPort, QObject *parent = nullptr);
    ~Socks5Tcp() override;

    // 经 127.0.0.1:socksPort 向 mihomo 发起 CONNECT 到 dstHost:dstPort。
    // dstHost 可为 IPv4 字面量（ATYP 0x01）、IPv6 字面量（ATYP 0x04，透明网关的 v6 连接走这条）或域名（0x03）。
    void connectTo(const QString &dstHost, quint16 dstPort, const QString &user) override;
    void write(const QByteArray &data) override; // 隧道建立后写入上行字节
    // 同上，但直接吃裸缓冲 —— 给「字节本来就不在 QByteArray 里」的调用方（NetStack 的 lwIP
    // 收包回调，字节在 pbuf 里）用，省掉一次 QByteArray 堆分配 + 全量拷贝。
    //
    // ★ 生命周期契约（NetStack.cpp 的零拷贝上行路径依赖它，改本函数前先看那边的注释）：
    //   data 只需在**本次调用期间**有效。实现必须保证返回前字节已被完整复制走
    //   （已建立时靠 QIODevice::write 同步拷进 QTcpSocket 写缓冲；未建立时深拷贝进 pending）。
    //   绝不允许把 data 指针存起来晚点再用（例如 QByteArray::fromRawData 存进队列）。
    void write(const char *data, qsizetype size) override;
    void closeTunnel() override;                 // 主动关闭
    bool isEstablished() const override;

    // —————————————————— 背压（流控）接口 ——————————————————
    // 上层（NetStack）两个方向都要限流，否则慢的一头会让快的一头把内存吃光。
    //
    // 上行水位：还没真正交给内核的字节数 = established 前的缓冲 + QTcpSocket 的写队列。
    // NetStack 用它决定「要不要延后归还 lwIP 的接收窗口」。
    qint64 bytesToWrite() const override;
    // 下行闸门：暂停后不再从 socket 读，字节先积在 Qt 读缓冲（已设上限）再积在内核缓冲里，
    // 填满后本端 TCP 窗口自然关闭 —— 背压就这样传回远端，而不是在我们进程里堆成无界队列。
    // 恢复时若缓冲里还有存货，会**排队(queued)**补发一次 dataReceived：本函数常常是从
    // lwIP 的回调里被调到的，同步重入会在调用方脚下把连接对象析构掉。
    void setReadPaused(bool paused) override;
    bool isReadPaused() const override;

    // 信号（established/dataReceived/failed/closed/upstreamBytesWritten）继承自 IOutboundTcp。

private:
    quint16 m_socksPort;
    class Priv;
    Priv *d;
};

class Socks5Udp : public IOutboundUdp
{
    Q_OBJECT
public:
    explicit Socks5Udp(quint16 socksPort, QObject *parent = nullptr);
    ~Socks5Udp() override;

    // 建立 UDP ASSOCIATE 会话（经 127.0.0.1:socksPort）。ready() 后可 sendTo()。
    void associate(const QString &user) override;
    // 发一个 UDP 载荷到 dstIp:dstPort（按 dstIp 协议自动选 ATYP：IPv4=0x01 / IPv6=0x04）。
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override;
    void closeSession() override;
    bool isReady() const override;

    // 信号（ready/datagramReceived/failed/closed）继承自 IOutboundUdp。

private:
    quint16 m_socksPort;
    class Priv;
    Priv *d;
};

// SOCKS5 出站工厂 —— 把 mihomo 混合端口绑进去，产出拨该端口的 Socks5Tcp/Udp。
// 这是 NetStack 的默认出站工厂（现状：所有连接都走 mihomo）。
class Socks5OutboundFactory : public OutboundFactory
{
public:
    explicit Socks5OutboundFactory(quint16 socksPort) : m_socksPort(socksPort) {}
    IOutboundTcp *createTcp(QObject *parent) override { return new Socks5Tcp(m_socksPort, parent); }
    IOutboundUdp *createUdp(QObject *parent) override { return new Socks5Udp(m_socksPort, parent); }

private:
    quint16 m_socksPort;
};
