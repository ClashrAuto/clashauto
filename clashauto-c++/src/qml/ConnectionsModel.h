#pragma once

// QML 适配器：把「全部连接」弹窗的连接列表暴露成一个可过滤的 QAbstractListModel。
// 复刻旧项目 MainWindow::showConnectionsDialog 的 P4「热更新」：按连接 id 保留卡片行，
// 可见 id 集合不变时只原地更新已有行的下载/上传/离线（dataChanged），集合变化时才
// 增删行（begin/endInsert/RemoveRows）——这样 ListView 每秒刷新不会把滚动位置弹回顶部。
// 风格对齐 NodeListModel（薄绑定层，不改后端）。
#include <QAbstractListModel>
#include <QString>
#include <QVariantList>
#include <QVector>

class ConnectionsModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)             // 全量连接数（Offline(N) = count - onlineTotal）
    Q_PROPERTY(int onlineTotal READ onlineTotal NOTIFY onlineTotalChanged) // 全量中 !offline 的数量（Online(N)）

public:
    enum Roles {
        TypeRole = Qt::UserRole + 1,
        HostRole,
        ChainRole,
        DownloadRole, // qlonglong 原始字节
        UploadRole,   // qlonglong 原始字节
        ConnIdRole,   // 连接 id（closeConnectionById 用）
        OfflineRole
    };

    explicit ConnectionsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_raw.size(); }
    int onlineTotal() const { return m_onlineTotal; }

    // 最新一次全量拉取（每项 QVariantMap {type,host,chain,download,upload,id,offline}）——增量套用。
    void setRaw(const QVariantList &conns);
    // 过滤状态（在线/离线开关 + host 子串搜索）——增量套用。
    Q_INVOKABLE void setFilter(bool showOnline, bool showOffline, const QString &query);

signals:
    void countChanged();
    void onlineTotalChanged();

private:
    struct Conn {
        QString type;
        QString host;
        QString chain;
        QString id;
        qlonglong download = 0;
        qlonglong upload = 0;
        bool offline = false;
        // 除 id 外的可变字段是否一致（id 相同才会比较，用于判断是否需 dataChanged）
        bool sameFields(const Conn &o) const
        {
            return type == o.type && host == o.host && chain == o.chain
                   && download == o.download && upload == o.upload && offline == o.offline;
        }
    };

    // 把当前可见行 m_rows 增量变形成过滤后的目标列表（保留已有行次序、新 id 追加末尾、删除消失的 id）。
    void recompute();

    QVector<Conn> m_raw;   // 全量（未过滤）
    QVector<Conn> m_rows;  // 当前可见行（ListView 消费）
    bool m_showOnline = true;
    bool m_showOffline = true;
    QString m_query;
    int m_onlineTotal = 0;
};
