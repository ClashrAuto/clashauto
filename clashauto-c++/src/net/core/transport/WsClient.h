#pragma once

// 最小 WebSocket 客户端（RFC6455）—— 把 WS 当**字节流**载体，不是消息通道。
//
// ★ 谁用它、为什么手写：VMess/VLESS/Trojan 的 ws 传输把协议自己的字节塞进 WebSocket 帧里穿过 CDN
//   （帧只是伪装外壳，CDN 看到的是「正常的 WebSocket 长连接」）。协议侧要的是「写进去的字节原样从
//   另一端出来」的**流语义**，而不是 QtWebSockets 那种「一条消息一个信号」的**消息语义**——用
//   QWebSocket 会强行给数据切上消息边界、还得多拉一个 Qt 组件依赖。所以手写一个只做「帧化/去帧化」
//   的最小实现：出方向把字节切成 binary 帧（客户端必须 mask），入方向解帧后把 payload 拼回连续字节流。
//
// ★ 传输由调用方注入（依赖倒置）：start() 收一个**已连接**的 QAbstractSocket——
//   · ws  → 传一个已 connected 的 QTcpSocket；
//   · wss → 传 TlsClient::socket()（已 encrypted 的 QSslSocket）。
//   两种都是 QAbstractSocket，WsClient 只在其上收发字节，不关心底下是否加密。socket 所有权**不**转移
//   （通常与本对象同 parent，由上层统一析构）。
//
// ★ 随机数：mask key 与 Sec-WebSocket-Key 都用 QRandomGenerator::system()（RFC 要求 mask 不可预测）。
//
// 用法时序：new WsClient → connect established/dataReceived/closed/errorOccurred →
//   start(sock, host, path, port)（内部发 HTTP Upgrade、校验 101）→ established() 后 write()/dataReceived()。
// 跨平台：只用 Qt Core/Network，无 QtWebSockets。
#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QString>

namespace coastcore {

class WsClient : public QObject
{
    Q_OBJECT
public:
    explicit WsClient(QObject *parent = nullptr);
    ~WsClient() override;

    // 在已连接的 sock 上发起 WebSocket 握手。
    //   sock —— 已连接（wss 时已加密）的底层 socket，所有权不转移。
    //   host —— HTTP Host 头用的主机名（= 节点 wsHost，空则由调用方传 server）。
    //   path —— GET 请求路径（= 节点 wsPath，空则用 "/"）。
    //   port —— 仅当非 80/443 时附到 Host 头（":port"）；0 表示不附。
    // 握手 101 校验通过 → established()；失败 → errorOccurred()。
    void start(QAbstractSocket *sock, const QString &host, const QString &path, quint16 port);

    // established 后：把字节流写出去（本次调用的 data 切成一个 binary 帧，masked）。返回写入底层的字节数。
    // 未 established 时缓冲，握手成功后连同缓冲一起发。
    qint64 write(const QByteArray &data);

    // 底层尚未发出的字节数（背压用；透传底层 socket->bytesToWrite）。
    qint64 bytesToWrite() const;

    // 主动关闭：发一个 close 帧（若已握手）并让上层收到 closed()。
    void close();

    bool isEstablished() const;

signals:
    void established();                       // 握手完成，字节流可用
    void dataReceived(const QByteArray &data); // 入方向去帧后的连续字节
    void closed();                            // 对端关闭 / 收到 close 帧
    void errorOccurred(const QString &reason); // 握手失败 / 帧协议错

private:
    void onReadyRead();
    void onHandshakeBytes(); // 握手阶段：累积 HTTP 响应直到 \r\n\r\n 并校验
    void parseFrames();      // 数据阶段：从 m_inbuf 解出尽可能多的完整帧
    qint64 sendFrame(quint8 opcode, const QByteArray &payload); // 组一个 masked 帧发出
    void fail(const QString &reason);

    QAbstractSocket *m_sock = nullptr; // 注入的传输，不持有
    bool m_established = false;
    bool m_closed = false;

    QByteArray m_acceptExpected; // 期望的 Sec-WebSocket-Accept（握手前算好）
    QByteArray m_httpResp;       // 握手期累积的 HTTP 响应字节
    QByteArray m_inbuf;          // 数据期累积的帧字节（可能半帧，凑够再解）
    QByteArray m_pendingOut;     // established 前排队的出方向应用字节
};

} // namespace coastcore
