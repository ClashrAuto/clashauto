#include "SubscriptionsController.h"

#include "../CoreController.h"

#include <QVariantMap>

SubscriptionsController::SubscriptionsController(SubscriptionStore *store, CoreController *core, QObject *parent)
    : QAbstractListModel(parent), m_store(store), m_core(core)
{
    // 「更新所有订阅」会连续触发多次 subscriptionUpdated；把 rebuildConfig（生成 full.yaml + 热重载核心）
    // 经 500ms single-shot 去抖合并——500ms 内的多次变更只重建/热重载一次（对齐 Widgets 版）。
    m_rebuildTimer.setSingleShot(true);
    connect(&m_rebuildTimer, &QTimer::timeout, this, [this] {
        if (m_core)
            m_core->rebuildConfig();
    });

    if (m_store) {
        connect(m_store, &SubscriptionStore::subscriptionUpdated, this,
                [this](int, bool ok, const QString &message, bool changed) {
            emit logMessage(message);
            reload();
            // 内容没变就不重建 full.yaml、不热重载核心（自动更新周期短时避免无谓重载）。
            if (ok && changed)
                m_rebuildTimer.start(500);
        });
    }

    reload();
}

int SubscriptionsController::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_items.size();
}

QVariant SubscriptionsController::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const SubscriptionSummary &s = m_items.at(index.row());
    switch (role) {
    case NameRole:
        return s.name.isEmpty() ? s.url : s.name;
    case UrlRole:
        return s.url;
    case TypeRole:
        return s.type.isEmpty() ? QStringLiteral("sub") : s.type;
    case UseRole:
        return s.use;
    case NodeCountRole:
        return s.nodeCount;
    case EnabledNodeCountRole:
        return s.enabledNodeCount;
    case UpdateTimeRole:
        return s.updateTime;
    default:
        return {};
    }
}

QHash<int, QByteArray> SubscriptionsController::roleNames() const
{
    return {
        {NameRole, "name"},
        {UrlRole, "url"},
        {TypeRole, "type"},
        {UseRole, "use"},
        {NodeCountRole, "nodeCount"},
        {EnabledNodeCountRole, "enabledNodeCount"},
        {UpdateTimeRole, "updateTime"},
    };
}

void SubscriptionsController::reload()
{
    if (!m_store)
        return;
    beginResetModel();
    m_items = m_store->load();
    endResetModel();
    emit countChanged();
}

bool SubscriptionsController::add(const QString &name, const QString &url, const QString &type)
{
    if (!m_store || url.trimmed().isEmpty())
        return false;
    const QString t = type.isEmpty() ? QStringLiteral("sub") : type;
    if (!m_store->addSubscription(name.trimmed(), url.trimmed(), t))
        return false;
    reload();
    emit logMessage(QString::fromUtf8("已添加订阅"));
    return true;
}

bool SubscriptionsController::edit(int index, const QString &name, const QString &url, const QString &type, int updateTime)
{
    if (!m_store || url.trimmed().isEmpty())
        return false;
    const QString t = type.isEmpty() ? QStringLiteral("sub") : type;
    if (!m_store->editSubscription(index, name.trimmed(), url.trimmed(), t))
        return false;
    m_store->setSubscriptionUpdateTime(index, updateTime);
    reload();
    triggerRebuild();
    emit logMessage(QString::fromUtf8("订阅已更新（自动更新周期 %1 分钟）").arg(updateTime));
    return true;
}

bool SubscriptionsController::remove(int index)
{
    if (!m_store || !m_store->removeSubscription(index))
        return false;
    reload();
    triggerRebuild();
    emit logMessage(QString::fromUtf8("已删除订阅"));
    return true;
}

bool SubscriptionsController::setEnabled(int index, bool enabled)
{
    if (!m_store || !m_store->setSubscriptionEnabled(index, enabled))
        return false;
    reload();
    triggerRebuild();
    return true;
}

void SubscriptionsController::update(int index)
{
    if (!m_store)
        return;
    emit logMessage(QString::fromUtf8("正在更新订阅..."));
    m_store->updateSubscription(index); // 异步；完成经 subscriptionUpdated 回刷
}

void SubscriptionsController::updateAll()
{
    if (!m_store)
        return;
    const int n = m_items.size();
    for (int i = 0; i < n; ++i)
        m_store->updateSubscription(i);
    emit logMessage(QString::fromUtf8("正在更新全部订阅..."));
}

void SubscriptionsController::apply()
{
    if (m_core)
        m_core->rebuildConfig();
}

QVariantList SubscriptionsController::nodesOf(int index)
{
    QVariantList out;
    if (!m_store)
        return out;
    const QVector<SubscriptionNodeSummary> nodes = m_store->nodes(index);
    out.reserve(nodes.size());
    for (const SubscriptionNodeSummary &n : nodes) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), n.name);
        m.insert(QStringLiteral("server"), n.server);
        m.insert(QStringLiteral("port"), n.port);
        m.insert(QStringLiteral("use"), n.use);
        out.push_back(m);
    }
    return out;
}

bool SubscriptionsController::setNodeEnabled(int index, int nodeIndex, bool enabled)
{
    if (!m_store || !m_store->setNodeEnabled(index, nodeIndex, enabled))
        return false;
    reload();
    triggerRebuild();
    return true;
}

bool SubscriptionsController::setAllNodesEnabled(int index, bool enabled)
{
    if (!m_store || !m_store->setAllNodesEnabled(index, enabled))
        return false;
    reload();
    triggerRebuild();
    return true;
}

void SubscriptionsController::triggerRebuild()
{
    if (m_core)
        m_rebuildTimer.start(500);
}
