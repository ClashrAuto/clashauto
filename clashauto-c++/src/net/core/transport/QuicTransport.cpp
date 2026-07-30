#include "QuicTransport.h"

#include <QAtomicInteger>
#include <QByteArray>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>

#include <cstring>

#include <msquic.h>

// ============================================================================
// 说明：本文件把 msquic(C, 回调式, 回调在 msquic 工作线程) 封成 Qt 化的 QuicTransport / QuicStream。
// 核心手法 = 「回调里拷字节 + QMetaObject::invokeMethod(qobj, lambda, QueuedConnection) 投回 Qt 线程」。
// 见 QuicTransport.h 顶部的线程/生命周期说明。凡标 ★TODO / ★需真机 的点见报告。
// ============================================================================

namespace {

// —— 进程级 msquic 库单例：MsQuicOpen2 拿 API 表 + 一个 Registration。懒初始化, 线程安全靠函数内 static。
class MsQuicLib
{
public:
    static MsQuicLib &instance()
    {
        static MsQuicLib s_lib;
        return s_lib;
    }

    const QUIC_API_TABLE *api() const { return m_api; }
    HQUIC registration() const { return m_reg; }
    bool ok() const { return m_api != nullptr && m_reg != nullptr; }

private:
    MsQuicLib()
    {
        if (QUIC_FAILED(MsQuicOpen2(&m_api))) {
            m_api = nullptr;
            return;
        }
        QUIC_REGISTRATION_CONFIG regConfig;
        std::memset(&regConfig, 0, sizeof(regConfig));
        regConfig.AppName = "coast";
        regConfig.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;
        if (QUIC_FAILED(m_api->RegistrationOpen(&regConfig, &m_reg))) {
            m_reg = nullptr;
        }
    }
    ~MsQuicLib()
    {
        // 进程退出时清理。RegistrationClose 会等所有连接收尾。
        if (m_api) {
            if (m_reg)
                m_api->RegistrationClose(m_reg);
            MsQuicClose(m_api);
        }
    }
    const QUIC_API_TABLE *m_api = nullptr;
    HQUIC m_reg = nullptr;
};

inline const QUIC_API_TABLE *Api() { return MsQuicLib::instance().api(); }

} // namespace

namespace coastcore {

// ============================================================================
//                                QuicStream
// ============================================================================

class QuicStream::Priv
{
public:
    explicit Priv(QuicStream *owner) : q(owner) {}

    QuicStream *q = nullptr;
    QuicTransport *owner = nullptr; // 归属连接(仅用于 parent 关系, 不解引用其内部)
    HQUIC stream = nullptr;
    bool started = false;
    bool closedEmitted = false;
    QAtomicInt receiveEnabled{1};
    QAtomicInteger<qint64> inFlight{0}; // 已入队未完成发送的字节

    // —— 一次 StreamSend 的挂起上下文：QByteArray 必须活到 SEND_COMPLETE ——
    struct SendCtx {
        QByteArray data;
        QUIC_BUFFER buf;
    };

    // ---- 以下 emitXxx 均由「投回 Qt 线程」的 lambda 调用, 在 QObject 线程里 emit(nested 类可访问信号) ----
    void emitStarted()
    {
        started = true;
        emit q->started();
    }
    void emitData(const QByteArray &b) { emit q->dataReceived(b); }
    void emitSendCompleted(qint64 n) { emit q->sendCompleted(n); }
    void emitPeerShutdown() { emit q->peerSendShutdown(); }
    void emitFailed(const QString &r)
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->failed(r);
    }
    void emitClosed()
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->closed();
    }

    // marshal 帮手：把一个动作投回 q 所在线程执行(q 析构则被 Qt 丢弃)。
    template <typename F>
    void post(F &&fn)
    {
        QMetaObject::invokeMethod(q, std::forward<F>(fn), Qt::QueuedConnection);
    }

    // ---- msquic 回调(msquic 线程) ----
    QUIC_STATUS handleEvent(QUIC_STREAM_EVENT *ev)
    {
        const QUIC_API_TABLE *api = Api();
        switch (ev->Type) {
        case QUIC_STREAM_EVENT_START_COMPLETE:
            if (QUIC_FAILED(ev->START_COMPLETE.Status)) {
                post([this] { emitFailed(QStringLiteral("quic stream start failed")); });
            } else {
                post([this] { emitStarted(); });
            }
            break;
        case QUIC_STREAM_EVENT_RECEIVE: {
            // 把本次事件里的所有 QUIC_BUFFER 拼成一个 QByteArray(拷贝走, 事件返回后其内存即失效)。
            QByteArray chunk;
            // reserve 前给个界:TotalBufferLength 是 uint64, 直接 int() 可能溢出成负值 → reserve 语义混乱;
            //   过大值 reserve 又可能 bad_alloc。单次 RECEIVE 受流控窗口约束, 实际很小, 这里只作防御。
            if (ev->RECEIVE.TotalBufferLength > 0 && ev->RECEIVE.TotalBufferLength <= (64u << 20))
                chunk.reserve(int(ev->RECEIVE.TotalBufferLength));
            for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER &b = ev->RECEIVE.Buffers[i];
                chunk.append(reinterpret_cast<const char *>(b.Buffer), int(b.Length));
            }
            const bool fin = (ev->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
            if (!chunk.isEmpty())
                post([this, chunk] { emitData(chunk); });
            if (fin)
                post([this] { emitPeerShutdown(); });
            break;
        }
        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            auto *sc = static_cast<SendCtx *>(ev->SEND_COMPLETE.ClientContext);
            qint64 n = sc ? qint64(sc->data.size()) : 0;
            delete sc; // 发送缓冲此刻可释放
            inFlight.fetchAndAddOrdered(-n);
            if (n > 0)
                post([this, n] { emitSendCompleted(n); });
            break;
        }
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            post([this] { emitPeerShutdown(); });
            break;
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            post([this] { emitFailed(QStringLiteral("quic stream aborted by peer")); });
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            post([this] { emitClosed(); });
            break;
        default:
            break;
        }
        Q_UNUSED(api);
        return QUIC_STATUS_SUCCESS;
    }

    static QUIC_STATUS QUIC_API sCallback(HQUIC /*stream*/, void *ctx, QUIC_STREAM_EVENT *ev)
    {
        return static_cast<Priv *>(ctx)->handleEvent(ev);
    }
};

QuicStream::QuicStream(QObject *parent) : QObject(parent), d(new Priv(this)) {}

QuicStream::~QuicStream()
{
    // ★ 关键收尾顺序(见头文件说明)：先 Shutdown 停数据面, 再 **阻塞式** StreamClose(返回后保证不再回调),
    //   最后析构。这样 msquic 线程里不会有本对象的回调还在飞。
    if (d->stream) {
        const QUIC_API_TABLE *api = Api();
        if (api) {
            api->StreamShutdown(d->stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            api->StreamClose(d->stream);
        }
        d->stream = nullptr;
    }
    delete d;
}

void QuicStream::send(const QByteArray &data, bool fin)
{
    const QUIC_API_TABLE *api = Api();
    if (!api || !d->stream) {
        return;
    }
    // 空 + fin：只发 FIN, 无 buffer。
    if (data.isEmpty() && fin) {
        api->StreamShutdown(d->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        return;
    }
    if (data.isEmpty()) {
        return;
    }
    // 挂起上下文持有 data 到 SEND_COMPLETE(msquic 契约:缓冲需保活到发送完成)。
    auto *sc = new Priv::SendCtx;
    sc->data = data; // 隐式共享, 引用同一份底层, 不额外拷贝
    sc->buf.Buffer = reinterpret_cast<uint8_t *>(const_cast<char *>(sc->data.constData()));
    sc->buf.Length = uint32_t(sc->data.size());
    d->inFlight.fetchAndAddOrdered(qint64(sc->data.size()));
    const QUIC_SEND_FLAGS flags = fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE;
    const QUIC_STATUS st = api->StreamSend(d->stream, &sc->buf, 1, flags, sc);
    if (QUIC_FAILED(st)) {
        d->inFlight.fetchAndAddOrdered(-qint64(sc->data.size()));
        delete sc;
    }
}

void QuicStream::shutdownSend()
{
    const QUIC_API_TABLE *api = Api();
    if (api && d->stream)
        api->StreamShutdown(d->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
}

void QuicStream::abort()
{
    const QUIC_API_TABLE *api = Api();
    if (api && d->stream)
        api->StreamShutdown(d->stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
}

qint64 QuicStream::bytesInFlight() const { return d->inFlight.loadAcquire(); }

void QuicStream::setReceiveEnabled(bool enabled)
{
    const QUIC_API_TABLE *api = Api();
    d->receiveEnabled.storeRelease(enabled ? 1 : 0);
    if (api && d->stream)
        api->StreamReceiveSetEnabled(d->stream, enabled ? TRUE : FALSE);
}

bool QuicStream::isReceiveEnabled() const { return d->receiveEnabled.loadAcquire() != 0; }

bool QuicStream::isStarted() const { return d->started; }

// ============================================================================
//                               QuicTransport
// ============================================================================

// msquic 的 QUIC_STATUS → 人能看懂的名字。只覆盖握手期真会碰到的那几个（其余打十六进制即可）。
// ★ 别小看这张表：真机上「HY2 拨不通」的第一手线索就是这里的 ALPN/证书/超时之分。
static QString quicStatusName(QUIC_STATUS st)
{
    if (st == QUIC_STATUS_CONNECTION_TIMEOUT) return QStringLiteral("连接超时(对端没响应)");
    if (st == QUIC_STATUS_CONNECTION_IDLE)    return QStringLiteral("空闲超时");
    if (st == QUIC_STATUS_UNREACHABLE)        return QStringLiteral("目标不可达");
    if (st == QUIC_STATUS_ALPN_NEG_FAILURE)   return QStringLiteral("ALPN 协商失败");
    if (st == QUIC_STATUS_VER_NEG_ERROR)      return QStringLiteral("QUIC 版本协商失败");
    if (st == QUIC_STATUS_CONNECTION_REFUSED) return QStringLiteral("对端拒绝连接");
    if (st == QUIC_STATUS_BAD_CERTIFICATE)    return QStringLiteral("证书无效");
    if (st == QUIC_STATUS_UNKNOWN_CERTIFICATE) return QStringLiteral("证书未知(对端不认)");
    if (st == QUIC_STATUS_CERT_UNTRUSTED_ROOT) return QStringLiteral("证书根不受信任");
    if (st == QUIC_STATUS_CERT_EXPIRED)       return QStringLiteral("证书已过期");
    if (st == QUIC_STATUS_TLS_ERROR)          return QStringLiteral("TLS 错误");
    if (st == QUIC_STATUS_HANDSHAKE_FAILURE)  return QStringLiteral("握手失败");
    if (st == QUIC_STATUS_ABORTED)            return QStringLiteral("被中止");
    if (st == QUIC_STATUS_PROTOCOL_ERROR)     return QStringLiteral("协议错误");
    // ★ QUIC_STATUS_TLS_ALERT / QUIC_STATUS_CERT_ERROR 在 msquic 里是**带参宏**(alert 号 / 证书错误号)，
    //   不能当常量比。但可以用它算出「TLS alert 区间」，把对端发来的 alert 号直接解出来 ——
    //   这比一句「传输错误」有用得多（例如 alert 40=handshake_failure、47=illegal_parameter）。
    if (st >= QUIC_STATUS_TLS_ALERT(0) && st <= QUIC_STATUS_TLS_ALERT(255))
        return QStringLiteral("对端 TLS alert %1").arg(quint32(st - QUIC_STATUS_TLS_ALERT(0)));
    return QStringLiteral("传输错误");
}

// HTTP/3 应用层错误码 → 名字（RFC 9114 §8.1 / RFC 9204 §6）。Hy2 跑在 HTTP/3 上，对端关连接时
// 带的就是这些码；把它翻出来，「握手过了但被关」这一类故障才有得查。
static QString h3ErrorName(quint64 ec)
{
    switch (ec) {
    case 0x100: return QStringLiteral("H3_NO_ERROR");
    case 0x101: return QStringLiteral("H3_GENERAL_PROTOCOL_ERROR");
    case 0x102: return QStringLiteral("H3_INTERNAL_ERROR");
    case 0x103: return QStringLiteral("H3_STREAM_CREATION_ERROR");
    case 0x104: return QStringLiteral("H3_CLOSED_CRITICAL_STREAM");
    case 0x105: return QStringLiteral("H3_FRAME_UNEXPECTED");
    case 0x106: return QStringLiteral("H3_FRAME_ERROR");
    case 0x107: return QStringLiteral("H3_EXCESSIVE_LOAD");
    case 0x108: return QStringLiteral("H3_ID_ERROR");
    case 0x109: return QStringLiteral("H3_SETTINGS_ERROR");
    case 0x10a: return QStringLiteral("H3_MISSING_SETTINGS");
    case 0x10b: return QStringLiteral("H3_REQUEST_REJECTED");
    case 0x10c: return QStringLiteral("H3_REQUEST_CANCELLED");
    case 0x10d: return QStringLiteral("H3_REQUEST_INCOMPLETE");
    case 0x10e: return QStringLiteral("H3_MESSAGE_ERROR");
    case 0x10f: return QStringLiteral("H3_CONNECT_ERROR");
    case 0x110: return QStringLiteral("H3_VERSION_FALLBACK");
    case 0x200: return QStringLiteral("QPACK_DECOMPRESSION_FAILED");
    case 0x201: return QStringLiteral("QPACK_ENCODER_STREAM_ERROR");
    case 0x202: return QStringLiteral("QPACK_DECODER_STREAM_ERROR");
    default:    return QStringLiteral("应用层错误");
    }
}

// 对端(服务器)开过来的流的「接住并丢弃」回调。
// ★ 为什么必须有：既然我们通告了 PeerUnidiStreamCount>0，服务器就真会开流过来 ——
//   Hy2 场景下那是 HTTP/3 的控制流(SETTINGS/GOAWAY)与 QPACK 编解码流。我们不需要它们的内容
//   (Hy2/TUIC 的数据面走我们自己开的流 + 数据报)，但**必须接住**：msquic 要求应用为对端流指定
//   回调并最终 StreamClose，否则句柄泄漏 / 连接被拆。返回 SUCCESS 即视为数据已消费。
static QUIC_STATUS QUIC_API sDrainPeerStream(HQUIC stream, void * /*ctx*/, QUIC_STREAM_EVENT *ev)
{
    if (ev->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        if (const QUIC_API_TABLE *api = Api())
            api->StreamClose(stream); // 句柄的关闭责任在我们这边
    }
    return QUIC_STATUS_SUCCESS;
}

class QuicTransport::Priv
{
public:
    explicit Priv(QuicTransport *owner) : q(owner) {}

    QuicTransport *q = nullptr;
    HQUIC conn = nullptr;
    HQUIC config = nullptr;
    bool connected = false;
    bool closedEmitted = false;
    QString closeInfo;  // 最近一次关闭/失败的可读原因(供上层拼进错误信息, 见 lastCloseReason())
    QByteArray alpn; // 保活:ConfigurationOpen 期间需有效(实际只用在 open 里, 留存无妨)

    QAtomicInt dgramSendEnabled{0};
    QAtomicInteger<int> dgramMaxLen{0};

    template <typename F>
    void post(F &&fn)
    {
        QMetaObject::invokeMethod(q, std::forward<F>(fn), Qt::QueuedConnection);
    }

    void emitConnected()
    {
        connected = true;
        emit q->connected();
    }
    void emitFailed(const QString &r)
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->connectionFailed(r);
    }
    void emitClosed()
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->connectionClosed();
    }
    void emitDatagram(const QByteArray &b) { emit q->datagramReceived(b); }
    void emitDgramState(bool en, int maxLen) { emit q->datagramStateChanged(en, maxLen); }

    QUIC_STATUS handleConnEvent(QUIC_CONNECTION_EVENT *ev)
    {
        switch (ev->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            post([this] { emitConnected(); });
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT: {
            // ★ 一定要把 msquic 的 Status/ErrorCode 带出去：握手失败的真因几乎全在这两个数里
            //   （ALPN 谈崩 / 证书被拒 / 版本谈崩 / 干脆超时=对端没在听或包没到），
            //   丢掉它们就只剩一句「握手失败」,真机排查时等于瞎子。
            const QUIC_STATUS st = ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
            const quint64 ec = quint64(ev->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
            post([this, st, ec] {
                const QString why = QStringLiteral("%1 (status=0x%2, err=%3)")
                                        .arg(quicStatusName(st)).arg(quint32(st), 0, 16).arg(ec);
                closeInfo = QStringLiteral("传输层关闭: ") + why;
                if (connected)
                    emitClosed();
                else
                    emitFailed(QStringLiteral("quic 握手失败: ") + why);
            });
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER: {
            // ★ 对端主动关时带的是**应用层**错误码。跑 HTTP/3 的 Hy2 全靠它区分「密码错」「H3 帧不对」
            //   「QPACK 解不开」—— 丢掉它就只剩「连接被关了」，等于没有线索。
            const quint64 ec = quint64(ev->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
            post([this, ec] {
                closeInfo = QStringLiteral("对端关闭: %1 (app err=0x%2)").arg(h3ErrorName(ec)).arg(ec, 0, 16);
                emitClosed();
            });
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            post([this] { emitClosed(); });
            break;
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            // 服务器开过来的流(HTTP/3 控制流 / QPACK 流 …)：接住并丢弃，见 sDrainPeerStream。
            if (const QUIC_API_TABLE *api = Api()) {
                api->SetCallbackHandler(ev->PEER_STREAM_STARTED.Stream,
                                        reinterpret_cast<void *>(&sDrainPeerStream), nullptr);
            }
            break;
        case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED: {
            const bool en = ev->DATAGRAM_STATE_CHANGED.SendEnabled != FALSE;
            const int maxLen = int(ev->DATAGRAM_STATE_CHANGED.MaxSendLength);
            dgramSendEnabled.storeRelease(en ? 1 : 0);
            dgramMaxLen.storeRelease(maxLen);
            post([this, en, maxLen] { emitDgramState(en, maxLen); });
            break;
        }
        case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED: {
            // 发送侧数据报的持有缓冲(sendDatagram 里 new 的 QByteArray)在其状态终结时释放。
            if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(ev->DATAGRAM_SEND_STATE_CHANGED.State)) {
                auto *hold = static_cast<QByteArray *>(ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
                delete hold;
            }
            break;
        }
        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
            const QUIC_BUFFER *b = ev->DATAGRAM_RECEIVED.Buffer;
            QByteArray copy(reinterpret_cast<const char *>(b->Buffer), int(b->Length));
            post([this, copy] { emitDatagram(copy); });
            break;
        }
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    static QUIC_STATUS QUIC_API sConnCallback(HQUIC /*c*/, void *ctx, QUIC_CONNECTION_EVENT *ev)
    {
        return static_cast<Priv *>(ctx)->handleConnEvent(ev);
    }
};

QuicTransport::QuicTransport(QObject *parent) : QObject(parent), d(new Priv(this)) {}

QuicTransport::~QuicTransport()
{
    // ★ 关键收尾顺序(见头文件)：msquic 里 stream 句柄是 connection 的子对象, ConnectionClose 后即失效。
    //   若把 QuicStream 交给 ~QObject 去删, 那发生在本函数体(已 ConnectionClose)之后 → StreamClose
    //   在 ConnectionClose 之后跑 = UAF。故这里必须 **先** 显式删光所有子 QuicStream(各自析构里做
    //   StreamShutdown(ABORT)+StreamClose), 再 ConnectionShutdown/ConnectionClose。
    //   createStream() 恒以本 transport 为 parent, 故子 QuicStream 都能由 findChildren 找到。
    const QList<QuicStream *> streams = findChildren<QuicStream *>(QString(), Qt::FindDirectChildrenOnly);
    for (QuicStream *s : streams)
        delete s; // ~QuicStream: StreamShutdown+StreamClose(阻塞式, 返回后该流不再回调)

    if (d->conn) {
        const QUIC_API_TABLE *api = Api();
        if (api) {
            // 所有流已 StreamClose 完毕。先 Shutdown, 再阻塞式 ConnectionClose(返回后不再回调)。
            api->ConnectionShutdown(d->conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api->ConnectionClose(d->conn);
        }
        d->conn = nullptr;
    }
    if (d->config) {
        const QUIC_API_TABLE *api = Api();
        if (api)
            api->ConfigurationClose(d->config);
        d->config = nullptr;
    }
    delete d;
}

void QuicTransport::openConnection(const QString &host, quint16 port, const QByteArray &alpn,
                                   const QString &sni, bool skipVerify)
{
    const QUIC_API_TABLE *api = Api();
    HQUIC reg = MsQuicLib::instance().registration();
    if (!api || !reg) {
        d->post([this] { d->emitFailed(QStringLiteral("msquic not initialized")); });
        return;
    }

    d->alpn = alpn;
    QUIC_BUFFER alpnBuf;
    alpnBuf.Buffer = reinterpret_cast<uint8_t *>(const_cast<char *>(d->alpn.constData()));
    alpnBuf.Length = uint32_t(d->alpn.size());

    // ★★ 必须显式给**入向**流配额，不能用默认(NULL)。
    //   PeerUnidiStreamCount/PeerBidiStreamCount 是「允许对端向我们开多少条流」，msquic 默认 **0**。
    //   曾经这里传 NULL，注释还写着「客户端不需要设 stream 配额(由服务器授予)」—— 那是把方向弄反了：
    //   服务器授予的是**我们能开多少条**，而 HTTP/3 要求服务器能开它自己的控制流(单向)。
    //   真机后果(官方 hysteria2 服务端 + quic-go 帧日志)：
    //     Processed Transport Parameters: ... MaxUniStreamNum: 0   ← 我们通告 0
    //     server <- StreamFrame{StreamID: 2 / 0 ...}               ← 我们的控制流+认证请求都收到了
    //     Closing connection with error: Application error 0x100 (local)  ← 服务器开不出控制流,直接收摊
    //   表现为「QUIC 握手成功、认证请求已送达，但服务器一言不发地关连接、日志里连一行都没有」。
    //   单向给足(HTTP/3 要控制流+QPACK 编/解码流=3 条；TUIC 的 quic 模式 UDP 也走服务端单向流)；
    //   双向保持 0：Hy2/TUIC 的服务器不会向客户端开双向流，不开口就不必伺候。
    QUIC_SETTINGS settings;
    std::memset(&settings, 0, sizeof(settings));
    settings.PeerUnidiStreamCount = 16;
    settings.IsSet.PeerUnidiStreamCount = 1;
    settings.PeerBidiStreamCount = 0;
    settings.IsSet.PeerBidiStreamCount = 1;
    // 数据报(Hy2/TUIC 的 UDP 回程)在此一并声明，比建连后再 SetParam 更早、更稳。
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = 1;
    if (QUIC_FAILED(api->ConfigurationOpen(reg, &alpnBuf, 1, &settings, sizeof(settings), nullptr,
                                           &d->config))) {
        d->post([this] { d->emitFailed(QStringLiteral("quic ConfigurationOpen failed")); });
        return;
    }

    QUIC_CREDENTIAL_CONFIG cred;
    std::memset(&cred, 0, sizeof(cred));
    cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (skipVerify) {
        cred.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    } else {
        // ★ OpenSSL(quictls) 后端默认不校验, 需显式要求内建校验;Schannel 默认即校验, 置此位无害。
        cred.Flags |= QUIC_CREDENTIAL_FLAG_USE_TLS_BUILTIN_CERTIFICATE_VALIDATION;
    }
    if (QUIC_FAILED(api->ConfigurationLoadCredential(d->config, &cred))) {
        d->post([this] { d->emitFailed(QStringLiteral("quic LoadCredential failed")); });
        return;
    }

    if (QUIC_FAILED(api->ConnectionOpen(reg, &Priv::sConnCallback, d, &d->conn))) {
        d->post([this] { d->emitFailed(QStringLiteral("quic ConnectionOpen failed")); });
        return;
    }

    // 数据报接收已在上面的 QUIC_SETTINGS 里声明(真机 quic-go 日志确认对端收到
    // MaxDatagramFrameSize: 65535)，这里不再重复 SetParam。

    // TLS ServerName(=SNI/证书校验名)：优先用 sni, 空才回落 host。
    const QByteArray sniName = (sni.isEmpty() ? host : sni).toUtf8();
    const QByteArray hostUtf8 = host.toUtf8();

    // 连接目标固定为 host:port。当 host 是 IP 字面量时, 显式把对端地址钉在这个 IP 上
    //   (QUIC_PARAM_CONN_REMOTE_ADDRESS), 这样传给 ConnectionStart 的 ServerName 就只当 TLS SNI /
    //   证书校验用, 不会被拿去做 DNS —— 从而「server=IP + sni=域名」能真正工作(域前置)。
    //   host 是域名时无法在此免解析地钉地址, 只能把 host 交给 ConnectionStart 做 DNS(见下)。
    QUIC_ADDR remoteAddr;
    std::memset(&remoteAddr, 0, sizeof(remoteAddr));
    const bool hostIsIp = QuicAddrFromString(hostUtf8.constData(), port, &remoteAddr) != FALSE;
    const char *serverName = nullptr;
    if (hostIsIp) {
        api->SetParam(d->conn, QUIC_PARAM_CONN_REMOTE_ADDRESS, sizeof(remoteAddr), &remoteAddr);
        serverName = sniName.constData(); // 连到已钉住的 IP, SNI 用 sniName(可为域名)
    } else {
        // host 是域名:ConnectionStart 用它做 DNS + SNI。msquic 只有一个 ServerName 参数, 无法在走 DNS
        //   的同时用另一个名字做 SNI;此场景下 sni≠host 时以 host 为准保证连得上(见报告)。
        serverName = hostUtf8.constData();
    }
    // 注:serverName 指向本作用域内的 sniName / hostUtf8, 二者活到本函数返回;ConnectionStart 内部会
    //     复制 ServerName, 故调用期间有效即可。
    const QUIC_STATUS st = api->ConnectionStart(d->conn, d->config, QUIC_ADDRESS_FAMILY_UNSPEC,
                                                serverName, port);
    if (QUIC_FAILED(st)) {
        d->post([this] { d->emitFailed(QStringLiteral("quic ConnectionStart failed")); });
    }
}

// 内部工厂:造 QuicStream + StreamOpen + StreamStart。
// 作为 QuicTransport 成员实现 —— QuicTransport 是 QuicStream 的 friend(见头文件), 故可访问其私有
// 构造函数、私有 d 指针与私有嵌套类型 QuicStream::Priv(含 sCallback)。
QuicStream *QuicTransport::createStream(bool unidirectional)
{
    const QUIC_API_TABLE *api = Api();
    auto *s = new QuicStream(this);
    s->d->owner = this;
    if (!api || !d->conn) {
        s->d->post([s] { s->d->emitFailed(QStringLiteral("no quic connection")); });
        return s;
    }
    const QUIC_STREAM_OPEN_FLAGS openFlags =
        unidirectional ? QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL : QUIC_STREAM_OPEN_FLAG_NONE;
    if (QUIC_FAILED(
            api->StreamOpen(d->conn, openFlags, &QuicStream::Priv::sCallback, s->d, &s->d->stream))) {
        s->d->post([s] { s->d->emitFailed(QStringLiteral("quic StreamOpen failed")); });
        return s;
    }
    if (QUIC_FAILED(api->StreamStart(s->d->stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        s->d->post([s] { s->d->emitFailed(QStringLiteral("quic StreamStart failed")); });
    }
    return s;
}

QuicStream *QuicTransport::openBidiStream() { return createStream(false); }

QuicStream *QuicTransport::openUniStream() { return createStream(true); }

void QuicTransport::sendDatagram(const QByteArray &payload)
{
    const QUIC_API_TABLE *api = Api();
    if (!api || !d->conn || d->dgramSendEnabled.loadAcquire() == 0)
        return;
    const int maxLen = d->dgramMaxLen.loadAcquire();
    if (maxLen > 0 && payload.size() > maxLen)
        return; // 超长:UDP 语义直接丢(上层可自行分片, 见 Hy2/TUIC 的分片逻辑)
    // 数据报也要求缓冲活到发送完成:复用一个持有 QByteArray 的上下文, 在连接回调的
    // DATAGRAM_SEND_STATE_CHANGED(final) 里释放。此处为简洁用一次性 new+主动 leak-safe:
    auto *hold = new QByteArray(payload);
    QUIC_BUFFER buf;
    buf.Buffer = reinterpret_cast<uint8_t *>(const_cast<char *>(hold->constData()));
    buf.Length = uint32_t(hold->size());
    // ClientSendContext = hold;缓冲在 handleConnEvent 的 DATAGRAM_SEND_STATE_CHANGED(final) 里释放。
    const QUIC_STATUS st = api->DatagramSend(d->conn, &buf, 1, QUIC_SEND_FLAG_NONE, hold);
    if (QUIC_FAILED(st))
        delete hold;
}

bool QuicTransport::datagramSendEnabled() const { return d->dgramSendEnabled.loadAcquire() != 0; }
int QuicTransport::datagramMaxSendLength() const { return d->dgramMaxLen.loadAcquire(); }

bool QuicTransport::keyingMaterialSupported()
{
    // ★ 判据是 **COAST_HAVE_QUIC_KEYING**（由 CMake 真编译探测决定），不是 QUIC_API_ENABLE_PREVIEW_FEATURES。
    //   踩过的坑：那个宏只是「请求打开 preview 分支」，**被定义 ≠ 这套 API 真的存在**——用 msquic 的
    //   最新发布版 v2.5.9 真头编译（树莓派实测）时，即便定义了它，QUIC_KEYING_MATERIAL_CONFIG 与
    //   QUIC_API_TABLE::ConnectionExportKeyingMaterial 依然**不存在**，于是整个 QuicTransport.cpp 编不过。
    //   所以改成由 CMake 拿真头 try_compile 探一次，探到才定义 COAST_HAVE_QUIC_KEYING。
#ifdef COAST_HAVE_QUIC_KEYING
    return true;
#else
    return false;
#endif
}

QByteArray QuicTransport::exportKeyingMaterial(const QByteArray &label, const QByteArray &context,
                                               int outLen)
{
#ifdef COAST_HAVE_QUIC_KEYING
    const QUIC_API_TABLE *api = Api();
    if (!api || !d->conn || outLen <= 0)
        return {};
    // ★ msquic 的 QUIC_KEYING_MATERIAL_CONFIG.Label 是 **_Field_z_ 的 NUL 结尾 const char***, 结构体里
    //   没有 LabelLength 字段;底层 OpenSSL 后端要给 SSL_export_keying_material 传 llen, 只能对该 C 串
    //   取 strlen。而 TUIC v5 的 exporter label = 16 字节 UUID 原文(RFC5705, 见 tuic SPEC), 可能含
    //   0x00。若 UUID 含内嵌 0x00, msquic 会在第一个 0x00 处截断 label → 导出的 token 与 quinn/rustls
    //   (label 为任意字节切片)不一致 → 服务器认证失败。随机 UUID 命中 0x00 的概率 ≈ 1-(255/256)^16 ≈ 6%。
    //   经此公有 API **无法** 忠实导出含内嵌 NUL 的 label(需要一个带 LabelLength 的 exporter, 属 msquic
    //   侧改动 —— 见报告)。这里的正确做法是:检测到内嵌 NUL 就 **明确失败**(返回空), 让上层 TUIC 立刻
    //   报「keying material export failed」而不是拿着错 token 静默去认证、最终被服务器动断而难以定位。
    if (label.contains('\0'))
        return {};
    QUIC_KEYING_MATERIAL_CONFIG cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.Label = label.constData(); // QByteArray::constData() 保证以 '\0' 结尾;上面已排除内嵌 NUL
    cfg.Context = reinterpret_cast<const uint8_t *>(context.constData());
    cfg.ContextLength = uint32_t(context.size());
    cfg.OutputLength = uint32_t(outLen);
    QByteArray out(outLen, Qt::Uninitialized);
    if (QUIC_FAILED(api->ConnectionExportKeyingMaterial(
            d->conn, &cfg, reinterpret_cast<uint8_t *>(out.data()))))
        return {};
    return out;
#else
    Q_UNUSED(label);
    Q_UNUSED(context);
    Q_UNUSED(outLen);
    // 这套 exporter API 在当前 msquic 里不存在（v2.5.9 就没有；见 keyingMaterialSupported 的说明）→
    // TUIC 的 token 导不出来。上层 TuicOutbound 会因 token.size()!=32 明确失败，而不是拿错 token 静默认证。
    return {};
#endif
}

void QuicTransport::close()
{
    const QUIC_API_TABLE *api = Api();
    if (api && d->conn)
        api->ConnectionShutdown(d->conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
}

bool QuicTransport::isConnected() const { return d->connected; }

// 最近一次关闭/失败的可读原因。上层(Hy2/TUIC)把它拼进自己的错误信息，避免只剩一句
// 「连接被关了」——握手成功之后的故障(H3 帧错、QPACK 解不开、认证被拒)全靠这个码定位。
QString QuicTransport::lastCloseReason() const { return d->closeInfo; }

} // namespace coastcore
