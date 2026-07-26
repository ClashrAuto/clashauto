#include "DeviceStore.h"
#include "AppConfig.h"
#include "Sqlite.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QVariant>

// ———————————————————————————— 类型/模式 ↔ 字符串 ————————————————————————————
QString DeviceStore::typeKey(DeviceType t)
{
    switch (t) {
    case DeviceType::Phone:       return QStringLiteral("phone");
    case DeviceType::Tablet:      return QStringLiteral("tablet");
    case DeviceType::Computer:    return QStringLiteral("computer");
    case DeviceType::Router:      return QStringLiteral("router");
    case DeviceType::TvBox:       return QStringLiteral("tvbox");
    case DeviceType::Speaker:     return QStringLiteral("speaker");
    case DeviceType::Printer:     return QStringLiteral("printer");
    case DeviceType::Camera:      return QStringLiteral("camera");
    case DeviceType::GameConsole: return QStringLiteral("game");
    case DeviceType::Nas:         return QStringLiteral("nas");
    case DeviceType::IoT:         return QStringLiteral("iot");
    default:                      return QStringLiteral("unknown");
    }
}

DeviceType DeviceStore::typeFromKey(const QString &k)
{
    static const QHash<QString, DeviceType> m = {
        {"phone", DeviceType::Phone},   {"tablet", DeviceType::Tablet},
        {"computer", DeviceType::Computer}, {"router", DeviceType::Router},
        {"tvbox", DeviceType::TvBox},   {"speaker", DeviceType::Speaker},
        {"printer", DeviceType::Printer}, {"camera", DeviceType::Camera},
        {"game", DeviceType::GameConsole}, {"nas", DeviceType::Nas},
        {"iot", DeviceType::IoT},
    };
    return m.value(k, DeviceType::Unknown);
}

QString DeviceStore::modeKey(DevicePolicyMode m)
{
    switch (m) {
    case DevicePolicyMode::Rule:   return QStringLiteral("rule");
    case DevicePolicyMode::Global: return QStringLiteral("global");
    case DevicePolicyMode::Direct: return QStringLiteral("direct");
    case DevicePolicyMode::Reject: return QStringLiteral("reject");
    default:                       return QStringLiteral("follow");
    }
}

DevicePolicyMode DeviceStore::modeFromKey(const QString &k)
{
    static const QHash<QString, DevicePolicyMode> m = {
        {"rule", DevicePolicyMode::Rule}, {"global", DevicePolicyMode::Global},
        {"direct", DevicePolicyMode::Direct}, {"reject", DevicePolicyMode::Reject},
        {"follow", DevicePolicyMode::Follow},
    };
    return m.value(k, DevicePolicyMode::Follow);
}

QString DeviceStore::socksUser(const QString &mac)
{
    const QString norm = normalizeMac(mac);
    if (norm.isEmpty())
        return {};
    return QStringLiteral("dev-") + QString(norm).remove(':');
}

bool DeviceStore::isLocalMachineIp(const QString &ip)
{
    if (ip.isEmpty())
        return false;
    if (isLoopbackIp(ip))
        return true;
    // 本机全部网卡的 IPv4（含 TUN 的 198.18.0.1 —— 开增强模式后本机流量的 sourceIP 就是它）。
    // 枚举网卡是系统调用，而这函数每条连接都要问一次；网卡地址不会秒级变化，缓存 30s 足够。
    // 只在 GUI 线程调用（流量聚合 / 历史库都在那儿），故用函数内静态缓存即可。
    static QSet<QString> cache;
    static QElapsedTimer age;
    if (!age.isValid() || age.elapsed() > 30000) {
        cache.clear();
        const QList<QHostAddress> addrs = QNetworkInterface::allAddresses();
        for (const QHostAddress &a : addrs)
            if (a.protocol() == QAbstractSocket::IPv4Protocol)
                cache.insert(a.toString());
        age.restart();
    }
    return cache.contains(ip);
}

QString DeviceStore::normalizeMac(const QString &raw)
{
    // 抽出 12 个十六进制字符，统一成小写 aa:bb:cc:dd:ee:ff。
    static const QRegularExpression nonHex("[^0-9a-fA-F]");
    QString hex = QString(raw).remove(nonHex).toLower();
    if (hex.size() != 12)
        return {};
    // 全 0（00:00:...）或全 f（广播）视为无效
    if (hex == "000000000000" || hex == "ffffffffffff")
        return {};
    QString out;
    out.reserve(17);
    for (int i = 0; i < 12; i += 2) {
        if (i)
            out += ':';
        out += hex.mid(i, 2);
    }
    return out;
}

// ———————————————————————————— DeviceRecord ————————————————————————————
QString DeviceRecord::displayName() const
{
    if (!alias.isEmpty())   return alias;
    if (!autoName.isEmpty()) return autoName;
    if (!model.isEmpty())   return model;
    if (!vendor.isEmpty())  return vendor;
    return ip.isEmpty() ? mac : ip;
}

// ———————————————————————————— DeviceStore ————————————————————————————
DeviceStore::DeviceStore(const QString &configDir, QObject *parent)
    : QObject(parent), m_configDir(configDir), m_connName(QStringLiteral("coast-devices"))
{
    m_db = sqlite::open(m_connName, configDir);
    m_ok = m_db.isOpen();
    if (m_ok) {
        ensureSchema();
        importLegacyJson(); // 旧版的 devices.json（如果还在）先搬进来
    }
    load();
}

DeviceStore::~DeviceStore()
{
    if (m_dirty)
        save(); // 防抖计时器还没到点就退出：别丢掉最后那次编辑
    sqlite::close(m_db, m_connName);
}

void DeviceStore::ensureSchema()
{
    QSqlQuery q(m_db);
    // 一台设备一行，mac 作主键。列名用下划线风格，与 conn 表一致。
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS device ("
                          "mac TEXT PRIMARY KEY,"
                          // —— 用户编辑 / 长期保留 ——
                          "alias TEXT NOT NULL DEFAULT '',"
                          "type_override TEXT NOT NULL DEFAULT '',"
                          "first_seen TEXT NOT NULL DEFAULT '',"
                          "proxy_enabled INTEGER NOT NULL DEFAULT 0,"
                          "policy_mode TEXT NOT NULL DEFAULT '',"
                          "policy_target TEXT NOT NULL DEFAULT '',"
                          "total_up INTEGER NOT NULL DEFAULT 0,"
                          "total_down INTEGER NOT NULL DEFAULT 0,"
                          "today_up INTEGER NOT NULL DEFAULT 0,"
                          "today_down INTEGER NOT NULL DEFAULT 0,"
                          "today_date TEXT NOT NULL DEFAULT '',"
                          // —— 上次见到时的「长相」：下次启动当首屏，等本轮扫描回来再覆盖 ——
                          "ip TEXT NOT NULL DEFAULT '',"
                          "auto_name TEXT NOT NULL DEFAULT '',"
                          "model TEXT NOT NULL DEFAULT '',"
                          "vendor TEXT NOT NULL DEFAULT '',"
                          "auto_type TEXT NOT NULL DEFAULT '',"
                          "is_self INTEGER NOT NULL DEFAULT 0,"
                          "is_gateway INTEGER NOT NULL DEFAULT 0,"
                          "last_seen TEXT NOT NULL DEFAULT '')"));
    // ConfigBuilder 每次生成配置都要查「谁开着代理」，给它一条索引。
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS device_proxy ON device(proxy_enabled)"));
}

void DeviceStore::importLegacyJson()
{
    const QString legacy = QDir(m_configDir).filePath(QStringLiteral("devices.json"));
    if (!QFile::exists(legacy))
        return;
    QSqlQuery q(m_db);
    // 库里已经有设备了 = 早就迁过（或用户是新装），别拿旧文件把新数据盖回去。
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM device")) && q.next()
        && q.value(0).toLongLong() > 0) {
        return;
    }
    QFile f(legacy);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    f.close();

    QSqlQuery ins(m_db);
    ins.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO device (mac,alias,type_override,first_seen,proxy_enabled,"
        "policy_mode,policy_target,total_up,total_down,today_up,today_down,today_date,"
        "ip,auto_name,model,vendor,auto_type,is_self,is_gateway,last_seen) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    m_db.transaction();
    int n = 0;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const QString mac = normalizeMac(o.value(QStringLiteral("mac")).toString());
        if (mac.isEmpty())
            continue;
        ins.bindValue(0, mac);
        ins.bindValue(1, sqlite::nn(o.value(QStringLiteral("alias")).toString()));
        ins.bindValue(2, sqlite::nn(o.value(QStringLiteral("typeOverride")).toString()));
        ins.bindValue(3, sqlite::nn(o.value(QStringLiteral("firstSeen")).toString()));
        ins.bindValue(4, o.value(QStringLiteral("proxyEnabled")).toBool() ? 1 : 0);
        ins.bindValue(5, sqlite::nn(o.value(QStringLiteral("policyMode")).toString()));
        ins.bindValue(6, sqlite::nn(o.value(QStringLiteral("policyTarget")).toString()));
        ins.bindValue(7, o.value(QStringLiteral("totalUp")).toVariant().toLongLong());
        ins.bindValue(8, o.value(QStringLiteral("totalDown")).toVariant().toLongLong());
        ins.bindValue(9, o.value(QStringLiteral("todayUp")).toVariant().toLongLong());
        ins.bindValue(10, o.value(QStringLiteral("todayDown")).toVariant().toLongLong());
        ins.bindValue(11, sqlite::nn(o.value(QStringLiteral("todayDate")).toString()));
        ins.bindValue(12, sqlite::nn(o.value(QStringLiteral("ip")).toString()));
        ins.bindValue(13, sqlite::nn(o.value(QStringLiteral("autoName")).toString()));
        ins.bindValue(14, sqlite::nn(o.value(QStringLiteral("model")).toString()));
        ins.bindValue(15, sqlite::nn(o.value(QStringLiteral("vendor")).toString()));
        ins.bindValue(16, sqlite::nn(o.value(QStringLiteral("autoType")).toString()));
        ins.bindValue(17, o.value(QStringLiteral("isSelf")).toBool() ? 1 : 0);
        ins.bindValue(18, o.value(QStringLiteral("isGateway")).toBool() ? 1 : 0);
        ins.bindValue(19, sqlite::nn(o.value(QStringLiteral("lastSeen")).toString()));
        if (ins.exec())
            ++n;
    }
    m_db.commit();
    // 搬完把旧文件改名：留个后悔药，同时不让它继续看起来像「权威数据」（ConfigBuilder 以前就是
    // 直接读它的）。改名失败也不要紧——上面的「库里已有设备就跳过」已经保证不会重复导入。
    QFile::rename(legacy, legacy + QStringLiteral(".migrated"));
    qInfo("DeviceStore: 已从 devices.json 迁移 %d 台设备到 coast.db", n);
}

int DeviceStore::indexOf(const QString &mac) const { return m_index.value(mac, -1); }

DeviceRecord *DeviceStore::find(const QString &mac)
{
    const int i = indexOf(mac);
    return i >= 0 ? &m_devices[i] : nullptr;
}

const DeviceRecord *DeviceStore::find(const QString &mac) const
{
    const int i = indexOf(mac);
    return i >= 0 ? &m_devices[i] : nullptr;
}

void DeviceStore::load()
{
    if (!m_ok)
        return; // 没库：纯内存跑（设备页照常用，只是重启后不记得）
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT mac,alias,type_override,first_seen,proxy_enabled,policy_mode,policy_target,"
            "total_up,total_down,today_up,today_down,today_date,"
            "ip,auto_name,model,vendor,auto_type,is_self,is_gateway,last_seen FROM device"))) {
        qWarning("DeviceStore: 读设备表失败: %s", q.lastError().text().toUtf8().constData());
        return;
    }
    while (q.next()) {
        DeviceRecord d;
        d.mac = normalizeMac(q.value(0).toString());
        if (d.mac.isEmpty())
            continue;
        d.alias = q.value(1).toString();
        d.typeOverride = typeFromKey(q.value(2).toString());
        d.firstSeen = QDateTime::fromString(q.value(3).toString(), Qt::ISODate);
        d.proxyEnabled = q.value(4).toBool();
        d.policyMode = modeFromKey(q.value(5).toString());
        d.policyTarget = q.value(6).toString();
        d.totalUp = q.value(7).toLongLong();
        d.totalDown = q.value(8).toLongLong();
        d.todayUp = q.value(9).toLongLong();
        d.todayDown = q.value(10).toLongLong();
        d.todayDate = q.value(11).toString();
        // —— 上次见到时的「长相」——
        // 这些字段本来算运行时数据、只由扫描填，于是**没扫描完之前列表是一排没名字没 IP 的空行**
        // （程序刚起、或刚点进设备页的头几秒）。存下来当作首屏：进页面立刻看到上次那份列表，
        // 扫描回来再逐行覆盖成最新的。
        d.ip = q.value(12).toString();
        d.autoName = q.value(13).toString();
        d.model = q.value(14).toString();
        d.vendor = q.value(15).toString();
        d.autoType = typeFromKey(q.value(16).toString());
        d.isSelf = q.value(17).toBool();
        d.isGateway = q.value(18).toBool();
        d.lastSeen = QDateTime::fromString(q.value(19).toString(), Qt::ISODate);
        // online 与 inLanSubnet **故意不持久化**：前者要靠本轮探测才算数（存成 true 会让离线设备
        // 一直显示在线），后者决定「能不能开代理」——换了网络还沿用上次的判断，会让用户对着一台
        // 其实劫持不到的设备点开关。两者都留 false，等这一轮扫描说话。
        m_index.insert(d.mac, m_devices.size());
        m_devices.append(d);
    }
}

void DeviceStore::save()
{
    if (!m_ok) {
        m_dirty = false;
        return;
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO device (mac,alias,type_override,first_seen,proxy_enabled,"
        "policy_mode,policy_target,total_up,total_down,today_up,today_down,today_date,"
        "ip,auto_name,model,vendor,auto_type,is_self,is_gateway,last_seen) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    m_db.transaction(); // 整表一个事务：几十行，比维护脏行标记省心，也比逐行 commit 快得多
    int failed = 0;
    for (const DeviceRecord &d : m_devices) {
        // 只落盘「有价值保留」的记录：有用户编辑或有累计流量的才存，纯发现的临时设备不写。
        const bool worth = !d.alias.isEmpty() || d.typeOverride != DeviceType::Unknown
                           || d.proxyEnabled || d.policyMode != DevicePolicyMode::Follow
                           || d.totalUp || d.totalDown || d.firstSeen.isValid();
        if (!worth)
            continue;
        // **按位置 bindValue，不能 addBindValue**：后者在循环里是往绑定列表尾部追加，
        // 第二行起就绑到不存在的占位符上、整批写失败（HistoryStore 那边踩过一次）。
        q.bindValue(0, d.mac);
        q.bindValue(1, sqlite::nn(d.alias));
        q.bindValue(2, d.typeOverride != DeviceType::Unknown ? typeKey(d.typeOverride) : QString(""));
        q.bindValue(3, d.firstSeen.isValid() ? d.firstSeen.toString(Qt::ISODate) : QString(""));
        q.bindValue(4, d.proxyEnabled ? 1 : 0);
        q.bindValue(5, d.policyMode != DevicePolicyMode::Follow ? modeKey(d.policyMode) : QString(""));
        q.bindValue(6, sqlite::nn(d.policyTarget));
        q.bindValue(7, d.totalUp);
        q.bindValue(8, d.totalDown);
        q.bindValue(9, d.todayUp);
        q.bindValue(10, d.todayDown);
        q.bindValue(11, sqlite::nn(d.todayDate));
        q.bindValue(12, sqlite::nn(d.ip));
        q.bindValue(13, sqlite::nn(d.autoName));
        q.bindValue(14, sqlite::nn(d.model));
        q.bindValue(15, sqlite::nn(d.vendor));
        q.bindValue(16, d.autoType != DeviceType::Unknown ? typeKey(d.autoType) : QString(""));
        q.bindValue(17, d.isSelf ? 1 : 0);
        q.bindValue(18, d.isGateway ? 1 : 0);
        q.bindValue(19, d.lastSeen.isValid() ? d.lastSeen.toString(Qt::ISODate) : QString(""));
        if (!q.exec())
            ++failed;
    }
    m_db.commit();
    if (failed > 0) // 静默失败最要命：写不进去时至少让日志/报错上报看得见
        qWarning("DeviceStore: %d 台设备写入失败: %s", failed,
                 q.lastError().text().toUtf8().constData());
    m_dirty = false;
}

QVector<DeviceStore::ProxyDeviceRow> DeviceStore::proxiedDevices(const QString &configDir)
{
    QVector<ProxyDeviceRow> out;
    // 临时连接：ConfigBuilder 生成配置时才调，用完就关（连接名带 -cfg 与常驻的两条区分开）。
    const QString conn = QStringLiteral("coast-devices-cfg");
    QSqlDatabase db = sqlite::open(conn, configDir);
    if (!db.isOpen())
        return out;
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT mac,policy_mode,policy_target FROM device "
                              "WHERE proxy_enabled=1 ORDER BY mac"))) {
        while (q.next()) {
            ProxyDeviceRow r;
            r.mac = q.value(0).toString().trimmed();
            r.policyMode = q.value(1).toString().trimmed();
            r.policyTarget = q.value(2).toString().trimmed();
            if (!r.mac.isEmpty())
                out.append(r);
        }
    }
    q.finish();
    sqlite::close(db, conn);
    return out;
}

void DeviceStore::scheduleSave()
{
    if (m_dirty)
        return;
    m_dirty = true;
    QTimer::singleShot(1500, this, [this] {
        if (m_dirty)
            save();
    });
}

void DeviceStore::rolloverTodayIfNeeded(DeviceRecord &d) const
{
    // 换天：今日流量清零（不影响 total）。日期串由控制器在调用前塞入 d.todayDate 之外，这里用
    // lastSeen 的日期作为「今天」判断——由聚合器保证 lastSeen 已更新到当前拍。
    const QString today = d.lastSeen.isValid() ? d.lastSeen.date().toString(Qt::ISODate)
                                               : QString();
    if (today.isEmpty())
        return;
    if (d.todayDate != today) {
        d.todayDate = today;
        d.todayUp = 0;
        d.todayDown = 0;
    }
}

void DeviceStore::mergeDiscovered(const QVector<DeviceRecord> &found)
{
    bool anyChange = false;
    for (const DeviceRecord &f : found) {
        const QString mac = normalizeMac(f.mac);
        if (mac.isEmpty())
            continue;
        int i = indexOf(mac);
        if (i < 0) {
            DeviceRecord d = f;
            d.mac = mac;
            if (!d.firstSeen.isValid())
                d.firstSeen = f.lastSeen.isValid() ? f.lastSeen : QDateTime::currentDateTime();
            d.onlineSince = d.lastSeen;
            m_index.insert(mac, m_devices.size());
            m_devices.append(d);
            anyChange = true;
            emit deviceAdded(mac);
            scheduleSave();
            continue;
        }
        DeviceRecord &d = m_devices[i];
        // 覆盖运行时/身份字段；保留用户编辑与累计流量。
        const bool wasOnline = d.online;
        // 「长相」类字段现在是要落盘的（下次启动当首屏），变了就排一次保存。**只看这几个**：
        // online/lastSeen 每 5s 热更新都在变，跟着它们存盘等于每 5s 写一次磁盘，纯浪费。
        const bool looksChanged = (!f.ip.isEmpty() && f.ip != d.ip)
                                  || (!f.autoName.isEmpty() && f.autoName != d.autoName)
                                  || (!f.model.isEmpty() && f.model != d.model)
                                  || (!f.vendor.isEmpty() && f.vendor != d.vendor)
                                  || (f.autoType != DeviceType::Unknown && f.autoType != d.autoType)
                                  || f.isSelf != d.isSelf || f.isGateway != d.isGateway;
        if (!f.ip.isEmpty())      d.ip = f.ip;
        if (!f.autoName.isEmpty()) d.autoName = f.autoName;
        if (!f.model.isEmpty())   d.model = f.model;
        if (!f.vendor.isEmpty())  d.vendor = f.vendor;
        if (f.autoType != DeviceType::Unknown) d.autoType = f.autoType;
        d.isSelf = f.isSelf;
        d.isGateway = f.isGateway;
        if (looksChanged)
            scheduleSave();
        d.inLanSubnet = f.inLanSubnet;
        d.online = f.online;
        if (f.lastSeen.isValid()) d.lastSeen = f.lastSeen;
        if (f.online && !wasOnline)
            d.onlineSince = d.lastSeen; // 上线：重置在线起点
        anyChange = true; // 保守：交给控制器的模型层做增量比对
    }
    if (anyChange)
        emit changed();
}

void DeviceStore::markOffline(const QString &mac)
{
    const int i = indexOf(mac);
    if (i < 0 || !m_devices[i].online)
        return;
    m_devices[i].online = false;
    m_devices[i].rateUp = m_devices[i].rateDown = 0;
    m_devices[i].connCount = 0;
    emit changed();
}

void DeviceStore::setAlias(const QString &mac, const QString &alias)
{
    DeviceRecord *d = find(mac);
    if (!d || d->alias == alias)
        return;
    d->alias = alias;
    emit changed();
    scheduleSave();
}

void DeviceStore::setTypeOverride(const QString &mac, DeviceType type)
{
    DeviceRecord *d = find(mac);
    if (!d || d->typeOverride == type)
        return;
    d->typeOverride = type;
    emit changed();
    scheduleSave();
}

void DeviceStore::setProxyEnabled(const QString &mac, bool on)
{
    DeviceRecord *d = find(mac);
    if (!d || d->proxyEnabled == on)
        return;
    d->proxyEnabled = on;
    emit changed();
    scheduleSave();
}

void DeviceStore::setPolicy(const QString &mac, DevicePolicyMode mode, const QString &target)
{
    DeviceRecord *d = find(mac);
    if (!d || (d->policyMode == mode && d->policyTarget == target))
        return;
    d->policyMode = mode;
    d->policyTarget = target;
    emit changed();
    scheduleSave();
}

void DeviceStore::applyTraffic(const QString &mac, qint64 sessionUp, qint64 sessionDown,
                               qint64 rateUp, qint64 rateDown, int connCount)
{
    DeviceRecord *d = find(mac);
    if (!d)
        return;
    // sessionUp/Down 是「本次会话至今的累计」，取相对上一拍的增量并入 today/total。
    const qint64 dUp = sessionUp - d->sessionUp;
    const qint64 dDown = sessionDown - d->sessionDown;
    d->sessionUp = sessionUp;
    d->sessionDown = sessionDown;
    d->rateUp = rateUp;
    d->rateDown = rateDown;
    d->connCount = connCount;
    if (dUp > 0 || dDown > 0) {
        rolloverTodayIfNeeded(*d);
        if (dUp > 0)   { d->totalUp += dUp;     d->todayUp += dUp; }
        if (dDown > 0) { d->totalDown += dDown; d->todayDown += dDown; }
        scheduleSave();
    }
    // 不发 changed()：流量每秒刷新、频率高，由控制器聚合后统一刷新模型，避免每设备各触发一次全表重建。
}

const DeviceRecord *DeviceStore::findByIp(const QString &ip) const
{
    if (ip.isEmpty())
        return nullptr;
    for (const DeviceRecord &d : m_devices)
        if (d.ip == ip)
            return &d;
    return nullptr;
}
