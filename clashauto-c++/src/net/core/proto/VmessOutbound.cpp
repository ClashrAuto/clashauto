#include "VmessOutbound.h"

#include "../ProxyConfig.h"        // ProxyNode
#include "OutboundRegistry.h"      // OutboundRegistry
#include "../crypto/Aead.h"        // coastcore::Aead（AES-128-GCM / ChaCha20-Poly1305 密封/解封）
#include "../transport/TlsClient.h"
#include "../transport/WsClient.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QList>
#include <QMessageAuthenticationCode>
#include <QMetaObject>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>

#include <functional>

#include <openssl/evp.h> // 仅为 AuthID 的单块 AES-128-ECB（Qt 不提供分组密码）

// ============================================================================
// 说明总览（配合 VmessOutbound.h 的注释读）
//
// 本文件三层：
//   1) 匿名命名空间的**纯函数密码学原语**（CRC32 / FNV1a / MD5 / 递归 KDF / AES-ECB / ChaCha 扩key），
//      逐字节对齐 v2ray-core 的 proxy/vmess/aead + encoding，错一字节即静默失败（密文能生成但对端解不开）。
//   2) VmessCodec —— 一条 VMess 会话的**无 I/O** 编解码状态机：造请求头、封上行分块、拆响应头+下行分块。
//      不碰任何 socket / Qt 信号，方便 TCP 与 UDP 两个出站复用同一套加解密。
//   3) VmessOutboundTcp / VmessOutboundUdp —— 把 VmessCodec 接到具体传输（裸 TCP / TlsClient / WsClient）
//      上，背压/信号/零拷贝上行的语义对齐 DirectOutbound。
// ============================================================================

using coastcore::Aead;
using coastcore::TlsClient;
using coastcore::WsClient;

namespace {

// —— 随机字节（mask/key/nonce 都要不可预测，用 system() CSPRNG，同 WsClient）——
QByteArray randomBytes(int n)
{
    if (n <= 0)
        return {};
    QByteArray b(n, Qt::Uninitialized);
    const int words = n / 4;
    if (words > 0)
        QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(b.data()), words);
    for (int i = words * 4; i < n; ++i)
        b[i] = char(quint8(QRandomGenerator::system()->generate() & 0xFF));
    return b;
}

QByteArray md5(const QByteArray &in)
{
    return QCryptographicHash::hash(in, QCryptographicHash::Md5);
}

QByteArray sha256(const QByteArray &in)
{
    return QCryptographicHash::hash(in, QCryptographicHash::Sha256);
}

// FNV-1a 32-bit —— VMess 指令头末 4 字节校验（对前面全部字节）。offset basis / prime 为标准常量。
quint32 fnv1a32(const QByteArray &in)
{
    quint32 h = 2166136261u;
    for (int i = 0; i < in.size(); ++i) {
        h ^= quint8(in.at(i));
        h *= 16777619u;
    }
    return h;
}

// CRC32 (IEEE，反射多项式 0xEDB88320) —— AuthID 前 12 字节的校验。Qt 无现成实现，这里逐位算。
quint32 crc32Ieee(const QByteArray &in)
{
    quint32 crc = 0xFFFFFFFFu;
    for (int i = 0; i < in.size(); ++i) {
        crc ^= quint8(in.at(i));
        for (int b = 0; b < 8; ++b) {
            const quint32 mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// uuid 字符串 → 16 字节。QByteArray::fromHex 会跳过非十六进制字符（连字符/花括号），所以带不带 '-' 都行。
// 非标准「任意字符串 → UUID」映射（v2ray 的 custom id）暂不支持 —— 返回空，调用方据此判失败。
// TODO(真机): 支持 v2ray custom-string→UUIDv5 派生（少数订阅会用）。
QByteArray parseUuid(const QString &s)
{
    const QByteArray raw = QByteArray::fromHex(s.toLatin1());
    return raw.size() == 16 ? raw : QByteArray();
}

// ChaCha20-Poly1305 的 key 需从 16B 扩成 32B：key32 = MD5(k16) || MD5(MD5(k16))（VMess 专用扩展）。
QByteArray chacha20Key(const QByteArray &k16)
{
    QByteArray key = md5(k16); // 前 16
    key += md5(key);           // 后 16 = MD5(MD5(k16))
    return key;                // 32
}

// —— VMess 递归 KDF ——
// v2ray 的 KDF 是「嵌套 HMAC-SHA256」：根用固定 key "VMess AEAD KDF"，之后每个 label 再套一层
// HMAC —— 内层 HMAC 充当外层 HMAC 的「哈希函数」。最终把真正的 key 作为消息喂进最外层，取摘要。
//   KDF(key, "A","B") = HMAC_{H = HMAC_{H = HMAC_{H=SHA256}("VMess AEAD KDF")}("A")}("B") (key)
// SHA-256 的分组是 64 字节、摘要 32 字节，任一层 HMAC 皆如此，故通用 HMAC 用固定 blockSize=64。
using HashFn = std::function<QByteArray(const QByteArray &)>;

// 以任意哈希 H（blockSize=64、任意摘要长）为底的 HMAC：HMAC(K,m)=H((K'^opad) || H((K'^ipad) || m))。
QByteArray genericHmac(const HashFn &H, const QByteArray &key, const QByteArray &msg)
{
    const int B = 64;
    QByteArray k = key;
    if (k.size() > B)
        k = H(k);
    if (k.size() < B)
        k.append(B - k.size(), '\0');
    QByteArray ipad(B, Qt::Uninitialized);
    QByteArray opad(B, Qt::Uninitialized);
    for (int i = 0; i < B; ++i) {
        ipad[i] = char(quint8(k.at(i)) ^ 0x36);
        opad[i] = char(quint8(k.at(i)) ^ 0x5c);
    }
    return H(opad + H(ipad + msg));
}

QByteArray vmessKdf(const QByteArray &key, const QList<QByteArray> &path)
{
    // 根「哈希函数」= HMAC-SHA256(key="VMess AEAD KDF", ·)，用 Qt 的静态 HMAC 直接算，最稳。
    HashFn H = [](const QByteArray &x) {
        return QMessageAuthenticationCode::hash(x, QByteArrayLiteral("VMess AEAD KDF"),
                                                QCryptographicHash::Sha256);
    };
    // 每个 label 再包一层：新哈希函数 H'(x) = HMAC_{底=旧H}(key=label, x)。
    for (const QByteArray &label : path) {
        HashFn prev = H;
        H = [prev, label](const QByteArray &x) { return genericHmac(prev, label, x); };
    }
    return H(key);
}

QByteArray vmessKdf16(const QByteArray &key, const QList<QByteArray> &path)
{
    return vmessKdf(key, path).left(16); // KDF16：取前 16 字节（AES-128 的 key）
}

// AuthID 的单块 AES-128-ECB 加密（16→16）。Qt 无分组密码，落 OpenSSL EVP；关 padding，只跑一整块。
QByteArray aesEcbEncryptBlock(const QByteArray &key16, const QByteArray &in16)
{
    if (key16.size() != 16 || in16.size() != 16)
        return {};
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};
    QByteArray out;
    out.resize(32); // ECB 关 padding 后恒 16，但预留一整块余量给 Final
    auto *outPtr = reinterpret_cast<unsigned char *>(out.data());
    int len = 0;
    int total = 0;
    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                               reinterpret_cast<const unsigned char *>(key16.constData()),
                               nullptr) != 1)
            break;
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        if (EVP_EncryptUpdate(ctx, outPtr, &len,
                              reinterpret_cast<const unsigned char *>(in16.constData()), 16) != 1)
            break;
        total = len;
        if (EVP_EncryptFinal_ex(ctx, outPtr + total, &len) != 1)
            break;
        total += len;
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok || total != 16)
        return {};
    out.resize(16);
    return out;
}

// 把 (port, address) 按 VMess 规则写进 buf：port(2 BE) → atyp(1) → addr。
//   atyp: 1=IPv4(4B) / 2=域名(1B len + bytes) / 3=IPv6(16B)。host 是 IP 字面量则按 v4/v6，否则当域名。
void writeAddressPort(QByteArray &buf, const QString &host, quint16 port)
{
    buf.append(char((port >> 8) & 0xFF));
    buf.append(char(port & 0xFF));

    const QHostAddress addr(host);
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        buf.append(char(1));
        const quint32 v4 = addr.toIPv4Address();
        buf.append(char((v4 >> 24) & 0xFF));
        buf.append(char((v4 >> 16) & 0xFF));
        buf.append(char((v4 >> 8) & 0xFF));
        buf.append(char(v4 & 0xFF));
    } else if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        buf.append(char(3));
        const Q_IPV6ADDR v6 = addr.toIPv6Address();
        buf.append(reinterpret_cast<const char *>(v6.c), 16);
    } else {
        QByteArray d = host.toUtf8();
        if (d.size() > 255)
            d = d.left(255); // 域名超 255 极罕见；截断（TODO: 理论上应报错）
        buf.append(char(2));
        buf.append(char(quint8(d.size())));
        buf.append(d);
    }
}

// 把指令头明文封成 VMessAEAD 请求头线格式：AuthID(16) || AEAD(len,2B)=18 || connNonce(8) || AEAD(header)。
// 两段 AEAD 均 AES-128-GCM，key/nonce 由 KDF(cmdKey, <label> || AuthID || connNonce) 派生，aad=AuthID。
QByteArray sealVmessAeadHeader(const QByteArray &cmdKey, const QByteArray &header)
{
    // —— AuthID：time8(BE) || rand4 || CRC32(前12)(BE)，AES-128-ECB 加密一整块 ——
    const QByteArray authKey =
        vmessKdf16(cmdKey, {QByteArrayLiteral("AES Auth ID Encryption")});
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QByteArray idp;
    for (int i = 7; i >= 0; --i)
        idp.append(char(quint8((now >> (i * 8)) & 0xFF)));
    idp.append(randomBytes(4));
    const quint32 crc = crc32Ieee(idp);
    idp.append(char((crc >> 24) & 0xFF));
    idp.append(char((crc >> 16) & 0xFF));
    idp.append(char((crc >> 8) & 0xFF));
    idp.append(char(crc & 0xFF));
    const QByteArray authId = aesEcbEncryptBlock(authKey, idp); // 16

    const QByteArray connNonce = randomBytes(8);

    // —— 长度段 AEAD ——（明文 = header 长度 2B BE）
    const QByteArray lenKey = vmessKdf16(
        cmdKey, {QByteArrayLiteral("VMess Header AEAD Key_Length"), authId, connNonce});
    const QByteArray lenNonce =
        vmessKdf(cmdKey, {QByteArrayLiteral("VMess Header AEAD Nonce_Length"), authId, connNonce})
            .left(12);
    QByteArray lenPlain;
    const quint16 hl = quint16(header.size());
    lenPlain.append(char((hl >> 8) & 0xFF));
    lenPlain.append(char(hl & 0xFF));
    const QByteArray lenSealed =
        Aead::seal(Aead::Method::Aes128Gcm, lenKey, lenNonce, authId, lenPlain);

    // —— 载荷段 AEAD ——（明文 = 指令头本身）
    const QByteArray payKey =
        vmessKdf16(cmdKey, {QByteArrayLiteral("VMess Header AEAD Key"), authId, connNonce});
    const QByteArray payNonce =
        vmessKdf(cmdKey, {QByteArrayLiteral("VMess Header AEAD Nonce"), authId, connNonce})
            .left(12);
    const QByteArray paySealed =
        Aead::seal(Aead::Method::Aes128Gcm, payKey, payNonce, authId, header);

    return authId + lenSealed + connNonce + paySealed;
}

// ============================ VmessCodec（无 I/O 编解码状态机）============================
class VmessCodec
{
public:
    bool ok = false; // uuid 解析成功 = 可用

    // —— 从 ProxyNode 初始化：算 cmdKey、随机 body key/iv、选 security、预派生上/下行 body 密钥 ——
    void init(const ProxyNode &n)
    {
        const QByteArray uuid = parseUuid(n.uuid);
        if (uuid.size() != 16) {
            ok = false;
            return;
        }
        m_cmdKey = md5(uuid + QByteArrayLiteral("c48619fe-8f02-49e0-b9e9-edf763e17e21"));

        const QString c = n.cipher.trimmed().toLower();
        if (c == QLatin1String("chacha20-poly1305") ||
            c == QLatin1String("chacha20-ietf-poly1305")) {
            m_sec = Aead::Method::ChaCha20Poly1305;
            m_secType = 0x04;
        } else {
            // aes-128-gcm / auto / 空 一律用 aes-128-gcm。
            // TODO(真机): security "none"(0x05,无加密只鉴权) 未实现；"auto" 恒选 aes-128-gcm。
            m_sec = Aead::Method::Aes128Gcm;
            m_secType = 0x03;
        }

        m_reqBodyKey = randomBytes(16);
        m_reqBodyIV = randomBytes(16);
        m_responseAuth = quint8(randomBytes(1).at(0));

        // 上行 body：aes 用 16B key、chacha 扩成 32B；nonce 用 reqBodyIV 派生。
        m_upKey = (m_sec == Aead::Method::ChaCha20Poly1305) ? chacha20Key(m_reqBodyKey) : m_reqBodyKey;
        m_upIV = m_reqBodyIV;

        // 响应 body：responseKey/IV = SHA256(reqBodyKey/IV)[:16]。
        m_respBodyKey = sha256(m_reqBodyKey).left(16);
        m_respBodyIV = sha256(m_reqBodyIV).left(16);
        m_downKey = (m_sec == Aead::Method::ChaCha20Poly1305) ? chacha20Key(m_respBodyKey)
                                                              : m_respBodyKey;
        m_downIV = m_respBodyIV;

        ok = true;
    }

    // 造完整的 VMessAEAD 请求头线字节（含指令头 + AEAD 密封）。cmd: 0x01 tcp / 0x02 udp。
    QByteArray sealRequestHeader(quint8 cmd, const QString &host, quint16 port) const
    {
        return sealVmessAeadHeader(m_cmdKey, buildInstructionHeader(cmd, host, port));
    }

    // 封一个上行 body 分块：[encLen 2B BE][AEAD(chunk)]，encLen = 密文+tag 长度。counter 递增。
    // 基础版**不做** chunk-masking（option 未置该位，故 encLen 明文写）。TODO(真机): shake128 掩码。
    QByteArray sealBodyChunk(const QByteArray &plain)
    {
        QByteArray nonce;
        nonce.append(char((m_upCounter >> 8) & 0xFF));
        nonce.append(char(m_upCounter & 0xFF));
        nonce.append(m_upIV.mid(2, 10)); // 共 12 字节 = counter(2) || iv[2:12]
        const QByteArray cipher = Aead::seal(m_sec, m_upKey, nonce, QByteArray(), plain);
        ++m_upCounter;
        const quint16 L = quint16(cipher.size());
        QByteArray out;
        out.append(char((L >> 8) & 0xFF));
        out.append(char(L & 0xFF));
        out.append(cipher);
        return out;
    }

    // 喂入新到达的下行字节，推进「先解响应头、再拆 body 分块」的状态机。
    //   *out  追加解出的每个 body 分块明文（TCP 拼流 / UDP 每块一份 datagram）
    //   *eof  收到空分块（对端结束发送）时置真
    //   *err  解密/鉴权/协议错时置错误串
    // 返回 false 表示致命错（调用方 fail）；true 表示正常（可能还需更多字节，属正常等待）。
    bool feed(const QByteArray &in, QList<QByteArray> *out, bool *eof, QString *err)
    {
        m_recvBuf += in;

        if (!m_respHeaderParsed) {
            const QByteArray rlk =
                vmessKdf16(m_respBodyKey, {QByteArrayLiteral("AEAD Resp Header Len Key")});
            const QByteArray rln =
                vmessKdf(m_respBodyIV, {QByteArrayLiteral("AEAD Resp Header Len IV")}).left(12);
            if (m_recvBuf.size() < 18)
                return true; // 长度段(2+16)还没到齐
            QByteArray lenPlain;
            if (!Aead::open(Aead::Method::Aes128Gcm, rlk, rln, QByteArray(), m_recvBuf.left(18),
                            &lenPlain) ||
                lenPlain.size() < 2) {
                *err = QStringLiteral("vmess: 响应长度头解密失败");
                return false;
            }
            const quint16 respLen = (quint16(quint8(lenPlain.at(0))) << 8) | quint8(lenPlain.at(1));
            const int need = 18 + int(respLen) + 16;
            if (m_recvBuf.size() < need)
                return true; // 载荷段还没到齐
            const QByteArray rpk =
                vmessKdf16(m_respBodyKey, {QByteArrayLiteral("AEAD Resp Header Key")});
            const QByteArray rpn =
                vmessKdf(m_respBodyIV, {QByteArrayLiteral("AEAD Resp Header IV")}).left(12);
            QByteArray hdr;
            if (!Aead::open(Aead::Method::Aes128Gcm, rpk, rpn, QByteArray(),
                            m_recvBuf.mid(18, int(respLen) + 16), &hdr) ||
                hdr.isEmpty()) {
                *err = QStringLiteral("vmess: 响应头解密失败");
                return false;
            }
            // hdr[0] 必须回显我们发出的 responseAuth，否则不是我们的服务端 / 密钥不符。
            if (quint8(hdr.at(0)) != m_responseAuth) {
                *err = QStringLiteral("vmess: 响应鉴权字节不匹配");
                return false;
            }
            // hdr 余下字节（option / command / 动态端口指令）本实现不消费。TODO(真机): 处理动态端口。
            m_recvBuf.remove(0, need);
            m_respHeaderParsed = true;
        }

        // —— body 分块：[len 2B][cipher+tag] ——（无 masking，len 直接读）
        for (;;) {
            if (m_recvBuf.size() < 2)
                break;
            const quint16 L = (quint16(quint8(m_recvBuf.at(0))) << 8) | quint8(m_recvBuf.at(1));
            if (L < 16 || L > 0x3FFF + 16) { // 至少要含 16B tag；上限 = 最大明文分块 + tag
                *err = QStringLiteral("vmess: 下行分块长度非法");
                return false;
            }
            if (m_recvBuf.size() < 2 + int(L))
                break; // 分块没收全
            const QByteArray cipher = m_recvBuf.mid(2, int(L));
            QByteArray nonce;
            nonce.append(char((m_downCounter >> 8) & 0xFF));
            nonce.append(char(m_downCounter & 0xFF));
            nonce.append(m_downIV.mid(2, 10));
            QByteArray plain;
            if (!Aead::open(m_sec, m_downKey, nonce, QByteArray(), cipher, &plain)) {
                *err = QStringLiteral("vmess: 下行分块解密失败");
                return false;
            }
            ++m_downCounter;
            m_recvBuf.remove(0, 2 + int(L));
            if (plain.isEmpty()) { // 空分块 = 对端结束发送（EOF 标记）
                *eof = true;
                break;
            }
            out->append(plain);
        }
        return true;
    }

private:
    // 组装指令头明文（VMessAEAD 密封前的那段），逐字段对齐 v2ray 的 EncodeRequestHeader。
    QByteArray buildInstructionHeader(quint8 cmd, const QString &host, quint16 port) const
    {
        QByteArray h;
        h.append(char(1));            // 版本 = 1
        h.append(m_reqBodyIV);        // 16
        h.append(m_reqBodyKey);       // 16
        h.append(char(m_responseAuth)); // 1
        // option：仅置 ChunkStream(0x01)。TODO(真机): 现代客户端默认还会置 ChunkMasking(0x04) +
        //   GlobalPadding(0x08) + AuthenticatedLength(0x10)，本基础版都不置（服务端读 option 自适应，
        //   非 masking 走 PlainChunkSizeParser、counter nonce，正好与本实现一致）。
        h.append(char(0x01));
        // 高 4 位随机 padding 长度 0..15，低 4 位 security(0x03 aes-128-gcm / 0x04 chacha20)。
        const quint8 padLen = quint8(randomBytes(1).at(0)) & 0x0F;
        h.append(char((padLen << 4) | m_secType));
        h.append(char(0));            // 保留 = 0
        h.append(char(cmd));          // 0x01 tcp / 0x02 udp
        writeAddressPort(h, host, port); // port(2 BE) + atyp + addr
        if (padLen > 0)
            h.append(randomBytes(padLen)); // padding（内容随机，参与 FNV 校验）
        const quint32 f = fnv1a32(h);      // FNV1a-32 over 上面全部字节
        h.append(char((f >> 24) & 0xFF));
        h.append(char((f >> 16) & 0xFF));
        h.append(char((f >> 8) & 0xFF));
        h.append(char(f & 0xFF));
        return h;
    }

    Aead::Method m_sec = Aead::Method::Aes128Gcm;
    quint8 m_secType = 0x03;
    QByteArray m_cmdKey;
    QByteArray m_reqBodyKey;
    QByteArray m_reqBodyIV;
    quint8 m_responseAuth = 0;

    QByteArray m_upKey, m_upIV;     // 上行 body AEAD key（16/32）+ nonce 派生用 iv
    QByteArray m_downKey, m_downIV; // 下行 body
    QByteArray m_respBodyKey, m_respBodyIV; // 响应头 AEAD 派生用
    quint16 m_upCounter = 0;
    quint16 m_downCounter = 0;

    bool m_respHeaderParsed = false;
    QByteArray m_recvBuf; // 累积的下行密文（可能半个响应头/半个分块，凑够再解）
};

} // namespace

// ============================================================================
// VmessOutboundTcp
// ============================================================================
class VmessOutboundTcp::Priv
{
public:
    explicit Priv(VmessOutboundTcp *owner) : q(owner) {}

    VmessOutboundTcp *q = nullptr;
    VmessCodec codec;

    // —— 节点/目标 ——
    QString serverHost;
    quint16 serverPort = 0;
    QString network; // "ws" 或空(=tcp)
    bool tlsOn = false;
    bool skipVerify = false;
    QString sni, wsHost, wsPath;
    QString dstHost;
    quint16 dstPort = 0;

    // —— 传输（三选一）——
    QTcpSocket *raw = nullptr;
    TlsClient *tls = nullptr;
    WsClient *ws = nullptr;

    // —— 状态 ——
    bool established = false;
    bool headerSent = false;
    bool closedEmitted = false;
    bool readPaused = false;
    QByteArray pending;      // established 前的上行 app 字节
    QByteArray heldInbound;  // ws 专用：暂停期间被推来的下行字节（裸 TCP/TLS 走 socket 缓冲，不用它）

    // —— 传输无关封装 ——
    void transportWrite(const QByteArray &b)
    {
        if (ws)
            ws->write(b);
        else if (tls)
            tls->write(b);
        else if (raw)
            raw->write(b);
    }
    qint64 transportBytesToWrite() const
    {
        if (ws)
            return ws->bytesToWrite();
        if (tls)
            return tls->bytesToWrite();
        if (raw)
            return raw->bytesToWrite();
        return 0;
    }
    QByteArray transportReadAll()
    {
        if (tls)
            return tls->readAll();
        if (raw)
            return raw->readAll();
        return {}; // ws 是推模式，不在此读
    }
    void transportAbort()
    {
        if (ws)
            ws->close();
        if (tls)
            tls->abort();
        if (raw)
            raw->abort();
    }

    void fail(const QString &reason)
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->failed(reason);
        transportAbort();
    }
    void emitClosed()
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->closed();
    }

    // 上行：把 app 字节切成 <=0x3FFF 的分块封出去，并（异步）回报已消费的 app 字节数。
    void sendAppBytes(const QByteArray &app)
    {
        int off = 0;
        while (off < app.size()) {
            const int n = qMin(app.size() - off, 0x3FFF);
            transportWrite(codec.sealBodyChunk(app.mid(off, n)));
            off += n;
        }
        if (app.size() > 0) {
            const qint64 nApp = app.size();
            // 异步回报，避免在 write() 内同步 emit 触发上层再次 write 的重入（对齐 DirectOutbound 的
            // 「排队补读」思路）。语义：字节已交给传输的写缓冲，可据此归还 lwIP 接收窗口。
            QMetaObject::invokeMethod(
                q, [this, nApp] { emit q->upstreamBytesWritten(nApp); }, Qt::QueuedConnection);
        }
    }

    void onTransportConnected()
    {
        if (headerSent)
            return;
        headerSent = true;
        transportWrite(codec.sealRequestHeader(0x01, dstHost, dstPort)); // cmd=0x01 TCP
        established = true;
        emit q->established();
        if (!pending.isEmpty()) {
            const QByteArray p = pending;
            pending.clear();
            sendAppBytes(p);
        }
    }

    void feedTransport(const QByteArray &bytes)
    {
        if (closedEmitted)
            return;
        QList<QByteArray> payloads;
        bool eof = false;
        QString err;
        if (!codec.feed(bytes, &payloads, &eof, &err)) {
            fail(err);
            return;
        }
        for (const QByteArray &p : payloads)
            emit q->dataReceived(p);
        if (eof)
            emitClosed();
    }

    void startWs(QAbstractSocket *sock)
    {
        ws = new WsClient(q);
        QObject::connect(ws, &WsClient::established, q, [this] { onTransportConnected(); });
        QObject::connect(ws, &WsClient::dataReceived, q, [this](const QByteArray &b) {
            if (readPaused) {
                heldInbound += b; // 暂停：先攒着（ws 是推模式，无法留在内核缓冲）
                return;
            }
            feedTransport(b);
        });
        QObject::connect(ws, &WsClient::closed, q,
                         [this] { established ? emitClosed() : fail(QStringLiteral("ws 关闭")); });
        QObject::connect(ws, &WsClient::errorOccurred, q,
                         [this](const QString &r) { established ? emitClosed() : fail(r); });
        ws->start(sock, wsHost.isEmpty() ? serverHost : wsHost, wsPath, serverPort);
    }

    void setupTransport()
    {
        const bool useWs = (network == QLatin1String("ws"));
        if (useWs && tlsOn) {
            tls = new TlsClient(q);
            QObject::connect(tls, &TlsClient::connected, q, [this] { startWs(tls->socket()); });
            QObject::connect(tls, &TlsClient::errorOccurred, q,
                             [this](const QString &r) { established ? emitClosed() : fail(r); });
            tls->connectToHost(serverHost, serverPort, sni.isEmpty() ? serverHost : sni,
                               {QByteArrayLiteral("http/1.1")}, skipVerify);
        } else if (useWs) {
            raw = new QTcpSocket(q);
            raw->setProxy(QNetworkProxy::NoProxy);
            QObject::connect(raw, &QTcpSocket::connected, q, [this] {
                raw->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                startWs(raw);
            });
            QObject::connect(raw, &QAbstractSocket::errorOccurred, q,
                             [this](QAbstractSocket::SocketError) {
                                 established ? emitClosed() : fail(raw->errorString());
                             });
            raw->connectToHost(serverHost, serverPort);
        } else if (tlsOn) {
            tls = new TlsClient(q);
            tls->setReadBufferSize(64 * 1024);
            QObject::connect(tls, &TlsClient::connected, q, [this] { onTransportConnected(); });
            QObject::connect(tls, &TlsClient::readyRead, q, [this] {
                if (readPaused)
                    return;
                feedTransport(tls->readAll());
            });
            QObject::connect(tls, &TlsClient::disconnected, q, [this] {
                established ? emitClosed() : fail(QStringLiteral("tls 关闭"));
            });
            QObject::connect(tls, &TlsClient::errorOccurred, q,
                             [this](const QString &r) { established ? emitClosed() : fail(r); });
            tls->connectToHost(serverHost, serverPort, sni.isEmpty() ? serverHost : sni, {},
                               skipVerify);
        } else {
            raw = new QTcpSocket(q);
            raw->setProxy(QNetworkProxy::NoProxy);
            raw->setReadBufferSize(64 * 1024);
            QObject::connect(raw, &QTcpSocket::connected, q, [this] {
                raw->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                onTransportConnected();
            });
            QObject::connect(raw, &QTcpSocket::readyRead, q, [this] {
                if (readPaused)
                    return;
                feedTransport(raw->readAll());
            });
            QObject::connect(raw, &QTcpSocket::disconnected, q, [this] { emitClosed(); });
            QObject::connect(raw, &QAbstractSocket::errorOccurred, q,
                             [this](QAbstractSocket::SocketError) {
                                 established ? emitClosed() : fail(raw->errorString());
                             });
            raw->connectToHost(serverHost, serverPort);
        }
    }
};

VmessOutboundTcp::VmessOutboundTcp(const ProxyNode &node, QObject *parent)
    : IOutboundTcp(parent), d(new Priv(this))
{
    d->codec.init(node);
    d->serverHost = node.server;
    d->serverPort = node.port;
    d->network = node.network.trimmed().toLower();
    d->tlsOn = node.tls;
    d->skipVerify = node.skipCertVerify;
    d->sni = node.sni;
    d->wsHost = node.wsHost;
    d->wsPath = node.wsPath;
}

VmessOutboundTcp::~VmessOutboundTcp()
{
    delete d;
}

void VmessOutboundTcp::connectTo(const QString &dstHost, quint16 dstPort, const QString & /*user*/)
{
    // user（每设备 mihomo 身份）VMess 协议层用不到（单一 uuid）——忽略。TODO(真机): 每设备策略。
    if (!d->codec.ok) {
        QTimer::singleShot(0, this,
                           [this] { d->fail(QStringLiteral("vmess: uuid 无效（需 16 字节 UUID）")); });
        return;
    }
    d->dstHost = dstHost;
    d->dstPort = dstPort;
    d->setupTransport();
}

void VmessOutboundTcp::write(const QByteArray &data)
{
    if (d->established)
        d->sendAppBytes(data);
    else
        d->pending += data; // established 前缓冲（隐式共享，零拷贝）
}

void VmessOutboundTcp::write(const char *data, qsizetype size)
{
    if (!data || size <= 0)
        return;
    if (d->established) {
        // established：sealBodyChunk 内部读 plaintext 到 EVP 时已拷走，fromRawData 只需活到本次调用，安全。
        d->sendAppBytes(QByteArray::fromRawData(data, size));
    } else {
        d->pending.append(data, size); // 未 established：字节要留到握手后，必须深拷贝
    }
}

void VmessOutboundTcp::closeTunnel()
{
    d->transportAbort();
    d->emitClosed();
}

bool VmessOutboundTcp::isEstablished() const
{
    return d->established;
}

qint64 VmessOutboundTcp::bytesToWrite() const
{
    // pending（未 established 的上行 app 字节）+ 传输层写缓冲。分块 AEAD 的 ~18B/块 开销未计入，
    // 属可接受的近似（略低估，不至于让上层误判「已排空」而无闸）。
    return qint64(d->pending.size()) + d->transportBytesToWrite();
}

void VmessOutboundTcp::setReadPaused(bool paused)
{
    if (d->readPaused == paused)
        return;
    d->readPaused = paused;
    if (paused)
        return;
    // 恢复：排队补读（同 DirectOutbound —— 补读会同步 emit dataReceived，上层槽可能当场拆掉本对象，
    // 故延到事件循环里做；对象若已析构，排队事件随之丢弃）。
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (d->readPaused)
                return;
            if (d->ws) {
                if (!d->heldInbound.isEmpty()) {
                    const QByteArray b = d->heldInbound;
                    d->heldInbound.clear();
                    d->feedTransport(b);
                }
            } else {
                const QByteArray b = d->transportReadAll();
                if (!b.isEmpty())
                    d->feedTransport(b);
            }
        },
        Qt::QueuedConnection);
}

bool VmessOutboundTcp::isReadPaused() const
{
    return d->readPaused;
}

// ============================================================================
// VmessOutboundUdp —— VMess UDP（cmd=0x02），隧道跑在到服务端的 TCP/TLS 之上。
//
// ★ 已知局限（TODO 真机）：基础 VMess UDP 的目标地址写死在指令头里，天然是**单目标**。IOutboundUdp
//   的 sendTo 却可发往任意目标。本实现以**第一个** sendTo 的目标建立会话；之后发往**同一**目标 OK，
//   发往**不同**目标仍从同一会话走（服务端会按头里的目标投递 → 目的错）。多目标需 packet-addr 选项或
//   每目标一条会话，此处标 TODO。DNS（53）等单目标场景可用。
// ============================================================================
class VmessOutboundUdp::Priv
{
public:
    explicit Priv(VmessOutboundUdp *owner) : q(owner) {}

    VmessOutboundUdp *q = nullptr;
    VmessCodec codec;

    QString serverHost;
    quint16 serverPort = 0;
    QString network;
    bool tlsOn = false;
    bool skipVerify = false;
    QString sni, wsHost, wsPath;

    QTcpSocket *raw = nullptr;
    TlsClient *tls = nullptr;
    WsClient *ws = nullptr;

    bool ready = false;
    bool started = false;      // 首个 sendTo 已触发建连
    bool established = false;   // 请求头已发
    bool headerSent = false;
    bool closedEmitted = false;

    QHostAddress targetIp; // 会话绑定的目标（首个 sendTo）
    quint16 targetPort = 0;
    QList<QByteArray> pendingDatagrams; // established 前排队的上行 datagram

    void transportWrite(const QByteArray &b)
    {
        if (ws)
            ws->write(b);
        else if (tls)
            tls->write(b);
        else if (raw)
            raw->write(b);
    }
    void transportAbort()
    {
        if (ws)
            ws->close();
        if (tls)
            tls->abort();
        if (raw)
            raw->abort();
    }

    void fail(const QString &reason)
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->failed(reason);
        transportAbort();
    }
    void emitClosed()
    {
        if (closedEmitted)
            return;
        closedEmitted = true;
        emit q->closed();
    }

    // 一个 datagram = 一个 body 分块。>0x3FFF 的极大 datagram 会被切成多块（=多个 datagram，语义不对），
    // 但 UDP 载荷极少超 16KB，标 TODO。
    void sendDatagram(const QByteArray &payload)
    {
        int off = 0;
        if (payload.isEmpty()) {
            transportWrite(codec.sealBodyChunk(QByteArray()));
            return;
        }
        while (off < payload.size()) {
            const int n = qMin(payload.size() - off, 0x3FFF);
            transportWrite(codec.sealBodyChunk(payload.mid(off, n)));
            off += n;
        }
    }

    void onTransportConnected()
    {
        if (headerSent)
            return;
        headerSent = true;
        transportWrite(codec.sealRequestHeader(0x02, targetIp.toString(), targetPort)); // cmd=0x02
        established = true;
        for (const QByteArray &dg : pendingDatagrams)
            sendDatagram(dg);
        pendingDatagrams.clear();
    }

    void feedTransport(const QByteArray &bytes)
    {
        if (closedEmitted)
            return;
        QList<QByteArray> payloads;
        bool eof = false;
        QString err;
        if (!codec.feed(bytes, &payloads, &eof, &err)) {
            fail(err);
            return;
        }
        for (const QByteArray &p : payloads)
            emit q->datagramReceived(targetIp, targetPort, p); // 每块一份 datagram，源=会话目标
        if (eof)
            emitClosed();
    }

    void startWs(QAbstractSocket *sock)
    {
        ws = new WsClient(q);
        QObject::connect(ws, &WsClient::established, q, [this] { onTransportConnected(); });
        QObject::connect(ws, &WsClient::dataReceived, q,
                         [this](const QByteArray &b) { feedTransport(b); });
        QObject::connect(ws, &WsClient::closed, q,
                         [this] { established ? emitClosed() : fail(QStringLiteral("ws 关闭")); });
        QObject::connect(ws, &WsClient::errorOccurred, q,
                         [this](const QString &r) { established ? emitClosed() : fail(r); });
        ws->start(sock, wsHost.isEmpty() ? serverHost : wsHost, wsPath, serverPort);
    }

    void setupTransport()
    {
        const bool useWs = (network == QLatin1String("ws"));
        if (useWs && tlsOn) {
            tls = new TlsClient(q);
            QObject::connect(tls, &TlsClient::connected, q, [this] { startWs(tls->socket()); });
            QObject::connect(tls, &TlsClient::errorOccurred, q,
                             [this](const QString &r) { established ? emitClosed() : fail(r); });
            tls->connectToHost(serverHost, serverPort, sni.isEmpty() ? serverHost : sni,
                               {QByteArrayLiteral("http/1.1")}, skipVerify);
        } else if (useWs) {
            raw = new QTcpSocket(q);
            raw->setProxy(QNetworkProxy::NoProxy);
            QObject::connect(raw, &QTcpSocket::connected, q, [this] {
                raw->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                startWs(raw);
            });
            QObject::connect(raw, &QAbstractSocket::errorOccurred, q,
                             [this](QAbstractSocket::SocketError) {
                                 established ? emitClosed() : fail(raw->errorString());
                             });
            raw->connectToHost(serverHost, serverPort);
        } else if (tlsOn) {
            tls = new TlsClient(q);
            QObject::connect(tls, &TlsClient::connected, q, [this] { onTransportConnected(); });
            QObject::connect(tls, &TlsClient::readyRead, q, [this] { feedTransport(tls->readAll()); });
            QObject::connect(tls, &TlsClient::disconnected, q, [this] {
                established ? emitClosed() : fail(QStringLiteral("tls 关闭"));
            });
            QObject::connect(tls, &TlsClient::errorOccurred, q,
                             [this](const QString &r) { established ? emitClosed() : fail(r); });
            tls->connectToHost(serverHost, serverPort, sni.isEmpty() ? serverHost : sni, {},
                               skipVerify);
        } else {
            raw = new QTcpSocket(q);
            raw->setProxy(QNetworkProxy::NoProxy);
            QObject::connect(raw, &QTcpSocket::connected, q, [this] {
                raw->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                onTransportConnected();
            });
            QObject::connect(raw, &QTcpSocket::readyRead, q,
                             [this] { feedTransport(raw->readAll()); });
            QObject::connect(raw, &QTcpSocket::disconnected, q, [this] { emitClosed(); });
            QObject::connect(raw, &QAbstractSocket::errorOccurred, q,
                             [this](QAbstractSocket::SocketError) {
                                 established ? emitClosed() : fail(raw->errorString());
                             });
            raw->connectToHost(serverHost, serverPort);
        }
    }
};

VmessOutboundUdp::VmessOutboundUdp(const ProxyNode &node, QObject *parent)
    : IOutboundUdp(parent), d(new Priv(this))
{
    d->codec.init(node);
    d->serverHost = node.server;
    d->serverPort = node.port;
    d->network = node.network.trimmed().toLower();
    d->tlsOn = node.tls;
    d->skipVerify = node.skipCertVerify;
    d->sni = node.sni;
    d->wsHost = node.wsHost;
    d->wsPath = node.wsPath;
}

VmessOutboundUdp::~VmessOutboundUdp()
{
    delete d;
}

void VmessOutboundUdp::associate(const QString & /*user*/)
{
    // 建连要等第一个 sendTo 才知道目标（VMess UDP 目标写死在头里）。ready() 必须异步发（同 DirectOutbound：
    // 上层是先 createUdp→connect ready→associate 的，同步 emit 会丢）。
    QTimer::singleShot(0, this, [this] {
        if (d->closedEmitted)
            return;
        if (!d->codec.ok) {
            d->fail(QStringLiteral("vmess: uuid 无效（需 16 字节 UUID）"));
            return;
        }
        d->ready = true;
        emit ready();
    });
}

void VmessOutboundUdp::sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload)
{
    if (!d->ready || d->closedEmitted)
        return;
    if (!d->started) {
        d->started = true;
        d->targetIp = dstIp;
        d->targetPort = dstPort;
        d->pendingDatagrams.append(payload);
        d->setupTransport();
        return;
    }
    // TODO(真机): dstIp/dstPort != targetIp/targetPort 时本会话无法正确投递（单目标限制），此处仍按会话
    //   目标发出。多目标需 packet-addr 或每目标一条会话。
    if (d->established)
        d->sendDatagram(payload);
    else
        d->pendingDatagrams.append(payload);
}

void VmessOutboundUdp::closeSession()
{
    d->transportAbort();
    d->emitClosed();
}

bool VmessOutboundUdp::isReady() const
{
    return d->ready;
}

// ============================================================================
// 注册
// ============================================================================
void registerVmess(OutboundRegistry &reg)
{
    reg.registerProto(
        QStringLiteral("vmess"),
        [](const ProxyNode &n, QObject *p) -> IOutboundTcp * { return new VmessOutboundTcp(n, p); },
        [](const ProxyNode &n, QObject *p) -> IOutboundUdp * { return new VmessOutboundUdp(n, p); });
}
