#pragma once

// 进程内 **VLESS** 出站 —— IOutbound* 的一个具名协议实现（对齐注册表约定，见 OutboundRegistry.h）。
//
// VLESS 是一个「无自有加密」的转发协议：机密性完全依赖底下叠的 TLS（tls:true）。它把「目标地址 + 命令」
// 写进流首的一段请求头，之后就是裸字节双向管道。传输可跑在裸 TCP / TLS / WebSocket(ws, 可再叠 TLS=wss)
// 上（由 node.network / node.tls 决定）。契约、背压、信号时序尽量逐点对齐 DirectOutbound / Socks5Client，
// 让 NetStack 侧对「出站是直连 / SOCKS5 / VLESS」无感。
//
// —— 线格式（本单元实现）——
//  请求头（流建立后最先发，紧接初始 payload）：
//    [0x00 版本][uuid 16B][0x00 addonLen][cmd: 0x01 tcp / 0x02 udp][port 2B 大端][addrType][addr]
//    addrType：0x01=IPv4(4B)、0x02=域名(1B 长度 + 域名 UTF-8)、0x03=IPv6(16B)。★ 与 SOCKS5 的编号不同。
//  响应：服务端回的流首 = [版本 1B][addonLen 1B]，跳过其后 addonLen 字节，再往后才是真实下行 payload。
//  UDP over 流（cmd=0x02）：每个 UDP 包按 [len 2B 大端][payload] 分帧；上下行同此。
//
// 头文件按注册约定只对外暴露注册函数；协议类都定义在此以便 AUTOMOC 处理其 Q_OBJECT（.cpp 内引用它们）。
#include "../../IOutbound.h"
#include "../ProxyConfig.h" // ProxyNode（按值持有一份快照拷贝）

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QString>

class QTcpSocket;
class OutboundRegistry;

namespace coastcore {
class TlsClient;
class WsClient;
} // namespace coastcore

// —— 内部：一条 VLESS 承载流 —— TCP 出站与 UDP 出站共用它。
// 负责：选传输(裸TCP/TLS/ws/wss) → 连上后发请求头(+缓冲的初始上行) → 剥离响应头 → 之后是裸字节管道。
// 对上暴露「流就绪 / 下行字节(已剥响应头) / 上行已写出量 / 失败 / 关闭」几个信号，具体协议语义(TCP 直通、
// UDP 的 [len][payload] 分帧)交由使用方在 data() 之上再处理。
class VlessStream : public QObject
{
    Q_OBJECT
public:
    VlessStream(const ProxyNode &node, QObject *parent = nullptr);
    ~VlessStream() override = default;

    // 起流：cmd=0x01(tcp)/0x02(udp)，目标 dstHost:dstPort（dstHost 可为 IPv4/IPv6 字面量或域名）。
    void start(quint8 cmd, const QString &dstHost, quint16 dstPort);

    // 上行写入（流未就绪则缓冲，就绪时连同请求头一并发出，保证「头在前，初始 payload 紧随」）。
    void write(const QByteArray &data);
    void write(const char *data, qsizetype size); // 裸缓冲：未就绪深拷贝，就绪时同步写出

    qint64 bytesToWrite() const;      // 未排空的上行字节（含缓冲 + 底层 socket 队列）
    void setReadPaused(bool paused);  // 下行背压（裸TCP/TLS 真闸；ws 因推模型退化为进程内缓冲，见 .cpp）
    bool isReadPaused() const { return m_readPaused; }
    bool isReady() const { return m_streamReady; }
    void abort();                     // 主动拆流（幂等），随后 emit closed()

signals:
    void ready();                              // 请求头已发出，流可用
    void data(const QByteArray &payload);      // 剥掉 VLESS 响应头后的下行裸字节
    void wrote(qint64 n);                       // 底层 socket 排空进度（供 upstreamBytesWritten）
    void failed(const QString &reason);        // 流建立前失败
    void closed();                             // 流关闭（建立后对端关 / 主动拆）

private:
    void startWs();
    void onStreamUp();           // 承载流就绪(TCP connected / TLS encrypted / ws established)：发头
    void onRawReadyRead();
    void onTlsReadyRead();
    void onWsData(const QByteArray &d);
    void feedDown(QByteArray chunk); // 剥响应头 + 背压缓冲 + emit data
    void onTransportError(const QString &reason);
    void onTransportClosed();
    void finishFailed(const QString &reason);
    void finishClosed();
    void abortTransports();
    void writeCarrier(const QByteArray &b);
    QByteArray buildHeader() const;

    ProxyNode m_node;
    QByteArray m_uuid16;      // 解析后的 16 字节 uuid
    bool m_uuidValid = false;
    quint8 m_cmd = 0x01;
    QString m_dstHost;
    quint16 m_dstPort = 0;
    bool m_useWs = false;

    QTcpSocket *m_tcp = nullptr;          // 裸 TCP，或 ws-over-raw 的底层
    coastcore::TlsClient *m_tls = nullptr; // TLS，或 wss 的底层
    coastcore::WsClient *m_ws = nullptr;   // network==ws 时的帧化层
    QAbstractSocket *m_bottom = nullptr;   // 最底层 socket（bytesWritten / 供 ws 注入）

    QByteArray m_pendingUp;   // 流就绪前缓冲的上行字节
    bool m_streamReady = false;
    bool m_finished = false;  // failed/closed 已发（终态，抑制二次）

    bool m_respHeaderDone = false; // VLESS 响应头(2+addonLen)是否已剥完
    QByteArray m_respBuf;          // 响应头累积缓冲（可能跨多次 readyRead）

    bool m_readPaused = false;
    bool m_resumeScheduled = false;
    QByteArray m_downPending;      // 背压期间缓冲的下行字节（主要给 ws 推模型兜底）
};

// —— VLESS TCP 出站 —— 薄封装：把 VlessStream 的字节管道桥到 IOutboundTcp 契约。
class VlessOutboundTcp : public IOutboundTcp
{
    Q_OBJECT
public:
    VlessOutboundTcp(const ProxyNode &node, QObject *parent = nullptr);
    ~VlessOutboundTcp() override = default;

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
    VlessStream *m_stream = nullptr;
    // connectTo() 之前 write() 进来的上行字节（承载流还没建）。契约见 IOutbound.h：
    // NetStack 把拨号推迟一拍，设备的首个数据段常常先到 —— 早先这里 m_stream 为空直接丢弃。
    QByteArray m_preDial;
    bool m_established = false;
    bool m_closedEmitted = false;
};

// —— VLESS UDP 出站 —— UDP over 流：首个 sendTo 的目标写进请求头，之后每包按 [len 2B][payload] 分帧。
// ★ 局限（见 .cpp 的 TODO）：基础 VLESS-over-流是单目标；同一会话发往不同目标的包会落到首个目标。
class VlessOutboundUdp : public IOutboundUdp
{
    Q_OBJECT
public:
    VlessOutboundUdp(const ProxyNode &node, QObject *parent = nullptr);
    ~VlessOutboundUdp() override = default;

    void associate(const QString &user) override;
    void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) override;
    void closeSession() override;
    bool isReady() const override;

private:
    void onStreamData(const QByteArray &chunk);
    void emitClosed();

    ProxyNode m_node;
    VlessStream *m_stream = nullptr;
    bool m_ready = false;         // IOutboundUdp 就绪（associate 后异步置位）
    bool m_streamStarted = false; // 承载流是否已在首个 sendTo 时起
    bool m_closedEmitted = false;
    QHostAddress m_targetIp; // 请求头里的目标（= 首个 sendTo）
    quint16 m_targetPort = 0;
    QByteArray m_downAcc; // 下行 [len][payload] 分帧累积
};

// 注册入口（约定：OutboundRegistry.cpp 的 registerBuiltinProtocols 里 #include 本头并调一行）。
void registerVless(OutboundRegistry &reg);
