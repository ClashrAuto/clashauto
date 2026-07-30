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
