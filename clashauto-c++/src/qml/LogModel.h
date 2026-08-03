#pragma once

// QML 适配器：把应用日志流暴露成 list model，供 LogsPage 的 ListView 消费。
// 属于「薄绑定层」——不改后端，只把已有信号（CoreController::logUpdated /
// ClashService::logUpdated）整形给 QML。复刻 Widgets 版 appendLog/appendTimeline：
//   · 「主日志」标签：应用全部日志（ClashService 的 app 日志 + 核心日志）；
//   · 「Clash 内核」标签：仅核心日志（CoreController::logUpdated）。
// 每条日志在 append 时派生时间戳与严重级别（红=失败/错误、橙=警告、绿=成功/完成、蓝=信息），
// 最新置顶，封顶 100 条（对齐 Widgets 版 kMaxLogEntries）。
//
// 说明（局限）：Widgets 版有大量 MainWindow 内部直接 appendLog 调用（订阅更新/下载/设置等），
// 那些是 MainWindow 的内部调用，不经信号；QML 版没有 MainWindow，故它们不会到达本 model。
// 到达本 model 的日志源就是后端两个信号——这与 QmlBridge 的 pushLog 合流口径一致。
#include <QAbstractListModel>
#include <QDateTime>
#include <QString>
#include <QVector>

class CoreController;
class ClashService;

// 单个日志时间线的 model（一个标签页一个实例）。角色：text / time / severity。
class LogEntryModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCountProp NOTIFY countChanged)

public:
    enum Roles {
        TextRole = Qt::UserRole + 1, // 日志正文
        TimeRole,                    // "yyyy-MM-dd HH:mm:ss"（append 时派生）
        SeverityRole                 // "error"/"warn"/"success"/"info"（QML 据此上色圆点）
    };

    explicit LogEntryModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int rowCountProp() const { return rowCount(); }

    // 追加一条日志：派生时间戳 + 严重级别，最新插到第 0 行，超过 100 条丢弃最旧。
    //
    // ★ **不再逐条动模型。** 每来一行就 beginInsertRows + beginRemoveRows，会让 ListView
    //   反复重排版；日志速率一高就变成纯浪费。真机实测（树莓派网关，239 上 20 并发短连接
    //   持续加压，核心 log-level 分别取 info / warning，交替两轮）：
    //       log-level=info     核心 21.5%/21.1%   **App 27.5%/28.2%**
    //       log-level=warning  核心 18.2%/20.2%   **App  3.3%/ 3.8%**
    //   —— App 比核心还费，而它在 TPROXY 下根本不在数据面上；perf 采样的热点是
    //   QTextLine::layout_helper / harfbuzz（文字排版）与 QML 绑定机械，全是在"显示"日志。
    //   网关上更荒唐：headless 跑着，一个人都没在看。
    //   所以：日志一律先进环形缓冲；**只有可见时**才按 kFlushMs 合并成一次批量插入。
    void append(const QString &message);

    /// 这个时间线当前可不可见（LogsPage 显示时才为 true）。不可见时只缓冲、不碰模型。
    void setActive(bool active);
    bool isActive() const { return m_active; }

    // 由 message 派生严重级别（复刻 appendTimeline 的判定与顺序）。
    static QString severityFor(const QString &message);

signals:
    void countChanged();

private:
    struct Row {
        QString text;
        QString time;
        QString severity;
    };
    void flush(); // 把缓冲里的行一次性插进模型（单次 beginInsertRows）

    static constexpr int kMaxRows = 100;
    static constexpr int kFlushMs = 100; // 可见时最多每 100ms 更新一次视图
    QVector<Row> m_rows;    // 索引 0 = 最新（已进模型的）
    QVector<Row> m_pending; // 索引 0 = 最新（尚未进模型的）
    bool m_active = false;
    bool m_flushScheduled = false;
};

// 门面：持有两个 LogEntryModel（主日志 + 核心日志），把后端两个信号路由进去。
// 生命周期由 main_qml.cpp 拥有；本类只持有裸指针、不接管后端所有权。
class LogModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(LogEntryModel *mainModel READ mainModel CONSTANT)
    Q_PROPERTY(LogEntryModel *coreModel READ coreModel CONSTANT)

public:
    LogModel(CoreController *core, ClashService *clash, QObject *parent = nullptr);

    LogEntryModel *mainModel() { return &m_main; }
    LogEntryModel *coreModel() { return &m_core; }

    /// LogsPage 可见性：QML 在页面显示/隐藏时调一次。两个时间线一起开关。
    Q_INVOKABLE void setActive(bool active);

private:
    LogEntryModel m_main; // 全部日志
    LogEntryModel m_core; // 仅核心日志
};
