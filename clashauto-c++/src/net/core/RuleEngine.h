#pragma once

// CoastCore 的**规则引擎** —— 把一份 mihomo 风格的规则表编译成加速结构，供数据面在每建一条新连接
// 时问一句「这个目的地该走哪个策略/节点」。它只做**匹配决策**，不碰协议、不进注册表、不解析域名
//（域名→IP 由 DnsResolver 负责；本引擎的 match() 拿到的是已知的 host 和/或 ip）。
//
// ★ 语义对齐 mihomo：规则表**自上而下、首条命中即生效**。所以即便本引擎把规则按类型分桶加速，
//   最终裁决仍以「原始表里位置最靠前的那条命中规则」为准 —— 每条编译后的规则都带着它在原表中的
//   序号 order，match() 在所有命中项里取 order 最小的那条的 target。这样既有 O(标签数) 的域名
//   哈希查找 / 预解析好的 CIDR 比对，又不丢 mihomo 的首命中顺序语义。
//
// ★ 线程模型（对齐 ProxyConfigStore 的「不可变快照 + 原子换」）：
//   · 数据面在多个工作线程上并发调用 match()（只读）。
//   · 控制面（设置页改规则 / 热重载）调用 setRules() 造一份**全新**的已编译快照，用一次加锁的
//     指针交换换上去。在途的 match() 各自攥着自己那份快照的 shared_ptr，读得好好的，直到返回。
//   · 临界区只覆盖「拷/换一个 shared_ptr」，数据面读快照内容全程无锁。
//
// GEOIP 的国家码来源见 setGeoipDatabasePath() / setCountryResolver() 的说明（MmdbFile 目前只有
// 校验/落盘、没有查询 API，故这里自带一个只读 mmdb 国家码查询器，或接受外部注入的解析回调）。

#include <QHostAddress>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

// 前置声明：已编译快照与 GeoIP 只读库都定义在 .cpp，头里只持有它们的 shared_ptr（不完整类型
// 用 shared_ptr 持有/析构是安全的，删除器在构造点即已擦除类型）。
struct RuleSnapshot;
class RuleGeoDb;

// 解 YAML **双引号**标量里的转义（`\U0001F3AF` → 🎯 等）。实现与理由在 RuleEngine.cpp。
// ★ 导出成 API 是因为它不只规则要用：ProxyConfigBuilder 解析 proxy-groups 的组名时踩的是
//   **同一个**坑（emoji 组名以转义形式写在 full.yaml 里，不解开就永远匹配不上）。
//   共用一份实现，避免两处各写一遍再慢慢漂移。
QString decodeYamlEscapes(const QString &in);

class RuleEngine
{
public:
    // 规则类型。语义对齐 mihomo；Unknown 表示解析不出来的行（会被丢弃，不进快照）。
    enum class Type {
        Domain,        // 完全匹配一个域名
        DomainSuffix,  // 匹配该域名及其任意子域（按标签边界）
        DomainKeyword, // 域名里包含该子串
        IpCidr,        // IPv4 网段
        IpCidr6,       // IPv6 网段
        GeoIp,         // 按 IP 归属国家码
        Match,         // 兜底：恒命中
        Unknown
    };

    // 一条规则。target = 命中后要走的节点名 / 策略组名（如 "DIRECT"、"PROXY"、"香港"）。
    // MATCH 的 payload 为空；GEOIP 的 payload 是国家码（如 "CN"）；其余 payload 见各自类型。
    struct Rule {
        Type type = Type::Unknown;
        QString payload;
        QString target;
        bool noResolve = false; // IP/GEOIP 规则的 no-resolve 标记：告诉解析侧「别为匹配这条去解析域名」。
                                // 对 match() 本身无影响（IP 已给定），仅供集成侧决策，故这里只存不判。
    };

    // 类型名 ⇄ 枚举（大小写不敏感解析）。
    static Type typeFromString(const QString &s);
    static QString typeToString(Type t);

    // 解析一行 mihomo 规则文本，形如 "TYPE,payload,target[,no-resolve]"（MATCH 为 "MATCH,target"）。
    // 解不出来返回 type==Unknown。承接 ConfigBuilder 的 rules.json：其中 "rule" 条目正是这种整行文本。
    static Rule parseRule(const QString &line);

    RuleEngine();
    ~RuleEngine();

    RuleEngine(const RuleEngine &) = delete;
    RuleEngine &operator=(const RuleEngine &) = delete;

    // 换上一份新规则表（可重入，配合热重载）。内部编译成加速结构后原子换上，不阻塞在途的 match()。
    void setRules(QVector<Rule> rules);
    // 便捷重载：整行文本直接进（内部逐行 parseRule，丢弃 Unknown 行）。
    void setRulesFromLines(const QStringList &lines);

    // —— GEOIP 国家码来源（二选一，都不设则 GEOIP 规则恒失配、被跳过）——
    // A) 注入一个只读 mmdb 路径：本引擎自带的查询器会 mmap/读入该库，按 IP 查出 ISO 国家码。
    //    传空串或无效库则清空。可重入（热更新 GeoIP 后重设即可）。
    void setGeoipDatabasePath(const QString &mmdbPath);
    // B) 注入一个外部国家码解析回调（优先级高于 A）：给 IP 返回其国家码（如 "CN"，未知返回空）。
    //    留给集成侧对接任何现成的 GeoIP 实现（含将来 MmdbFile 若补上查询 API）。回调需自身线程安全。
    using CountryResolver = std::function<QString(const QHostAddress &)>;
    void setCountryResolver(CountryResolver resolver);

    // 数据面入口（只读、线程安全）。域名规则用 host、IP 规则用 ip、GEOIP 用 ip 查国家码比对、
    // MATCH 恒中。返回命中规则（原表中最靠前那条）的 target；一条都没命中返回空串。
    // host 可为空（只有 IP 的裸连接）；ip 可为 null（只有域名、尚未解析）。
    QString match(const QString &host, const QHostAddress &ip) const;

    // 同 match，但**多告诉调用方一件事：这次到底判得了判不了**。
    //
    // ★ 为什么需要它：mihomo 对**不带 no-resolve** 的 IP 类规则会先把域名解析成 IP 再比对。我们只有
    //   域名（ip 为 null）时无从得知那条规则会不会命中；若它排在我们最佳命中之前，谁赢就取决于那次
    //   解析 —— 这时**任何自作结论都可能与核心不一致（=误路由）**。needsResolve=true 就是这个信号，
    //   调用方应据此**回退核心**（核心会解析，结论必然对）。
    //   带 no-resolve 的 IP 规则在无 ip 时 mihomo 也跳过 → 我们跳过=零分歧，不算歧义。
    //   实测本项目规则表 312 条 IP 类规则中 311 条带 no-resolve，唯一不带的是 `GEOIP,CN`。
    struct MatchOutcome {
        QString target;           // 命中的 target（策略组名/节点名/DIRECT…）；判不了或没命中为空
        bool needsResolve = false; // true = 需要先解析域名才能判 → 调用方回退核心
    };
    MatchOutcome matchEx(const QString &host, const QHostAddress &ip) const;

    // 可选：headless 自测（env COAST_RULE_SELFTEST 时由集成侧调用）。跑几条 IP-CIDR / DOMAIN-SUFFIX
    // 断言，全过返回 true，否则 qWarning 打出失败项并返回 false。纯同步、不依赖事件循环。
    static bool selfTest();

private:
    std::shared_ptr<const RuleSnapshot> snapshot() const; // 加锁取当前快照的一份 shared_ptr 拷贝
    QString lookupCountry(const QHostAddress &ip) const;  // 走 resolver 或自带 mmdb 查询器

    mutable QMutex m_mutex;                        // 只护下面三个 shared_ptr/function 的读写
    std::shared_ptr<const RuleSnapshot> m_snapshot; // 当前生效的已编译规则快照
    std::shared_ptr<const RuleGeoDb> m_geo;        // 自带 mmdb 国家码查询器（可空）
    CountryResolver m_resolver;                     // 外部注入的国家码解析回调（可空，优先）
};
