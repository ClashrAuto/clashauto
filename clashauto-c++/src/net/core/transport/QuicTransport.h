#pragma once

// QUIC 传输薄封装 —— msquic(微软, C API, 跨平台) 的一层「只暴露协议出站真正要用到的东西」的皮。
//
// ★ 谁用它：所有跑在 QUIC 之上的协议出站 —— 目前是 Hysteria2 与 TUIC(v5)。它们把 QuicTransport
//   当一条「已建立的 QUIC 连接」：openConnection() 起 UDP+QUIC+TLS1.3 握手，connected() 即隧道就绪；
//   之后既可开双向流(openBidiStream，用于 TCP 代理 / Hy2 的 HTTP/3 认证 / TUIC 的 Connect)，
//   也可发/收不可靠数据报(sendDatagram / datagramReceived，用于 Hy2 与 TUIC 的 UDP 中继)。
//
// ★ 为什么单独封而不让协议直接调 msquic：
//   · msquic 是**回调式**且回调发生在它自己的工作线程上。协议出站(IOutbound*)全部活在 Qt 事件循环
//     线程里，语义、信号、生命周期都按 QObject 来。这层皮的核心职责就是把 msquic 回调 marshal 回
//     Qt 线程 —— 回调里**当场把事件里的字节拷成 QByteArray**，再用
//     QMetaObject::invokeMethod(qobj, lambda, Qt::QueuedConnection) 投递到 qobj 所在线程 emit 信号。
//     用 qobj 自己作 context：对象析构时未投递的排队事件被 Qt 丢弃(同 DirectOutbound 的异步补读套路)，
//     从根本上规避「回调里访问已析构对象」。
//   · 把 ALPN / SNI / skipVerify / 凭据 收敛成一个 openConnection 入参，避免每个协议各写一遍
//     QUIC_CREDENTIAL_CONFIG / QUIC_CONFIGURATION 样板。
//   · 统一 msquic 的全局初始化(MsQuicOpen2 + Registration)成一个进程级单例(见 .cpp 的 MsQuicLib)。
//
// ★ 线程/生命周期(务必真机验证 —— 见报告的「需真机验证」清单)：
//   析构顺序是关键，且 msquic 里 **stream 句柄是 connection 句柄的子对象** —— ConnectionClose 之后
//   一切 stream 句柄立即失效，此时再 StreamClose 即 UAF。故 ~QuicTransport 必须严格按：
//     ① 逐个 StreamShutdown(ABORT) + StreamClose 关掉 **所有** 子 QuicStream(阻塞式, 返回后不再回调)；
//     ② ConnectionShutdown 停连接数据面；
//     ③ ConnectionClose(阻塞式)；
//     ④ 之后才允许 QObject(及基类)析构。
//   ★ 关键前提：QuicStream 虽是 QuicTransport 的 QObject 子对象, 但 **不能** 依赖 ~QObject 去删它们
//     —— 那发生在 ~QuicTransport 函数体(已 ConnectionClose)之后, 顺序就反了。所以 ~QuicTransport
//     函数体里必须先显式删光子 QuicStream, 再 ConnectionClose(见 .cpp)。
//   如此「回调仍在 msquic 线程里跑」与「本对象正在析构」不会并发。
//
// 跨平台：msquic 的 TLS 后端在 Windows 上可用 Schannel、其它平台用 quictls/OpenSSL3(见报告的 CMake 建议)。
// 本文件只 #include <msquic.h>，不碰任何平台专有 API。
#include <QByteArray>
#include <QObject>
#include <QString>

namespace coastcore {

class QuicTransport;

// ================================ QuicStream ================================
// 一条 QUIC 流(双向或单向发送)。协议侧用它当一根有序可靠的字节管道：
//   · 双向流(openBidiStream)：Hy2 的 TCPRequest / HTTP/3 请求流、TUIC 的 Connect/Packet(流模式)。
//   · 单向发送流(openUniStream)：Hy2 认证需要的 HTTP/3 控制流(先发 SETTINGS)。
// 发送背压：send() 入队的字节在收到 sendCompleted 前一直「在飞」(内存需保活到 SEND_COMPLETE)，
//   bytesInFlight() 把这些未确认字节计入，供 IOutbound::bytesToWrite() 反映真实水位。
// 接收背压：setReceiveEnabled(false) 直接让 msquic 停发 RECEIVE 事件(远端被 QUIC 流控压住)，
//   true 恢复 —— 语义等价于 DirectOutbound 的 readPaused，但由 QUIC 栈原生实现,无需自己缓冲。
class QuicStream : public QObject
{
    Q_OBJECT
public:
    ~QuicStream() override;

    // 发送字节。fin=true 表示这是本流最后一段(发完即优雅关闭发送方向, 等价 StreamSend FIN 标志)。
    // data 会被本对象持有到底层 SEND_COMPLETE 为止(满足 msquic「缓冲需保活到发送完成」的契约)。
    void send(const QByteArray &data, bool fin = false);
    // 优雅关闭发送方向(发 FIN, 不带数据)。
    void shutdownSend();
    // 立即中止收发两个方向(abort)。
    void abort();

    // —— 背压 ——
    // 已入队但尚未被 QUIC 栈确认发送完成的字节数(含尚未 START_COMPLETE 前的缓冲)。
    qint64 bytesInFlight() const;
    // 开/关下行接收(底层 StreamReceiveSetEnabled)。关=远端被流控压住;开=恢复收 RECEIVE。
    void setReceiveEnabled(bool enabled);
    bool isReceiveEnabled() const;

    bool isStarted() const; // START_COMPLETE 已成功

signals:
    void started();                        // 流已在对端就绪(START_COMPLETE, Status 成功)
    void dataReceived(const QByteArray &data); // 下行字节(已从 msquic 事件拷出)
    void sendCompleted(qint64 bytes);      // 这一批 bytes 已被 QUIC 栈确认(用于归还发送水位)
    void peerSendShutdown();               // 对端发完了(收到 FIN, 下行 EOF)
    void failed(const QString &reason);    // 流启动失败 / 被对端 abort
    void closed();                         // 流彻底关闭(SHUTDOWN_COMPLETE)

private:
    friend class QuicTransport;
    explicit QuicStream(QObject *parent);
    class Priv;
    Priv *d;
};

// ============================== QuicTransport ==============================
class QuicTransport : public QObject
{
    Q_OBJECT
public:
    explicit QuicTransport(QObject *parent = nullptr);
    ~QuicTransport() override;

    // 发起到 host:port 的 QUIC 连接。
    //   alpn       —— ALPN 协议名(Hy2 恒 "h3"; TUIC 默认无特定, 按需)。单个 ALPN 即够用。
    //   sni        —— TLS ServerName(=SNI/证书校验名); 空则回落用 host。
    //                 当 host 是 IP 字面量时, 连接目标钉在 host:port(QUIC_PARAM_CONN_REMOTE_ADDRESS),
    //                 sni 仅用作 TLS SNI —— 支持「server=IP + sni=域名」的域前置式配置。
    //   skipVerify —— true=不校验服务器证书(自签场景)。
    // 握手成功 → connected(); 失败 → connectionFailed()。
    void openConnection(const QString &host, quint16 port, const QByteArray &alpn,
                        const QString &sni, bool skipVerify);

    // 开一条**客户端发起的双向流**(已 StreamStart)。所有权交给调用方(parent 可设本 transport)。
    // 连接尚未 connected 时也可调:流会在连接就绪后真正开始(msquic 支持 pending start)。
    QuicStream *openBidiStream();
    // 开一条**客户端发起的单向发送流**(HTTP/3 控制流用)。
    QuicStream *openUniStream();

    // 发一个不可靠数据报(Hy2/TUIC 的 UDP)。总长须 <= datagramMaxSendLength()。
    // 未连接 / 未协商数据报 / 超长 → 静默丢弃(UDP 语义, 丢包由上层重传或忽略)。
    void sendDatagram(const QByteArray &payload);
    bool datagramSendEnabled() const; // 对端已允许收数据报(DATAGRAM_STATE_CHANGED)
    int datagramMaxSendLength() const; // 单个数据报最大可发字节

    // 导出 TLS keying material(RFC5705/QUIC exporter) —— TUIC v5 的 token 由此派生:
    //   label = 客户端 UUID(16 字节原文), context = 明文密码, outLen = 32。
    // ★ 依赖 msquic 的 ConnectionExportKeyingMaterial(**QUIC_API_ENABLE_PREVIEW_FEATURES**, v2.6+)。
    //   编译期不可用时本函数恒返回空 QByteArray, keyingMaterialSupported() 返回 false(见报告 TODO)。
    QByteArray exportKeyingMaterial(const QByteArray &label, const QByteArray &context, int outLen);
    static bool keyingMaterialSupported();

    void close();            // 优雅关闭连接
    bool isConnected() const;

signals:
    void connected();                          // QUIC+TLS 握手完成, 可开流/发数据报
    void connectionFailed(const QString &reason);
    void connectionClosed();                   // 连接关闭(对端 / 传输错误 / 本地 close)
    void datagramReceived(const QByteArray &payload);
    void datagramStateChanged(bool sendEnabled, int maxSendLength);

private:
    QuicStream *createStream(bool unidirectional); // openBidiStream/openUniStream 的共同实现
    class Priv;
    Priv *d;
};

} // namespace coastcore
