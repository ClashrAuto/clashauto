#pragma once

// QML 适配器：把 ClashService 的 QVector<NodeInfo> 暴露成一个可过滤的 list model，
// 供 StatusPage 的 ListView 消费。属于「薄绑定层」——不改后端，只把已有数据整形给 QML。
#include "../ClashService.h"

#include <QAbstractListModel>
#include <QVector>

class NodeListModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCountProp NOTIFY countChanged)

public:
    enum Roles {
        DisplayRole = Qt::UserRole + 1, // 「组名 → 使用中节点」或裸节点名
        RawNameRole,                    // node.name，selectNode 用
        DelayRole,                      // int 毫秒（排序/过滤用）
        BadgeTextRole,                  // "速度/延迟" 药丸文本
        BadgeColorRole,                 // 药丸背景色（#AARRGGBB）
        ActiveRole                      // 是否当前使用中
    };

    explicit NodeListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int rowCountProp() const { return rowCount(); }

    // 由 QmlBridge 在 ClashService::nodesUpdated 时调用
    void setNodes(const QVector<NodeInfo> &nodes);
    // 搜索过滤（大小写不敏感子串匹配）
    void setFilter(const QString &filter);

signals:
    void countChanged();

private:
    void rebuild();
    static QString badgeColor(int delay);
    static QString speedText(qint64 value);

    QVector<NodeInfo> m_all;      // 全量（未过滤）
    QVector<NodeInfo> m_visible;  // 过滤后的可见行
    QString m_filter;
};
