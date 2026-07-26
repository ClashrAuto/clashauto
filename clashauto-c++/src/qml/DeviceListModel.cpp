#include "DeviceListModel.h"

#include <QHostAddress>
#include <QSet>
#include <algorithm>

DeviceListModel::DeviceListModel(QObject *parent) : QAbstractListModel(parent) {}

int DeviceListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant DeviceListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows.at(index.row());
    switch (role) {
    case MacRole:       return r.mac;
    case IpRole:        return r.ip;
    case NameRole:      return r.name;
    case TypeKeyRole:   return r.typeKey;
    case VendorRole:    return r.vendor;
    case OnlineRole:    return r.online;
    case ProxiedRole:   return r.proxied;
    case RateUpRole:    return r.rateUp;
    case RateDownRole:  return r.rateDown;
    // 与 rebuildSelected() 里的流量字段一致，统一以 double 交给 QML（那边 property 是 real）。
    case TodayUpRole:   return static_cast<double>(r.todayUp);
    case TodayDownRole: return static_cast<double>(r.todayDown);
    case IsSelfRole:    return r.isSelf;
    case IsGatewayRole: return r.isGateway;
    case ProxyableRole: return r.proxyable;
    default:            return {};
    }
}

QHash<int, QByteArray> DeviceListModel::roleNames() const
{
    return {
        {MacRole, "mac"},         {IpRole, "ip"},           {NameRole, "name"},
        {TypeKeyRole, "typeKey"}, {VendorRole, "vendor"},   {OnlineRole, "online"},
        {ProxiedRole, "proxied"}, {RateUpRole, "rateUp"},   {RateDownRole, "rateDown"},
        {TodayUpRole, "todayUp"}, {TodayDownRole, "todayDown"},
        {IsSelfRole, "isSelf"},   {IsGatewayRole, "isGateway"},
        {ProxyableRole, "proxyable"},
    };
}

DeviceListModel::Row DeviceListModel::toRow(const DeviceRecord &d)
{
    Row r;
    r.mac = d.mac;
    r.ip = d.ip;
    r.name = d.displayName();
    r.typeKey = DeviceStore::typeKey(d.effectiveType());
    r.vendor = d.vendor;
    r.online = d.online;
    r.proxied = d.proxyEnabled;
    r.isSelf = d.isSelf;
    r.isGateway = d.isGateway;
    r.proxyable = d.proxyable();
    r.rateUp = d.rateUp;
    r.rateDown = d.rateDown;
    r.todayUp = d.todayUp;
    r.todayDown = d.todayDown;
    // IP 排序按**数值**而不是字符串：".9" 必须排在 ".140" 前面，字符串比较会反过来。
    const QHostAddress addr(d.ip);
    r.ipKey = addr.protocol() == QAbstractSocket::IPv4Protocol ? addr.toIPv4Address()
                                                               : 0xFFFFFFFFu; // 没 IP 的排最后
    return r;
}

QString DeviceListModel::macAt(int row) const
{
    return (row >= 0 && row < m_rows.size()) ? m_rows.at(row).mac : QString();
}

void DeviceListModel::setDevices(const QVector<DeviceRecord> &devices)
{
    m_all.clear();
    m_all.reserve(devices.size());
    for (const DeviceRecord &d : devices)
        m_all.append(toRow(d));
    reconcile();
}

void DeviceListModel::setFilter(const QString &query, bool onlineOnly)
{
    if (query == m_query && onlineOnly == m_onlineOnly)
        return;
    m_query = query;
    m_onlineOnly = onlineOnly;
    reconcile();
}

QVector<DeviceListModel::Row> DeviceListModel::buildTarget() const
{
    QVector<Row> t;
    t.reserve(m_all.size());
    for (const Row &r : m_all) {
        if (m_onlineOnly && !r.online)
            continue;
        if (!m_query.isEmpty()) {
            const bool hit = r.name.contains(m_query, Qt::CaseInsensitive)
                             || r.ip.contains(m_query, Qt::CaseInsensitive)
                             || r.mac.contains(m_query, Qt::CaseInsensitive)
                             || r.vendor.contains(m_query, Qt::CaseInsensitive);
            if (!hit)
                continue;
        }
        t.append(r);
    }
    // 排序：在线优先 → 今日流量（MB 档位）降序 → IP 升序 → MAC（兜底定序）。
    // **排序键里绝不能出现实时速率**：速率每一拍都在变，一旦拿它排序，只要有设备在跑流量，
    // 每次刷新都会重新排列 → reconcile 的 beginMoveRows 把行搬来搬去，列表肉眼可见地抖。
    // 今日流量是单调累加的，只会偶尔超车一次；再按 MB 取档（todayBucket）压掉最后一点
    // 「两台用量相近的设备来回互超」的抖动。同档位内按 IP 数值排，同 IP（或都没 IP）按 MAC——
    // 保证任何时候次序都是确定的，不随上游容器的遍历顺序漂移。
    std::stable_sort(t.begin(), t.end(), [](const Row &a, const Row &b) {
        if (a.online != b.online) return a.online > b.online;
        const qint64 ta = a.todayBucket(), tb = b.todayBucket();
        if (ta != tb) return ta > tb;
        if (a.ipKey != b.ipKey) return a.ipKey < b.ipKey;
        return a.mac < b.mac;
    });
    return t;
}

void DeviceListModel::reconcile()
{
    const QVector<Row> target = buildTarget();

    // 目标 mac → 目标行 索引
    QHash<QString, int> targetIndex;
    targetIndex.reserve(target.size());
    for (int i = 0; i < target.size(); ++i)
        targetIndex.insert(target.at(i).mac, i);

    // A：删除已不在目标里的行（从后往前）。
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (!targetIndex.contains(m_rows.at(i).mac)) {
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }

    // B：把存活行按目标次序重排（逐位选择：为每个目标位找到对应 mac 当前所在处，前移到位）。
    // 用 beginMoveRows 保留委托状态/选中高亮，不整表重建。
    int writePos = 0;
    for (const Row &tr : target) {
        int from = -1;
        for (int i = writePos; i < m_rows.size(); ++i) {
            if (m_rows.at(i).mac == tr.mac) { from = i; break; }
        }
        if (from < 0)
            continue; // 目标里的新 mac，B 阶段跳过，留给 C 插入
        if (from != writePos) {
            beginMoveRows(QModelIndex(), from, from, QModelIndex(), writePos);
            m_rows.move(from, writePos);
            endMoveRows();
        }
        ++writePos;
    }

    // C：原地更新可变字段 + 插入新行，按目标次序。
    for (int i = 0; i < target.size(); ++i) {
        const Row &tr = target.at(i);
        if (i < m_rows.size() && m_rows.at(i).mac == tr.mac) {
            if (!m_rows.at(i).sameFields(tr)) {
                m_rows[i] = tr;
                emit dataChanged(index(i), index(i));
            }
        } else {
            beginInsertRows(QModelIndex(), i, i);
            m_rows.insert(i, tr);
            endInsertRows();
        }
    }
    // 若尾部还有多余（理论上 A 已清），保险截断。
    while (m_rows.size() > target.size()) {
        const int last = m_rows.size() - 1;
        beginRemoveRows(QModelIndex(), last, last);
        m_rows.removeAt(last);
        endRemoveRows();
    }

    emit countChanged();
    recomputeStats();
}

void DeviceListModel::recomputeStats()
{
    int online = 0, proxied = 0;
    for (const Row &r : m_all) {
        if (r.online) ++online;
        if (r.proxied) ++proxied;
    }
    if (online != m_onlineCount || proxied != m_proxiedCount) {
        m_onlineCount = online;
        m_proxiedCount = proxied;
        emit statsChanged();
    }
}
