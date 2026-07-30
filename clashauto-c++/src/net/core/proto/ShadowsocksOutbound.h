#pragma once

// 进程内 **Shadowsocks-AEAD（SIP004）** 出站 —— IOutbound* 的一个具名协议实现。
//
// 分流判定命中某个 type=="ss" 的节点时，NetStack 终结出的那条连接不再回拨 mihomo，而是由本对象
// 用一个裸 QTcpSocket/QUdpSocket 直接连到 ss 服务器（node.server:node.port），在本进程里完成
// Shadowsocks-AEAD 的加解密。相比「拨 SOCKS5 → mihomo → ss」省掉一趟本机回环 + mihomo 转发。
//
// 契约与时序**逐点对齐 Socks5Tcp/DirectOutboundTcp**（见 IOutbound.h / Socks5Client.cpp /
// DirectOutbound.cpp）：established 前缓冲上行明文、零拷贝上行、背压闸门（bytesToWrite/setReadPaused）、
// established/dataReceived/failed/closed 的信号语义完全一致，NetStack 那侧对「出站是 SOCKS5 / 直连 /
// Shadowsocks」完全无感。差别只在管道内部套了一层 SS-AEAD 加解密，握手上只多一步「流首发随机 salt」。
//
// 加解密原语来自 coastcore::Aead（AES-GCM / ChaCha20-Poly1305，见 crypto/Aead.h）与 coastcore::Kdf
// （EVP_BytesToKey 主密钥 + HKDF-SHA1 子密钥，见 crypto/Kdf.h）。协议细节（chunk 格式 / nonce 递增 /
// 目标地址编码）见 .cpp 的注释。
//
// ★ 两处务必做对，否则线上必翻车：
//   · socket 都要 setProxy(QNetworkProxy::NoProxy)：本 App 自己会开系统代理，否则「直连 ss 服务器」
//     又被 Qt 默认 ApplicationProxy 绕回代理里。SS 是明文 TCP（无 TLS），直连服务器即可。
//   · UDP 的 ready() 必须**异步**发（同 DirectOutboundUdp）：NetStack 先 createUdp、connect 好 ready
//     槽、才 associate；同步 emit ready() 会在上层连上信号之前丢掉。
#include "../../IOutbound.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

class QTcpSocket;
class QUdpSocket;
struct ProxyNode;
class OutboundRegistry;

// 一条 TCP 连接的 Shadowsocks-AEAD 出站隧道。connectTo() 起裸 TCP 到 ss 服务器；TCP connected 即
// established()（SS 无服务器握手应答），之后就是一根双向字节管道，管道内部按 SIP004 chunk 加解密。
class ShadowsocksOutboundTcp : public IOutboundTcp
{
    Q_OBJECT
public:
    // 从节点参数构造：node.server/port（ss 服务器）、node.cipher（AEAD 方法）、node.password（主密钥源）。
    explicit ShadowsocksOutboundTcp(const ProxyNode &node, QObject *parent = nullptr);
    ~ShadowsocksOutboundTcp() override;

    // 连到 ss 服务器；dstHost:dstPort 是**最终**目标（编码进加密流首段的目标地址）。dstHost 可为
    // IPv4/IPv6 字面量或域名。user 对 SS 无意义（无每设备分流/认证），忽略。cipher 非法则本次直接 failed。
    void connectTo(const QString &dstHost, quint16 dstPort, const QString &user) override;
    void write(const QByteArray &data) override;
    // 裸缓冲零拷贝上行 —— 生命周期契约同 Socks5Tcp（见 IOutbound.h）：data 只需活到本次调用返回。
    void write(const char *data, qsizetype size) override;
    void closeTunnel() override;
    bool isEstablished() const override;

    // —— 背压（流控）：语义见 Socks5Client.h，实现与 Socks5Tcp/DirectOutboundTcp 一致 ——
    qint64 bytesToWrite() const override;
    void setReadPaused(bool paused) override;
    bool isReadPaused() const override;

    // 信号（established/dataReceived/failed/closed/upstreamBytesWritten）继承自 IOutboundTcp。

private:
    class Priv;
    Priv *d;
};

// 一次 UDP 会话的 Shadowsocks-AEAD 出站。每个上行 datagram 独立封包（随机 salt + 单次 AEAD 密封，
// SIP004 UDP 格式）。associate() 后异步 ready()；sendTo() 发一个 datagram 到目标；收到 ss 服务器回程
// datagram → 解封出源地址与载荷 → datagramReceived()。一个会话复用于该设备一个源端口的多目标 UDP。
class ShadowsocksOutboundUdp : public IOutboundUdp
{
    Q_OBJECT
public:
    explicit ShadowsocksOutboundUdp(const ProxyNode &node, QObject *parent = nullptr);
    ~ShadowsocksOutboundUdp() override;

    void associate(const QString &user) override;
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override;
    void closeSession() override;
    bool isReady() const override;

    // 信号（ready/datagramReceived/failed/closed）继承自 IOutboundUdp。

private:
    class Priv;
    Priv *d;
};

// 注册入口 —— 由 OutboundRegistry.cpp 的 registerBuiltinProtocols() 调一行（集成侧统一做，本单元不改它）。
// 内部 reg.registerProto("ss", tcpCreator, udpCreator)，creator 从 ProxyNode 造对应出站对象。
void registerShadowsocks(OutboundRegistry &reg);
