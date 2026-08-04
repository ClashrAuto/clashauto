#pragma once

// 进程内 VMess（VMessAEAD）出站 —— CoastCore 迄今最复杂的一个协议实现。
//
// ★ 只做「现行标准」的 VMessAEAD（AEAD 请求头 + AEAD 响应头），不做已被淘汰的旧 MD5-header 版
//   （legacy header 用 40 字节 MD5 时间戳鉴权 + 明文长度，早已被服务端逐步下线）。VMessAEAD 的
//   请求头本身被一层 AES-128-GCM 密封，鉴权用一个由时间 + 随机 + CRC32 组成、再 AES-ECB 加密的
//   16 字节 AuthID，抗重放、抗主动探测，是 v2ray/xray 现在默认走的那套。
//
// ★ 与 Trojan/SS 的最大不同：VMess 没有「服务端先回一句握手」的往返。客户端把请求头一发，就可以
//   立刻往里灌 body；服务端的**响应头**是**贴在下行 body 前面**一起回来的。所以本实现里：
//     · established()   = 传输层（TCP/TLS/WS）通 + 请求头已发出（此后 NetStack 就能写上行）。
//     · 下行第一段字节 = 先当作 AEAD 响应头解出来校验（responseAuth 对得上），余下的才是 body 分块。
//
// ★ 派生链（务必逐字节对齐 v2ray，错了是静默失败 —— 密文能生成但服务端解不开）：
//     cmdKey  = MD5(uuid16 || "c48619fe-8f02-49e0-b9e9-edf763e17e21")
//     递归 KDF = 以 "VMess AEAD KDF" 为根、逐个 label 套一层 HMAC-SHA256 的嵌套 HMAC（见 .cpp）
//     AuthID  = AES-128-ECB( KDF16(cmdKey,"AES Auth ID Encryption"),  time8||rand4||CRC32(前12)  )
//     请求头两段 AEAD（长度段 / 载荷段）的 key/nonce 由 KDF(cmdKey, <各自 label> || AuthID || connNonce) 派生
//     响应 key/iv = SHA256(bodyKey)[:16] / SHA256(bodyIV)[:16]，响应头 AEAD 亦由 KDF 派生
//
// ★ 传输：node.tls ? TlsClient : 裸 QTcpSocket；network=="ws" 时在其上再叠一层 WsClient（wss = ws over
//   TlsClient::socket()）。VMess 的 UDP（cmd=0x02）同样隧道在这条 TCP/TLS 之上，不是真 UDP socket。
//
// ★ 背压 / 信号 / 零拷贝上行的语义**对齐 DirectOutbound**（见其 .cpp 注释），差别只在 VMess 要在两端
//   各套一层「分块 AEAD 编解码」。拿不准的线格式细节（chunk-masking / global-padding / UDP 多目标）
//   在 .cpp 里标了 TODO，留真机验证 —— 宁可标 TODO 也不瞎编。
#include "../../IOutbound.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

class OutboundRegistry;
struct ProxyNode;

// 一条 TCP 连接的 VMess 出站隧道（指令头 cmd=0x01）。
class VmessOutboundTcp : public IOutboundTcp
{
    Q_OBJECT
public:
    // 从 ProxyNode 构造（取 server/port/uuid/cipher/network/tls/ws* 等）。parent 用于 Qt 对象树。
    explicit VmessOutboundTcp(const ProxyNode &node, QObject *parent = nullptr);
    ~VmessOutboundTcp() override;

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

// 一次 UDP 会话的 VMess 出站（指令头 cmd=0x02，隧道在 TCP/TLS 之上）。
class VmessOutboundUdp : public IOutboundUdp
{
    Q_OBJECT
public:
    explicit VmessOutboundUdp(const ProxyNode &node, QObject *parent = nullptr);
    ~VmessOutboundUdp() override;

    void associate(const QString &user) override;
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override;
    void closeSession() override;
    bool isReady() const override;

private:
    class Priv;
    Priv *d;
};

// 注册约定（见 OutboundRegistry.h）：把 "vmess" 映射到本单元的 Tcp/Udp 构造器。
void registerVmess(OutboundRegistry &reg);
