#pragma once

// QML 适配器：把 DeviceStore 的设备列表暴露成可过滤的 QAbstractListModel，供 DevicesPage 左列表消费。
// 增量更新（对齐 NodeListModel/ConnectionsModel）：以 **MAC 为身份**，可见集合不变时只 dataChanged
// 原地刷（在线/IP/速率/名称等），集合变化才 begin/endInsert/RemoveRows —— 热更新不弹滚动位置。
#include "../DeviceStore.h"

#include <QAbstractListModel>
#include <QString>
#include <QVector>

class DeviceListModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCountProp NOTIFY countChanged)
    Q_PROPERTY(int onlineCount READ onlineCount NOTIFY statsChanged)
    Q_PROPERTY(int proxiedCount READ proxiedCount NOTIFY statsChanged)

public:
    enum Roles {
        MacRole = Qt::UserRole + 1,
        IpRole,
        NameRole,      // 展示名（别名>自动名>型号>厂商>IP）
        TypeKeyRole,   // 类型 key（phone/router/... 供选图标 + i18n）
        VendorRole,
        OnlineRole,
        ProxiedRole,   // 代理开关
        RateUpRole,
        RateDownRole,
        IsSelfRole,
        IsGatewayRole,
        ProxyableRole, // 能否开代理（非本机/网关 + 在可劫持网段内）
    };

    explicit DeviceListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int rowCountProp() const { return m_rows.size(); }
    int onlineCount() const { return m_onlineCount; }
    int proxiedCount() const { return m_proxiedCount; }

    // 由 DevicesController 在 store 变化时调用：传入全量记录（已按显示序排好）。
    void setDevices(const QVector<DeviceRecord> &devices);
    // 过滤：搜索子串 + 仅在线。
    Q_INVOKABLE void setFilter(const QString &query, bool onlineOnly);
    // 行 → MAC（详情选择用）。
    Q_INVOKABLE QString macAt(int row) const;

signals:
    void countChanged();
    void statsChanged();

private:
    // 一行的展示快照 + 变更比对。
    struct Row {
        QString mac, ip, name, typeKey, vendor;
        bool online = false, proxied = false, isSelf = false, isGateway = false;
        bool proxyable = false;
        qint64 rateUp = 0, rateDown = 0;
        bool sameFields(const Row &o) const
        {
            return ip == o.ip && name == o.name && typeKey == o.typeKey && vendor == o.vendor
                   && online == o.online && proxied == o.proxied && isSelf == o.isSelf
                   && isGateway == o.isGateway && proxyable == o.proxyable
                   && rateUp == o.rateUp && rateDown == o.rateDown;
        }
    };
    static Row toRow(const DeviceRecord &d);
    // 排序键：在线优先 → 本机/网关置顶 → 速率降 → 名称。返回已排序+过滤的目标行。
    QVector<Row> buildTarget() const;
    void reconcile();
    void recomputeStats();

    QVector<Row> m_all;     // 全量（未过滤）
    QVector<Row> m_rows;    // 可见行
    QString m_query;
    bool m_onlineOnly = false;
    int m_onlineCount = 0;
    int m_proxiedCount = 0;
};
