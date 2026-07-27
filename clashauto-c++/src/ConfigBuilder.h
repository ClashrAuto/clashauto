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

    // 从 YAML 文本提取全部节点名 / 策略组名。静态公开：规则编辑器「节点」下拉
    // 需要读 full.yaml 列出全部候选（SettingsController::proxyGroupNames）。
    static QStringList proxyNames(const QString &yaml);
    static QStringList existingGroupNames(const QString &yaml);

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
};
