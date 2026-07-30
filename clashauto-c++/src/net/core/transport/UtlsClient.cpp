#include "UtlsClient.h"

#include "../crypto/Aead.h" // 复用记录层 AEAD（AES-GCM / ChaCha20-Poly1305）

#include <QCryptographicHash>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QtEndian>

#include <openssl/evp.h>
#include <openssl/x509.h> // REALITY 服务端证书解析（d2i_X509 / 取 Ed25519 公钥 / 取签名）

// ================================================================================================
// 实现说明 —— 读这段再读代码。
//
// 本文件把三件事串起来：
//   (A) uTLS 风格的 ClientHello 字节构造（尽量像 Chrome；REALITY 认证注入 session_id）。
//   (B) 一个 happy-path 的 TLS 1.3 客户端状态机（RFC 8446 §4/§7）：ServerHello → 密钥调度 →
//       解密 EncryptedExtensions/Certificate/CertificateVerify/Finished → 发 client Finished →
//       导出应用流量密钥 → 应用数据记录层收发。
//   (C) 记录层 AEAD（复用 coastcore::Aead）+ HKDF/Transcript（Qt SHA-256/384）。
//
// ★★ 明确的「未做 / 需真机」清单（务必与调用者/审阅者对齐）★★
//   1. REALITY 服务端证书的 **AuthKey 签名核验** 已实现（verifyRealityCertificate）：解析服务端
//      Certificate，取 leaf 证书的 Ed25519 公钥 pub，算 HMAC-SHA512(authKey, pub) 与证书 Signature
//      比对；只有匹配才认「对端握有 REALITY 私钥」，否则在 emit connected()/发任何内层头**之前**中止。
//      这挡住了「TLS1.3 握手成功≠真 REALITY 服务端」——被透明转发到真实站点/被 MITM 的对端算不出
//      该签名，会被拒。**注意：这不是 X.509 证书链校验**（REALITY 故意用自签证书 + AuthKey 代替 PKI）。
//   2. 不处理 HelloRetryRequest（服务端若要求换 group 就直接失败）。
//   3. 不处理 CertificateVerify 的签名校验（TLS 层）；服务端真实性改由 (1) 的 REALITY AuthKey 核验保证。
//   4. 不处理 KeyUpdate、不处理握手消息跨多条记录的复杂分片（仅做了基础的按长度重组）。
//   5. Chrome 指纹是「够像」而非 uTLS 那样逐版本、随机化 GREASE —— 用固定 GREASE。REALITY 的认证
//      不依赖指纹精确（AAD=我们自己发出的 ClientHello 全文，自洽），但抗审查伪装度取决于它。
//   6. 全流程**本机无法验证字节级正确性**；密钥调度/记录层严格按 RFC，但仍须对 xray REALITY 真机联调。
// ================================================================================================

namespace coastcore {

namespace {

// —— 固定 GREASE 值（uTLS 会随机挑 0x?a?a；这里取两个确定值）——
// RFC 8446 §4.2：同一扩展块内**不得**出现两个相同 type 的扩展。ClientHello 头尾各有一个 GREASE
// 扩展，必须用**不同**的 GREASE 码点，否则严格服务端会因扩展类型重复而 abort。故头用 kGrease、
// 尾用 kGrease2。（列表内的 GREASE 占位——supported_groups/key_share/supported_versions/cipher_suites——
// 是「值」而非「扩展类型」，同值复用不违规，沿用 kGrease。）
constexpr quint16 kGrease = 0x0a0a;
constexpr quint16 kGrease2 = 0x1a1a;

// —— 小工具：大端写入 ——
void put8(QByteArray &b, quint8 v) { b.append(char(v)); }
void put16(QByteArray &b, quint16 v)
{
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}
void put24(QByteArray &b, quint32 v)
{
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}

QByteArray randomBytes(int n)
{
    QByteArray out(n, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(out.data()), n / 4);
    // 尾部不足 4 的余数单独填
    for (int i = (n / 4) * 4; i < n; ++i)
        out[i] = char(QRandomGenerator::system()->bounded(256));
    return out;
}

QCryptographicHash::Algorithm hashAlg(bool sha384)
{
    return sha384 ? QCryptographicHash::Sha384 : QCryptographicHash::Sha256;
}

QByteArray transcriptHash(bool sha384, const QByteArray &data)
{
    return QCryptographicHash::hash(data, hashAlg(sha384));
}

QByteArray hmac(bool sha384, const QByteArray &key, const QByteArray &data)
{
    return QMessageAuthenticationCode::hash(data, key, hashAlg(sha384));
}

// HKDF-Extract(salt, IKM) = HMAC-Hash(salt, IKM)。
QByteArray hkdfExtract(bool sha384, const QByteArray &salt, const QByteArray &ikm)
{
    return hmac(sha384, salt, ikm);
}

// HKDF-Expand(PRK, info, L)（RFC 5869 §2.3）。
QByteArray hkdfExpand(bool sha384, const QByteArray &prk, const QByteArray &info, int len)
{
    const int hashLen = sha384 ? 48 : 32;
    QByteArray okm, t;
    quint8 counter = 1;
    while (okm.size() < len) {
        QByteArray in = t;
        in += info;
        in.append(char(counter));
        t = hmac(sha384, prk, in);
        okm += t;
        ++counter;
        if (counter == 0) // 溢出保护：L 上限 255*hashLen
            break;
        Q_UNUSED(hashLen);
    }
    return okm.left(len);
}

// HKDF-Expand-Label（RFC 8446 §7.1）。
QByteArray hkdfExpandLabel(bool sha384, const QByteArray &secret, const QByteArray &label,
                           const QByteArray &context, int len)
{
    QByteArray hkdfLabel;
    put16(hkdfLabel, quint16(len));
    QByteArray full = QByteArray("tls13 ") + label;
    put8(hkdfLabel, quint8(full.size()));
    hkdfLabel += full;
    put8(hkdfLabel, quint8(context.size()));
    hkdfLabel += context;
    return hkdfExpand(sha384, secret, hkdfLabel, len);
}

// Derive-Secret(Secret, Label, Messages) = HKDF-Expand-Label(Secret, Label, Transcript-Hash(Messages), Hash.len)
QByteArray deriveSecret(bool sha384, const QByteArray &secret, const QByteArray &label,
                        const QByteArray &transcriptBytes)
{
    const int hashLen = sha384 ? 48 : 32;
    return hkdfExpandLabel(sha384, secret, label, transcriptHash(sha384, transcriptBytes), hashLen);
}

// —— X25519（OpenSSL EVP）——
// 生成临时密钥对，返回 EVP_PKEY*（所有权归调用方，用完 EVP_PKEY_free），并把 32B 公钥写入 pubOut。
EVP_PKEY *x25519Generate(QByteArray &pubOut)
{
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!pctx)
        return nullptr;
    EVP_PKEY *pkey = nullptr;
    if (EVP_PKEY_keygen_init(pctx) != 1 || EVP_PKEY_keygen(pctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(pctx);
        return nullptr;
    }
    EVP_PKEY_CTX_free(pctx);
    unsigned char pub[32];
    size_t publen = sizeof(pub);
    if (EVP_PKEY_get_raw_public_key(pkey, pub, &publen) != 1 || publen != 32) {
        EVP_PKEY_free(pkey);
        return nullptr;
    }
    pubOut = QByteArray(reinterpret_cast<const char *>(pub), 32);
    return pkey;
}

// ECDH：本端私钥(priv) × 对端 32B 公钥 → 32B 共享密钥。失败返回空。
QByteArray x25519Ecdh(EVP_PKEY *priv, const QByteArray &peerPub32)
{
    if (!priv || peerPub32.size() != 32)
        return {};
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(peerPub32.constData()), 32);
    if (!peer)
        return {};
    EVP_PKEY_CTX *dctx = EVP_PKEY_CTX_new(priv, nullptr);
    QByteArray out;
    if (dctx && EVP_PKEY_derive_init(dctx) == 1 && EVP_PKEY_derive_set_peer(dctx, peer) == 1) {
        unsigned char sec[32];
        size_t slen = sizeof(sec);
        if (EVP_PKEY_derive(dctx, sec, &slen) == 1 && slen == 32)
            out = QByteArray(reinterpret_cast<const char *>(sec), 32);
    }
    if (dctx)
        EVP_PKEY_CTX_free(dctx);
    EVP_PKEY_free(peer);
    return out;
}

} // namespace

// ================================================================================================

UtlsClient::UtlsClient(QObject *parent) : QObject(parent) {}

UtlsClient::~UtlsClient()
{
    if (m_ephemeral)
        EVP_PKEY_free(m_ephemeral);
}

void UtlsClient::setReality(const QByteArray &serverPublicKey32, const QByteArray &shortId)
{
    m_reality = true;
    m_serverPub = serverPublicKey32;
    m_shortId = shortId;
}

QAbstractSocket *UtlsClient::socket() const
{
    return m_sock;
}

void UtlsClient::connectToHost(const QString &host, quint16 port, const QString &sni,
                               const QStringList &alpnList)
{
    m_host = host;
    m_port = port;
    m_sni = sni.isEmpty() ? host : sni;
    m_alpn = alpnList;
    m_state = State::TcpConnecting;

    m_sock = new QTcpSocket(this);
    m_sock->setProxy(QNetworkProxy::NoProxy); // 同 TlsClient/Direct：别被系统代理绕回去
    connect(m_sock, &QTcpSocket::connected, this, [this] { onTcpConnected(); });
    connect(m_sock, &QTcpSocket::readyRead, this, [this] { onReadyRead(); });
    connect(m_sock, &QTcpSocket::disconnected, this, [this] {
        if (m_state == State::Connected)
            emit disconnected();
        else if (!m_errored)
            fail(QStringLiteral("utls: 连接在握手完成前关闭"));
    });
    connect(m_sock, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { onSocketError(); });
    m_sock->connectToHost(host, port);
}

void UtlsClient::onSocketError()
{
    if (m_state == State::Connected) {
        emit disconnected();
        return;
    }
    fail(m_sock ? m_sock->errorString() : QStringLiteral("utls: socket error"));
}

void UtlsClient::onTcpConnected()
{
    m_sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    QByteArray hello = buildClientHello();
    if (hello.isEmpty()) {
        fail(QStringLiteral("utls: 构造 ClientHello 失败"));
        return;
    }
    m_transcript += hello; // CH 进 transcript

    // ClientHello 走明文 handshake 记录（type=22）。
    QByteArray rec;
    put8(rec, 22);
    put16(rec, 0x0303); // legacy_record_version
    put16(rec, quint16(hello.size()));
    rec += hello;
    m_sock->write(rec);

    m_state = State::SentClientHello;
}

// ================================ ClientHello 构造（uTLS/Chrome + REALITY）=========================

QByteArray UtlsClient::buildClientHello()
{
    // 生成本次握手的临时 X25519 密钥（key_share 与 REALITY ECDH 共用）。
    if (m_ephemeral) {
        EVP_PKEY_free(m_ephemeral);
        m_ephemeral = nullptr;
    }
    m_ephemeral = x25519Generate(m_ephemeralPub);
    if (!m_ephemeral)
        return {};

    m_clientRandom = randomBytes(32);
    m_sessionId = QByteArray(32, '\0'); // REALITY 模式下先全零（作 GCM 的 AAD 基底）；否则下面填随机

    // —— 扩展块 —— 顺序尽量贴 Chrome（见文件头说明；REALITY 认证不依赖其精确性）。
    QByteArray exts;
    auto addExt = [&](quint16 id, const QByteArray &data) {
        put16(exts, id);
        put16(exts, quint16(data.size()));
        exts += data;
    };

    // GREASE（空）
    addExt(kGrease, QByteArray());

    // server_name (0)
    {
        const QByteArray host = m_sni.toUtf8();
        QByteArray sn;
        QByteArray list;
        put8(list, 0x00); // name_type=host_name
        put16(list, quint16(host.size()));
        list += host;
        put16(sn, quint16(list.size()));
        sn += list;
        addExt(0x0000, sn);
    }
    // extended_master_secret (23) 空
    addExt(0x0017, QByteArray());
    // renegotiation_info (0xff01) = 0x00
    addExt(0xff01, QByteArray(1, '\0'));
    // supported_groups (10)：GREASE + x25519(0x001d) + secp256r1(0x0017) + secp384r1(0x0018)
    {
        QByteArray g;
        QByteArray list;
        put16(list, kGrease);
        put16(list, 0x001d);
        put16(list, 0x0017);
        put16(list, 0x0018);
        put16(g, quint16(list.size()));
        g += list;
        addExt(0x000a, g);
    }
    // ec_point_formats (11) = [0x00] uncompressed
    {
        QByteArray p;
        put8(p, 0x01);
        put8(p, 0x00);
        addExt(0x000b, p);
    }
    // session_ticket (35) 空
    addExt(0x0023, QByteArray());
    // ALPN (16)（仅当调用方给了；REALITY+VLESS 常留空）
    if (!m_alpn.isEmpty()) {
        QByteArray protos;
        for (const QString &a : m_alpn) {
            const QByteArray p = a.toUtf8();
            put8(protos, quint8(p.size()));
            protos += p;
        }
        QByteArray a;
        put16(a, quint16(protos.size()));
        a += protos;
        addExt(0x0010, a);
    }
    // status_request (5) = [status_type=1, responder_id_list=0, request_ext=0]
    {
        QByteArray s;
        put8(s, 0x01);
        put16(s, 0x0000);
        put16(s, 0x0000);
        addExt(0x0005, s);
    }
    // signature_algorithms (13)：Chrome 常见集合
    {
        static const quint16 sigs[] = {0x0403, 0x0804, 0x0401, 0x0503,
                                       0x0805, 0x0501, 0x0806, 0x0601};
        QByteArray list;
        for (quint16 s : sigs)
            put16(list, s);
        QByteArray sa;
        put16(sa, quint16(list.size()));
        sa += list;
        addExt(0x000d, sa);
    }
    // signed_certificate_timestamp (18) 空
    addExt(0x0012, QByteArray());
    // key_share (51)：GREASE(1B 占位) + x25519(0x001d, 32B 公钥)
    {
        QByteArray entries;
        // GREASE entry：group=GREASE, key_exchange=1 字节 0x00
        put16(entries, kGrease);
        put16(entries, 0x0001);
        put8(entries, 0x00);
        // x25519 entry
        put16(entries, 0x001d);
        put16(entries, 0x0020); // 32
        entries += m_ephemeralPub;
        QByteArray ks;
        put16(ks, quint16(entries.size()));
        ks += entries;
        addExt(0x0033, ks);
    }
    // psk_key_exchange_modes (45) = [len=1, psk_dhe_ke(0x01)]
    {
        QByteArray m;
        put8(m, 0x01);
        put8(m, 0x01);
        addExt(0x002d, m);
    }
    // supported_versions (43) = [len, GREASE, 0x0304]
    {
        QByteArray v;
        put8(v, 0x04);
        put16(v, kGrease);
        put16(v, 0x0304);
        addExt(0x002b, v);
    }
    // compress_certificate (27) = [len=2, brotli(0x0002)]
    {
        QByteArray c;
        put8(c, 0x02);
        put16(c, 0x0002);
        addExt(0x001b, c);
    }
    // 收尾 GREASE（payload 0x00）—— 必须与头部 GREASE 用**不同**码点（见 kGrease2 说明）。
    addExt(kGrease2, QByteArray(1, '\0'));

    // —— cipher_suites —— GREASE + 三个 TLS1.3 套件
    QByteArray ciphers;
    put16(ciphers, kGrease);
    put16(ciphers, 0x1301); // TLS_AES_128_GCM_SHA256
    put16(ciphers, 0x1302); // TLS_AES_256_GCM_SHA384
    put16(ciphers, 0x1303); // TLS_CHACHA20_POLY1305_SHA256

    // —— 组装 ClientHello body ——
    QByteArray body;
    put16(body, 0x0303); // legacy_version = TLS1.2（TLS1.3 靠 supported_versions）
    body += m_clientRandom; // 32
    put8(body, 0x20);       // session_id len = 32
    body += m_sessionId;    // 32（REALITY 先全零，稍后 GCM 覆盖；否则随机）
    put16(body, quint16(ciphers.size()));
    body += ciphers;
    put8(body, 0x01); // compression_methods len
    put8(body, 0x00); // null
    put16(body, quint16(exts.size()));
    body += exts;

    // —— handshake 头 ——
    QByteArray msg;
    put8(msg, 0x01); // ClientHello
    put24(msg, quint32(body.size()));
    msg += body;

    // session_id 内容在 handshake 消息里的偏移：1(type)+3(len)+2(ver)+32(random)+1(sid_len)=39
    const int sidOffset = 39;
    const int randomOffset = 6; // 1+3+2

    if (m_reality) {
        if (!applyRealityAuth(msg, sidOffset, randomOffset))
            return {};
    } else {
        // 非 REALITY：session_id 用随机 32B（并同步回 body 已嵌入的位置）。
        const QByteArray rnd = randomBytes(32);
        msg.replace(sidOffset, 32, rnd);
        m_sessionId = rnd;
    }
    return msg;
}

bool UtlsClient::applyRealityAuth(QByteArray &helloMsg, int sidOffset, int randomOffset)
{
    if (m_serverPub.size() != 32) {
        // publicKey 缺失/长度不对：无法认证。诚实失败而非静默发出无效认证。
        // （publicKey 由主控从 ProxyNode.realityPublicKey 解码后经 setReality 传入。）
        return false;
    }
    // 1) ECDH：临时私钥 × 服务端 REALITY 公钥 → 共享密钥。
    QByteArray ecdh = x25519Ecdh(m_ephemeral, m_serverPub);
    if (ecdh.size() != 32)
        return false;

    // 2) HKDF-SHA256(secret=ecdh, salt=random[:20], info="REALITY") 读 32B → authKey。
    const QByteArray salt = helloMsg.mid(randomOffset, 20);         // random[:20]
    const QByteArray nonce = helloMsg.mid(randomOffset + 20, 12);   // random[20:32]
    const QByteArray prk = hkdfExtract(/*sha384=*/false, salt, ecdh);
    const QByteArray authKey = hkdfExpand(false, prk, QByteArray("REALITY"), 32);
    // 存起来：既用于本函数密封 session_id，也用于握手末尾 verifyRealityCertificate 的 HMAC-SHA512。
    m_authKey = authKey;

    // 3) 结构化 sid16 = [ver(3) reserved(1) timestamp(4,BE) shortId(8, 右侧补零)]
    QByteArray sid16(16, '\0');
    // 版本占位（服务端一般只记录不硬校验；上线可改为镜像某真实 xray 版本，见 TODO）。
    sid16[0] = char(0x00);
    sid16[1] = char(0x00);
    sid16[2] = char(0x00);
    sid16[3] = char(0x00);
    const quint32 now = quint32(QDateTime::currentSecsSinceEpoch());
    qToBigEndian(now, reinterpret_cast<uchar *>(sid16.data()) + 4);
    // shortId：0..8 字节，写到 sid16[8:16]，不足右侧留零。
    const int sidn = qMin(8, int(m_shortId.size()));
    for (int i = 0; i < sidn; ++i)
        sid16[8 + i] = m_shortId[i];

    // 4) AAD = 「session_id 区域全零」的 helloMsg 全文（此刻 helloMsg 的 sid 区正是 32 个零）。
    const QByteArray aad = helloMsg;

    // 5) AES-256-GCM 密封 sid16 → 32B（密文16 || tag16），覆盖 session_id。
    const QByteArray sealed = Aead::seal(Aead::Method::Aes256Gcm, authKey, nonce, aad, sid16);
    if (sealed.size() != 32)
        return false;
    helloMsg.replace(sidOffset, 32, sealed);
    m_sessionId = sealed;

    // ★ TODO(需真机)：REALITY 版本字节、以及服务端对 timestamp 窗口的容忍度，需对 xray 服务端实测校准。
    return true;
}

// ================================ 记录层收发 =====================================================

void UtlsClient::onReadyRead()
{
    if (!m_sock)
        return;
    // 真背压：应用数据阶段被暂停时**不从 socket 抽字节**——字节滞留在 Qt 读缓冲（受 setReadBufferSize
    // 上限约束），填满后 TCP 接收窗口关闭、远端停发。恢复时由 setReadPaused(false) 主动补抽一次。
    // 握手阶段(state!=Connected)永远读：握手不受上层流控影响，暂停握手会死锁。
    if (m_readPaused && m_state == State::Connected)
        return;
    m_recvBuf += m_sock->readAll();
    feedSocket();
}

void UtlsClient::feedSocket()
{
    // 按 5B 记录头切分：type(1) legacy_version(2) length(2)。
    for (;;) {
        if (m_recvBuf.size() < 5)
            return;
        const quint8 type = quint8(m_recvBuf.at(0));
        const int len = (int(quint8(m_recvBuf.at(3))) << 8) | int(quint8(m_recvBuf.at(4)));
        if (m_recvBuf.size() < 5 + len)
            return; // 记录还没收全
        const QByteArray fragment = m_recvBuf.mid(5, len);
        m_recvBuf.remove(0, 5 + len);
        handleRecord(type, fragment);
        if (m_errored)
            return;
    }
}

void UtlsClient::handleRecord(quint8 type, const QByteArray &fragment)
{
    switch (type) {
    case 20: // change_cipher_spec：TLS1.3 里纯为中间盒兼容，忽略。
        return;
    case 21: // alert（明文，仅可能在握手早期出现）
        fail(QStringLiteral("utls: 收到明文 alert（握手被拒？）"));
        return;
    case 22: // handshake（明文）—— 只可能是 ServerHello
        handleHandshakeBytes(fragment);
        return;
    case 23: { // application_data —— 加密记录
        QByteArray key, iv;
        quint64 *seq = nullptr;
        if (m_state == State::WaitFlight2) {
            key = m_sHsKey;
            iv = m_sHsIv;
            seq = &m_sHsSeq;
        } else if (m_state == State::Connected) {
            key = m_sApKey;
            iv = m_sApIv;
            seq = &m_sApSeq;
        } else {
            fail(QStringLiteral("utls: 意外的加密记录（密钥未就绪）"));
            return;
        }
        quint8 innerType = 0;
        QByteArray plain;
        if (!decryptRecord(fragment, key, iv, *seq, innerType, plain)) {
            fail(QStringLiteral("utls: 记录解密/验签失败"));
            return;
        }
        if (innerType == 22) {
            handleHandshakeBytes(plain); // EE/Cert/CertVerify/Finished，或握手后 NewSessionTicket
        } else if (innerType == 23) {
            if (!plain.isEmpty()) {
                m_appReadBuf += plain;
                emit readyRead();
            }
        } else if (innerType == 21) {
            // close_notify 或错误 alert：视为关闭。
            if (m_state == State::Connected)
                emit disconnected();
            else
                fail(QStringLiteral("utls: 握手期收到加密 alert"));
        }
        return;
    }
    default:
        fail(QStringLiteral("utls: 未知记录类型"));
        return;
    }
}

void UtlsClient::handleHandshakeBytes(const QByteArray &bytes)
{
    m_hsMsgBuf += bytes;
    for (;;) {
        if (m_hsMsgBuf.size() < 4)
            return;
        const quint8 hsType = quint8(m_hsMsgBuf.at(0));
        const int msgLen = (int(quint8(m_hsMsgBuf.at(1))) << 16) |
                           (int(quint8(m_hsMsgBuf.at(2))) << 8) | int(quint8(m_hsMsgBuf.at(3)));
        if (m_hsMsgBuf.size() < 4 + msgLen)
            return; // 消息还没收全（可能跨记录）
        const QByteArray full = m_hsMsgBuf.left(4 + msgLen);
        const QByteArray body = m_hsMsgBuf.mid(4, msgLen);
        m_hsMsgBuf.remove(0, 4 + msgLen);

        if (hsType == 0x02) { // ServerHello
            handleServerHello(body);
            if (m_errored)
                return;
            m_transcript += full;
            deriveHandshakeKeys();
            if (m_errored)
                return;
            m_state = State::WaitFlight2;
        } else if (m_state == State::WaitFlight2) {
            onFlight2Message(hsType, full, body);
        } else if (m_state == State::Connected) {
            // 握手后消息：NewSessionTicket(4) / KeyUpdate(24) —— 本实现忽略（KeyUpdate 未实现，见 TODO）。
        }
        if (m_errored)
            return;
    }
}

void UtlsClient::handleServerHello(const QByteArray &body)
{
    // body: legacy_version(2) random(32) sid_len(1) sid(..) cipher_suite(2) comp(1) ext_len(2) exts
    int p = 0;
    auto need = [&](int n) { return p + n <= body.size(); };
    if (!need(2 + 32 + 1))
        return fail(QStringLiteral("utls: ServerHello 过短"));
    p += 2; // legacy_version
    // random（含可能的 HelloRetryRequest 魔数——本实现不处理 HRR，见 TODO）
    p += 32;
    const int sidLen = quint8(body.at(p));
    p += 1;
    if (!need(sidLen + 2 + 1))
        return fail(QStringLiteral("utls: ServerHello 截断"));
    p += sidLen; // session_id echo
    const quint16 cipher = (quint16(quint8(body.at(p))) << 8) | quint8(body.at(p + 1));
    p += 2;
    p += 1; // legacy_compression_method
    if (!need(2))
        return fail(QStringLiteral("utls: ServerHello 无扩展"));
    const int extLen = (int(quint8(body.at(p))) << 8) | int(quint8(body.at(p + 1)));
    p += 2;
    if (!need(extLen))
        return fail(QStringLiteral("utls: ServerHello 扩展截断"));
    const int extEnd = p + extLen;

    // 选定套件参数。
    m_suite.id = cipher;
    if (cipher == 0x1301) {
        m_suite = {0x1301, 32, 16, false, int(Aead::Method::Aes128Gcm)};
    } else if (cipher == 0x1302) {
        m_suite = {0x1302, 48, 32, true, int(Aead::Method::Aes256Gcm)};
    } else if (cipher == 0x1303) {
        m_suite = {0x1303, 32, 32, false, int(Aead::Method::ChaCha20Poly1305)};
    } else {
        return fail(QStringLiteral("utls: 服务端选了不支持的 cipher_suite"));
    }

    // 解析扩展，取 supported_versions(0x2b, 须=0x0304) 与 key_share(0x33) 里服务端 x25519 公钥。
    bool tls13 = false;
    while (p + 4 <= extEnd) {
        const quint16 eid = (quint16(quint8(body.at(p))) << 8) | quint8(body.at(p + 1));
        const int elen = (int(quint8(body.at(p + 2))) << 8) | int(quint8(body.at(p + 3)));
        p += 4;
        if (p + elen > extEnd)
            break;
        const QByteArray edata = body.mid(p, elen);
        p += elen;
        if (eid == 0x002b) { // supported_versions
            if (edata.size() >= 2 &&
                (quint8(edata.at(0)) == 0x03 && quint8(edata.at(1)) == 0x04))
                tls13 = true;
        } else if (eid == 0x0033) { // key_share：group(2) key_len(2) key
            if (edata.size() >= 4) {
                const quint16 grp = (quint16(quint8(edata.at(0))) << 8) | quint8(edata.at(1));
                const int klen = (int(quint8(edata.at(2))) << 8) | int(quint8(edata.at(3)));
                if (grp == 0x001d && klen == 32 && edata.size() >= 4 + 32)
                    m_serverKeyShare = edata.mid(4, 32);
                // ★ TODO(HRR)：若 key_share 扩展只回带 group（无 key）＝HelloRetryRequest，本实现未处理。
            }
        }
    }
    if (!tls13)
        return fail(QStringLiteral("utls: 服务端未协商 TLS1.3"));
    if (m_serverKeyShare.size() != 32)
        return fail(QStringLiteral("utls: ServerHello 无 x25519 key_share（可能是 HRR，未支持）"));
}

void UtlsClient::onFlight2Message(quint8 hsType, const QByteArray &msgFull, const QByteArray &body)
{
    switch (hsType) {
    case 0x08: // EncryptedExtensions
        m_transcript += msgFull;
        break;
    case 0x0b: // Certificate
        // 先计入 transcript（server Finished 的 MAC 覆盖 CH..CertificateVerify，缺一不可）。
        m_transcript += msgFull;
        // ★ REALITY 服务端认证：仅完成 TLS1.3 握手**不足以**证明对端是真 REALITY 服务端——任何 TLS1.3
        //   对端都能让 Finished MAC 通过。真正的鉴别在这里：leaf 证书的 Ed25519 公钥 pub 满足
        //   HMAC-SHA512(authKey, pub) == 证书 Signature（只有握 REALITY 私钥者能派生 authKey→伪造该签名）。
        //   被透明转发到真实站点/被 MITM 的对端算不出，会在此被拒（emit failed，绝不进 Connected）。
        if (m_reality) {
            if (!verifyRealityCertificate(body)) {
                return fail(QStringLiteral(
                    "utls: REALITY 服务端证书 AuthKey 核验失败（非 REALITY 服务端 / 被转发到真实站 / MITM）"));
            }
            m_realityAuthOk = true;
        }
        break;
    case 0x0f: // CertificateVerify
        // TLS 层的 CertificateVerify 签名此实现不校验；服务端真实性由上面的 REALITY AuthKey 核验保证。
        m_transcript += msgFull;
        break;
    case 0x14: { // Finished（服务端）
        // 服务端 verify_data 应 = HMAC(finished_key_s, Transcript-Hash(CH..CertificateVerify))。
        const bool sha384 = m_suite.sha384;
        const QByteArray finishedKeyS =
            hkdfExpandLabel(sha384, m_sHsSecret, QByteArray("finished"), QByteArray(), m_suite.hashLen);
        const QByteArray expected =
            hmac(sha384, finishedKeyS, transcriptHash(sha384, m_transcript)); // 尚未含 server Finished
        if (body != expected) {
            return fail(QStringLiteral("utls: 服务端 Finished 校验失败（握手密钥不匹配）"));
        }
        m_transcript += msgFull; // 现在把 server Finished 计入 transcript
        m_serverFinishedSeen = true;
        finishHandshake();
        break;
    }
    default:
        // 其它握手消息（如 CertificateRequest）本实现未处理。
        m_transcript += msgFull;
        break;
    }
}

// REALITY 服务端认证 —— 解析 TLS1.3 Certificate 消息体，核验 leaf 证书。
//   Certificate 消息体(RFC 8446 §4.4.2)：
//     opaque certificate_request_context<0..2^8-1>;   // 服务端证书里恒空
//     CertificateEntry certificate_list<0..2^24-1>;   // 每项: cert_data<1..2^24-1> + extensions<0..2^16-1>
//   leaf = certificate_list 的第一项的 cert_data（DER 编码的 X.509 证书）。
// xray REALITY：取 leaf 的 Ed25519 公钥 pub，判 HMAC-SHA512(authKey, pub) == 证书 Signature 字段。
bool UtlsClient::verifyRealityCertificate(const QByteArray &certMsgBody)
{
    if (m_authKey.size() != 32)
        return false; // 没有 authKey（未走 REALITY 认证注入）——无从核验。

    const uchar *b = reinterpret_cast<const uchar *>(certMsgBody.constData());
    const int n = certMsgBody.size();
    int p = 0;
    auto need = [&](int k) { return p + k <= n; };

    if (!need(1))
        return false;
    const int ctxLen = b[p];
    p += 1;
    if (!need(ctxLen))
        return false;
    p += ctxLen; // certificate_request_context（服务端恒空）
    if (!need(3))
        return false;
    const int listLen = (int(b[p]) << 16) | (int(b[p + 1]) << 8) | int(b[p + 2]);
    p += 3;
    if (!need(listLen) || listLen < 3)
        return false;
    // 第一项 CertificateEntry：cert_data<1..2^24-1>
    const int certLen = (int(b[p]) << 16) | (int(b[p + 1]) << 8) | int(b[p + 2]);
    p += 3;
    if (certLen <= 0 || !need(certLen))
        return false;
    const QByteArray der = certMsgBody.mid(p, certLen);

    // —— 用 OpenSSL 解析 DER 证书，取 Ed25519 公钥与签名字段 ——
    const unsigned char *dp = reinterpret_cast<const unsigned char *>(der.constData());
    X509 *cert = d2i_X509(nullptr, &dp, long(der.size()));
    if (!cert)
        return false;

    bool ok = false;
    EVP_PKEY *pk = X509_get_pubkey(cert);
    if (pk && EVP_PKEY_base_id(pk) == EVP_PKEY_ED25519) {
        unsigned char pub[32];
        size_t publen = sizeof(pub);
        if (EVP_PKEY_get_raw_public_key(pk, pub, &publen) == 1 && publen == 32) {
            // 证书的签名字段（REALITY 服务端把它填成 HMAC-SHA512(authKey, pub)，64B）。
            const ASN1_BIT_STRING *psig = nullptr;
            const X509_ALGOR *palg = nullptr;
            X509_get0_signature(&psig, &palg, cert);
            if (psig) {
                const QByteArray sig(reinterpret_cast<const char *>(ASN1_STRING_get0_data(psig)),
                                     ASN1_STRING_length(psig));
                const QByteArray pubBa(reinterpret_cast<const char *>(pub), 32);
                const QByteArray expected = QMessageAuthenticationCode::hash(
                    pubBa, m_authKey, QCryptographicHash::Sha512); // 64B
                // 定长（都是 64B）比较；QByteArray::operator== 对内容比较即可。
                ok = (!expected.isEmpty() && sig == expected);
            }
        }
    }
    if (pk)
        EVP_PKEY_free(pk);
    X509_free(cert);
    return ok;
}

void UtlsClient::finishHandshake()
{
    // ★ REALITY 门禁：REALITY 模式下必须已通过服务端证书 AuthKey 核验，才允许进入应用数据阶段。
    //   走到这里但 m_realityAuthOk==false，意味着对端没送可认证的证书（或根本不是 REALITY 服务端）——
    //   在 emit connected() / 发任何内层 VLESS 头之前中止，避免把 UUID+真实目的地泄露给非 REALITY 对端。
    if (m_reality && !m_realityAuthOk) {
        fail(QStringLiteral("utls: REALITY 未通过服务端认证（未收到可核验的服务端证书）"));
        return;
    }

    // transcript 定格在「CH..server Finished」，用它导出应用流量密钥并算 client Finished。
    m_transcriptThroughServerFinished = m_transcript;
    deriveAppKeys();

    // 中间盒兼容：客户端在 Finished 前发一条 ChangeCipherSpec（Chrome 会发）。
    {
        QByteArray ccs;
        put8(ccs, 20);
        put16(ccs, 0x0303);
        put16(ccs, 0x0001);
        put8(ccs, 0x01);
        m_sock->write(ccs);
    }

    sendClientFinished();

    m_state = State::Connected;
    emit connected();

    // 握手结束时可能已有随后的 application_data 记录躺在 m_recvBuf 里（如 NewSessionTicket），继续吃。
    if (!m_recvBuf.isEmpty())
        feedSocket();
}

void UtlsClient::sendClientFinished()
{
    const bool sha384 = m_suite.sha384;
    const QByteArray finishedKeyC =
        hkdfExpandLabel(sha384, m_cHsSecret, QByteArray("finished"), QByteArray(), m_suite.hashLen);
    const QByteArray verifyData =
        hmac(sha384, finishedKeyC, transcriptHash(sha384, m_transcriptThroughServerFinished));

    QByteArray msg;
    put8(msg, 0x14); // Finished
    put24(msg, quint32(verifyData.size()));
    msg += verifyData;
    m_transcript += msg; // 计入 transcript（供潜在的 resumption；本实现不用）

    // 用 client handshake 流量密钥加密（contentType=22）。
    const QByteArray rec = encryptRecord(22, msg, m_cHsKey, m_cHsIv, m_cHsSeq);
    m_sock->write(rec);
}

// ================================ 密钥调度 =======================================================

void UtlsClient::deriveHandshakeKeys()
{
    const bool sha384 = m_suite.sha384;
    const int hashLen = m_suite.hashLen;
    const int keyLen = m_suite.keyLen;
    const QByteArray zeros(hashLen, '\0');

    // ECDHE 共享密钥（本端临时私钥 × 服务端 key_share）。
    const QByteArray shared = x25519Ecdh(m_ephemeral, m_serverKeyShare);
    if (shared.size() != 32) {
        fail(QStringLiteral("utls: ECDH（握手密钥）失败"));
        return;
    }

    const QByteArray earlySecret = hkdfExtract(sha384, zeros, zeros); // PSK=0
    const QByteArray derivedEarly = deriveSecret(sha384, earlySecret, QByteArray("derived"), QByteArray());
    m_handshakeSecret = hkdfExtract(sha384, derivedEarly, shared);

    // transcript 此刻 = CH..SH（handleHandshakeBytes 已 append SH）。
    m_cHsSecret = deriveSecret(sha384, m_handshakeSecret, QByteArray("c hs traffic"), m_transcript);
    m_sHsSecret = deriveSecret(sha384, m_handshakeSecret, QByteArray("s hs traffic"), m_transcript);

    m_cHsKey = hkdfExpandLabel(sha384, m_cHsSecret, QByteArray("key"), QByteArray(), keyLen);
    m_cHsIv = hkdfExpandLabel(sha384, m_cHsSecret, QByteArray("iv"), QByteArray(), 12);
    m_sHsKey = hkdfExpandLabel(sha384, m_sHsSecret, QByteArray("key"), QByteArray(), keyLen);
    m_sHsIv = hkdfExpandLabel(sha384, m_sHsSecret, QByteArray("iv"), QByteArray(), 12);
    m_sHsSeq = m_cHsSeq = 0;

    const QByteArray derivedHs = deriveSecret(sha384, m_handshakeSecret, QByteArray("derived"), QByteArray());
    m_masterSecret = hkdfExtract(sha384, derivedHs, zeros);
}

void UtlsClient::deriveAppKeys()
{
    const bool sha384 = m_suite.sha384;
    const int keyLen = m_suite.keyLen;

    m_cApSecret = deriveSecret(sha384, m_masterSecret, QByteArray("c ap traffic"),
                               m_transcriptThroughServerFinished);
    m_sApSecret = deriveSecret(sha384, m_masterSecret, QByteArray("s ap traffic"),
                               m_transcriptThroughServerFinished);

    m_cApKey = hkdfExpandLabel(sha384, m_cApSecret, QByteArray("key"), QByteArray(), keyLen);
    m_cApIv = hkdfExpandLabel(sha384, m_cApSecret, QByteArray("iv"), QByteArray(), 12);
    m_sApKey = hkdfExpandLabel(sha384, m_sApSecret, QByteArray("key"), QByteArray(), keyLen);
    m_sApIv = hkdfExpandLabel(sha384, m_sApSecret, QByteArray("iv"), QByteArray(), 12);
    m_cApSeq = m_sApSeq = 0;
}

// ================================ 记录层 AEAD ====================================================

QByteArray UtlsClient::encryptRecord(quint8 contentType, const QByteArray &plaintext,
                                     const QByteArray &key, const QByteArray &iv, quint64 &seq)
{
    // 内层明文 = content || content_type || (padding，本实现不加 padding)
    QByteArray inner = plaintext;
    inner.append(char(contentType));

    const int recLen = inner.size() + 16; // + tag
    // AAD = 密文记录头：type=23, legacy_version=0x0303, length。
    QByteArray aad;
    put8(aad, 23);
    put16(aad, 0x0303);
    put16(aad, quint16(recLen));

    // per-record nonce = iv XOR seq（seq 右对齐进 12B iv 的低 8 字节）。
    QByteArray nonce = iv; // 12B
    quint8 seqb[8];
    qToBigEndian(seq, seqb);
    for (int i = 0; i < 8; ++i)
        nonce[4 + i] = char(quint8(nonce.at(4 + i)) ^ seqb[i]);

    const QByteArray ct =
        Aead::seal(Aead::Method(m_suite.aeadMethod), key, nonce, aad, inner);
    ++seq;

    QByteArray rec = aad; // 5B 头
    rec += ct;            // 密文 || tag
    return rec;
}

bool UtlsClient::decryptRecord(const QByteArray &record, const QByteArray &key, const QByteArray &iv,
                               quint64 &seq, quint8 &innerType, QByteArray &plaintext)
{
    // record 是已剥掉 5B 头的密文分片（= 密文 || tag）。AAD 需按外层记录头重建。
    if (record.size() < 16)
        return false;
    QByteArray aad;
    put8(aad, 23);
    put16(aad, 0x0303);
    put16(aad, quint16(record.size()));

    QByteArray nonce = iv;
    quint8 seqb[8];
    qToBigEndian(seq, seqb);
    for (int i = 0; i < 8; ++i)
        nonce[4 + i] = char(quint8(nonce.at(4 + i)) ^ seqb[i]);

    QByteArray inner;
    if (!Aead::open(Aead::Method(m_suite.aeadMethod), key, nonce, aad, record, &inner))
        return false;
    ++seq;

    // 内层明文尾部：content_type，再往后可能有 0 填充（去掉尾部的 0，最后一个非零字节即 type）。
    int end = inner.size() - 1;
    while (end >= 0 && inner.at(end) == '\0')
        --end;
    if (end < 0)
        return false; // 全零，非法
    innerType = quint8(inner.at(end));
    plaintext = inner.left(end);
    return true;
}

// ================================ 应用数据管道 ===================================================

qint64 UtlsClient::write(const QByteArray &data)
{
    if (m_state != State::Connected || !m_sock)
        return -1;
    // TLS1.3 记录明文上限 = 2^14 字节(RFC 8446 §5.2 TLSPlaintext.length ≤ 16384)。超过就必须拆成
    // 多条 application_data 记录，否则对端 record_overflow。这里按 ≤16384 明文/条切分逐条加密发出。
    constexpr int kMaxRecordPlaintext = 16384;
    const int total = int(data.size());
    int off = 0;
    while (off < total) {
        const int n = qMin(kMaxRecordPlaintext, total - off);
        const QByteArray rec =
            encryptRecord(23, data.mid(off, n), m_cApKey, m_cApIv, m_cApSeq);
        m_sock->write(rec);
        off += n;
    }
    return total; // total==0 时不发空记录，直接返回 0
}

QByteArray UtlsClient::readAll()
{
    QByteArray out = m_appReadBuf;
    m_appReadBuf.clear();
    return out;
}

qint64 UtlsClient::bytesAvailable() const
{
    return m_appReadBuf.size();
}

qint64 UtlsClient::bytesToWrite() const
{
    return m_sock ? m_sock->bytesToWrite() : 0;
}

void UtlsClient::setReadBufferSize(qint64 size)
{
    if (m_sock)
        m_sock->setReadBufferSize(size);
}

void UtlsClient::setReadPaused(bool paused)
{
    if (m_readPaused == paused)
        return;
    m_readPaused = paused;
    // 恢复时：readyRead 对**已到达**的字节不会重新触发，故主动补抽一次（等价于 TlsClient/Socks5
    // 恢复后的一次补读）。onReadyRead 内部已因 m_readPaused==false 而正常抽取并解密下发。
    if (!paused && m_sock && m_state == State::Connected)
        onReadyRead();
}

void UtlsClient::abort()
{
    m_state = State::Closed;
    if (m_sock)
        m_sock->abort();
}

void UtlsClient::close()
{
    m_state = State::Closed;
    if (m_sock)
        m_sock->close();
}

void UtlsClient::fail(const QString &reason)
{
    if (m_errored)
        return;
    m_errored = true;
    m_state = State::Closed;
    emit errorOccurred(reason);
}

} // namespace coastcore
