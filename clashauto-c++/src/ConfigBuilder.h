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
    QString replaceTopLevelProxies(QString yaml, const QString &proxyBlock) const;
    QString replaceProxyListAt(QString yaml, qsizetype proxiesKey, const QStringList &values) const;
    QString appendSubscriptionGroups(QString yaml, const QVector<Subscription> &subscriptions) const;
    QMap<QString, QStringList> autoGroups(const QStringList &nodeNames) const;
    QString setScalar(QString yaml, const QString &key, const QString &value) const;
    QString setNestedScalar(QString yaml, const QString &section, const QString &key, const QString &value) const;
    QString normalizeEmptyProxies(QString yaml) const;
    QString yamlQuote(const QString &value) const;
    QString yamlScalar(const QString &line) const;

    AppConfig m_config;
};
