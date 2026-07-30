#pragma once

// 进程内 **Trojan**（over TLS）出站 —— IOutbound* 的又一非 SOCKS5 实现，供 CoastCore 拨号侧按
// node.type=="trojan" 分派。相比「拨 SOCKS5 → mihomo → trojan」省掉一趟本机回环 + mihomo 转发。
//
// ★ 传输：底层不是裸 QTcpSocket，而是 coastcore::TlsClient（QSslSocket 的薄封装）——
//   connectToHost(server, port, SNI, ALPN, skipVerify) 起 TCP+TLS 握手，TlsClient::connected()
//   （= QSslSocket::encrypted）即「隧道加密就绪」。SNI = node.sni.isEmpty()? node.server : node.sni；
//   skipVerify = node.skipCertVerify；ALPN 固定 {"h2","http/1.1"}。
//
// ★ 契约与时序**逐点对齐 Socks5Tcp/DirectOutbound**（背压、零拷贝上行的生命周期、established/closed
//   的信号语义、readPaused 的排队补读都照搬），差别只有两点：
//     1) 底层 socket 换成 TlsClient，established = TLS encrypted 就绪（且首包 trojan 头已写出）。
//     2) TLS 握手完成后**先发 trojan 首包**（见 .cpp 的首包格式），之后即裸双向字节管道，无额外封装。
//
// ★ TCP 首包（TLS 加密之上）：
//     hex(SHA224(password))(56 小写hex) || CRLF || [CMD=0x01][ATYP+addr+port] || CRLF || 初始payload
//   之后 write() 直接写 TLS，收到 TLS 明文字节直接上抛 dataReceived（无帧）。
//
// ★ UDP：CMD=0x03（UDP ASSOCIATE），首包头之后每个 UDP 包独立成帧：
//     [ATYP+addr+port][payloadLen 2B 大端][CRLF][payload]
//   上行 sendTo 按此封装写 TLS；下行是 TLS 流，需按该格式逐包分帧（跨 readyRead 半包累积）后上抛。
#include "../../IOutbound.h"

#include "../ProxyConfig.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

class OutboundRegistry;

// 一条 TCP 连接的 Trojan 隧道。connectTo() 起 TLS 握手；encrypted + 首包写出 → established()，
// 之后就是纯字节管道（write()/dataReceived()），对端 FIN/RST → closed()，握手/连接失败 → failed()。
class TrojanOutboundTcp : public IOutboundTcp
{
    Q_OBJECT
public:
    explicit TrojanOutboundTcp(const ProxyNode &node, QObject *parent = nullptr);
    ~TrojanOutboundTcp() override;

    // dstHost 可为 IPv4/IPv6 字面量或域名（域名交给 trojan 服务器解析，编入首包 ATYP=0x03）。
    // user 是设备的 mihomo 身份，trojan 不做每设备分流，忽略。
    void connectTo(const QString &dstHost, quint16 dstPort, const QString &user) override;
    void write(const QByteArray &data) override;
    // 裸缓冲上行 —— 生命周期契约与背压细节照抄 Socks5Tcp（见 IOutbound.h / Socks5Client.cpp）。
    void write(const char *data, qsizetype size) override;
    void closeTunnel() override;
    bool isEstablished() const override;

    // —— 背压（流控）：语义见 Socks5Client.h，实现与 Socks5Tcp 一致（底层量取自 TlsClient）——
    qint64 bytesToWrite() const override;
    void setReadPaused(bool paused) override;
    bool isReadPaused() const override;

    // 信号（established/dataReceived/failed/closed/upstreamBytesWritten）继承自 IOutboundTcp。

private:
    class Priv;
    Priv *d;
};

// 一次 UDP 会话的 Trojan 出站。associate() 起 TLS 握手，encrypted → 写 UDP ASSOCIATE 首包头 →
// 异步 ready()。sendTo() 把载荷按 trojan UDP 帧封装写 TLS；下行 TLS 流按帧解析后 datagramReceived()。
class TrojanOutboundUdp : public IOutboundUdp
{
    Q_OBJECT
public:
    explicit TrojanOutboundUdp(const ProxyNode &node, QObject *parent = nullptr);
    ~TrojanOutboundUdp() override;

    void associate(const QString &user) override;
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override;
    void closeSession() override;
    bool isReady() const override;

    // 信号（ready/datagramReceived/failed/closed）继承自 IOutboundUdp。

private:
    class Priv;
    Priv *d;
};

// 注册入口（约定见 OutboundRegistry.h）：把 "trojan" 映射到上面两个类的构造器。
// 在 OutboundRegistry.cpp 的 registerBuiltinProtocols() 里 #include 本头并调一行 registerTrojan(reg)。
void registerTrojan(OutboundRegistry &reg);
