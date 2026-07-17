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
    int updateTime = 0; // 该订阅的自动更新周期（分钟）；0 表示沿用全局默认
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
    bool setSubscriptionUpdateTime(int index, int minutes);
    bool addSubscription(const QString &name, const QString &url, const QString &type);
    bool editSubscription(int index, const QString &name, const QString &url, const QString &type);
    bool removeSubscription(int index);
    void updateSubscription(int index);
    bool updateSubscriptionFromTextForTest(int index, const QString &yaml, QString *message = nullptr);
    QString effectiveUrlForTest(int index);
    QString path() const;

signals:
    // changed=false 表示更新成功但内容与上次完全一致（含节点启用状态）——
    // 调用方可据此跳过 full.yaml 重建与核心热重载（自动更新周期短时避免无谓重载）
    void subscriptionUpdated(int index, bool ok, QString message, bool changed = true);

private:
    void ensureFile() const;
    QString readText() const;
    bool writeText(const QString &text) const;
    SubscriptionSummary summaryAt(int index);
    bool replaceSubscriptionList(int index, const QString &nodeListYaml, bool *changed = nullptr);
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
