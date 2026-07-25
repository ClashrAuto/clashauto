#include "DeviceStore.h"
#include "AppConfig.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

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
    : QObject(parent), m_path(QDir(configDir).filePath("devices.json"))
{
    load();
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
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    f.close();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        DeviceRecord d;
        d.mac = normalizeMac(o.value("mac").toString());
        if (d.mac.isEmpty())
            continue;
        d.alias = o.value("alias").toString();
        d.typeOverride = typeFromKey(o.value("typeOverride").toString());
        d.firstSeen = QDateTime::fromString(o.value("firstSeen").toString(), Qt::ISODate);
        d.proxyEnabled = o.value("proxyEnabled").toBool();
        d.policyMode = modeFromKey(o.value("policyMode").toString());
        d.policyTarget = o.value("policyTarget").toString();
        d.totalUp = o.value("totalUp").toVariant().toLongLong();
        d.totalDown = o.value("totalDown").toVariant().toLongLong();
        d.todayUp = o.value("todayUp").toVariant().toLongLong();
        d.todayDown = o.value("todayDown").toVariant().toLongLong();
        d.todayDate = o.value("todayDate").toString();
        m_index.insert(d.mac, m_devices.size());
        m_devices.append(d);
    }
}

void DeviceStore::save()
{
    QJsonArray arr;
    for (const DeviceRecord &d : m_devices) {
        // 只落盘「有价值保留」的记录：有用户编辑或有累计流量的才存，纯发现的临时设备不写。
        const bool worth = !d.alias.isEmpty() || d.typeOverride != DeviceType::Unknown
                           || d.proxyEnabled || d.policyMode != DevicePolicyMode::Follow
                           || d.totalUp || d.totalDown || d.firstSeen.isValid();
        if (!worth)
            continue;
        QJsonObject o;
        o["mac"] = d.mac;
        if (!d.alias.isEmpty()) o["alias"] = d.alias;
        if (d.typeOverride != DeviceType::Unknown) o["typeOverride"] = typeKey(d.typeOverride);
        if (d.firstSeen.isValid()) o["firstSeen"] = d.firstSeen.toString(Qt::ISODate);
        if (d.proxyEnabled) o["proxyEnabled"] = true;
        if (d.policyMode != DevicePolicyMode::Follow) o["policyMode"] = modeKey(d.policyMode);
        if (!d.policyTarget.isEmpty()) o["policyTarget"] = d.policyTarget;
        if (d.totalUp) o["totalUp"] = QString::number(d.totalUp);
        if (d.totalDown) o["totalDown"] = QString::number(d.totalDown);
        if (d.todayUp) o["todayUp"] = QString::number(d.todayUp);
        if (d.todayDown) o["todayDown"] = QString::number(d.todayDown);
        if (!d.todayDate.isEmpty()) o["todayDate"] = d.todayDate;
        arr.append(o);
    }
    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    f.close();
    AppConfig::makeWritable(m_path); // qrc 拷来的目录可能只读；对齐工程惯例
    m_dirty = false;
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
        if (!f.ip.isEmpty())      d.ip = f.ip;
        if (!f.autoName.isEmpty()) d.autoName = f.autoName;
        if (!f.model.isEmpty())   d.model = f.model;
        if (!f.vendor.isEmpty())  d.vendor = f.vendor;
        if (f.autoType != DeviceType::Unknown) d.autoType = f.autoType;
        d.isSelf = f.isSelf;
        d.isGateway = f.isGateway;
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
