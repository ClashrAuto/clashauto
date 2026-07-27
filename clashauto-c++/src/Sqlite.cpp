#include "Sqlite.h"

#include <QCoreApplication>
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
    // ① 真正要紧的一步：关库。它作用在 db 对象自身（不查 Qt 的全局连接表），任何时候都有效，
    //    SQLite 也正是在这里做最后一次 checkpoint 并删掉 -wal/-shm。
    if (db.isOpen())
        db.close();
    db = QSqlDatabase(); // 先断开引用，再注销连接，否则 Qt 报 "connection is still in use"

    // ② 注销连接名：这一步要查 Qt 的**全局连接表**，而那张表在没有 QCoreApplication 时不可用。
    //
    // 为什么这里真的会没有 QCoreApplication：DeviceStore / HistoryStore 都 `new …(&app)`，
    // 是 QApplication 的子对象。Qt 删子对象是在 **~QObject** 里，而 ~QObject 排在整条析构链的
    // 最后 —— 也就是 ~QCoreApplication 的函数体（已经把自己的 self 指针清空）**之后**。于是
    // 这两个 store 的析构跑到这里时 QCoreApplication::instance() 已经是 nullptr，
    // removeDatabase 只会打一句 "QSqlDatabase requires a QCoreApplication" 然后什么都不做。
    //
    // 真机实证这**纯属噪声、不丢数据**（树莓派上验的）：两个 store 的落盘都挂在 aboutToQuit 上
    // （那时 QCoreApplication 还活着），退出后库里没有 -wal/-shm 残留、integrity_check=ok、
    // 最后一次写入的时间戳正好是关停那一秒 —— 说明上面 ① 的 db.close() 一直是生效的，
    // 只有这句注册表记账被跳过；而进程都要结束了，那张表注不注销毫无意义。
    // 所以这里直接按条件跳过，把日志尾巴上那两行没有信息量的警告去掉。
    //
    // ★ 反过来的规矩（别踩）：**任何真正的写库动作都不能放在析构里**，必须挂 aboutToQuit 或更早。
    //   放析构里，QSqlQuery 虽然不查全局表、看似还能跑，但那已经是 Qt 明确不支持的时点，
    //   驱动插件随时可能先一步被卸掉。现有两个 store 的析构里那两句 `if (m_dirty) save()` /
    //   `if (m_ok) flush(true)` 只是兜底——正常路径下 aboutToQuit 已经把活干完、它们是空转。
    if (!connName.isEmpty() && QCoreApplication::instance())
        QSqlDatabase::removeDatabase(connName);
}

} // namespace sqlite
