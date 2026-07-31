#pragma once

#include "AppConfig.h"

#include <QMap>
#include <QString>

class ConfigBuilder
{
public:
    explicit ConfigBuilder(AppConfig config);

    QString ensureFullConfig(bool tunEnabled);
    bool writeTunEnabled(const QString &filePath, bool enabled) const;

    // 「全部切换到进程内」的端口换位：进程内入站占走 mixedPort 时，核心的 mixed-port 要挪到
    // 别的端口（mixedPort+1，只作回退出口）。>0 = 覆盖 full.yaml 里写的 mixed-port；0 = 还原为
    // m_config.mixedPort。只影响**下一次** ensureFullConfig 的产物，调用方负责热重载/重启核心。
    void setCoreMixedPort(int port) { m_coreMixedPort = port; }
    int coreMixedPort() const { return m_coreMixedPort > 0 ? m_coreMixedPort : m_config.mixedPort; }

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
    QString normalizeEmptyProxies(QString yaml) const;
    QString yamlQuote(const QString &value) const;
    static QString yamlScalar(const QString &line); // 静态：供上面两个静态解析函数调用

    AppConfig m_config;
    int m_coreMixedPort = 0; // >0 = 覆盖核心的 mixed-port（端口换位时 = mixedPort+1），见 setCoreMixedPort
};
