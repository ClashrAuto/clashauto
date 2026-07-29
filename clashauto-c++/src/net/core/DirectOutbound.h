#pragma once

// 进程内**直连**出站 —— IOutbound* 的第一个非 SOCKS5 实现。不经任何代理，直接由本进程拨向目标。
//
// 这是 CoastCore「把出站搬进程内」的最小一块：当分流判定为「直连」（DIRECT / Direct 模式 / 规则命中
// 直连）时，NetStack 终结出的那条连接不再回拨 mihomo，而是本对象用一个裸 QTcpSocket/QUdpSocket 直接
// 连出去。相比「拨 SOCKS5 → mihomo → 再直连」省掉了一整趟本机回环 + mihomo 的转发开销。
//
// 契约与时序**逐点对齐 Socks5Tcp/Socks5Udp**（见 Socks5Client.h/.cpp），差别只有一个：没有 SOCKS5
// 握手 —— TCP 的 connected 直接就是 established()，UDP 无需 ASSOCIATE 即刻可用。除此之外背压、零拷贝
// 上行、信号语义全部照搬，这样 NetStack 那侧对「出站是 SOCKS5 还是直连」完全无感。
//
// ★ 两处必须做对，否则线上必翻车：
//   · 两个 socket 都要 setProxy(QNetworkProxy::NoProxy)。否则一旦本机开着系统代理（本 App 自己就会
//     开），Qt 的默认 socket 会去读全局 ApplicationProxy，直连又被绕回代理 —— 直连也就名不副实了。
//   · UDP 的 ready() 必须**异步**发（见 .cpp）：NetStack 是先 createUdp()、connect 好 ready 槽、才
//     associate() 的；若 associate() 里同步 emit ready()，那一下上层还没 connect 上，信号直接丢失。
#include "../IOutbound.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

class QTcpSocket;
class QUdpSocket;

// 一条 TCP 连接的直连隧道。connectTo() 即 QTcpSocket::connectToHost；connected → established()，
// 之后就是纯字节管道（write()/dataReceived()），对端 FIN/RST → closed()，连接失败 → failed()。
class DirectOutboundTcp : public IOutboundTcp
{
    Q_OBJECT
public:
    explicit DirectOutboundTcp(QObject *parent = nullptr);
    ~DirectOutboundTcp() override;

    // 直连到 dstHost:dstPort。dstHost 可为 IPv4/IPv6 字面量或域名 —— 交给 QTcpSocket::connectToHost
    // 自行解析（域名走系统 DNS）。user 是设备的 mihomo 身份，直连不需要它（无每设备分流），忽略。
    void connectTo(const QString &dstHost, quint16 dstPort, const QString &user) override;
    void write(const QByteArray &data) override;
    // 裸缓冲零拷贝上行 —— 生命周期契约与背压细节照抄 Socks5Tcp（见 IOutbound.h / Socks5Client.cpp）。
    void write(const char *data, qsizetype size) override;
    void closeTunnel() override;
    bool isEstablished() const override;

    // —— 背压（流控）：语义见 Socks5Client.h，实现与 Socks5Tcp 一致 ——
    qint64 bytesToWrite() const override;
    void setReadPaused(bool paused) override;
    bool isReadPaused() const override;

    // 信号（established/dataReceived/failed/closed/upstreamBytesWritten）继承自 IOutboundTcp。

private:
    class Priv;
    Priv *d;
};

// 一次 UDP 会话的直连出站。associate() 后即刻可用（异步 emit ready()）；sendTo() → writeDatagram，
// 收到回程 datagram → datagramReceived()。一个会话可复用于该设备一个源端口的多目标 UDP（含 DNS）。
class DirectOutboundUdp : public IOutboundUdp
{
    Q_OBJECT
public:
    explicit DirectOutboundUdp(QObject *parent = nullptr);
    ~DirectOutboundUdp() override;

    void associate(const QString &user) override; // 直连无握手：绑本地端口后异步 ready()
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override;
    void closeSession() override;
    bool isReady() const override;

    // 信号（ready/datagramReceived/failed/closed）继承自 IOutboundUdp。

private:
    class Priv;
    Priv *d;
};
