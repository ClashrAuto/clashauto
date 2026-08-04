#include "Aead.h"

#include <openssl/evp.h>

// —— 实现说明 ——
// 全部走 EVP 的「AEAD 通用」调用序列（OpenSSL 1.1 起对 GCM 与 ChaCha20-Poly1305 完全一致，只有
// EVP_CIPHER* 不同），所以 seal/open 各写一份、按 method 选 cipher 即可，不必为每种密码分叉：
//   Init(cipher) → CTRL(SET_IVLEN) → Init(key,iv) → [Update(aad)] → Update(data) → Final → CTRL(GET/SET_TAG)
// 关键点：
//  · 先 EncryptInit(cipher, NULL, NULL, NULL) 只设 cipher，再 CTRL(SET_IVLEN) 定 IV 长（默认 GCM 是
//    12 恰好够，但 chacha20-poly1305 的默认也是 12；显式设一遍最稳，未来换 nonce 长也不用改这里），
//    最后 EncryptInit(NULL, NULL, key, iv) 才真正灌 key/iv —— 这个「分两次 Init」是 EVP AEAD 的规定
//    姿势，合成一步会导致 IVLEN 设置晚于 key 而被忽略。
//  · GCM/ChaCha20-Poly1305 都是流式（无分组填充），EncryptFinal 不产出额外字节，但仍要调以触发
//    tag 生成。
//  · aad 用「output 传 NULL」的 Update 喂入（只参与认证不产密文）。
//  · open() 里 DecryptFinal 的返回值 <=0 即认证失败 —— 这是 AEAD 唯一的验签闸门，绝不能忽略。

namespace coastcore {

namespace {

// method → 对应的 EVP_CIPHER*（静态单例，libcrypto 内部管理，勿释放）。Invalid → nullptr。
const EVP_CIPHER *cipherFor(Aead::Method m)
{
    switch (m) {
    case Aead::Method::Aes128Gcm:
        return EVP_aes_128_gcm();
    case Aead::Method::Aes256Gcm:
        return EVP_aes_256_gcm();
    case Aead::Method::ChaCha20Poly1305:
        return EVP_chacha20_poly1305();
    default:
        return nullptr;
    }
}

} // namespace

Aead::Method Aead::methodFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QStringLiteral("aes-128-gcm"))
        return Method::Aes128Gcm;
    if (n == QStringLiteral("aes-256-gcm"))
        return Method::Aes256Gcm;
    // 裸 "chacha20-poly1305" 在 SS/Clash 生态里也指 IETF(96-bit nonce) 变体，与带 -ietf- 的等价。
    if (n == QStringLiteral("chacha20-ietf-poly1305") || n == QStringLiteral("chacha20-poly1305"))
        return Method::ChaCha20Poly1305;
    return Method::Invalid;
}

int Aead::keySize(Method m)
{
    switch (m) {
    case Method::Aes128Gcm:
        return 16;
    case Method::Aes256Gcm:
    case Method::ChaCha20Poly1305:
        return 32;
    default:
        return 0;
    }
}

QByteArray Aead::seal(Method m, const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                      const QByteArray &plaintext)
{
    const EVP_CIPHER *cipher = cipherFor(m);
    if (!cipher || key.size() != keySize(m) || nonce.size() != nonceSize())
        return {}; // 参数不自洽：拒绝，绝不用错长度的 key/iv 硬跑

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};

    // 产物先按「明文长 + tag」预留；GCM/ChaCha20 密文与明文等长，末尾 16 字节留给 tag。
    QByteArray out;
    out.resize(plaintext.size() + tagSize());
    auto *outPtr = reinterpret_cast<unsigned char *>(out.data());
    int len = 0;
    int cipherLen = 0;
    bool ok = false;

    do {
        if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce.size(), nullptr) != 1)
            break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(nonce.constData())) != 1)
            break;
        // AAD：output 传 NULL 表示只喂认证、不产密文。空 aad 跳过（喂 0 长度也无害，但省一步）。
        if (!aad.isEmpty()) {
            int aadLen = 0;
            if (EVP_EncryptUpdate(ctx, nullptr, &aadLen,
                                  reinterpret_cast<const unsigned char *>(aad.constData()),
                                  aad.size()) != 1)
                break;
        }
        if (!plaintext.isEmpty()) {
            if (EVP_EncryptUpdate(ctx, outPtr, &len,
                                  reinterpret_cast<const unsigned char *>(plaintext.constData()),
                                  plaintext.size()) != 1)
                break;
            cipherLen = len;
        }
        // Final：流式密码不产额外字节，但必须调以定格 tag。
        if (EVP_EncryptFinal_ex(ctx, outPtr + cipherLen, &len) != 1)
            break;
        cipherLen += len;
        // 取出 16 字节 tag，直接写到密文之后（out 已预留）。
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, tagSize(), outPtr + cipherLen) != 1)
            break;
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    out.resize(cipherLen + tagSize()); // 理论上恒 = plaintext.size()+16，稳妥起见按实际截
    return out;
}

bool Aead::open(Method m, const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                const QByteArray &ciphertextWithTag, QByteArray *out)
{
    if (out)
        out->clear();
    const EVP_CIPHER *cipher = cipherFor(m);
    if (!cipher || !out || key.size() != keySize(m) || nonce.size() != nonceSize())
        return false;
    if (ciphertextWithTag.size() < tagSize())
        return false; // 连 tag 都不够，必假

    const int cipherLen = ciphertextWithTag.size() - tagSize();
    const auto *cipherPtr = reinterpret_cast<const unsigned char *>(ciphertextWithTag.constData());
    const auto *tagPtr = cipherPtr + cipherLen;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;

    QByteArray plain;
    plain.resize(cipherLen);
    auto *plainPtr = reinterpret_cast<unsigned char *>(plain.data());
    int len = 0;
    int plainLen = 0;
    bool ok = false;

    do {
        if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce.size(), nullptr) != 1)
            break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(nonce.constData())) != 1)
            break;
        if (!aad.isEmpty()) {
            int aadLen = 0;
            if (EVP_DecryptUpdate(ctx, nullptr, &aadLen,
                                  reinterpret_cast<const unsigned char *>(aad.constData()),
                                  aad.size()) != 1)
                break;
        }
        if (cipherLen > 0) {
            if (EVP_DecryptUpdate(ctx, plainPtr, &len, cipherPtr, cipherLen) != 1)
                break;
            plainLen = len;
        }
        // 灌入期望 tag，再 Final 验签。Final 返回 <=0 = tag 不匹配（数据被篡改 / key 错）。
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, tagSize(),
                                const_cast<unsigned char *>(tagPtr)) != 1)
            break;
        if (EVP_DecryptFinal_ex(ctx, plainPtr + plainLen, &len) != 1)
            break; // ★ 验签失败：绝不外泄，直接失败
        plainLen += len;
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        out->clear(); // 认证未过：抹掉任何已解出的中间字节
        return false;
    }
    plain.resize(plainLen);
    *out = plain;
    return true;
}

} // namespace coastcore
