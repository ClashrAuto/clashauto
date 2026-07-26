#pragma once

// 全应用共用一个 SQLite 库：configDir/coast.db。
//
// 现在有三个使用方，各自开一条连接（同进程多连接同一个文件是 SQLite 支持的用法，WAL 下读写
// 互不阻塞；写与写之间靠 busy_timeout 排队）：
//   · DeviceStore  —— device 表（设备台账：身份/别名/策略/累计流量/上次的长相）
//   · HistoryStore —— conn 表（上网历史：连接结束时落一条）
//   · ConfigBuilder —— 只读查「开着代理的设备」，生成网关 listener + IN-USER 规则
//     （见 DeviceStore::proxiedDevices，它开一条临时连接，用完即关）
//
// 把「路径 + pragma + 旧库迁移」收在这里，免得三处各写一份、哪天 pragma 漏了一个就出怪事。
#include <QString>

class QSqlDatabase;

namespace sqlite {

// 库文件绝对路径（configDir/coast.db）。顺带做一次一次性迁移：早期版本把连接历史写在
// history.db 里，若只有它存在就整体改名过来（含 -wal/-shm），历史不丢。
QString databasePath(const QString &configDir);

// 按连接名打开（不存在则建）。已设 WAL + synchronous=NORMAL + busy_timeout。
// 失败返回一个 !isOpen() 的 QSqlDatabase——调用方应据此整体空转，而不是崩掉。
QSqlDatabase open(const QString &connName, const QString &configDir);

// 关闭并注销连接（析构里调；先释放 QSqlDatabase 引用再 removeDatabase，否则 Qt 会警告）。
void close(QSqlDatabase &db, const QString &connName);

// 绑定用：null QString 绑出来是 SQL NULL，撞上列的 NOT NULL 会让整批写入失败（空 mac、
// 没嗅到域名的 host 天天都是空的）。统一用它转成真正的空串。
inline QString nn(const QString &v)
{
    return v.isNull() ? QString(QLatin1String("")) : v;
}

} // namespace sqlite
