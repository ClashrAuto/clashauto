#include "LogModel.h"

#include "../ClashService.h"
#include "../CoreController.h"

#include <QDateTime>
#include <QTimer>

LogEntryModel::LogEntryModel(QObject *parent) : QAbstractListModel(parent) {}

int LogEntryModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_rows.size();
}

QVariant LogEntryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows.at(index.row());
    switch (role) {
    case TextRole:
        return r.text;
    case TimeRole:
        return r.time;
    case SeverityRole:
        return r.severity;
    default:
        return {};
    }
}

QHash<int, QByteArray> LogEntryModel::roleNames() const
{
    return {
        {TextRole, "text"},
        {TimeRole, "time"},
        {SeverityRole, "severity"},
    };
}

QString LogEntryModel::severityFor(const QString &message)
{
    // 复刻 MainWindow::appendTimeline 的判定与优先级顺序：错误 > 警告 > 成功 > 信息。
    const QString low = message.toLower();
    if (low.contains("fail") || low.contains("error") || message.contains(QString::fromUtf8("失败")))
        return QStringLiteral("error");   // 红
    if (low.contains("warn"))
        return QStringLiteral("warn");    // 橙
    if (low.contains("finished") || low.contains("success") || low.contains(" ok")
        || message.contains(QString::fromUtf8("已")) || message.contains(QString::fromUtf8("完成")))
        return QStringLiteral("success"); // 绿
    return QStringLiteral("info");        // 蓝
}

void LogEntryModel::append(const QString &message)
{
    Row row;
    row.text = message;
    row.time = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    row.severity = severityFor(message);

    // 先进缓冲（最新置顶）。缓冲本身也只留 kMaxRows 条 —— 不可见期间日志再多，
    // 内存占用也是常数，切回日志页时看到的就是"最近 100 条"，与一直可见时一致。
    m_pending.prepend(row);
    while (m_pending.size() > kMaxRows)
        m_pending.removeLast();

    if (!m_active)
        return; // 不可见：一次模型信号都不发，ListView 不会被唤醒去排版

    // 可见：合并 kFlushMs 内的所有行，一次性插入。日志速率再高，视图也最多每 100ms 动一次。
    if (m_flushScheduled)
        return;
    m_flushScheduled = true;
    QTimer::singleShot(kFlushMs, this, [this] {
        m_flushScheduled = false;
        flush();
    });
}

void LogEntryModel::flush()
{
    if (m_pending.isEmpty())
        return;
    const int n = m_pending.size();
    // 最新置顶：整批插到第 0..n-1 行（m_pending 本身已是"最新在前"）。
    beginInsertRows(QModelIndex(), 0, n - 1);
    for (int i = n - 1; i >= 0; --i)
        m_rows.prepend(m_pending.at(i));
    endInsertRows();
    m_pending.clear();

    // 只保留最近 100 条：超出的一次性从末尾移除（不再逐条发信号）。
    if (m_rows.size() > kMaxRows) {
        const int first = kMaxRows;
        const int last = m_rows.size() - 1;
        beginRemoveRows(QModelIndex(), first, last);
        m_rows.remove(first, last - first + 1);
        endRemoveRows();
    }

    emit countChanged();
}

void LogEntryModel::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (m_active)
        flush(); // 刚切回来：把不可见期间攒下的立刻补上，用户不会看到空白
}

LogModel::LogModel(CoreController *core, ClashService *clash, QObject *parent)
    : QObject(parent), m_main(this), m_core(this)
{
    // 应用日志（ClashService）→ 仅主日志。
    if (clash) {
        connect(clash, &ClashService::logUpdated, this,
                [this](const QString &message) { m_main.append(message); });
    }
    // 核心日志（CoreController）→ 主日志 + Clash 内核（对齐 Widgets：核心日志也进主时间线）。
    if (core) {
        connect(core, &CoreController::logUpdated, this, [this](const QString &message) {
            m_main.append(message);
            m_core.append(message);
        });
    }
}

void LogModel::setActive(bool active)
{
    m_main.setActive(active);
    m_core.setActive(active);
}
