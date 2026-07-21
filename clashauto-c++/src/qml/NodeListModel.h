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
        RawNowRole,                     // node.now，组行禁用其实际使用叶子用（对齐旧项目 disableNodeByName）
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
    // 切换加载态中：仅原地刷延迟/速度药丸，不重排/不改 active/不增删（对齐旧项目 updateNodeBadges）。
    void updateBadges(const QVector<NodeInfo> &nodes);
    // 用当前已存数据（m_all + 过滤）重新对账一次（切换兜底结束后重渲染活动节点/按钮用）。
    void resync() { reconcile(); }
    // 搜索过滤（大小写不敏感子串匹配）
    void setFilter(const QString &filter);

signals:
    void countChanged();

private:
    // 增量对账：把当前可见行 m_visible 原地演进到「按 m_all 次序过滤后的目标列表」——
    // 删除离场行、把存活行移到目标次序、原地改可变字段、新行插到对应位置。绝不 beginResetModel，
    // 因此 ListView 的滚动位置/委托状态/选中高亮全部保留（对齐旧项目 syncNodeRows）。
    void reconcile();
    static QString badgeColor(int delay);
    static QString speedText(qint64 value);

    QVector<NodeInfo> m_all;      // 全量（未过滤，已由 ClashService 按 活动置顶/速度降/延迟升 排好序）
    QVector<NodeInfo> m_visible;  // 过滤后的可见行（模型实际暴露给 QML 的行）
    QString m_filter;
};
