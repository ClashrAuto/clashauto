#include "LogModel.h"

#include "../ClashService.h"
#include "../CoreController.h"

#include <QDateTime>

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

    // 最新置顶：插到第 0 行（对齐 el-timeline :reverse=true / insertWidget(0)）。
    beginInsertRows(QModelIndex(), 0, 0);
    m_rows.prepend(row);
    endInsertRows();

    // 只保留最近 100 条：超出则从末尾（最旧）逐条移除。
    while (m_rows.size() > kMaxRows) {
        const int last = m_rows.size() - 1;
        beginRemoveRows(QModelIndex(), last, last);
        m_rows.removeLast();
        endRemoveRows();
    }

    emit countChanged();
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
