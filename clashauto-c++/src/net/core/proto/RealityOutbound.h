#pragma once

// 进程内 **REALITY** 出站 —— IOutbound* 的具名协议实现（对齐注册表约定，见 OutboundRegistry.h）。
//
// ★ REALITY 与 VLESS 的关系：REALITY 不是一个独立的承载协议，而是 **VLESS 的一种 TLS 传输方案**。
//   在 xray/mihomo 的配置里它表现为 `type: vless` + `reality-opts`（publicKey/shortId）+ `tls: true` +
//   `flow: xtls-rprx-vision`。承载在流上的**内层协议仍是 VLESS**（同样的请求头/响应头，见 VlessOutbound）。
//   与普通 VLESS-over-TLS 的唯一区别在**底下那层 TLS**：
//     · 普通 VLESS-over-TLS 用系统 TLS 栈（QSslSocket），SNI=真实服务器，证书正常校验。
//     · REALITY 用**自建的 uTLS/TLS1.3 客户端**（见 transport/UtlsClient）：ClientHello 伪装成浏览器，
//       并把「服务端 REALITY 公钥 × 本次握手临时私钥」的 ECDH 认证数据塞进 session_id；SNI=一个真实
//       大站(dest)，服务端据认证决定接管(跑 VLESS)或透明放行到真实站。
//   本单元故意注册成**独立 type "reality"** 而非复用 "vless"，纯为**路由/分派**方便（拨号侧按 type 选到
//   本实现）。主控在把订阅里的 `type:vless + reality-opts` 映射进 ProxyNode 时，应把 type 归一为
//   "reality"（详见 .cpp 顶部与本仓报告）。
//
// ★★★ 诚实声明 ★★★：底层 TLS1.3/REALITY 握手由 UtlsClient 完成，**本机无法做字节级验证**。
//   REALITY 服务端证书的 AuthKey 核验**已实现**（UtlsClient::verifyRealityCertificate，认证失败即中止，
//   不会泄露内层 VLESS 头）；仍有已知未实现项（xtls-rprx-vision flow 的流量整形、HRR 等）——详见
//   UtlsClient.cpp 与本文件 .cpp 的 TODO。上线前必须对 xray REALITY 服务端真机联调。
//
// —— 内层线格式（= VLESS，与 VlessOutbound 完全一致）——
//  请求头：[0x00 版本][uuid 16B][0x00 addonLen][cmd][port 2B BE][addrType][addr]
//  响应头：[版本 1B][addonLen 1B]，跳过其后 addonLen 字节。
//  UDP over 流：每包 [len 2B BE][payload]。

#include "../../IOutbound.h"
#include "../ProxyConfig.h"

#include <QByteArray>
#include <QHostAddress>
#include <QString>

class OutboundRegistry;

namespace coastcore {
class UtlsClient;
}

// —— 从 ProxyNode 抽取 REALITY 所需参数 ——
// ★ 字段来源：ProxyConfig.h 的 ProxyNode 已有 realityPublicKey / realityShortId / fingerprint / flow，
//   parseRealityParams 直接解码填充（见 .cpp）。publicKey 解码后非 32B 时 valid==false，start() 诚实失败。
struct RealityParams
{
    QString server;
    quint16 port = 0;
    QString uuid;
    QString sni;          // = dest / 伪装目标站 ServerName
    QByteArray publicKey; // 32B 裸公钥（从 realityPublicKey 的 base64/hex 解码后）
    QByteArray shortId;   // 0..8B 裸字节（从 realityShortId 的 hex 解码后，可空）
    QString fingerprint;  // "chrome" 等（空=chrome）
    QString flow;         // "xtls-rprx-vision" 等；vision 流量整形未实现
    bool valid = false;   // publicKey 是否就绪（决定能否真正认证）
};

// —— 一条 REALITY 承载流 —— TCP/UDP 出站共用。UtlsClient 建好 TLS1.3 通道后发 VLESS 请求头，
//    剥响应头，之后是裸字节管道。信号语义对齐 VlessStream。
class RealityStream : public QObject
{
    Q_OBJECT
public:
    RealityStream(const ProxyNode &node, QObject *parent = nullptr);
    ~RealityStream() override = default;

    void start(quint8 cmd, const QString &dstHost, quint16 dstPort);
    void write(const QByteArray &data);
    void write(const char *data, qsizetype size);

    qint64 bytesToWrite() const;
    void setReadPaused(bool paused);
    bool isReadPaused() const { return m_readPaused; }
    bool isReady() const { return m_streamReady; }
    void abort();

signals:
    void ready();
    void data(const QByteArray &payload);
    void wrote(qint64 n);
    void failed(const QString &reason);
    void closed();

private:
    void onTlsUp();
    void onTlsReadyRead();
    void feedDown(QByteArray chunk);
    void onTransportError(const QString &reason);
    void onTransportClosed();
    void finishFailed(const QString &reason);
    void finishClosed();
    void writeCarrier(const QByteArray &b);
    QByteArray buildVlessHeader() const;

    ProxyNode m_node;
    RealityParams m_rp;
    QByteArray m_uuid16;
    bool m_uuidValid = false;
    quint8 m_cmd = 0x01;
    QString m_dstHost;
    quint16 m_dstPort = 0;

    coastcore::UtlsClient *m_tls = nullptr;

    QByteArray m_pendingUp;
    bool m_streamReady = false;
    bool m_finished = false;

    bool m_respHeaderDone = false;
    QByteArray m_respBuf;

    bool m_readPaused = false;
    bool m_resumeScheduled = false;
    QByteArray m_downPending;
};

// —— REALITY TCP 出站 —— 薄封装：RealityStream 字节管道 → IOutboundTcp 契约。
class RealityOutboundTcp : public IOutboundTcp
{
    Q_OBJECT
public:
    RealityOutboundTcp(const ProxyNode &node, QObject *parent = nullptr);
    ~RealityOutboundTcp() override = default;

    void connectTo(const QString &dstHost, quint16 dstPort, const QString &user) override;
    void write(const QByteArray &data) override;
    void write(const char *data, qsizetype size) override;
    void closeTunnel() override;
    bool isEstablished() const override;

    qint64 bytesToWrite() const override;
    void setReadPaused(bool paused) override;
    bool isReadPaused() const override;

private:
    void emitClosed();

    ProxyNode m_node;
    RealityStream *m_stream = nullptr;
    // connectTo() 之前 write() 进来的上行字节（承载流还没建）。契约见 IOutbound.h：
    // NetStack 把拨号推迟一拍，设备的首个数据段常常先到 —— 早先这里 m_stream 为空直接丢弃。
    QByteArray m_preDial;
    bool m_established = false;
    bool m_closedEmitted = false;
};

// —— REALITY UDP 出站 —— VLESS-over-流 UDP（单目标，见 .cpp TODO），语义对齐 VlessOutboundUdp。
class RealityOutboundUdp : public IOutboundUdp
{
    Q_OBJECT
public:
    RealityOutboundUdp(const ProxyNode &node, QObject *parent = nullptr);
    ~RealityOutboundUdp() override = default;

    void associate(const QString &user) override;
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override;
    void closeSession() override;
    bool isReady() const override;

private:
    void onStreamData(const QByteArray &chunk);
    void emitClosed();

    ProxyNode m_node;
    RealityStream *m_stream = nullptr;
    bool m_ready = false;
    bool m_streamStarted = false;
    bool m_closedEmitted = false;
    QHostAddress m_targetIp;
    quint16 m_targetPort = 0;
    QByteArray m_downAcc;
};

// 注册入口（约定：OutboundRegistry.cpp 的 registerBuiltinProtocols 里 #include 本头并调一行）。
void registerReality(OutboundRegistry &reg);
