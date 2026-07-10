#pragma once

#include "AppConfig.h"

#include <QObject>
#include <QNetworkAccessManager>
#include <QMap>
#include <QString>
#include <QVector>

struct SubscriptionSummary {
    QString name;
    QString url;
    QString type;
    bool use = false;
    int nodeCount = 0;
    int enabledNodeCount = 0;
};

struct SubscriptionNodeSummary {
    QString name;
    QString server;
    QString port;
    bool use = true;
};

class SubscriptionStore final : public QObject
{
    Q_OBJECT

public:
    explicit SubscriptionStore(AppConfig config, QObject *parent = nullptr);

    QVector<SubscriptionSummary> load();
    QVector<SubscriptionNodeSummary> nodes(int subscriptionIndex);
    bool setNodeEnabled(int subscriptionIndex, int nodeIndex, bool enabled);
    bool setAllNodesEnabled(int subscriptionIndex, bool enabled);
    bool setSubscriptionEnabled(int index, bool enabled);
    bool addSubscription(const QString &name, const QString &url, const QString &type);
    bool editSubscription(int index, const QString &name, const QString &url, const QString &type);
    bool removeSubscription(int index);
    void updateSubscription(int index);
    bool updateSubscriptionFromTextForTest(int index, const QString &yaml, QString *message = nullptr);
    QString effectiveUrlForTest(int index);
    QString path() const;

signals:
    void subscriptionUpdated(int index, bool ok, QString message);

private:
    void ensureFile() const;
    QString readText() const;
    bool writeText(const QString &text) const;
    SubscriptionSummary summaryAt(int index);
    bool replaceSubscriptionList(int index, const QString &nodeListYaml);
    QMap<QString, bool> existingNodeUseByEndpoint(int index) const;
    QMap<QString, QString> existingNodeYamlByEndpoint(int index) const;
    QString parseProxyList(const QString &yaml, const QMap<QString, bool> &previousUse, const QMap<QString, QString> &previousYaml, int *count) const;
    void updateSubscriptionFromText(int index, const QString &yaml);
    bool nodeAllowed(const QString &name) const;
    QUrl effectiveUrl(const SubscriptionSummary &summary) const;
    QString scalar(const QString &value) const;
    QString quote(const QString &value) const;

    AppConfig m_config;
    QNetworkAccessManager m_network;
};
