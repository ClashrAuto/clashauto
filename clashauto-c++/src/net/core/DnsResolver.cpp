#include "DnsResolver.h"

#include <QDateTime>
#include <QDnsLookup>

#include <climits>

namespace {
// fake-ip 池：198.18.0.0/15（RFC2544 基准测试保留段，公网不会用到，做假 IP 很安全）。
constexpr quint32 kPoolBase = 0xC6120000u; // 198.18.0.0
constexpr quint32 kPoolMask = 0xFFFE0000u; // /15
constexpr quint32 kPoolSize = 0x20000u;    // 2^17 = 131072 个地址
constexpr int kDefaultTtl = 60;            // 无 TTL 时的兜底缓存秒数
constexpr int kMaxTtl = 3600;              // 缓存上限，防超长 TTL 长期钉住脏记录

// 旁听表容量上限：满了整表清空重来（fake-ip 段本身只有 /15，正常设备量远达不到；
// 清空的代价只是「这一轮少数连接暂时反查不到域名 → 回退核心」，不会错路由）。
constexpr int kMaxLearned = 65536;

// 读一份 DNS 报文里的一个名字（支持 RFC1035 的 0xC0 压缩指针）。
// pos 会前进到该名字之后；越界/非法/指针成环一律返回 false（调用方直接放弃这份报文）。
bool readDnsName(const QByteArray &w, int &pos, QString *out, int depth = 0)
{
    if (depth > 8) {
        return false; // 压缩指针链太深/成环
    }
    QStringList labels;
    for (;;) {
        if (pos < 0 || pos >= w.size()) {
            return false;
        }
        const quint8 len = quint8(w.at(pos));
        if (len == 0) {
            ++pos; // 根标签，名字结束
            break;
        }
        if ((len & 0xC0) == 0xC0) { // 压缩指针：本体占 2 字节，内容在 target 处
            if (pos + 1 >= w.size()) {
                return false;
            }
            const int target = ((int(len & 0x3F)) << 8) | int(quint8(w.at(pos + 1)));
            pos += 2;
            QString rest;
            int p2 = target;
            if (!readDnsName(w, p2, &rest, depth + 1)) {
                return false;
            }
            if (!rest.isEmpty()) {
                labels << rest;
            }
            if (out) {
                *out = labels.join(QLatin1Char('.'));
            }
            return true; // 指针之后名字即终止
        }
        if ((len & 0xC0) != 0) {
            return false; // 0x40/0x80 是保留组合，非法
        }
        if (pos + 1 + int(len) > w.size()) {
            return false;
        }
        labels << QString::fromLatin1(w.constData() + pos + 1, int(len));
        pos += 1 + int(len);
    }
    if (out) {
        *out = labels.join(QLatin1Char('.'));
    }
    return true;
}
} // namespace

// 一次 domain 的 A+AAAA 在途请求（合并两个 QDnsLookup 的结果）。
struct DnsResolver::Pending {
    QString domain;
    QDnsLookup *a = nullptr;
    QDnsLookup *aaaa = nullptr;
    bool aDone = false;
    bool aaaaDone = false;
    QList<QHostAddress> addrs;
    int minTtl = INT_MAX;
};

DnsResolver::DnsResolver(QObject *parent) : QObject(parent) {}

DnsResolver::~DnsResolver()
{
    // 放弃在途请求：QDnsLookup 是本对象的子对象，随 ~QObject 释放；这里只回收非 QObject 的 Pending。
    QMutexLocker lock(&m_mutex);
    for (Pending *p : m_pending) {
        delete p;
    }
    m_pending.clear();
}

void DnsResolver::setUseFakeIp(UseFakeIpPredicate pred)
{
    QMutexLocker lock(&m_mutex);
    m_useFakeIp = std::move(pred);
}

bool DnsResolver::useFakeIp(const QString &domain) const
{
    UseFakeIpPredicate pred;
    {
        QMutexLocker lock(&m_mutex);
        pred = m_useFakeIp;
    }
    return pred ? pred(domain) : false;
}

void DnsResolver::setNameserver(const QHostAddress &server)
{
    QMutexLocker lock(&m_mutex);
    m_nameserver = server;
}

// ============================ fake-ip ============================

bool DnsResolver::isFakeIp(const QHostAddress &ip)
{
    return ip.protocol() == QAbstractSocket::IPv4Protocol
        && (ip.toIPv4Address() & kPoolMask) == kPoolBase;
}

QHostAddress DnsResolver::fakeFor(const QString &domain)
{
    const QString d = domain.trimmed().toLower();
    if (d.isEmpty()) {
        return QHostAddress();
    }
    QMutexLocker lock(&m_mutex);
    const auto existing = m_domainToOffset.constFind(d);
    if (existing != m_domainToOffset.constEnd()) {
        return QHostAddress(kPoolBase + existing.value()); // 已分配，复用（映射稳定）
    }

    const quint32 off = m_nextOffset;
    // 游标前进；有效偏移取 [1, kPoolSize-2]（跳过 0=网络地址、末尾地址），环形复用。
    m_nextOffset = (m_nextOffset >= kPoolSize - 2) ? 1u : (m_nextOffset + 1);

    // 该槽若被旧域名占着（池满后环形复用），先把旧映射清掉 —— 最久未分配者被回收。
    const auto occupant = m_offsetToDomain.constFind(off);
    if (occupant != m_offsetToDomain.constEnd()) {
        m_domainToOffset.remove(occupant.value());
        m_offsetToDomain.erase(occupant);
    }
    m_domainToOffset.insert(d, off);
    m_offsetToDomain.insert(off, d);
    return QHostAddress(kPoolBase + off);
}

QString DnsResolver::domainForFake(const QHostAddress &ip) const
{
    if (!isFakeIp(ip)) {
        return QString();
    }
    const quint32 off = ip.toIPv4Address() - kPoolBase;
    QMutexLocker lock(&m_mutex);
    return m_offsetToDomain.value(off);
}

// ============ 旁听核心分配的 fake-ip（见头文件那段「为什么需要它」） ============

void DnsResolver::learnFromDnsResponse(const QByteArray &wire)
{
    if (wire.size() < 12) {
        return; // 连报文头都不够
    }
    const auto be16 = [&wire](int off) -> quint16 {
        return quint16((quint16(quint8(wire.at(off))) << 8) | quint16(quint8(wire.at(off + 1))));
    };
    const quint16 flags = be16(2);
    if ((flags & 0x8000) == 0) {
        return; // QR=0：这是查询不是应答
    }
    if ((flags & 0x000F) != 0) {
        return; // RCODE != 0：错误应答，没有可信记录
    }
    const int qd = int(be16(4));
    const int an = int(be16(6));
    if (qd < 1 || an < 1) {
        return;
    }

    int pos = 12;
    QString qname;
    if (!readDnsName(wire, pos, &qname) || pos + 4 > wire.size()) {
        return;
    }
    pos += 4; // QTYPE + QCLASS
    for (int i = 1; i < qd; ++i) { // 正常只有 1 个问题；多的照规矩跳过
        if (!readDnsName(wire, pos, nullptr) || pos + 4 > wire.size()) {
            return;
        }
        pos += 4;
    }
    const QString domain = qname.trimmed().toLower();
    if (domain.isEmpty()) {
        return;
    }

    // 扫应答段，挑出「A 记录 且 落在 fake-ip 段」的地址（CNAME 等一律跳过，只按 RDLENGTH 前进）。
    QVector<quint32> fakes;
    for (int i = 0; i < an; ++i) {
        if (!readDnsName(wire, pos, nullptr) || pos + 10 > wire.size()) {
            return;
        }
        const quint16 type = be16(pos);
        const quint16 rdlen = be16(pos + 8);
        pos += 10;
        if (pos + int(rdlen) > wire.size()) {
            return;
        }
        if (type == 1 && rdlen == 4) { // A
            const quint32 v = (quint32(quint8(wire.at(pos))) << 24)
                            | (quint32(quint8(wire.at(pos + 1))) << 16)
                            | (quint32(quint8(wire.at(pos + 2))) << 8)
                            | quint32(quint8(wire.at(pos + 3)));
            if ((v & kPoolMask) == kPoolBase) {
                fakes.append(v);
            }
        }
        pos += int(rdlen);
    }
    if (fakes.isEmpty()) {
        return; // 这条应答里没有 fake-ip（真实 IP 不记，见头文件说明）
    }

    QMutexLocker lock(&m_mutex);
    if (m_learnedFake.size() >= kMaxLearned) {
        m_learnedFake.clear(); // 容量兜底：清空重来，只会让少数连接暂时回退核心，不会错路由
    }
    for (const quint32 v : fakes) {
        m_learnedFake.insert(v, domain);
    }
}

QString DnsResolver::domainForLearnedIp(const QHostAddress &ip) const
{
    if (ip.protocol() != QAbstractSocket::IPv4Protocol) {
        return QString();
    }
    QMutexLocker lock(&m_mutex);
    return m_learnedFake.value(ip.toIPv4Address());
}

// ============================ 真实解析（异步） ============================

void DnsResolver::resolveReal(const QString &domain)
{
    const QString d = domain.trimmed().toLower();
    if (d.isEmpty()) {
        return;
    }

    QHostAddress ns;
    {
        QMutexLocker lock(&m_mutex);
        const auto cit = m_cache.constFind(d);
        if (cit != m_cache.constEnd() && cit->expiryMs > QDateTime::currentMSecsSinceEpoch()) {
            // 命中新鲜缓存：仍走信号（调用方只连信号），排到下一拍发，避免同步重入调用方。
            const QList<QHostAddress> addrs = cit->addrs;
            QMetaObject::invokeMethod(
                this, [this, d, addrs] { emit resolved(d, addrs); }, Qt::QueuedConnection);
            return;
        }
        if (m_pending.contains(d)) {
            return; // 同域名已在途：合并，等它那次完成时统一 emit
        }
        // 占位（先登记，避免并发重复发起）；QDnsLookup 在锁外创建。
        m_pending.insert(d, nullptr);
        ns = m_nameserver;
    }

    Pending *pend = new Pending;
    pend->domain = d;
    pend->a = new QDnsLookup(QDnsLookup::A, d, this);
    pend->aaaa = new QDnsLookup(QDnsLookup::AAAA, d, this);
    if (!ns.isNull()) {
        pend->a->setNameserver(ns);
        pend->aaaa->setNameserver(ns);
    }
    {
        QMutexLocker lock(&m_mutex);
        m_pending.insert(d, pend); // 把占位换成真正的 Pending
    }
    connect(pend->a, &QDnsLookup::finished, this, [this, pend] { onLookupFinished(pend, false); });
    connect(pend->aaaa, &QDnsLookup::finished, this,
            [this, pend] { onLookupFinished(pend, true); });
    pend->a->lookup();
    pend->aaaa->lookup();
}

void DnsResolver::onLookupFinished(Pending *pend, bool isAaaa)
{
    QDnsLookup *dl = isAaaa ? pend->aaaa : pend->a;
    if (dl && dl->error() == QDnsLookup::NoError) {
        const QList<QDnsHostAddressRecord> recs = dl->hostAddressRecords();
        for (const QDnsHostAddressRecord &r : recs) {
            const QHostAddress a = r.value();
            if (!pend->addrs.contains(a)) {
                pend->addrs.append(a);
            }
            pend->minTtl = qMin(pend->minTtl, int(r.timeToLive()));
        }
    }
    if (isAaaa) {
        pend->aaaaDone = true;
    } else {
        pend->aDone = true;
    }
    if (pend->aDone && pend->aaaaDone) {
        finalize(pend);
    }
}

void DnsResolver::finalize(Pending *pend)
{
    const QString d = pend->domain;
    const QList<QHostAddress> addrs = pend->addrs;
    int ttl = (pend->minTtl > 0 && pend->minTtl != INT_MAX) ? pend->minTtl : kDefaultTtl;
    if (ttl > kMaxTtl) {
        ttl = kMaxTtl;
    }
    {
        QMutexLocker lock(&m_mutex);
        if (!addrs.isEmpty()) {
            m_cache.insert(d, CacheEntry{addrs, QDateTime::currentMSecsSinceEpoch()
                                                    + qint64(ttl) * 1000});
        }
        m_pending.remove(d);
    }
    if (pend->a) {
        pend->a->deleteLater();
    }
    if (pend->aaaa) {
        pend->aaaa->deleteLater();
    }
    if (addrs.isEmpty()) {
        emit failed(d, QStringLiteral("no A/AAAA records"));
    } else {
        emit resolved(d, addrs);
    }
    delete pend;
}

void DnsResolver::clearCache()
{
    QMutexLocker lock(&m_mutex);
    m_cache.clear();
}
