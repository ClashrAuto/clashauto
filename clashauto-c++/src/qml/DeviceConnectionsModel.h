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
    // 喂入全量连接原始表（含 sourceIP）。
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
    QVector<Conn> m_rows;
};
