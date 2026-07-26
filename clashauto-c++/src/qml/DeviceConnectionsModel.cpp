#include "DeviceConnectionsModel.h"

#include <QSet>
#include <QVariantMap>

DeviceConnectionsModel::DeviceConnectionsModel(QObject *parent) : QAbstractListModel(parent) {}

int DeviceConnectionsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant DeviceConnectionsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Conn &c = m_rows.at(index.row());
    switch (role) {
    case HostRole:     return c.host;
    case ChainRole:    return c.chain;
    case TypeRole:     return c.type;
    case DownloadRole: return c.download;
    case UploadRole:   return c.upload;
    case ConnIdRole:   return c.id;
    default:           return {};
    }
}

QHash<int, QByteArray> DeviceConnectionsModel::roleNames() const
{
    return {
        {HostRole, "host"},         {ChainRole, "chain"}, {TypeRole, "type"},
        {DownloadRole, "download"}, {UploadRole, "upload"}, {ConnIdRole, "connId"},
    };
}

void DeviceConnectionsModel::setSourceIp(const QString &ip)
{
    if (ip == m_sourceIp)
        return;
    m_sourceIp = ip;
    recompute();
}

void DeviceConnectionsModel::setUser(const QString &user)
{
    if (user == m_user)
        return;
    m_user = user;
    recompute();
}

void DeviceConnectionsModel::setRaw(const QVariantList &conns)
{
    m_raw = conns;
    recompute();
}

void DeviceConnectionsModel::recompute()
{
    // 目标：m_raw 中「sourceIP==m_sourceIp 或 inboundUser==m_user」且在线的连接。后者覆盖透明网关
    // 代理的设备（其连接 sourceIP 恒为 127.0.0.1，只能靠 dev-<mac> 用户名归属）。
    QHash<QString, int> targetIndex;
    QVector<Conn> filtered;
    if (!m_sourceIp.isEmpty() || !m_user.isEmpty()) {
        for (const QVariant &v : m_raw) {
            const QVariantMap m = v.toMap();
            const bool byIp = !m_sourceIp.isEmpty()
                              && m.value("sourceIP").toString() == m_sourceIp;
            const bool byUser = !m_user.isEmpty()
                                && m.value("inboundUser").toString() == m_user;
            if (!byIp && !byUser)
                continue;
            if (m.value("offline").toBool())
                continue;
            Conn c;
            c.host = m.value("host").toString();
            c.chain = m.value("chain").toString();
            c.type = m.value("type").toString();
            c.id = m.value("id").toString();
            c.download = m.value("download").toLongLong();
            c.upload = m.value("upload").toLongLong();
            targetIndex.insert(c.id, filtered.size());
            filtered.append(c);
        }
    }

    // A：删已消失的
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (!targetIndex.contains(m_rows.at(i).id)) {
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }
    // B：原地刷存活行
    for (int i = 0; i < m_rows.size(); ++i) {
        const Conn &nw = filtered.at(targetIndex.value(m_rows.at(i).id));
        if (!m_rows.at(i).sameFields(nw)) {
            m_rows[i] = nw;
            emit dataChanged(index(i), index(i));
        }
    }
    // C：追加新连接
    QSet<QString> present;
    for (const Conn &c : m_rows)
        present.insert(c.id);
    QVector<Conn> toAppend;
    for (const Conn &c : filtered)
        if (!present.contains(c.id))
            toAppend.append(c);
    if (!toAppend.isEmpty()) {
        beginInsertRows(QModelIndex(), m_rows.size(), m_rows.size() + toAppend.size() - 1);
        m_rows += toAppend;
        endInsertRows();
    }
    emit countChanged();
}
