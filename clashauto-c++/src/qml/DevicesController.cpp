#include "DevicesController.h"
#include "../ClashService.h"
#include "../CoreController.h"
#include "../DeviceStore.h"
#include "../LanScanner.h"
#include "../net/LanGateway.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

DevicesController::DevicesController(DeviceStore *store, ClashService *clash, CoreController *core,
                                    LanGateway *gateway, QObject *parent)
    : QObject(parent), m_store(store), m_clash(clash), m_core(core), m_gateway(gateway)
{
    m_scanner = new LanScanner(this);

    connect(m_scanner, &LanScanner::discovered, this, &DevicesController::onDiscovered);
    connect(m_scanner, &LanScanner::scanningChanged, this, [this](bool s) {
        if (s != m_scanning) {
            m_scanning = s;
            emit scanningChanged();
        }
        emit topologyChanged(); // 扫描完拓扑已更新（SSDP 名称迟到也借此轻触发）
    });
    // 台账变化（编辑/发现/离线）→ 刷新列表模型 + 详情。
    connect(m_store, &DeviceStore::changed, this, [this] {
        refreshModel();
        rebuildSelected();
    });

    m_livenessTimer = new QTimer(this);
    m_livenessTimer->setInterval(5000);
    connect(m_livenessTimer, &QTimer::timeout, this, [this] {
        QStringList ips;
        for (const DeviceRecord &d : m_store->devices())
            if (!d.ip.isEmpty())
                ips << d.ip;
        m_scanner->refreshLiveness(ips);
    });

    m_connTimer = new QTimer(this);
    m_connTimer->setInterval(1000);
    connect(m_connTimer, &QTimer::timeout, this, &DevicesController::pollConnections);

    refreshModel();
}

QString DevicesController::localIp() const { return m_scanner ? m_scanner->localIp() : QString(); }
QString DevicesController::gatewayIp() const { return m_scanner ? m_scanner->gatewayIp() : QString(); }
int DevicesController::deviceCount() const { return m_store ? m_store->devices().size() : 0; }
bool DevicesController::gatewayReady() const { return m_gateway && m_gateway->isAvailable(); }

void DevicesController::ensureGatewayConfigured()
{
    if (!m_gateway || !m_scanner)
        return;
    // 用当前扫描到的拓扑配置网关（网卡/本机 IP+MAC/网关 IP+MAC + mihomo 专用网关端口）。
    // 网关 MAC 需从 ARP 表拿到；扫描过至少一轮后才有值。
    m_gateway->configure(m_scanner->interfaceName(), m_scanner->localIp(), m_scanner->localMac(),
                         m_scanner->gatewayIp(), m_scanner->gatewayMac(), DeviceStore::kGatewayPort);
}

void DevicesController::onDiscovered(const QVector<DeviceRecord> &devices)
{
    m_store->mergeDiscovered(devices);
    // 离线判定：本轮快照没刷新到、且 lastSeen 超 15s 的设备置离线（宽限 15s≈3 轮 5s 热更新，
    // 避免单次探测丢包造成的在线/离线抖动）。先收集 mac 再统一标记，避免边遍历边改的再入。
    const QDateTime now = QDateTime::currentDateTime();
    QStringList stale;
    for (const DeviceRecord &d : m_store->devices())
        if (d.online && d.lastSeen.isValid() && d.lastSeen.secsTo(now) > 15)
            stale << d.mac;
    for (const QString &mac : stale)
        m_store->markOffline(mac);
    // 扫描后拓扑（含网关 MAC）已知 → 配置网关，使 gatewayReady 反映真实可用性。
    ensureGatewayConfigured();
    // mergeDiscovered/markOffline 会 emit changed → refreshModel/rebuildSelected 已连；此处补拓扑通知。
    emit topologyChanged();
    emit overviewChanged();
}

void DevicesController::refreshModel()
{
    m_model.setDevices(m_store->devices());
    emit overviewChanged();
}

void DevicesController::scan()
{
    if (m_scanner)
        m_scanner->scanFull();
}

void DevicesController::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active) {
        scan();                       // 进页面立刻扫一次
        m_livenessTimer->start();
        m_connTimer->start();
        m_clock.restart();
    } else {
        m_livenessTimer->stop();
        m_connTimer->stop();
    }
}

void DevicesController::select(const QString &mac)
{
    if (mac == m_selectedMac)
        return;
    m_selectedMac = mac;
    const DeviceRecord *d = m_store->find(mac);
    m_connModel.setSourceIp(d ? d->ip : QString());
    rebuildSelected();
    emit selectedChanged();
}

void DevicesController::rebuildSelected()
{
    m_selectedDevice.clear();
    const DeviceRecord *d = m_selectedMac.isEmpty() ? nullptr : m_store->find(m_selectedMac);
    if (d) {
        m_selectedDevice["mac"] = d->mac;
        m_selectedDevice["ip"] = d->ip;
        m_selectedDevice["name"] = d->displayName();
        m_selectedDevice["alias"] = d->alias;
        m_selectedDevice["autoName"] = d->autoName;
        m_selectedDevice["model"] = d->model;
        m_selectedDevice["vendor"] = d->vendor;
        m_selectedDevice["typeKey"] = DeviceStore::typeKey(d->effectiveType());
        m_selectedDevice["typeOverride"] = DeviceStore::typeKey(d->typeOverride);
        m_selectedDevice["online"] = d->online;
        m_selectedDevice["isSelf"] = d->isSelf;
        m_selectedDevice["isGateway"] = d->isGateway;
        m_selectedDevice["proxyEnabled"] = d->proxyEnabled;
        m_selectedDevice["policyMode"] = DeviceStore::modeKey(d->policyMode);
        m_selectedDevice["policyTarget"] = d->policyTarget;
        m_selectedDevice["rateUp"] = static_cast<double>(d->rateUp);
        m_selectedDevice["rateDown"] = static_cast<double>(d->rateDown);
        m_selectedDevice["connCount"] = d->connCount;
        m_selectedDevice["sessionUp"] = static_cast<double>(d->sessionUp);
        m_selectedDevice["sessionDown"] = static_cast<double>(d->sessionDown);
        m_selectedDevice["totalUp"] = static_cast<double>(d->totalUp);
        m_selectedDevice["totalDown"] = static_cast<double>(d->totalDown);
        m_selectedDevice["todayUp"] = static_cast<double>(d->todayUp);
        m_selectedDevice["todayDown"] = static_cast<double>(d->todayDown);
        m_selectedDevice["firstSeen"] = d->firstSeen.isValid()
                                            ? d->firstSeen.toString("yyyy-MM-dd") : QString();
        m_selectedDevice["onlineSince"] = d->onlineSince.isValid()
                                              ? d->onlineSince.toMSecsSinceEpoch() : 0;
        m_selectedDevice["protectable"] = !(d->isSelf || d->isGateway); // 可劫持（本机/网关不可）
    }
    emit selectedChanged();
}

void DevicesController::setProxyEnabled(const QString &mac, bool on)
{
    const DeviceRecord *d = m_store->find(mac);
    if (!d || d->isSelf || d->isGateway)
        return; // 本机/网关保护：不允许开代理
    const QString ip = d->ip;
    m_store->setProxyEnabled(mac, on);
    m_store->save(); // 立刻落盘，供 ConfigBuilder 读 devices.json 生成网关 listener + auth/IN-USER
    if (m_core)
        m_core->rebuildConfig(); // 重生成 full.yaml + 热重载 mihomo（网关口 + 每设备身份就绪）
    if (m_gateway) {
        ensureGatewayConfigured();
        if (on) {
            QString err;
            if (!m_gateway->enableDevice(mac, ip, DeviceStore::socksUser(mac), &err)
                && !err.isEmpty())
                emit gatewayError(err);
        } else {
            m_gateway->disableDevice(mac);
        }
    }
    emit topologyChanged(); // gatewayReady 可能变化
}

void DevicesController::setAlias(const QString &mac, const QString &alias)
{
    m_store->setAlias(mac, alias.trimmed());
}

void DevicesController::setTypeOverride(const QString &mac, const QString &typeKey)
{
    m_store->setTypeOverride(mac, DeviceStore::typeFromKey(typeKey));
}

void DevicesController::setPolicy(const QString &mac, const QString &modeKey, const QString &target)
{
    m_store->setPolicy(mac, DeviceStore::modeFromKey(modeKey), target.trimmed());
    m_store->save();             // 立刻落盘，供 ConfigBuilder
    if (m_core)
        m_core->rebuildConfig(); // 生成 IN-USER 规则 + 热重载
}

void DevicesController::closeDeviceConnections(const QString &mac)
{
    const DeviceRecord *d = m_store->find(mac);
    if (!d || !m_clash)
        return;
    const QString ip = d->ip;
    m_clash->fetchConnections([this, ip](QJsonArray arr) {
        for (const QJsonValue &v : arr) {
            const QJsonObject c = v.toObject();
            if (c.value("metadata").toObject().value("sourceIP").toString() == ip)
                m_clash->closeConnection(c.value("id").toString());
        }
    });
}

void DevicesController::pollConnections()
{
    if (!m_clash || m_connInFlight)
        return;
    m_connInFlight = true;
    m_clash->fetchConnections([this](QJsonArray arr) {
        m_connInFlight = false;
        QVariantList list;
        list.reserve(arr.size());
        for (const QJsonValue &v : arr) {
            const QJsonObject c = v.toObject();
            const QJsonObject meta = c.value("metadata").toObject();
            QVariantMap m;
            QString host = meta.value("host").toString();
            if (host.isEmpty())
                host = meta.value("destinationIP").toString();
            QString type = meta.value("type").toString();
            if (type.isEmpty())
                type = meta.value("network").toString();
            const QJsonArray chains = c.value("chains").toArray();
            m["host"] = host;
            m["type"] = type;
            m["chain"] = chains.isEmpty() ? QStringLiteral("-") : chains.first().toString();
            m["id"] = c.value("id").toString();
            m["download"] = static_cast<qlonglong>(c.value("download").toInteger());
            m["upload"] = static_cast<qlonglong>(c.value("upload").toInteger());
            m["sourceIP"] = meta.value("sourceIP").toString();
            m["inboundUser"] = meta.value("inboundUser").toString();
            m["offline"] = false;
            list.append(m);
        }
        m_connModel.setRaw(list); // 详情连接列表（按选中设备 sourceIP 过滤）
        aggregate(list);
    });
}

void DevicesController::aggregate(const QVariantList &conns)
{
    // dt：距上一拍毫秒。首拍或过久用 1000ms 兜底。
    qint64 dt = m_clock.isValid() ? m_clock.restart() : 1000;
    if (dt <= 0)
        dt = 1000;

    // 每连接 id → 上次累计字节，逐连接取增量（>=0），避免连接关闭导致的和值回退。
    QHash<QString, QPair<qint64, qint64>> &connBytes = m_connBytes; // id → (down,up)（跨拍保留）
    QSet<QString> seen;
    QHash<QString, QPair<qint64, qint64>> devDelta; // mac → (dDown,dUp) 本拍
    QHash<QString, int> devConn;                    // mac → 活动连接数

    // ip → mac 映射（本拍）
    QHash<QString, QString> ipToMac;
    for (const DeviceRecord &d : m_store->devices())
        if (!d.ip.isEmpty())
            ipToMac.insert(d.ip, d.mac);

    for (const QVariant &v : conns) {
        const QVariantMap m = v.toMap();
        const QString ip = m.value("sourceIP").toString();
        const QString mac = ipToMac.value(ip);
        if (mac.isEmpty())
            continue;
        const QString id = m.value("id").toString();
        const qint64 down = m.value("download").toLongLong();
        const qint64 up = m.value("upload").toLongLong();
        seen.insert(id);
        const QPair<qint64, qint64> last = connBytes.value(id, {0, 0});
        const qint64 dd = down >= last.first ? down - last.first : down;   // 新连接=全量；回退=当前值
        const qint64 du = up >= last.second ? up - last.second : up;
        connBytes.insert(id, {down, up});
        auto &acc = devDelta[mac];
        acc.first += dd;
        acc.second += du;
        devConn[mac] += 1;
    }
    // 清理本拍未出现的连接 id（已关闭，字节已计入）。
    for (auto it = connBytes.begin(); it != connBytes.end();) {
        if (!seen.contains(it.key()))
            it = connBytes.erase(it);
        else
            ++it;
    }

    // 应用到台账：累加会话/累计，算速率；并汇总总速率。曾有速率、本拍无流量的设备归零一次。
    qint64 totalUp = 0, totalDown = 0;
    QSet<QString> active;
    for (auto it = devDelta.constBegin(); it != devDelta.constEnd(); ++it) {
        const QString mac = it.key();
        const qint64 dDown = it.value().first;
        const qint64 dUp = it.value().second;
        active.insert(mac);
        Prev &p = m_prev[mac];
        p.down += dDown;
        p.up += dUp;
        const qint64 rateDown = dt > 0 ? dDown * 1000 / dt : 0;
        const qint64 rateUp = dt > 0 ? dUp * 1000 / dt : 0;
        m_store->applyTraffic(mac, p.up, p.down, rateUp, rateDown, devConn.value(mac));
        totalUp += rateUp;
        totalDown += rateDown;
    }
    // 归零：上拍活动、本拍不活动的设备速率清 0（会话/累计保留）。
    for (auto it = m_prev.begin(); it != m_prev.end(); ++it) {
        if (!active.contains(it.key())) {
            m_store->applyTraffic(it.key(), it.value().up, it.value().down, 0, 0, 0);
        }
    }

    m_totalRateUp = totalUp;
    m_totalRateDown = totalDown;

    // 流量是每秒高频更新：统一在这里刷新一次模型/详情/概览（applyTraffic 本身不发信号）。
    refreshModel();
    rebuildSelected();
    emit overviewChanged();
}
