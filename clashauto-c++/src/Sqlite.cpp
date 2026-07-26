#include "Sqlite.h"

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace sqlite {

QString databasePath(const QString &configDir)
{
    QDir().mkpath(configDir);
    const QString unified = QDir(configDir).filePath(QStringLiteral("coast.db"));
    const QString legacy = QDir(configDir).filePath(QStringLiteral("history.db"));
    // 一次性迁移：上一版只有连接历史，库叫 history.db。现在设备台账也进来了，名字得中性些。
    // 只在「新名字还不存在、旧名字存在」时改名，连 -wal/-shm 一起搬（否则 SQLite 会把遗留的
    // WAL 当成另一个库的，轻则丢最后几条、重则报库损坏）。
    if (!QFile::exists(unified) && QFile::exists(legacy)) {
        QFile::rename(legacy, unified);
        QFile::rename(legacy + QStringLiteral("-wal"), unified + QStringLiteral("-wal"));
        QFile::rename(legacy + QStringLiteral("-shm"), unified + QStringLiteral("-shm"));
    }
    return unified;
}

QSqlDatabase open(const QString &connName, const QString &configDir)
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        return QSqlDatabase(); // 没驱动：返回无效连接，调用方整体空转（程序照常起）

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    db.setDatabaseName(databasePath(configDir));
    // 同一个文件有多条连接在写（台账 1.5s 一次、历史 5s 一批），撞上写锁时别立刻失败，等一会儿。
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000"));
    if (!db.open()) {
        QSqlDatabase::removeDatabase(connName);
        return QSqlDatabase();
    }

    QSqlQuery q(db);
    // WAL：写不阻塞读（这边每秒都在写，UI 还要查 Top 域名 / 设备表）。
    // NORMAL：崩溃最多丢最后几条，用这点风险换掉每次 commit 的 fsync——这是台账和历史，不是账本。
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    q.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
    return db;
}

void close(QSqlDatabase &db, const QString &connName)
{
    if (db.isOpen())
        db.close();
    db = QSqlDatabase(); // 先断开引用，再注销连接，否则 Qt 报 "connection is still in use"
    if (!connName.isEmpty())
        QSqlDatabase::removeDatabase(connName);
}

} // namespace sqlite
