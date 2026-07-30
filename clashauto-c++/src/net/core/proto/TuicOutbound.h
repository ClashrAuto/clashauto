#pragma once

// 进程内 **TUIC v5**(QUIC) 出站 —— IOutbound* 的 QUIC 系实现之一, 供 CoastCore 拨号侧按
// node.type=="tuic" 分派。
//
// ★ 传输：底层 coastcore::QuicTransport(msquic)。连接建立后:
//     · 先发 **Authenticate**(经一条单向流)：命令头 VER(0x05)|TYPE(0x00) || UUID(16) || TOKEN(32)。
//       TOKEN = TLS keying material 导出(label=UUID 原文, context=明文密码, 32 字节)。
//     · TCP 走一条双向流:首发 **Connect** 命令头 VER|TYPE(0x01)||ADDR, 其后即裸双向字节管道。
//     · UDP 走数据报(native 模式)：**Packet** 命令 VER|TYPE(0x02)||ASSOC_ID(2)|PKT_ID(2)
//       |FRAG_TOTAL(1)|FRAG_ID(1)|SIZE(2)|ADDR || payload;超 MTU 分片, 下行按 (assoc,pktId) 重组。
//   地址编码 ADDR = TYPE(1)|ADDR|PORT(2 大端):0x00 域名(1 字节长度前缀)、0x01 IPv4(4)、0x02 IPv6(16)、
//   0xff None。
//
// ★ 契约/时序对齐 Trojan/Direct:established = 连接就绪 + 已认证 + 已发 Connect 头(TUIC 无 Connect 响应,
//   双向流建立即视为通)。背压用 QuicStream 的 setReceiveEnabled / bytesInFlight。
//
// ★ ProxyNode 取用:server / port / uuid / password / sni / skipCertVerify。
//
// ★ 见报告 TODO/需真机:TOKEN 依赖 msquic 的 ConnectionExportKeyingMaterial(preview, v2.6+);
//   ALPN 缺省取 "h3"(应由配置提供)。
#include "../../IOutbound.h"

#include "../ProxyConfig.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

class OutboundRegistry;

class TuicOutboundTcp : public IOutboundTcp
{
    Q_OBJECT
public:
    explicit TuicOutboundTcp(const ProxyNode &node, QObject *parent = nullptr);
    ~TuicOutboundTcp() override;

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

class TuicOutboundUdp : public IOutboundUdp
{
    Q_OBJECT
public:
    explicit TuicOutboundUdp(const ProxyNode &node, QObject *parent = nullptr);
    ~TuicOutboundUdp() override;

    void associate(const QString &user) override;
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override;
    void closeSession() override;
    bool isReady() const override;

private:
    class Priv;
    Priv *d;
};

// 注册入口(约定见 OutboundRegistry.h)：把 "tuic" 映射到上面两类。
void registerTuic(OutboundRegistry &reg);
