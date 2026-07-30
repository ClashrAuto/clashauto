#include "DnsMessage.h"

#include <QStringList>

namespace coastcore {
namespace {

// 头部固定 12 字节：ID(2) FLAGS(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2)
constexpr int kHdr = 12;

inline quint16 rd16(const QByteArray &b, int at)
{
    return quint16((quint8(b.at(at)) << 8) | quint8(b.at(at + 1)));
}
inline void wr16(QByteArray &b, quint16 v)
{
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}
inline void wr32(QByteArray &b, quint32 v)
{
    b.append(char((v >> 24) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}

// 应答头：抄请求的 ID，置 QR=1、RA=1，沿用请求的 RD、OPCODE=0，rcode 由参数给。
// ancount 由参数给；NSCOUNT/ARCOUNT 恒 0（我们不发权威/附加段）。
QByteArray answerHeader(const DnsQuestion &q, quint16 ancount, quint8 rcode)
{
    QByteArray out;
    out.reserve(kHdr);
    wr16(out, q.id);
    // QR=1(0x8000) + AA=0 + TC=0 + RD(抄请求) + RA=1(0x0080) + rcode
    quint16 flags = 0x8000 | 0x0080 | quint16(rcode & 0x0F);
    if (q.recursionDesired)
        flags |= 0x0100;
    wr16(out, flags);
    wr16(out, 1);       // QDCOUNT：我们只处理单问题段
    wr16(out, ancount); // ANCOUNT
    wr16(out, 0);       // NSCOUNT
    wr16(out, 0);       // ARCOUNT
    return out;
}

} // namespace

DnsQuestion parseDnsQuestion(const QByteArray &wire)
{
    DnsQuestion q;
    if (wire.size() < kHdr + 5) // 头 + 至少 root(1) + qtype(2) + qclass(2)
        return q;
    const quint16 flags = rd16(wire, 2);
    if (flags & 0x8000)
        return q; // QR=1 是应答，不是我们该回的查询
    if (((flags >> 11) & 0x0F) != 0)
        return q; // OPCODE != 标准查询（IQUERY/STATUS/UPDATE…）→ 交给上游
    if (rd16(wire, 4) != 1)
        return q; // QDCOUNT != 1：多问题段极罕见，不自作主张

    // —— 解析 QNAME ——
    // ★ 查询报文里的 QNAME **不该**出现压缩指针（压缩只在应答的记录里用，且要指向前面已出现的名字）。
    //   碰到 0xC0 前缀一律判失败 → 走转发上游这条保守路径，而不是硬解。
    QStringList labels;
    int pos = kHdr;
    int guard = 0;
    while (pos < wire.size()) {
        const quint8 len = quint8(wire.at(pos));
        if (len == 0) {
            ++pos;
            break;
        }
        if ((len & 0xC0) != 0)
            return q; // 压缩指针或保留位 → 不处理
        if (++guard > 128)
            return q; // 标签数上限，防畸形报文兜圈
        if (pos + 1 + len > wire.size())
            return q;
        labels << QString::fromLatin1(wire.constData() + pos + 1, len).toLower();
        pos += 1 + len;
    }
    if (labels.isEmpty())
        return q; // 根查询，不处理
    if (pos + 4 > wire.size())
        return q;

    q.id = rd16(wire, 0);
    q.recursionDesired = (flags & 0x0100) != 0;
    q.qname = labels.join(QLatin1Char('.'));
    q.qtype = rd16(wire, pos);
    q.qclass = rd16(wire, pos + 2);
    q.questionEnd = pos + 4;
    q.ok = true;
    return q;
}

QByteArray buildDnsAnswerA(const QByteArray &query, const DnsQuestion &q, const QHostAddress &ip,
                           quint32 ttl)
{
    if (!q.ok || q.questionEnd > query.size() || ip.protocol() != QAbstractSocket::IPv4Protocol)
        return {};
    QByteArray out = answerHeader(q, 1, 0);
    out.append(query.constData() + kHdr, q.questionEnd - kHdr); // 问题段原样回抄
    // 答案段：NAME 用指向问题段名字的压缩指针(0xC00C = 偏移 12)，这是最小且通用的写法。
    wr16(out, 0xC00C);
    wr16(out, kDnsTypeA);
    wr16(out, 1); // CLASS=IN
    wr32(out, ttl);
    wr16(out, 4); // RDLENGTH
    const quint32 v4 = ip.toIPv4Address();
    wr32(out, v4);
    return out;
}

QByteArray buildDnsNoData(const QByteArray &query, const DnsQuestion &q, quint32 ttl)
{
    Q_UNUSED(ttl);
    if (!q.ok || q.questionEnd > query.size())
        return {};
    QByteArray out = answerHeader(q, 0, 0); // NOERROR + 零条记录 = NODATA
    out.append(query.constData() + kHdr, q.questionEnd - kHdr);
    return out;
}

QByteArray buildDnsServFail(const QByteArray &query, const DnsQuestion &q)
{
    if (!q.ok || q.questionEnd > query.size())
        return {};
    QByteArray out = answerHeader(q, 0, 2); // RCODE=2 SERVFAIL
    out.append(query.constData() + kHdr, q.questionEnd - kHdr);
    return out;
}

// ============================== 自检（KAT）==============================
bool dnsSelfTest(QString *why)
{
    auto fail = [&](const QString &m) {
        if (why)
            *why = m;
        return false;
    };

    // 向量：对 www.baidu.com 的标准 A 查询（ID=0xC0A5, RD=1, QDCOUNT=1）。
    // 与 tools/outbound_e2e 里 HY2UDP 用例用的是同一条，来源可追。
    const QByteArray q1 =
        QByteArray::fromHex("c0a5010000010000000000000377777705626169647503636f6d0000010001");
    DnsQuestion q = parseDnsQuestion(q1);
    if (!q.ok)
        return fail(QStringLiteral("标准 A 查询解析失败"));
    if (q.qname != QStringLiteral("www.baidu.com"))
        return fail(QStringLiteral("qname='%1'（期望 www.baidu.com）").arg(q.qname));
    if (q.qtype != kDnsTypeA || q.qclass != 1)
        return fail(QStringLiteral("qtype=%1 qclass=%2（期望 1/1）").arg(q.qtype).arg(q.qclass));
    if (q.id != 0xC0A5 || !q.recursionDesired)
        return fail(QStringLiteral("id/RD 解析不对"));
    if (q.questionEnd != q1.size())
        return fail(QStringLiteral("questionEnd=%1（期望 %2）").arg(q.questionEnd).arg(q1.size()));

    // 合成 A 应答：头 12 + 问题段(len-12) + 答案段 16 字节
    const QByteArray a = buildDnsAnswerA(q1, q, QHostAddress(QStringLiteral("198.18.0.7")), 60);
    if (a.size() != q1.size() + 16)
        return fail(QStringLiteral("A 应答长度 %1（期望 %2）").arg(a.size()).arg(q1.size() + 16));
    if (quint8(a.at(0)) != 0xC0 || quint8(a.at(1)) != 0xA5)
        return fail(QStringLiteral("应答 ID 没抄对"));
    if (!(quint8(a.at(2)) & 0x80))
        return fail(QStringLiteral("应答没置 QR=1"));
    if (rd16(a, 6) != 1)
        return fail(QStringLiteral("ANCOUNT != 1"));
    if (a.mid(kHdr, q.questionEnd - kHdr) != q1.mid(kHdr))
        return fail(QStringLiteral("问题段没原样回抄"));
    // 答案段末 4 字节即 198.18.0.7
    const QByteArray rdata = a.right(4);
    if (quint8(rdata.at(0)) != 198 || quint8(rdata.at(1)) != 18 || quint8(rdata.at(2)) != 0
        || quint8(rdata.at(3)) != 7)
        return fail(QStringLiteral("A 记录里的 IP 不对：%1").arg(QString::fromLatin1(rdata.toHex(' '))));
    if (quint8(a.at(q.questionEnd)) != 0xC0 || quint8(a.at(q.questionEnd + 1)) != 0x0C)
        return fail(QStringLiteral("答案段没用 0xC00C 压缩指针"));

    // NODATA：ANCOUNT=0、RCODE=0（**不能**是 NXDOMAIN，否则设备连 v4 都不试了）
    const QByteArray nd = buildDnsNoData(q1, q, 60);
    if (nd.size() != q1.size())
        return fail(QStringLiteral("NODATA 应答长度 %1（期望 %2）").arg(nd.size()).arg(q1.size()));
    if (rd16(nd, 6) != 0 || (quint8(nd.at(3)) & 0x0F) != 0)
        return fail(QStringLiteral("NODATA 的 ANCOUNT/RCODE 不对"));

    // SERVFAIL：RCODE=2
    const QByteArray sf = buildDnsServFail(q1, q);
    if ((quint8(sf.at(3)) & 0x0F) != 2)
        return fail(QStringLiteral("SERVFAIL 的 RCODE 不对"));

    // —— 该拒绝的都要拒绝（拒绝 = 交给上游转发，绝不自作主张）——
    struct Bad { const char *hex; const char *what; };
    const Bad bads[] = {
        { "c0a5810000010001000000000377777705626169647503636f6d0000010001", "QR=1 的应答" },
        { "c0a5010000020000000000000377777705626169647503636f6d0000010001", "QDCOUNT=2" },
        { "c0a501000001000000000000c00c0000010001",                         "QNAME 用压缩指针" },
        { "c0a50100000100000000000003777777",                               "被截断的报文" },
        { "c0a5090000010000000000000377777705626169647503636f6d0000010001", "OPCODE!=0" },
    };
    for (const Bad &b : bads) {
        if (parseDnsQuestion(QByteArray::fromHex(b.hex)).ok)
            return fail(QStringLiteral("本该拒绝却解析成功：%1").arg(QString::fromLatin1(b.what)));
    }

    // AAAA 查询要能认出类型（我们据此回 NODATA 让设备回落 v4）
    const QByteArray q2 =
        QByteArray::fromHex("000101000001000000000000076578616d706c6503636f6d00001c0001");
    const DnsQuestion qa = parseDnsQuestion(q2);
    if (!qa.ok || qa.qtype != kDnsTypeAAAA || qa.qname != QStringLiteral("example.com"))
        return fail(QStringLiteral("AAAA 查询解析不对（qtype=%1 qname=%2）").arg(qa.qtype).arg(qa.qname));

    return true;
}

} // namespace coastcore
