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

class QuicTransport::Priv
{
public:
    explicit Priv(QuicTransport *owner) : q(owner) {}

    QuicTransport *q = nullptr;
    HQUIC conn = nullptr;
    HQUIC config = nullptr;
    bool connected = false;
    bool closedEmitted = false;
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
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            post([this] {
                if (connected)
                    emitClosed();
                else
                    emitFailed(QStringLiteral("quic transport shutdown (handshake/transport error)"));
            });
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            post([this] { emitClosed(); });
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            post([this] { emitClosed(); });
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
    if (d->conn) {
        const QUIC_API_TABLE *api = Api();
        if (api) {
            // 先 Shutdown(silent 静默不发 close 帧亦可), 再阻塞式 ConnectionClose(返回后不再回调)。
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

    // 用默认设置(NULL) —— 客户端不需要设 stream 配额(由服务器授予)。数据报接收另行 SetParam。
    if (QUIC_FAILED(api->ConfigurationOpen(reg, &alpnBuf, 1, nullptr, 0, nullptr, &d->config))) {
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

    // 开启数据报接收(Hy2/TUIC 的 UDP 回程要用)。★需真机验证:个别 msquic 版本要求经 QUIC_SETTINGS
    //   在 ConfigurationOpen 时设 DatagramReceiveEnabled;此处走连接级 SetParam, 需确认生效。
    const uint8_t one = 1;
    api->SetParam(d->conn, QUIC_PARAM_CONN_DATAGRAM_RECEIVE_ENABLED, sizeof(one), &one);

    // ServerName 同时用于 DNS 解析与 TLS SNI。
    // ★TODO/需真机:当 host 是 IP 字面量而 sni 是另一个域名(域前置式)时, msquic 把 ServerName 既做
    //   连接目标又做 SNI, 无法二者分离。此处以 host 为准保证连得上;若必须自定义 SNI 需另设 TLS 参数。
    const QByteArray serverName = (sni.isEmpty() ? host : host).toUtf8();
    Q_UNUSED(sni);
    const QUIC_STATUS st = api->ConnectionStart(d->conn, d->config, QUIC_ADDRESS_FAMILY_UNSPEC,
                                                serverName.constData(), port);
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
#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
    return true;
#else
    return false;
#endif
}

QByteArray QuicTransport::exportKeyingMaterial(const QByteArray &label, const QByteArray &context,
                                               int outLen)
{
#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
    const QUIC_API_TABLE *api = Api();
    if (!api || !d->conn || outLen <= 0)
        return {};
    // ★TODO/需真机:QUIC_KEYING_MATERIAL_CONFIG.Label 是 **null 结尾的 const char***, 而 TUIC 的
    //   exporter label = 16 字节 UUID 原文(可能含 0x00)。若 UUID 含 0x00, strlen 会截断 → token 错。
    //   多数 UUID 无 0x00, 常规可用;但这是与 quinn/rustls(label 为任意字节切片)的一处语义差, 须真机比对。
    QByteArray labelZ = label;
    labelZ.append('\0');
    QUIC_KEYING_MATERIAL_CONFIG cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.Label = labelZ.constData();
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
    return {}; // 编译期无 preview 特性 → TUIC token 不可用(见报告 TODO)
#endif
}

void QuicTransport::close()
{
    const QUIC_API_TABLE *api = Api();
    if (api && d->conn)
        api->ConnectionShutdown(d->conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
}

bool QuicTransport::isConnected() const { return d->connected; }

} // namespace coastcore
