#include "ConnectionsModel.h"

#include <QSet>
#include <QVariantMap>

ConnectionsModel::ConnectionsModel(QObject *parent) : QAbstractListModel(parent) {}

int ConnectionsModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_rows.size();
}

QVariant ConnectionsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Conn &c = m_rows.at(index.row());
    switch (role) {
    case TypeRole:
        return c.type;
    case HostRole:
        return c.host;
    case ChainRole:
        return c.chain;
    case DownloadRole:
        return c.download;
    case UploadRole:
        return c.upload;
    case ConnIdRole:
        return c.id;
    case OfflineRole:
        return c.offline;
    default:
        return {};
    }
}

QHash<int, QByteArray> ConnectionsModel::roleNames() const
{
    return {
        {TypeRole, "type"},
        {HostRole, "host"},
        {ChainRole, "chain"},
        {DownloadRole, "download"},
        {UploadRole, "upload"},
        {ConnIdRole, "connId"},
        {OfflineRole, "offline"},
    };
}

void ConnectionsModel::setRaw(const QVariantList &conns)
{
    const int prevCount = m_raw.size();
    m_raw.clear();
    m_raw.reserve(conns.size());
    int online = 0;
    for (const QVariant &v : conns) {
        const QVariantMap m = v.toMap();
        Conn c;
        c.type = m.value("type").toString();
        c.host = m.value("host").toString();
        c.chain = m.value("chain").toString();
        c.id = m.value("id").toString();
        c.download = m.value("download").toLongLong();
        c.upload = m.value("upload").toLongLong();
        c.offline = m.value("offline").toBool();
        if (!c.offline)
            ++online;
        m_raw.append(c);
    }
    if (online != m_onlineTotal) {
        m_onlineTotal = online;
        emit onlineTotalChanged();
    }
    if (m_raw.size() != prevCount)
        emit countChanged();
    recompute();
}

void ConnectionsModel::setFilter(bool showOnline, bool showOffline, const QString &query)
{
    if (showOnline == m_showOnline && showOffline == m_showOffline && query == m_query)
        return;
    m_showOnline = showOnline;
    m_showOffline = showOffline;
    m_query = query;
    recompute();
}

void ConnectionsModel::recompute()
{
    // 第一遍：算出过滤后的目标列表（保持 m_raw 的原始次序），并建 id → 下标 索引。
    QHash<QString, int> targetIndex;
    QVector<Conn> filtered;
    filtered.reserve(m_raw.size());
    targetIndex.reserve(m_raw.size());
    for (const Conn &c : m_raw) {
        if (c.offline && !m_showOffline)
            continue;
        if (!c.offline && !m_showOnline)
            continue;
        if (!m_query.isEmpty() && !c.host.contains(m_query, Qt::CaseInsensitive))
            continue;
        targetIndex.insert(c.id, filtered.size());
        filtered.append(c);
    }

    // Step A：删除已不在目标集合中的行（从后往前，保证下标稳定）。
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (!targetIndex.contains(m_rows.at(i).id)) {
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }

    // Step B：原地更新存活行的可变字段（下载/上传/离线等），仅在有变化时 dataChanged。
    //   删除后 m_rows 里存活行的相对次序不变，与目标里这些 id 的次序一致——无需移动。
    for (int i = 0; i < m_rows.size(); ++i) {
        const Conn &nw = filtered.at(targetIndex.value(m_rows.at(i).id));
        if (!m_rows.at(i).sameFields(nw)) {
            m_rows[i] = nw;
            emit dataChanged(index(i), index(i)); // 不传 roles = 全部角色
        }
    }

    // Step C：把新出现的 id（目标里、当前不存在的）按目标次序追加到末尾。
    QSet<QString> present;
    present.reserve(m_rows.size());
    for (const Conn &c : m_rows)
        present.insert(c.id);
    QVector<Conn> toAppend;
    for (const Conn &c : filtered) {
        if (!present.contains(c.id))
            toAppend.append(c);
    }
    if (!toAppend.isEmpty()) {
        beginInsertRows(QModelIndex(), m_rows.size(), m_rows.size() + toAppend.size() - 1);
        m_rows += toAppend;
        endInsertRows();
    }
}
