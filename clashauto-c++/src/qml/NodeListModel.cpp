#include "NodeListModel.h"

#include <QColor>

NodeListModel::NodeListModel(QObject *parent) : QAbstractListModel(parent) {}

int NodeListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_visible.size();
}

QVariant NodeListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};
    const NodeInfo &n = m_visible.at(index.row());
    switch (role) {
    case DisplayRole: {
        if (!n.now.isEmpty() && n.now != n.name)
            return QString::fromUtf8("%1 → %2").arg(n.name, n.now);
        return n.name;
    }
    case RawNameRole:
        return n.name;
    case DelayRole:
        return n.delay;
    case BadgeTextRole:
        return QString("%1/%2").arg(speedText(n.speed), n.delay == 0 ? QStringLiteral("-") : QString::number(n.delay));
    case BadgeColorRole:
        return badgeColor(n.delay);
    case ActiveRole:
        return n.active;
    default:
        return {};
    }
}

QHash<int, QByteArray> NodeListModel::roleNames() const
{
    return {
        {DisplayRole, "display"},
        {RawNameRole, "rawName"},
        {DelayRole, "delay"},
        {BadgeTextRole, "badgeText"},
        {BadgeColorRole, "badgeColor"},
        {ActiveRole, "active"},
    };
}

void NodeListModel::setNodes(const QVector<NodeInfo> &nodes)
{
    m_all = nodes;
    rebuild();
}

void NodeListModel::setFilter(const QString &filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    rebuild();
}

void NodeListModel::rebuild()
{
    // 简化实现：每次全量重建（phase 0 够用；后续可换 diff / QSortFilterProxyModel）。
    beginResetModel();
    m_visible.clear();
    m_visible.reserve(m_all.size());
    for (const NodeInfo &n : m_all) {
        if (m_filter.isEmpty() || n.name.contains(m_filter, Qt::CaseInsensitive))
            m_visible.push_back(n);
    }
    endResetModel();
    emit countChanged();
}

QString NodeListModel::badgeColor(int delay)
{
    // 复刻 MainWindow::delayColor：未测/超时→半透明白；否则由延迟在绿↔红间取色。
    if (delay <= 0)
        return QColor(255, 255, 255, 50).name(QColor::HexArgb);
    const int red = qMin(255, delay * 65535 / 1200 / 255);
    const int green = qMax(0, 255 - red);
    return QColor(red, green, 0, 230).name(QColor::HexArgb);
}

QString NodeListModel::speedText(qint64 value)
{
    double number = static_cast<double>(qMax<qint64>(0, value));
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (number >= 1024.0 && unit + 1 < 5) {
        number /= 1024.0;
        ++unit;
    }
    return QString::number(number, 'f', 2) + " " + units[unit];
}
