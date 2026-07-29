#pragma once

// 密钥派生（KDF）—— CoastCore 协议出站共用的两个密钥推导原语。
//
// ★ 为什么这一半用 Qt 而不用 OpenSSL：哈希与 HMAC 是 Qt Core 自带的（QCryptographicHash /
//   QMessageAuthenticationCode），能力足够且不牵扯 OpenSSL 的哈希 API，编译依赖更干净。只有裸
//   AEAD 分组密码 Qt 给不了，才落到 libcrypto（见 Aead.h）。所以这里刻意**不** #include 任何
//   openssl 头 —— 保持「哈希/KDF=Qt，AEAD=OpenSSL」的分工。
//
// 两个函数对应 Shadowsocks-AEAD 的两级派生：
//   password --EVP_BytesToKey(MD5)--> masterKey --HKDF-SHA1(salt,"ss-subkey")--> per-connection subkey
// subkey 才是喂给 Aead::seal/open 的那把 key（每条连接一个随机 salt → 一把不同的 subkey）。
#include <QByteArray>

namespace coastcore {

class Kdf
{
public:
    // Shadowsocks 主密钥派生 = OpenSSL 的 EVP_BytesToKey(cipher, MD5, salt=NULL, iter=1)：
    //   M[0] = MD5(password)
    //   M[i] = MD5(M[i-1] || password)
    //   masterKey = (M[0] || M[1] || ...) 取前 keyLen 字节
    // 无 salt、单轮迭代，是 SS 生态里从 password 生成对称密钥的通用约定（历史遗留，弱，但协议要求
    // 逐字节一致，不能自作主张换 KDF）。用 QCryptographicHash(Md5) 实现，不调 OpenSSL。
    static QByteArray ssMasterKey(const QByteArray &password, int keyLen);

    // HKDF（RFC5869）以 HMAC-SHA1 为 PRF，两步：
    //   Extract: PRK = HMAC-SHA1(key=salt, msg=ikm)         —— salt 为空则按 RFC 用 HashLen(20) 个 0
    //   Expand : T(0)="" ; T(i)=HMAC-SHA1(key=PRK, T(i-1)||info||byte(i)) ; OKM=T(1)||T(2)||… 取 outLen
    // Shadowsocks-AEAD 的每连接子密钥 = hkdfSha1(masterKey, salt, "ss-subkey", keyLen)。
    // 用 QMessageAuthenticationCode(Sha1) 实现。outLen 受 HKDF 上限 255*20 约束（SS 用不到那么长）。
    static QByteArray hkdfSha1(const QByteArray &key, const QByteArray &salt, const QByteArray &info,
                              int outLen);
};

} // namespace coastcore
