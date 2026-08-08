#pragma once

// AEAD（Authenticated Encryption with Associated Data）薄封装 —— CoastCore 各协议出站
// （Shadowsocks-AEAD / VMess-AEAD / …）共用的裸加密原语。
//
// ★ 为什么直接封 OpenSSL libcrypto(EVP) 而不用 Qt：Qt 只提供哈希/HMAC（QCryptographicHash /
//   QMessageAuthenticationCode），没有分组密码/AEAD。协议出站要的是「给定 key+nonce+aad，对一段
//   明文做 AES-GCM 或 ChaCha20-Poly1305 密封/解封」这种裸能力，只能落到 libcrypto 的 EVP 接口。
//   哈希/KDF 那一半仍走 Qt（见 Kdf.h），不在这里碰 OpenSSL 的哈希。
//
// ★ 语义约定（务必与调用方对齐，错了是静默的 —— 密文能生成但对端解不开，或验签假通过）：
//   · seal() 返回 **密文 || tag**，tag 恒 16 字节附在末尾（对齐 SS-AEAD / RFC5116 的惯例，
//     调用方不必分别管理 tag 缓冲）。
//   · open() 的入参就是 seal 的产物（密文||tag），内部拆出末 16 字节做验签；验签失败返回 false，
//     **绝不**把未通过认证的明文写给调用方（AEAD 的全部安全性系于此）。
//   · nonce 恒 12 字节：三种方法都用 96-bit IV —— GCM 的推荐 IV 长、ChaCha20-Poly1305(IETF) 的
//     固定 nonce 长都是 12。协议侧的「按序号递增 nonce」由协议自己算，这里只认一个 12 字节串。
//
// 本文件跨平台：只依赖 OpenSSL + Qt Core 的 QByteArray，不碰任何平台 API。
#include <QByteArray>
#include <QString>

namespace coastcore {

class Aead
{
public:
    // 支持的 AEAD 方法。名字对齐 Shadowsocks/Clash 配置里 cipher 字段的写法。
    enum class Method {
        Invalid = 0,
        Aes128Gcm,        // "aes-128-gcm"          key=16
        Aes256Gcm,        // "aes-256-gcm"          key=32
        ChaCha20Poly1305, // "chacha20-ietf-poly1305" key=32（IETF 变体，12 字节 nonce）
    };

    // 把配置里的 cipher 字符串解析成 Method；无法识别返回 Invalid。
    // 兼容别名："chacha20-poly1305" 与 "chacha20-ietf-poly1305" 视为同一个（都是 IETF/96-bit nonce
    // 的那个；SS 生态里裸 "chacha20-poly1305" 一般也指 IETF 变体）。大小写不敏感。
    static Method methodFromName(const QString &name);

    // —— 各方法的尺寸参数（字节）——
    // keySize：密钥长度（AES-128=16，AES-256/ChaCha20=32）。Invalid 返回 0。
    static int keySize(Method m);
    // nonceSize：恒 12（三种方法都是 96-bit nonce）。
    static int nonceSize() { return 12; }
    // tagSize：恒 16（Poly1305 / GCM 的认证标签都是 128-bit）。
    static int tagSize() { return 16; }

    // 密封：对 plaintext 做 AEAD 加密，附上 aad 参与认证（不加密 aad 本身）。
    // 返回 密文||tag（长度 = plaintext.size() + 16）。参数非法（方法 Invalid / key 或 nonce 长度不符）
    // 或底层出错时返回空 QByteArray。aad 可空。
    static QByteArray seal(Method m, const QByteArray &key, const QByteArray &nonce,
                           const QByteArray &aad, const QByteArray &plaintext);

    // 解封：验证末 16 字节的 tag 并解密。成功把明文写入 *out 并返回 true；
    // 验签失败 / 长度不足 / 参数非法一律返回 false，且**不**修改 *out 的内容为可用明文
    //（内部即便解出中间字节，验签失败时也丢弃、清空 out，绝不外泄未认证数据）。
    static bool open(Method m, const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                     const QByteArray &ciphertextWithTag, QByteArray *out);
};

} // namespace coastcore
