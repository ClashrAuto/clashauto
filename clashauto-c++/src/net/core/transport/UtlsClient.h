#pragma once

// UtlsClient —— 一个**自建的最小 TLS 1.3 客户端**，专为「需要控制 ClientHello 指纹」的场景而写。
//
// ★ 为什么不能用 TlsClient(QSslSocket)：QSslSocket 底层是 Qt 的 TLS 后端（Windows=Schannel、其它=
//   OpenSSL libssl），我们**无法**指定 ClientHello 的扩展顺序 / GREASE / cipher 集合 / key_share，
//   也拿不到握手用的临时 X25519 私钥。而 REALITY + uTLS 的全部要点恰恰是：
//     · uTLS：让 ClientHello 的字节序列**逐字节**伪装成某款浏览器（默认 Chrome）——扩展顺序、
//       GREASE 占位、supported_groups / signature_algorithms / ALPN / key_share=x25519、cipher 集合。
//     · REALITY：把「用服务端 REALITY 公钥 + 本次握手的临时 X25519 私钥做 ECDH 得到的共享密钥」派生出
//       的认证数据，AES-256-GCM 加密后塞进 ClientHello 的 **session_id**（32B）。服务端据此鉴别是否
//       为真客户端，是则接管连接（跑内层协议），否则把流量透明转发给真实目标站（SNI=dest）。
//   这两件事都要求「我们自己拼 ClientHello 字节、自己掌握握手私钥、自己跑 TLS1.3 状态机」，QSslSocket
//   一件都给不了 —— 所以本类从零实现一个够用的 TLS1.3 客户端（happy path），底层只用一根 QTcpSocket。
//
// ★ 加密原语的分工（沿用本项目既定约定，见 crypto/Aead.h、crypto/Kdf.h）：
//     · X25519 密钥生成 / ECDH：OpenSSL libcrypto 的 EVP（EVP_PKEY_X25519）—— Qt 给不了。
//     · 记录层 AEAD（AES-128/256-GCM、ChaCha20-Poly1305，12B nonce / 16B tag）：**复用**
//       coastcore::Aead（它就是封的 EVP，参数与 TLS1.3 记录层完全对得上）。
//     · HKDF-Extract / HKDF-Expand-Label / Transcript-Hash：用 Qt 的 QMessageAuthenticationCode /
//       QCryptographicHash（SHA-256 / SHA-384）在本文件内实现（不引新库，也不改 Kdf.h —— Kdf 那个是
//       SS 专用的 HKDF-SHA1）。
//
// ★ 对上暴露的接口**刻意对齐 transport/TlsClient**（connected/readyRead/disconnected/errorOccurred +
//   write/readAll/…），这样承载层（RealityOutbound 里的 VLESS 流）可以像用 TlsClient 一样用它，
//   几乎不必分叉代码路径。
//
// ★★★ 诚实声明（务必连同代码一起看）★★★
//   TLS1.3 握手 + REALITY 认证是**字节级敏感**的密码学协议，且本机**无法**做端到端验证（本项目也没有
//   TLS 测试床）。本文件是「结构完整 + 关键密码学/密钥调度按 RFC 8446 / REALITY 源码实现」，但：
//     · 只实现 happy path：不处理 HelloRetryRequest、不处理 key_update、不处理跨记录分片的复杂重组，
//       也**不做 X.509 证书链校验**（改用 REALITY 的 AuthKey 核验代替，见下）。
//     · REALITY 服务端证书的「AuthKey 签名」核验**已实现**（onFlight2Message 解析服务端 Certificate，
//       取 leaf 证书的 Ed25519 公钥，算 HMAC-SHA512(authKey, pub) 与证书 Signature 比对；只有匹配才
//       视为握到 REALITY 私钥的真服务端，否则在 emit connected()/发任何内层 VLESS 头之前 emit failed）。
//     · uTLS 指纹是「够像 Chrome」而非 uTLS 那样每次随机化 GREASE —— 用固定 GREASE 值（见 .cpp）。
//   凡此种种在 .cpp 中逐点用 TODO / 「需真机验证」标出。**上线前必须对 xray REALITY 服务端做真机联调。**

#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

// 前置声明：OpenSSL 的不透明类型（.cpp 里 #include <openssl/evp.h>）。
typedef struct evp_pkey_st EVP_PKEY;

class QTcpSocket;

namespace coastcore {

class UtlsClient : public QObject
{
    Q_OBJECT
public:
    // 目前只做 Chrome 指纹（默认）。其它浏览器指纹是后续 TODO（扩展集合/顺序不同）。
    enum class Fingerprint { Chrome };

    explicit UtlsClient(QObject *parent = nullptr);
    ~UtlsClient() override;

    void setFingerprint(Fingerprint fp) { m_fp = fp; }

    // —— 开启 REALITY 认证 ——
    // serverPublicKey：服务端 REALITY 的 X25519 公钥（**32 字节裸公钥**，调用方须先把 node 里的
    //   base64/hex 形式解码成 32B 再传进来）。
    // shortId：REALITY 的 shortId（**裸字节**，0..8 字节；调用方从 hex 解码后传入，右侧按需补零由本类做）。
    // 调用它即进入 REALITY 模式：ClientHello 的 session_id 会被替换成 AES-256-GCM 认证数据（见 .cpp）。
    // 不调用则退化为「纯 uTLS 指纹的普通 TLS1.3」（此时 session_id 用随机 32B）。
    void setReality(const QByteArray &serverPublicKey32, const QByteArray &shortId);

    // 发起 TCP 连接并在其上跑 TLS1.3 握手。
    //   host/port —— 实际连接的地址（= REALITY 服务器）。
    //   sni       —— ClientHello 里的 server_name（= REALITY 的 dest / 伪装目标站，如 www.microsoft.com）。
    //   alpnList  —— ALPN（REALITY+VLESS 一般留空；传入则写进 ALPN 扩展）。
    // 握手完成 → connected()；任何阶段失败 → errorOccurred()。
    void connectToHost(const QString &host, quint16 port, const QString &sni,
                       const QStringList &alpnList = {});

    // —— 握手完成后的**明文**字节管道（语义同 TlsClient）——
    qint64 write(const QByteArray &data);       // 明文 → 加密成 application_data 记录发出
    QByteArray readAll();                        // 取已解密的下行明文
    qint64 bytesAvailable() const;
    qint64 bytesToWrite() const;                 // 底层 socket 未排空的密文字节
    void setReadBufferSize(qint64 size);
    // 真背压：暂停时**不再从底层 socket 抽字节**，Qt 读缓冲填满后 TCP 接收窗口自然关闭，远端停发；
    // 恢复时补抽一次（对已到达的字节 readyRead 不会重新触发）。仅在应用数据阶段(Connected)生效，
    // 握手阶段永远读（握手不受上层流控影响）。语义对齐 TlsClient/Socks5 的「暂停即不读」。
    void setReadPaused(bool paused);
    bool isReadPaused() const { return m_readPaused; }
    void abort();
    void close();
    bool isEncrypted() const { return m_state == State::Connected; }

    // 底层 socket（连接后非空）。仅供需要读排空进度（bytesWritten）的场景连接信号；勿直接读写。
    QAbstractSocket *socket() const;

signals:
    void connected();                          // TLS1.3 握手完成，明文管道可用
    void readyRead();                          // 有可读下行明文
    void disconnected();                        // 对端关闭
    void errorOccurred(const QString &reason); // TCP / 握手 / 记录层任一环节出错

private:
    // 握手状态机。
    enum class State {
        Idle,
        TcpConnecting,   // 等 TCP connected
        SentClientHello, // 已发 CH，等 ServerHello（明文 type=22 记录）
        WaitFlight2,     // 已处理 SH、导出握手密钥，等 EE/Cert/CertVerify/Finished（type=23 加密）
        Connected,       // 握手完成，进入应用数据阶段
        Closed
    };

    // 协商出的密码套件参数（cipher_suite → AEAD 方法 + 哈希）。
    struct Suite {
        quint16 id = 0;
        int hashLen = 0;   // 32(SHA256) / 48(SHA384)
        int keyLen = 0;    // AEAD key 长度
        bool sha384 = false;
        // AEAD 方法用 coastcore::Aead::Method 的整数值缓存，避免头里 #include Aead.h（.cpp 里转回）。
        int aeadMethod = 0;
    };

    void onTcpConnected();
    void onReadyRead();
    void onSocketError();

    // 组 ClientHello（uTLS 指纹 + 可选 REALITY session_id 认证）。返回完整 handshake 消息字节。
    QByteArray buildClientHello();
    // REALITY：在已 marshal 好的 ClientHello 上做 session_id 认证注入（就地改 sidOffset 处 32B）。
    bool applyRealityAuth(QByteArray &helloMsg, int sidOffset, int randomOffset);

    // 记录层：把一条 TLS 记录（含 5B 头）分派处理。返回 false 表示致命错误（已 emit error）。
    void feedSocket();
    void handleRecord(quint8 type, const QByteArray &fragment);
    void handleHandshakeBytes(const QByteArray &bytes); // 累积并逐条解析 handshake 消息
    void handleServerHello(const QByteArray &body);
    void onFlight2Message(quint8 hsType, const QByteArray &msgFull, const QByteArray &body);
    // REALITY 服务端认证：解析 TLS1.3 Certificate 消息体，取 leaf 证书的 Ed25519 公钥，算
    // HMAC-SHA512(m_authKey, pub) 与证书 Signature 比对。匹配返回 true（真 REALITY 服务端）。
    bool verifyRealityCertificate(const QByteArray &certMsgBody);
    void finishHandshake();

    // 记录层收发（AEAD）。encryptRecord：把 (contentType, plaintext) 封成 application_data 记录字节。
    QByteArray encryptRecord(quint8 contentType, const QByteArray &plaintext, const QByteArray &key,
                             const QByteArray &iv, quint64 &seq);
    // decryptRecord：解开一条 application_data 记录，输出内层 (contentType, plaintext)。失败返回 false。
    bool decryptRecord(const QByteArray &record, const QByteArray &key, const QByteArray &iv,
                       quint64 &seq, quint8 &innerType, QByteArray &plaintext);

    void deriveHandshakeKeys();  // SH 之后：Early/Handshake Secret → c/s hs traffic → key/iv
    void deriveAppKeys();        // server Finished 之后：Master Secret → c/s ap traffic → key/iv
    void sendClientFinished();
    void fail(const QString &reason);

    // —— 成员 ——
    Fingerprint m_fp = Fingerprint::Chrome;
    QTcpSocket *m_sock = nullptr;
    State m_state = State::Idle;

    QString m_host, m_sni;
    quint16 m_port = 0;
    QStringList m_alpn;

    // REALITY
    bool m_reality = false;
    QByteArray m_serverPub;  // 32B
    QByteArray m_shortId;    // 0..8B
    // REALITY 认证密钥：HKDF-SHA256(ecdh, salt=random[:20], info="REALITY") 的 32B 输出。
    // applyRealityAuth 里既用它 AES-256-GCM 密封 session_id，也在 verifyRealityCertificate 里
    // 作为 HMAC-SHA512 的 key 核验服务端证书（xray REALITY：只有握 REALITY 私钥者能派生它）。
    QByteArray m_authKey;
    bool m_realityAuthOk = false; // 服务端证书 AuthKey 核验通过（REALITY 模式下未通过即中止握手）

    // 本次握手的临时 X25519 密钥（key_share 用的就是它；REALITY 的 ECDH 也用它）。
    EVP_PKEY *m_ephemeral = nullptr;
    QByteArray m_ephemeralPub; // 32B，写进 key_share
    QByteArray m_clientRandom; // 32B
    QByteArray m_sessionId;    // 32B（REALITY 模式=认证数据；否则随机）

    // 收发缓冲与状态
    QByteArray m_recvBuf;   // socket 原始字节（按 5B 头切记录）
    QByteArray m_hsMsgBuf;  // 明/密文解出的 handshake 消息累积（按 4B 头切消息）
    QByteArray m_transcript; // 握手 transcript（所有 handshake 消息串接，供 Transcript-Hash）
    QByteArray m_appReadBuf; // 已解密、待上层 readAll 的下行明文

    Suite m_suite;
    QByteArray m_serverKeyShare; // ServerHello 里服务端 x25519 公钥（32B）

    // 密钥调度产物
    QByteArray m_handshakeSecret;
    QByteArray m_masterSecret;
    QByteArray m_cHsSecret, m_sHsSecret;   // client/server handshake traffic secret
    QByteArray m_cHsKey, m_cHsIv, m_sHsKey, m_sHsIv;
    QByteArray m_cApSecret, m_sApSecret;
    QByteArray m_cApKey, m_cApIv, m_sApKey, m_sApIv;
    QByteArray m_transcriptThroughServerFinished; // 派生 app secret 用的 transcript 定格

    // 记录层序号（每个 epoch 独立，从 0 起）
    quint64 m_sHsSeq = 0, m_cHsSeq = 0, m_sApSeq = 0, m_cApSeq = 0;

    bool m_serverFinishedSeen = false;
    bool m_errored = false;
    bool m_readPaused = false; // 应用数据阶段的读暂停（真背压，见 setReadPaused）
};

} // namespace coastcore
