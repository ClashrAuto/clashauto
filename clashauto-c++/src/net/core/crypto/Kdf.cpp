#include "Kdf.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

namespace coastcore {

QByteArray Kdf::ssMasterKey(const QByteArray &password, int keyLen)
{
    if (keyLen <= 0)
        return {};
    QByteArray key;
    QByteArray prev; // 上一段 M[i-1]（首轮为空 → M[0]=MD5(password)）
    while (key.size() < keyLen) {
        QCryptographicHash md5(QCryptographicHash::Md5);
        md5.addData(prev);     // M[i] = MD5( M[i-1] || password )；首轮 prev 空即 MD5(password)
        md5.addData(password);
        prev = md5.result();   // 16 字节
        key += prev;
    }
    return key.left(keyLen);
}

QByteArray Kdf::hkdfSha1(const QByteArray &key, const QByteArray &salt, const QByteArray &info,
                         int outLen)
{
    if (outLen <= 0)
        return {};
    static const int kHashLen = 20; // SHA-1 输出长度
    // HKDF 上限：outLen <= 255 * HashLen。SS 用不到，越界直接返回空（避免死循环 / 溢出 i 的一字节）。
    if (outLen > 255 * kHashLen)
        return {};

    // —— Extract —— salt 作 HMAC 密钥，ikm(key) 作消息。salt 为空按 RFC5869 用 HashLen 个 0。
    const QByteArray effSalt = salt.isEmpty() ? QByteArray(kHashLen, '\0') : salt;
    QMessageAuthenticationCode ext(QCryptographicHash::Sha1);
    ext.setKey(effSalt);
    ext.addData(key);
    const QByteArray prk = ext.result();

    // —— Expand —— T(i)=HMAC(PRK, T(i-1)||info||i)，i 从 1 起、单字节计数器。
    QByteArray okm;
    QByteArray t; // T(0) = 空
    unsigned char counter = 1;
    while (okm.size() < outLen) {
        QMessageAuthenticationCode exp(QCryptographicHash::Sha1);
        exp.setKey(prk);
        exp.addData(t);
        exp.addData(info);
        exp.addData(QByteArray(1, char(counter)));
        t = exp.result();
        okm += t;
        ++counter;
    }
    return okm.left(outLen);
}

} // namespace coastcore
