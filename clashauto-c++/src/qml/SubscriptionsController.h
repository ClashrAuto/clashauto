#pragma once

// QML 适配器：把 SubscriptionStore（订阅 YAML 的增删改查 / 远程拉取 / 节点开关）暴露成一个
// 可供 QML ListView 消费的 QAbstractListModel + 一组 Q_INVOKABLE 动作。属于「薄绑定层」——
// 不改后端 SubscriptionStore，只把它的 QVector<SubscriptionSummary> 整形给 QML，并把订阅变更
// 后的「重建 full.yaml + 热重载核心」经 CoreController 触发（对齐 Widgets 版 MainWindow 行为）。
//
// 生命周期：SubscriptionStore / CoreController 由 main_qml.cpp 拥有；本类只持裸指针、不接管所有权。
#include <QAbstractListModel>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>

#include "../SubscriptionStore.h"

class CoreController;

class SubscriptionsController final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCountProp NOTIFY countChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1, // 订阅名（空则回退 URL）
        UrlRole,                     // 订阅 URL / 本地路径
        TypeRole,                    // "sub" / "clash"
        UseRole,                     // 是否启用（bool）
        NodeCountRole,               // 总节点数
        EnabledNodeCountRole,        // 启用节点数
        UpdateTimeRole               // 自动更新周期（分钟，0=全局默认）
    };

    // core 可为 nullptr（仅只读列表时）；非空则订阅变更后重建 full.yaml + 热重载核心。
    explicit SubscriptionsController(SubscriptionStore *store, CoreController *core, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int rowCountProp() const { return rowCount(); }

    // —— UI 动作 ——（对齐 MainWindow::createSubscriptionCard / reloadSubscriptions / showSubscriptionNodes）
    Q_INVOKABLE void reload();                    // 从 store 重载列表并增量刷新 model（热更新，不整表重置）
    Q_INVOKABLE bool add(const QString &name, const QString &url, const QString &type);
    Q_INVOKABLE bool edit(int index, const QString &name, const QString &url, const QString &type, int updateTime);
    Q_INVOKABLE bool remove(int index);
    Q_INVOKABLE bool setEnabled(int index, bool enabled);
    Q_INVOKABLE void update(int index);           // 更新单个订阅（异步，完成后经信号回刷）
    Q_INVOKABLE void updateAll();                 // 更新全部订阅
    Q_INVOKABLE void apply();                     // 立即重建 full.yaml + 热重载核心（对齐「应用」按钮）

    // 节点弹窗（对齐 showSubscriptionNodes）：返回 [{name,server,port,use}, ...]
    Q_INVOKABLE QVariantList nodesOf(int index);
    Q_INVOKABLE bool setNodeEnabled(int index, int nodeIndex, bool enabled);
    Q_INVOKABLE bool setAllNodesEnabled(int index, bool enabled);

signals:
    void countChanged();
    void logMessage(const QString &line); // 供页面显示反馈（「正在更新订阅…」等）

private:
    void triggerRebuild();          // 经 500ms 去抖调用 CoreController::rebuildConfig

    SubscriptionStore *m_store = nullptr;
    CoreController *m_core = nullptr;
    QVector<SubscriptionSummary> m_items;
    QTimer m_rebuildTimer;
};
