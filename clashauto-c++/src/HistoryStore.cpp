#include "HistoryStore.h"
#include "DeviceStore.h"
#include "Sqlite.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QVariant>

namespace {
// mihomo 的连接 start 是 RFC3339（"2026-07-27T00:11:22.334+08:00"）。解析不出来就用当前时间。
qint64 parseStart(const QString &s)
{
    if (s.isEmpty())
        return QDateTime::currentMSecsSinceEpoch();
    const QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (dt.isValid())
        return dt.toMSecsSinceEpoch();
    const QDateTime dt2 = QDateTime::fromString(s, Qt::ISODate);
    return dt2.isValid() ? dt2.toMSecsSinceEpoch() : QDateTime::currentMSecsSinceEpoch();
}
} // namespace

HistoryStore::HistoryStore(const QString &configDir, DeviceStore *devices, QObject *parent)
    : QObject(parent), m_devices(devices)
{
    openDatabase(configDir);
    if (!m_ok)
        return;

    createSchema();
    purgeOld();

    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(kFlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, [this] { flush(false); });
    m_flushTimer->start();

    m_purgeTimer = new QTimer(this);
    m_purgeTimer->setInterval(6 * 60 * 60 * 1000); // 6h
    connect(m_purgeTimer, &QTimer::timeout, this, &HistoryStore::purgeOld);
    m_purgeTimer->start();
}

HistoryStore::~HistoryStore()
{
    if (m_ok)
        flush(true); // 退出：在途长连接也各落一条，否则永远进不了库
    sqlite::close(m_db, m_connName);
}

void HistoryStore::openDatabase(const QString &configDir)
{
    // 库、pragma、旧库改名统一在 sqlite:: 里（设备台账也开在同一个文件上，见 Sqlite.h）。
    m_connName = QStringLiteral("coast-history"); // 专用连接名，别占默认连接
    m_path = sqlite::databasePath(configDir);
    m_db = sqlite::open(m_connName, configDir);
    m_ok = m_db.isOpen();
}

void HistoryStore::createSchema()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS conn ("
                          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                          "mac TEXT NOT NULL DEFAULT '',"
                          "host TEXT NOT NULL DEFAULT '',"
                          "dest_ip TEXT NOT NULL DEFAULT '',"
                          "chain TEXT NOT NULL DEFAULT '',"
                          "network TEXT NOT NULL DEFAULT '',"
                          "process TEXT NOT NULL DEFAULT '',"
                          "started_at INTEGER NOT NULL DEFAULT 0,"
                          "ended_at INTEGER NOT NULL DEFAULT 0,"
                          "up INTEGER NOT NULL DEFAULT 0,"
                          "down INTEGER NOT NULL DEFAULT 0)"));
    // 查询全是「某设备 + 某时间段」和「按域名聚合」，两条索引照着来。
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS conn_mac_ended ON conn(mac, ended_at)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS conn_ended ON conn(ended_at)"));
    q.exec(QStringLiteral("PRAGMA user_version=1")); // 将来改表结构时靠它判断要不要迁移
}

qint64 HistoryStore::recordCount() const
{
    if (!m_ok)
        return 0;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM conn")) && q.next())
        return q.value(0).toLongLong();
    return 0;
}

HistoryStore::Record HistoryStore::toRecord(const Live &l, qint64 endedAt) const
{
    Record r;
    r.mac = l.mac;
    r.host = l.host;
    r.destIp = l.destIp;
    r.chain = l.chain;
    r.network = l.network;
    r.process = l.process;
    r.startedAt = l.startedAt;
    r.endedAt = endedAt;
    r.up = l.up;
    r.down = l.down;
    return r;
}

void HistoryStore::observe(const QJsonArray &conns)
{
    if (!m_ok)
        return;

    // ip → mac、dev-<mac> 用户名 → mac、以及「本机」：与 DevicesController::aggregate 同一套归属口径。
    QHash<QString, QString> ipToMac, userToMac;
    QString selfMac;
    if (m_devices) {
        for (const DeviceRecord &d : m_devices->devices()) {
            if (!d.ip.isEmpty())
                ipToMac.insert(d.ip, d.mac);
            const QString u = DeviceStore::socksUser(d.mac);
            if (!u.isEmpty())
                userToMac.insert(u, d.mac);
            if (d.isSelf && selfMac.isEmpty())
                selfMac = d.mac;
        }
    }

    for (auto it = m_live.begin(); it != m_live.end(); ++it)
        it->seen = false;

    for (const QJsonValue &v : conns) {
        const QJsonObject c = v.toObject();
        const QString id = c.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        const QJsonObject meta = c.value(QStringLiteral("metadata")).toObject();

        Live &l = m_live[id];
        const QString srcIp = meta.value(QStringLiteral("sourceIP")).toString();
        const QString user = meta.value(QStringLiteral("inboundUser")).toString();
        if (l.startedAt == 0) { // 第一次见到这条连接：填不变的那些字段
            l.startedAt = parseStart(c.value(QStringLiteral("start")).toString());
            l.destIp = meta.value(QStringLiteral("destinationIP")).toString();
            l.network = meta.value(QStringLiteral("network")).toString();
            // 进程名同 QmlBridge：网关代理的连接查到的会是 Coast 自己，宁可留空也不误标。
            if (!user.startsWith(QStringLiteral("dev-")))
                l.process = meta.value(QStringLiteral("process")).toString();
        }
        // 归属**每拍都重算，直到归上为止**（不是只在第一次见到时算一次）：设备台账要等第一轮
        // 局域网扫描跑完才有内容，而连接从程序一启动就在记——只认第一眼的话，启动头十几秒里
        // 的连接会永久挂在「未归属」名下（实测第一版就是整库 mac 全空）。
        if (l.mac.isEmpty()) {
            l.mac = ipToMac.value(srcIp);
            if (l.mac.isEmpty())
                l.mac = userToMac.value(user); // 网关代理：sourceIP=127.0.0.1，只能靠用户名
            // 源地址是本机自己的网卡（回环 / TUN 的 198.18.0.1 …）且不是网关代理进来的
            // → 本机自己的流量，记到「本机」那台设备名下（口径与流量聚合一致）。
            if (l.mac.isEmpty() && DeviceStore::isLocalMachineIp(srcIp)
                && !user.startsWith(QStringLiteral("dev-")))
                l.mac = selfMac;
        }
        // host 可能迟到（sniffer 嗅探出域名后才填上），每拍都跟一次；空了不覆盖已有值。
        const QString host = meta.value(QStringLiteral("host")).toString();
        if (!host.isEmpty())
            l.host = host;
        const QJsonArray chains = c.value(QStringLiteral("chains")).toArray();
        if (!chains.isEmpty())
            l.chain = chains.first().toString();
        l.up = static_cast<qint64>(c.value(QStringLiteral("upload")).toInteger());
        l.down = static_cast<qint64>(c.value(QStringLiteral("download")).toInteger());
        l.seen = true;
    }

    // 本轮没出现的 id = 已断开 → 落一条历史。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_live.begin(); it != m_live.end();) {
        if (it->seen) {
            ++it;
            continue;
        }
        // 一个字节都没传的连接（失败的连接尝试/探测）不入库：数量巨大且毫无信息量。
        if (it->up > 0 || it->down > 0)
            m_pending.append(toRecord(it.value(), now));
        it = m_live.erase(it);
    }

    if (m_pending.size() >= kFlushThreshold)
        flush(false);
}

void HistoryStore::flush(bool includeLive)
{
    if (!m_ok)
        return;
    if (includeLive) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_live.begin(); it != m_live.end(); ++it) {
            if (it->up > 0 || it->down > 0)
                m_pending.append(toRecord(it.value(), now));
        }
        m_live.clear();
    }
    if (m_pending.isEmpty())
        return;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO conn (mac,host,dest_ip,chain,network,process,started_at,ended_at,up,down) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)"));
    m_db.transaction(); // 一个事务包住整批：逐条 commit 会把磁盘按在地上摩擦
    int failed = 0;
    using sqlite::nn; // null QString 会绑成 SQL NULL 撞 NOT NULL，统一转空串（见 Sqlite.h）
    for (const Record &r : m_pending) {
        // **按位置 bindValue，不能用 addBindValue**：addBindValue 是往绑定列表尾部追加，
        // exec() 之后并不会清空，循环里第二条就变成绑第 10~19 个占位符 → 全部 exec 失败，
        // 表里一条都不会有（这个洞正是 COAST_HISTORY_SELFTEST 抓出来的）。
        q.bindValue(0, nn(r.mac));
        q.bindValue(1, nn(r.host));
        q.bindValue(2, nn(r.destIp));
        q.bindValue(3, nn(r.chain));
        q.bindValue(4, nn(r.network));
        q.bindValue(5, nn(r.process));
        q.bindValue(6, r.startedAt);
        q.bindValue(7, r.endedAt);
        q.bindValue(8, r.up);
        q.bindValue(9, r.down);
        if (!q.exec())
            ++failed;
    }
    m_db.commit();
    if (failed > 0) // 静默失败最要命：写不进去时至少让日志/报错上报看得见
        qWarning("HistoryStore: %d/%d 条历史写入失败: %s", failed, int(m_pending.size()),
                 q.lastError().text().toUtf8().constData());
    m_pending.clear();
}

void HistoryStore::purgeOld()
{
    if (!m_ok)
        return;
    const qint64 cutoff = QDateTime::currentDateTime().addDays(-kKeepDays).toMSecsSinceEpoch();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM conn WHERE ended_at < ?"));
    q.addBindValue(cutoff);
    q.exec();
}

QVector<HistoryStore::DomainTotal> HistoryStore::topDomains(const QString &mac, int days,
                                                            int limit) const
{
    QVector<DomainTotal> out;
    if (!m_ok || limit <= 0)
        return out;

    const qint64 since = QDateTime::currentDateTime().addDays(-qMax(1, days)).toMSecsSinceEpoch();
    QHash<QString, qint64> agg;

    QSqlQuery q(m_db);
    if (mac.isEmpty()) {
        q.prepare(QStringLiteral("SELECT host, SUM(up + down) FROM conn "
                                 "WHERE ended_at >= ? AND host <> '' GROUP BY host"));
        q.addBindValue(since);
    } else {
        q.prepare(QStringLiteral("SELECT host, SUM(up + down) FROM conn "
                                 "WHERE mac = ? AND ended_at >= ? AND host <> '' GROUP BY host"));
        q.addBindValue(mac);
        q.addBindValue(since);
    }
    if (q.exec()) {
        while (q.next())
            agg[q.value(0).toString()] += q.value(1).toLongLong();
    }

    // 合并仍在途的连接：只查库的话，正开着的连接一条都算不进去——刚打开的网页在「常用域名」
    // 里会看不见，看起来就像统计坏了。
    for (auto it = m_live.constBegin(); it != m_live.constEnd(); ++it) {
        if (it->host.isEmpty())
            continue;
        if (!mac.isEmpty() && it->mac != mac)
            continue;
        agg[it->host] += it->up + it->down;
    }

    out.reserve(agg.size());
    for (auto it = agg.constBegin(); it != agg.constEnd(); ++it)
        out.append({it.key(), it.value()});
    std::sort(out.begin(), out.end(),
              [](const DomainTotal &a, const DomainTotal &b) { return a.bytes > b.bytes; });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

QVector<HistoryStore::DayTotal> HistoryStore::dailyTraffic(const QString &mac, int days) const
{
    QVector<DayTotal> out;
    const int n = qMax(1, days);
    const QDate today = QDate::currentDate();
    // 先铺满 n 天（含今天），再把查到的填进去——没有记录的日子也要有一格 0，曲线才不会错位。
    QHash<QString, int> slot;
    out.reserve(n);
    for (int i = n - 1; i >= 0; --i) {
        const QString day = today.addDays(-i).toString(Qt::ISODate);
        slot.insert(day, out.size());
        out.append({day, 0, 0});
    }
    if (!m_ok)
        return out;

    const qint64 since = QDateTime(today.addDays(-(n - 1)), QTime(0, 0)).toMSecsSinceEpoch();
    QSqlQuery q(m_db);
    // ended_at 是毫秒 unix 时间；'unixepoch'+'localtime' 让分组落在**本地**日期上，
    // 否则东八区的凌晨 8 点前会被算进前一天。
    const QString sel = QStringLiteral(
        "SELECT date(ended_at / 1000, 'unixepoch', 'localtime') AS d, SUM(up), SUM(down) "
        "FROM conn WHERE ended_at >= ?%1 GROUP BY d");
    if (mac.isEmpty()) {
        q.prepare(sel.arg(QString()));
        q.addBindValue(since);
    } else {
        q.prepare(sel.arg(QStringLiteral(" AND mac = ?")));
        q.addBindValue(since);
        q.addBindValue(mac);
    }
    if (q.exec()) {
        while (q.next()) {
            const int at = slot.value(q.value(0).toString(), -1);
            if (at >= 0) {
                out[at].up = q.value(1).toLongLong();
                out[at].down = q.value(2).toLongLong();
            }
        }
    }
    // 今天这格再把在途连接加上（同 topDomains 的理由）。
    for (auto it = m_live.constBegin(); it != m_live.constEnd(); ++it) {
        if (!mac.isEmpty() && it->mac != mac)
            continue;
        out.last().up += it->up;
        out.last().down += it->down;
    }
    return out;
}
