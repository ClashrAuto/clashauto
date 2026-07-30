#pragma once

// 进程内 **Hysteria2**(QUIC) 出站 —— IOutbound* 的 QUIC 系实现之一, 供 CoastCore 拨号侧按
// node.type=="hysteria2"(别名 "hy2") 分派。相比「拨 SOCKS5 → mihomo」省掉一趟本机回环 + 转发。
//
// ★ 传输：底层是 coastcore::QuicTransport(msquic 封装)。ALPN 恒 "h3"。握手完成后先做
//   **HTTP/3 认证**(见 .cpp)：向 https://hysteria/auth 发一个带 Hysteria-Auth 头的 POST,
//   期待 233 HyOK。认证通过后:
//     · TCP 代理走一条 QUIC 双向流:首帧 TCPRequest = varint(0x401) | varint(addrLen) | addr
//       | varint(padLen) | pad;服务器回 TCPResponse(status u8 | varint msgLen | msg | varint padLen
//       | pad);其后即裸双向字节管道。
//     · UDP 走 QUIC 数据报:UDPMessage = u32 sessionId | u16 pktId | u8 fragId | u8 fragCount
//       | varint addrLen | addr("host:port") | payload;超 MTU 分片, 下行按 pktId 重组。
//
// ★ 契约与时序对齐 Socks5Tcp/DirectOutbound/Trojan(背压、零拷贝上行生命周期、established/closed
//   信号语义、readPaused 的排队补读)。差别:established = QUIC 连上 + HTTP/3 认证过 + 代理流的
//   TCPResponse(status=OK) 已收到;背压用 QuicStream 的 setReceiveEnabled / bytesInFlight。
//
// ★ ProxyNode 取用字段:server / port / password(=Hysteria-Auth 凭据) / sni / skipCertVerify。
//
// ★ 见报告的「需真机验证 / TODO」:HTTP/3 认证响应的 QPACK 解码为最小实现(状态码判定 + 无 Huffman)。
#include "../../IOutbound.h"

#include "../ProxyConfig.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

class OutboundRegistry;

// 一条 TCP 连接的 Hysteria2 隧道。connectTo() 起 QUIC+HTTP/3 认证+代理流;established() 后即字节管道。
class Hysteria2OutboundTcp : public IOutboundTcp
{
    Q_OBJECT
public:
    explicit Hysteria2OutboundTcp(const ProxyNode &node, QObject *parent = nullptr);
    ~Hysteria2OutboundTcp() override;

    void connectTo(const QString &dstHost, quint16 dstPort, const QString &user) override;
    void write(const QByteArray &data) override;
    void write(const char *data, qsizetype size) override;
    void closeTunnel() override;
    bool isEstablished() const override;

    qint64 bytesToWrite() const override;
    void setReadPaused(bool paused) override;
    bool isReadPaused() const override;

private:
    class Priv;
    Priv *d;
};

// 一次 UDP 会话的 Hysteria2 出站。associate() 起 QUIC+HTTP/3 认证;ready() 后 sendTo() 走数据报。
class Hysteria2OutboundUdp : public IOutboundUdp
{
    Q_OBJECT
public:
    explicit Hysteria2OutboundUdp(const ProxyNode &node, QObject *parent = nullptr);
    ~Hysteria2OutboundUdp() override;

    void associate(const QString &user) override;
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override;
    void closeSession() override;
    bool isReady() const override;

private:
    class Priv;
    Priv *d;
};

// 注册入口(约定见 OutboundRegistry.h)：把 "hysteria2" 与别名 "hy2" 映射到上面两类。
void registerHysteria2(OutboundRegistry &reg);
