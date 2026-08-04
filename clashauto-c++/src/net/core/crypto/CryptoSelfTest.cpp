#include "CryptoSelfTest.h"

#include "Aead.h"
#include "Kdf.h"

#include <QByteArray>

#include <cstdio>

// 测试向量出处（都可公开核对）：
//  · AES-128-GCM：McGrew & Viega 的 GCM 规范 Test Case 4（带 AAD 的经典向量，被无数实现引用）。
//  · ChaCha20-Poly1305：RFC 8439 §2.8.2 的示例。
//  · HKDF-SHA1：RFC 5869 Test Case 4（Hash=SHA-1，含 salt/info）。
//  · EVP_BytesToKey：password="test" 经 `openssl enc -aes-256-cbc -k test -md md5 -nosalt -P` 打印的
//    key（前 16 字节即 MD5("test")=098f6bcd...b4f6，公开可复算）。
// 每条既验「正向算出期望值」，AEAD 再验「改一位 tag 必解不开」——把验签闸门也一起测到。

namespace coastcore {

namespace {

// 十六进制串 → 原始字节（忽略空白）。仅供本文件内嵌向量用。
QByteArray hx(const char *hex)
{
    return QByteArray::fromHex(QByteArray(hex));
}

int g_fail = 0;

void check(bool ok, const char *name)
{
    if (ok) {
        std::fprintf(stderr, "CRYPTO-KAT: PASS  %s\n", name);
    } else {
        std::fprintf(stderr, "CRYPTO-KAT: FAIL  %s\n", name);
        ++g_fail;
    }
}

// 跑一条 AEAD 正向+反向+篡改向量。
void aeadCase(const char *name, Aead::Method m, const char *keyHex, const char *nonceHex,
              const char *aadHex, const char *plainHex, const char *expCtHex, const char *expTagHex)
{
    const QByteArray key = hx(keyHex);
    const QByteArray nonce = hx(nonceHex);
    const QByteArray aad = hx(aadHex);
    const QByteArray plain = hx(plainHex);
    const QByteArray expect = hx(expCtHex) + hx(expTagHex); // seal 产物 = 密文||tag

    const QByteArray sealed = Aead::seal(m, key, nonce, aad, plain);
    check(sealed == expect, name); // 正向：密文+tag 完全等于期望向量

    // 反向：open 还原出原明文。
    QByteArray back;
    const bool opened = Aead::open(m, key, nonce, aad, sealed, &back);
    check(opened && back == plain, (QByteArray(name) + " open").constData());

    // 篡改：翻转 tag 最后一位，open 必须失败（AEAD 认证闸门）。
    QByteArray tampered = sealed;
    if (!tampered.isEmpty())
        tampered[tampered.size() - 1] = char(tampered.at(tampered.size() - 1) ^ 0x01);
    QByteArray junk;
    const bool rejected = !Aead::open(m, key, nonce, aad, tampered, &junk);
    check(rejected && junk.isEmpty(), (QByteArray(name) + " tamper-reject").constData());
}

} // namespace

int runCryptoSelfTest()
{
    g_fail = 0;

    // ——— AES-128-GCM（GCM 规范 Test Case 4）———
    aeadCase(
        "aes-128-gcm", Aead::Method::Aes128Gcm,
        "feffe9928665731c6d6a8f9467308308",             // key(16)
        "cafebabefacedbaddecaf888",                     // nonce(12)
        "feedfacedeadbeeffeedfacedeadbeefabaddad2",     // aad(20)
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", // plain
        "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", // ciphertext
        "5bc94fbc3221a5db94fae95ae7121a47");            // tag(16)

    // ——— ChaCha20-Poly1305（RFC 8439 §2.8.2）———
    aeadCase(
        "chacha20-ietf-poly1305", Aead::Method::ChaCha20Poly1305,
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", // key(32)
        "070000004041424344454647",                                         // nonce(12)
        "50515253c0c1c2c3c4c5c6c7",                                         // aad(12)
        // plaintext = "Ladies and Gentlemen of the class of '99: If I could offer you only one
        //              tip for the future, sunscreen would be it."
        "4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66202739393a2049662049"
        "20636f756c64206f6666657220796f75206f6e6c79206f6e652074697020666f7220746865206675747572652c20"
        "73756e73637265656e20776f756c642062652069742e",
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92"
        "728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b"
        "4831d7bc3ff4def08e4b7a9de576d26586cec64b6116",
        "1ae10b594f09e26a7e902ecbd0600691"); // tag(16)

    // ——— HKDF-SHA1（RFC 5869 Test Case 4）———
    {
        const QByteArray ikm = hx("0b0b0b0b0b0b0b0b0b0b0b");        // 11 octets
        const QByteArray salt = hx("000102030405060708090a0b0c");   // 13 octets
        const QByteArray info = hx("f0f1f2f3f4f5f6f7f8f9");          // 10 octets
        const QByteArray expected =
            hx("085a01ea1b10f36933068b56efa5ad81a4f14b822f5b091568a9cdd4f155fda2c22e422478d305f3f896");
        const QByteArray okm = Kdf::hkdfSha1(ikm, salt, info, 42);
        check(okm == expected, "hkdf-sha1 rfc5869-tc4");
    }

    // ——— EVP_BytesToKey / ssMasterKey（password="test"）———
    {
        // keyLen=16 → 就是 MD5("test")
        const QByteArray k16 = Kdf::ssMasterKey(QByteArrayLiteral("test"), 16);
        check(k16 == hx("098f6bcd4621d373cade4e832627b4f6"), "ssMasterKey test/16");
        // keyLen=32 → M[0]||M[1]，与 openssl CLI 打印的 aes-256 key 一致
        const QByteArray k32 = Kdf::ssMasterKey(QByteArrayLiteral("test"), 32);
        check(k32 == hx("098f6bcd4621d373cade4e832627b4f60a9172716ae6428409885b8b829ccb05"),
              "ssMasterKey test/32");
    }

    if (g_fail == 0)
        std::fprintf(stderr, "CRYPTO-KAT: 全部通过 ✅\n");
    else
        std::fprintf(stderr, "CRYPTO-KAT: %d 个用例失败 ❌\n", g_fail);
    std::fflush(stderr);
    return g_fail;
}

} // namespace coastcore
