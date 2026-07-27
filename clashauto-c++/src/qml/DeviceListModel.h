#pragma once

// QML 适配器：把 DeviceStore 的设备列表暴露成可过滤的 QAbstractListModel，供 DevicesPage 左列表消费。
// 增量更新（对齐 NodeListModel/ConnectionsModel）：以 **MAC 为身份**，可见集合不变时只 dataChanged
// 原地刷（在线/IP/速率/名称等），集合变化才 begin/endInsert/RemoveRows —— 热更新不弹滚动位置。
#include "../DeviceStore.h"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QVariantList>
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
        TodayUpRole,   // 今日累计上行字节（列表右侧常驻显示 + 排序主键）
        TodayDownRole, // 今日累计下行字节
        IsSelfRole,
        IsGatewayRole,
        ProxyableRole,  // 能否开代理（非本机/网关 + 在可劫持网段内）
        LastHostRole,   // 最后访问的地址（行最下面那行）
        RateUpHistRole, // 近 kHistPoints 拍的上/下行速率（行背景那张实时流量图）
        RateDownHistRole,
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
        QString mac, ip, name, typeKey, vendor, lastHost;
        bool online = false, proxied = false, isSelf = false, isGateway = false;
        bool proxyable = false;
        qint64 rateUp = 0, rateDown = 0;
        qint64 todayUp = 0, todayDown = 0;
        quint32 ipKey = 0xFFFFFFFFu; // ip 的数值形式（排序用；无/非 IPv4 排最后）
        bool sameFields(const Row &o) const
        {
            return ip == o.ip && name == o.name && typeKey == o.typeKey && vendor == o.vendor
                   && online == o.online && proxied == o.proxied && isSelf == o.isSelf
                   && isGateway == o.isGateway && proxyable == o.proxyable
                   && rateUp == o.rateUp && rateDown == o.rateDown
                   && todayUp == o.todayUp && todayDown == o.todayDown
                   && lastHost == o.lastHost;
        }
        // 排序用的今日流量档位：今日上下行之和按 MB 取整。用档位而不是精确字节，是为了不让
        // 两台用量相近的设备每来一拍就互相超车换位（列表又会抖）；档位每涨 1MB 才可能换一次位。
        qint64 todayBucket() const { return (todayUp + todayDown) >> 20; }
    };
    static Row toRow(const DeviceRecord &d);
    // 排序键：在线优先 → 今日流量（MB 档位）降序 → IP 升序 → MAC。**不含实时速率**：速率每拍
    // 都在变，拿它排序会让跑流量的设备一直换位置（列表抖个不停）。返回已排序+过滤的目标行。
    QVector<Row> buildTarget() const;
    void reconcile();
    void recomputeStats();

    // —— 每设备的实时速率历史（行背景那张图）——
    // 每台设备一条定长（kHistPoints）环形缓冲，**存在模型里而不是委托里**：ListView 会回收/销毁
    // 滚出视口的委托，历史挂在委托上滚一下就没了。setDevices 每次被调用不一定是「一拍」——
    // 台账任何变化（改别名、扫描结果）都会走到这里，所以按 kHistMinIntervalMs 节流，
    // 保证大致每秒一个样本，图的横轴才是时间。
    struct Hist {
        QVector<qint64> up, down;
    };
    void sampleHistory(const QVector<DeviceRecord> &devices);
    QVariantList histList(const QVector<qint64> &v) const;

    QVector<Row> m_all;     // 全量（未过滤）
    QVector<Row> m_rows;    // 可见行
    QString m_query;
    bool m_onlineOnly = false;
    int m_onlineCount = 0;
    int m_proxiedCount = 0;
    QHash<QString, Hist> m_hist; // mac → 速率历史
    QElapsedTimer m_histClock;
    static constexpr int kHistPoints = 40;
    static constexpr int kHistMinIntervalMs = 800;
};
