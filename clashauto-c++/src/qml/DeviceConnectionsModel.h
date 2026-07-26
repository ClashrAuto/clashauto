#pragma once

// 单个设备的实时连接列表（详情页下半）。与 ConnectionsModel 同构的增量模型，但按 **sourceIP**
// 过滤：只显示选中设备发起的连接。热更新时按连接 id 原地刷流量、集合变化才增删行（保滚动位置）。
//
// 数据来自 QmlBridge 每秒轮询的 /connections 原始表（每项 QVariantMap 含 sourceIP/host/chain/
// download/upload/id/offline/inboundUser）。M0：sourceIP 归属；M1 起可切 inboundUser。
#include <QAbstractListModel>
#include <QString>
#include <QVariantList>
#include <QVector>

class DeviceConnectionsModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        HostRole = Qt::UserRole + 1,
        ChainRole,
        TypeRole,
        DownloadRole,
        UploadRole,
        ConnIdRole,
    };
    explicit DeviceConnectionsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_rows.size(); }

    // 设置目标设备 IP（切换选中设备时调用；空则清空）。
    void setSourceIp(const QString &ip);
    // 设置目标设备的网关 SOCKS 用户名(dev-<mac>)。透明网关代理的连接 sourceIP 恒为 127.0.0.1,
    // 只能靠 inboundUser 归属;与 sourceIp 取「或」匹配。切换选中设备时和 setSourceIp 一起调。
    void setUser(const QString &user);
    // 喂入全量连接原始表（含 sourceIP / inboundUser）。
    void setRaw(const QVariantList &conns);

signals:
    void countChanged();

private:
    struct Conn {
        QString host, chain, type, id;
        qlonglong download = 0, upload = 0;
        bool sameFields(const Conn &o) const
        {
            return host == o.host && chain == o.chain && type == o.type
                   && download == o.download && upload == o.upload;
        }
    };
    void recompute();

    QVariantList m_raw;
    QString m_sourceIp;
    QString m_user; // 目标设备的 dev-<mac> 用户名（网关代理连接靠它归属）
    QVector<Conn> m_rows;
};
