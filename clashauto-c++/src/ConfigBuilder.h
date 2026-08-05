#pragma once

#include "AppConfig.h"

#include <QMap>
#include <QString>
#include <QVector>

class ConfigBuilder
{
public:
    explicit ConfigBuilder(AppConfig config);

    /// 一张可作为**出口**的物理网卡（由 DevicesController 每轮扫描后经 CoreController 推进来）。
    ///
    /// 用途只有一个：**让被代理设备的「直连」从它自己那张网卡出去**。
    /// 电脑同时接两个路由器时，被终结的连接全都由核心从本机重新发起，而核心只有一个默认出口
    /// —— 于是 B 网段设备的直连流量整段绕道 A 路由器的宽带（延迟高、还跟着 Windows 自动跃点
    /// 一起抖）。给每张卡生成一个 `type: direct` + `interface-name` 的出站，再把该卡上设备的
    /// 直连改指到它，这一层就对齐了。
    struct NicEgress {
        // 核心 `interface-name` 的口径 = Windows 适配器**友好名**（见 LanScanner::NicInfo::friendlyName）。
        // 空 = 这张卡不生成出站（宁可退回今天的行为，也不写一个必然拨号失败的 interface-name）。
        QString friendlyName;
        quint32 base = 0; // 网段（主机序），用来判设备属于哪张卡
        quint32 mask = 0;
        bool primary = false; // 主网卡（系统默认出口那张）。它本来就走默认出口，不需要改写。
        // 调用方每轮扫描都会拿新表和旧表比，一样就什么都不做（不能每 5s 热重载一次核心）。
        bool operator==(const NicEgress &o) const
        {
            return friendlyName == o.friendlyName && base == o.base && mask == o.mask
                    && primary == o.primary;
        }
        bool operator!=(const NicEgress &o) const { return !(*this == o); }
    };
    void setEgressNics(const QVector<NicEgress> &nics) { m_egressNics = nics; }

    // tunEnabled / ipv6Enabled 都按参数传而不是读 m_config：这个 ConfigBuilder 是启动时
    // 用 AppConfig 的一份副本构造的，运行时会变的开关读它只会拿到旧值。
    QString ensureFullConfig(bool tunEnabled, bool ipv6Enabled);
    bool writeTunEnabled(const QString &filePath, bool enabled) const;

    // 从 YAML 文本提取全部节点名 / 策略组名。静态公开：规则编辑器「节点」下拉
    // 需要读 full.yaml 列出全部候选（SettingsController::proxyGroupNames）。
    static QStringList proxyNames(const QString &yaml);
    static QStringList existingGroupNames(const QString &yaml);

    // 本机各网卡的 IPv6 全局单播前缀，已拼成 IP-CIDR6 规则行。**静态公开**：除了本类生成配置要用，
    // DevicesController 每轮扫描也拿它比对「网段变了没有」——变了才触发 rebuildConfig()。
    // 两处必须是同一份实现，否则会出现「探测说变了、生成出来却没变」的空转热重载。
    static QStringList localGlobal6Prefixes();

private:
    struct SubscriptionNode {
        QString name;
        QString yaml;
    };

    struct Subscription {
        QString name;
        bool use = false;
        bool speedtest = false;
        QVector<SubscriptionNode> nodes;
    };

    QString readText(const QString &path) const;
    void writeText(const QString &path, const QString &text) const;
    QString mergePlugin(const QString &base, const QString &plugin) const;
    QVector<Subscription> readSubscriptions() const;
    QString applySubscriptions(QString yaml, const QVector<Subscription> &subscriptions) const;
    QString applyCustomRules(QString yaml) const;
    QString applyDevicePolicies(QString yaml) const; // 设备台账(coast.db device 表) → 网关 SOCKS listener + IN-USER 规则
    QString applyPrivateNetworkRules(QString yaml) const; // 私网/回环/链路本地 → DIRECT，前插到 rules: 最顶
    QString applyRejectDevices(QString yaml) const;       // 「禁网」设备 → REJECT，必须压过私网直连
    QString applySniffer(QString yaml) const;        // 顶层 sniffer: 块——从 TLS SNI/HTTP Host 还原域名
    QString applyProfilePersistence(QString yaml) const; // 顶层 profile: 块——持久化 fake-ip 映射与节点选择，扛住热重载
    QString addToFirstGroup(QString yaml, const QStringList &names) const;
    QString replaceTopLevelProxies(QString yaml, const QString &proxyBlock) const;
    QString replaceProxyListAt(QString yaml, qsizetype proxiesKey, const QStringList &values) const;
    QString appendSubscriptionGroups(QString yaml, const QVector<Subscription> &subscriptions) const;
    QMap<QString, QStringList> autoGroups(const QStringList &nodeNames) const;
    QString setScalar(QString yaml, const QString &key, const QString &value) const;
    QString setNestedScalar(QString yaml, const QString &section, const QString &key, const QString &value) const;
    QString ensureProxyServerNameserver(QString yaml) const;
    /// 剔除订阅下发的 DNS 里**在墙内不可达**的那些（见 .cpp 里的实测数据）。
    /// 订阅方一条失效 DNS 就能让每条连接白等 5 秒，且用户完全无从得知。
    QString pruneUnreachableDns(QString yaml) const;
    QString applyIpv6(QString yaml, bool enabled) const; // ipv6 / dns.ipv6 / dns.fake-ip-range6 三件套
    QString normalizeEmptyProxies(QString yaml) const;
    QString yamlQuote(const QString &value) const;
    static QString yamlScalar(const QString &line); // 静态：供上面两个静态解析函数调用

    // —— 每网卡出口（见 NicEgress）——
    // 该 IP 落在哪张卡上；-1 = 不在任何已知网段。
    int egressNicFor(const QString &ip) const;
    // 往 proxies: 里追加若干条出站（整块 YAML 文本，含前导 "  - "）。空列表时原样返回。
    QString appendProxies(QString yaml, const QStringList &blocks) const;
    // 取 rules: 块里现有的全部规则行（"  - xxx" 原文，含缩进）。
    static QStringList currentRuleLines(const QString &yaml);
    // 把一条规则行里的目标 DIRECT 改写成 target（只改**目标字段**，不碰 payload 里的字样）。
    static QString retargetDirect(const QString &ruleLine, const QString &target);

    AppConfig m_config;
    QVector<NicEgress> m_egressNics;
};
