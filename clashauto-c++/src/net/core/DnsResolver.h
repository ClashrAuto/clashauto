#pragma once

// CoastCore 的 **DNS 解析器 + fake-ip 分配器** —— 数据面（NetStack / 拨号侧）要域名的两种服务：
//
//  1) **fake-ip**（同步、即时）：当某域名判定为「该走代理」时，用户态栈不能当场把它解析成真实 IP
//     （那会泄露 DNS、且可能解到被污染的地址），而是即刻发一个 198.18.0.0/15 保留段里的**假 IP**
//     回给设备。设备拿假 IP 建连 → 栈在 accept 时用 domainForFake() 把假 IP 反查回域名 → 交给出站
//     （SS/Trojan/…）按域名去连。这套是 mihomo fake-ip 模式的等价物，搬进本进程。
//     · fakeFor(domain) 同步返回该域名的假 IP（已分配则复用，未分配则从池里取一个）。
//     · domainForFake(ip) 反查假 IP → 域名（栈的 accept 回调里用，必须同步且线程安全）。
//     · 池是有限的（/15 = 131072 个），满了按最久未分配回收（环形复用）。
//
//  2) **真实解析**（异步）：直连 / GEOIP 需要域名对应的真实 IP 时走 resolveReal()：QDnsLookup 同时发
//     A(v4) 与 AAAA(v6)，两者都回来后合并、按 TTL 入缓存，emit resolved(domain, addrs)；都失败则
//     emit failed()。命中新鲜缓存则下一拍直接 emit resolved（仍走信号，调用方只连信号即可）。
//
// **是否用 fake-ip 由调用方注入的判定回调决定**（setUseFakeIp）：一般「走代理的域名用 fake-ip，走
// 直连/需要真实 IP 的用 resolveReal」。本类只提供两条路径与判定的挂载点，不自己决定策略。
//
// 线程模型：fake 映射表与解析缓存都用一把 QMutex 保护 —— fakeFor/domainForFake 会被数据面工作线程
// 同步直接调用。resolveReal 内部创建 QDnsLookup（QObject，需事件循环），**应在持有本对象的线程上调用**
// （通常是拥有 NetStack 的线程）；解析完成的信号也在该线程发出。
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

#include <functional>

class QDnsLookup;

class DnsResolver : public QObject
{
    Q_OBJECT
public:
    explicit DnsResolver(QObject *parent = nullptr);
    ~DnsResolver() override;

    DnsResolver(const DnsResolver &) = delete;
    DnsResolver &operator=(const DnsResolver &) = delete;

    // —— fake-ip 判定（可注入）——
    // 给定域名返回「是否给它分配 fake-ip」。未设置则默认全 false（等于关掉 fake-ip，全部走 resolveReal）。
    // 回调会被数据面线程调用，需自身线程安全（一般是读一份不可变规则快照，天然安全）。
    using UseFakeIpPredicate = std::function<bool(const QString &domain)>;
    void setUseFakeIp(UseFakeIpPredicate pred);
    // 便捷查询：本域名此刻是否该用 fake-ip（跑一遍已注入的判定；未注入 → false）。
    bool useFakeIp(const QString &domain) const;

    // 可选：指定用于 QDnsLookup 的上游 DNS 服务器（不设则用系统默认解析器）。可重入。
    void setNameserver(const QHostAddress &server);

    // —— fake-ip（同步、线程安全）——
    static bool isFakeIp(const QHostAddress &ip); // 是否落在 198.18.0.0/15
    QHostAddress fakeFor(const QString &domain);  // 取/分配该域名的假 IP（v4）
    QString domainForFake(const QHostAddress &ip) const; // 假 IP 反查域名；非本池/未分配返回空

    // —— 「旁听」核心分配的 fake-ip（同步、线程安全，不碰 QObject/事件循环，可从数据面线程调）——
    //
    // ★ 为什么需要它：被劫持设备的 DNS 被转投**核心**的 dns.listen，于是「域名 ↔ 198.18.x.x」这张表
    //   在核心手里。进程内出站拿到的目的地只是一个 fake-ip，反查不到域名就只能把它原样发给节点 ——
    //   节点当然路由不到（真机实测：节点侧 55 次 dial 198.18.x.x i/o timeout）。
    //   解法不是自己另建一套 fake-ip（那会让回退路径的核心认不出自己没分配过的 IP），而是**旁听**：
    //   DNS 应答回程经我们手时顺手解析一遍，把核心分配的 fake-ip→域名记下来。于是两条路都成立：
    //   进程内出站按域名拨；回退核心时也能给域名（规则匹配比给 IP 更准）。
    //
    // learnFromDnsResponse：解析一份 DNS 应答报文（wire 格式），把其中 **A 记录且落在 fake-ip 段** 的
    //   地址映射到问题名。只认应答(QR=1)、RCODE=0；解析不了/无 fake-ip 记录就静默忽略。
    //   只记 fake-ip（不记真实 IP）：表小、语义紧——真实 IP 的连接本就该照原样直连，不需要改写。
    //   注：核心的 fake-ip 池是 v4 的，故这里只看 A 记录；AAAA 返回的是真地址，不属本机制。
    void learnFromDnsResponse(const QByteArray &wire);
    // 旁听学到的 fake-ip → 域名；未学到/非 v4 返回空。
    QString domainForLearnedIp(const QHostAddress &ip) const;

    // —— 真实解析（异步）——
    // 触发对 domain 的 A+AAAA 解析。命中新鲜缓存则下一拍 emit resolved；否则发起查询，完成后 emit
    // resolved（有记录）或 failed（都无记录/都出错）。同一域名在途重复调用会被合并（只发一次查询）。
    // ★ 只能在**拥有本对象的线程**上调用（内部建 QDnsLookup，需要事件循环）；上面那两个同步方法不受此限。
    void resolveReal(const QString &domain);

    // 清空解析缓存（fake 映射不动）。热重载/切换上游 DNS 后可调。
    void clearCache();

signals:
    void resolved(const QString &domain, const QList<QHostAddress> &addresses);
    void failed(const QString &domain, const QString &error);

private:
    struct Pending; // 定义在 .cpp：一次 domain 的 A+AAAA 在途请求
    struct CacheEntry {
        QList<QHostAddress> addrs;
        qint64 expiryMs = 0; // QDateTime::currentMSecsSinceEpoch() 基准的过期时刻
    };

    void onLookupFinished(Pending *pend, bool isAaaa);
    void finalize(Pending *pend);

    mutable QMutex m_mutex; // 护 fake 映射 + 缓存 + 在途表
    // fake-ip：域名 ⇄ 池内偏移（0..poolSize-1；实际 IP = base + offset）
    QHash<QString, quint32> m_domainToOffset;
    QHash<quint32, QString> m_offsetToDomain;
    quint32 m_nextOffset = 1; // 环形游标（跳过 0=网络地址 与 末尾地址）

    QHash<QString, CacheEntry> m_cache;
    QHash<QString, Pending *> m_pending;
    // 旁听学到的「核心分配的 fake-ip(v4 整数) → 域名」。与上面自建池的两张表**互不相干**：
    // 那两张是本进程自己分配用的，这张是记核心分配的（当前生效的就是这张，见 learnFromDnsResponse）。
    QHash<quint32, QString> m_learnedFake;

    UseFakeIpPredicate m_useFakeIp;
    QHostAddress m_nameserver; // 可空
};
