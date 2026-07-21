#include "NodeListModel.h"

#include <QColor>
#include <QSet>

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
    case RawNowRole:
        return n.now;
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
        {RawNowRole, "rawNow"},
        {DelayRole, "delay"},
        {BadgeTextRole, "badgeText"},
        {BadgeColorRole, "badgeColor"},
        {ActiveRole, "active"},
    };
}

void NodeListModel::setNodes(const QVector<NodeInfo> &nodes)
{
    // 每秒轮询 / 测速都会走这里：m_all 已由 ClashService 排好序（活动置顶→速度降→延迟升）。
    m_all = nodes;
    reconcile(); // 原地演进，绝不整表重置——保滚动/保选中/药丸原地刷（对齐旧项目 syncNodeRows）
}

void NodeListModel::updateBadges(const QVector<NodeInfo> &nodes)
{
    // 切换加载态专用：核心每秒仍上报节点，但此刻不能重排/改活动/动按钮（会打断转圈与「其余禁用」）。
    // 只把最新的 delay/speed 原地写进药丸——对可见行发「仅药丸角色」的 dataChanged，绝不增删/移动。
    // 同步进 m_all（只改 delay/speed，保持次序/active 不变），使切换确认后的 reconcile 基于最新药丸值。
    QHash<QString, const NodeInfo *> byName;
    byName.reserve(nodes.size());
    for (const NodeInfo &n : nodes)
        byName.insert(n.name, &n);

    for (NodeInfo &a : m_all) {
        auto it = byName.constFind(a.name);
        if (it != byName.constEnd()) {
            a.delay = (*it)->delay;
            a.speed = (*it)->speed;
        }
    }
    for (int i = 0; i < m_visible.size(); ++i) {
        auto it = byName.constFind(m_visible.at(i).name);
        if (it == byName.constEnd())
            continue;
        if (m_visible.at(i).delay != (*it)->delay || m_visible.at(i).speed != (*it)->speed) {
            m_visible[i].delay = (*it)->delay;
            m_visible[i].speed = (*it)->speed;
            emit dataChanged(index(i), index(i), {DelayRole, BadgeTextRole, BadgeColorRole});
        }
    }
}

void NodeListModel::setFilter(const QString &filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    reconcile(); // 搜索过滤同样走增量：只增删差异行，不重置（对齐 ConnectionsModel::recompute）
}

void NodeListModel::reconcile()
{
    // —— 第一步：算出「目标可见列表」——保持 m_all 的排序次序，只按搜索词过滤 ——
    QVector<NodeInfo> target;
    target.reserve(m_all.size());
    for (const NodeInfo &n : m_all) {
        if (m_filter.isEmpty() || n.name.contains(m_filter, Qt::CaseInsensitive))
            target.push_back(n);
    }

    const int prevCount = m_visible.size();

    // 身份键 = 节点名（唯一）。目标名集合，用于判定「哪些现存行已离场」。
    QSet<QString> targetNames;
    targetNames.reserve(target.size());
    for (const NodeInfo &n : target)
        targetNames.insert(n.name);

    // —— Step A：删除已不在目标集合中的行（从后往前，下标稳定）——
    for (int i = m_visible.size() - 1; i >= 0; --i) {
        if (!targetNames.contains(m_visible.at(i).name)) {
            beginRemoveRows(QModelIndex(), i, i);
            m_visible.removeAt(i);
            endRemoveRows();
        }
    }
    // 此后 m_visible 里每一行的 name 都仍在 target 中（只是次序/字段可能与目标不一致）。

    // —— Step B：逐个目标位置对齐：把匹配行移到位（beginMoveRows），或插入新行，再原地改字段 ——
    for (int i = 0; i < target.size(); ++i) {
        const NodeInfo &want = target.at(i);
        // 在 [i, end) 里找 name 匹配的现存行（i 之前的都已就位）。
        int j = -1;
        for (int k = i; k < m_visible.size(); ++k) {
            if (m_visible.at(k).name == want.name) {
                j = k;
                break;
            }
        }
        if (j < 0) {
            // 新节点：插到目标位置 i（对齐旧项目「新行按目标次序出现」）
            beginInsertRows(QModelIndex(), i, i);
            m_visible.insert(i, want);
            endInsertRows();
            continue;
        }
        if (j != i) {
            // 存活行次序变了（多因测速后按速度/延迟重排）：把它上移到 i。绝不销毁重建，故不闪现。
            // j > i 恒成立（i 之前已对齐），上移的 destinationChild 就是 i。
            beginMoveRows(QModelIndex(), j, j, QModelIndex(), i);
            m_visible.move(j, i);
            endMoveRows();
        }
        // 原地更新可变字段（now/delay/speed/active）——仅在真有变化时 dataChanged，避免每秒白刷。
        if (m_visible.at(i) != want) {
            m_visible[i] = want;
            emit dataChanged(index(i), index(i)); // 不带 roles = 全部角色
        }
    }

    if (m_visible.size() != prevCount)
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
