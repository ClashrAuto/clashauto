#include "DevicesController.h"
#include "../ClashService.h"
#include "../CoreController.h"
#include "../DeviceStore.h"
#include "../HistoryStore.h"
#include "../LanScanner.h"
#include "../net/LanGateway.h"

#include <QDateTime>

#include <cstdio>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <algorithm>

DevicesController::DevicesController(DeviceStore *store, ClashService *clash, CoreController *core,
                                    LanGateway *gateway, HistoryStore *history, QObject *parent)
    : QObject(parent), m_store(store), m_clash(clash), m_core(core), m_gateway(gateway),
      m_history(history)
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
    // 新设备提醒：首轮扫描后再出现的新设备 → 发信号（main 连托盘）。
    connect(m_store, &DeviceStore::deviceAdded, this, &DevicesController::onDeviceAdded);

    // 网关自身的错误（打不开网卡 / 协议栈起不来）此前无人接听：LanGateway::deviceError 一直是
    // 悬空信号，用户只看得到 enableDevice 那句泛化的「网关未就绪（需要 root/CAP_NET_RAW…）」——
    // Windows 上真正的原因「未检测到 Npcap」就这么被吞了。转成 gatewayError 送到设备页浮动提示。
    // ensureGatewayConfigured() 每轮扫描都会重试 open，同一句错误只报一次，避免刷屏。
    if (m_gateway) {
        connect(m_gateway, &LanGateway::deviceError, this,
                [this](const QString &, const QString &msg) {
                    if (msg == m_lastGatewayErr)
                        return;
                    m_lastGatewayErr = msg;
                    emit gatewayError(msg);
                });
    }

    // 读持久化的「新设备提醒」偏好（org/app 已在 main 设为 Coast）。
    m_newDeviceAlert = QSettings().value(QStringLiteral("devices/newDeviceAlert"), true).toBool();

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

    // 重启后自动恢复代理。「代理网络」开关是**持久**的（存在 coast.db 的 device 表），劫持是**运行时**的：
    // 上次退出时 aboutToQuit→disableAll() 把 ARP 全还原了，新进程里没有任何东西把它们重新劫持，
    // 于是开关明明还开着、设备却在走直连。而扫描/热更新只在设备页可见时才跑，用户不进那个页面
    // 就永远不会恢复。这里在启动后主动扫一轮（只在确实有设备开着代理时才扫，别给普通用户平白
    // 加一轮网段探测），拿到 IP/MAC/网关拓扑后由 onDiscovered → resumeProxies() 重新上劫持。
    if (hasProxiedDevices()) {
        QTimer::singleShot(2000, this, [this] {
            if (m_active)
                return; // 用户已经先一步进了设备页，那边已经扫过
            scan();
            m_livenessTimer->start(); // 之后跟着 IP/在线态变化持续补劫持
        });
    }
}

bool DevicesController::hasProxiedDevices() const
{
    if (!m_store)
        return false;
    for (const DeviceRecord &d : m_store->devices())
        if (d.proxyEnabled)
            return true;
    return false;
}

void DevicesController::resumeProxies()
{
    if (!m_gateway)
        return;
    const QStringList active = m_gateway->activeDevices();
    for (const DeviceRecord &d : m_store->devices()) {
        if (!d.proxyEnabled || !d.proxyable() || !d.online || d.ip.isEmpty())
            continue;
        const bool armed = active.contains(d.mac);
        if (armed && m_armedIp.value(d.mac) == d.ip)
            continue; // 已在劫持，且还是同一个 IP —— 无事可做
        if (armed)
            m_gateway->disableDevice(d.mac); // IP 变了：按旧 IP 的劫持早已无效，拆了重上
        QString err;
        if (m_gateway->enableDevice(d.mac, d.ip, DeviceStore::socksUser(d.mac), &err)) {
            m_armedIp.insert(d.mac, d.ip);
            m_resumeErr.remove(d.mac);
        } else {
            m_armedIp.remove(d.mac);
            // 每轮发现都会重试，同一条原因只报一次，免得 5s 刷一次提示条。
            if (!err.isEmpty() && m_resumeErr.value(d.mac) != err) {
                m_resumeErr.insert(d.mac, err);
                emit gatewayError(err);
            }
        }
    }
}

QString DevicesController::localIp() const { return m_scanner ? m_scanner->localIp() : QString(); }
QString DevicesController::gatewayIp() const { return m_scanner ? m_scanner->gatewayIp() : QString(); }
int DevicesController::deviceCount() const { return m_store ? m_store->devices().size() : 0; }

// 概览条上的「今日总量」：直接把台账里每台设备的 todayUp/Down 加起来（十几台设备，随 UI 绑定
// 每秒求一次和的开销可以忽略；也省得再维护一份会和台账走神的缓存）。
// 注意这里天然只统计**归属到设备**的流量：归不到任何设备的连接不进任何一台的 today 计数。
double DevicesController::totalTodayUp() const
{
    qint64 sum = 0;
    if (m_store)
        for (const DeviceRecord &d : m_store->devices())
            sum += d.todayUp;
    return static_cast<double>(sum);
}

double DevicesController::totalTodayDown() const
{
    qint64 sum = 0;
    if (m_store)
        for (const DeviceRecord &d : m_store->devices())
            sum += d.todayDown;
    return static_cast<double>(sum);
}
bool DevicesController::gatewayReady() const { return m_gateway && m_gateway->isAvailable(); }

void DevicesController::ensureGatewayConfigured()
{
    if (!m_gateway || !m_scanner)
        return;
    // 用当前扫描到的拓扑配置网关。**每张物理网卡都配**——有线接 A 路由、WiFi 接 B 路由时，
    // 两个网段的设备都要能代理；LanGateway 按设备 IP 落在哪张卡的网段自动选对应那套。
    // 网关 MAC 需从 ARP 表拿到；扫描过至少一轮后才有值（每轮都会重新 configure 刷新）。
    QVector<LanGateway::NicSpec> specs;
    const QVector<LanScanner::NicInfo> nics = m_scanner->physicalNics();
    specs.reserve(nics.size());
    for (const LanScanner::NicInfo &n : nics) {
        LanGateway::NicSpec s;
        s.ifname = n.name;
        s.localIp = n.ip;
        s.localMac = n.mac;
        s.gatewayIp = n.gatewayIp;
        s.gatewayMac = n.gatewayMac;
        s.netmask = n.netmask;
        s.routerLinkLocal6 = n.routerLinkLocal6; // IPv6 拓扑（可空 → 该卡 v6 劫持 no-op）
        s.routerMac6 = n.routerMac6;
        s.localGlobal6 = n.localGlobal6;
        s.prefix6 = n.prefix6;
        specs.append(s);
    }
    m_gateway->configure(specs, DeviceStore::kGatewayPort);
    // 起来了就清掉去重记忆：之后再坏（拔网卡等）还要能重新报一次。
    if (m_gateway->isAvailable())
        m_lastGatewayErr.clear();
}

void DevicesController::onDeviceAdded(const QString &mac)
{
    // 首轮扫描的设备在 mergeDiscovered 内触发（此时 m_firstScanDone 仍为 false）→ 不提醒，避免刷屏。
    if (!m_firstScanDone || !m_newDeviceAlert)
        return;
    const DeviceRecord *d = m_store->find(mac);
    if (d && !d->isSelf)
        emit newDeviceFound(d->displayName());
}

void DevicesController::setNewDeviceAlert(bool on)
{
    if (on == m_newDeviceAlert)
        return;
    m_newDeviceAlert = on;
    QSettings().setValue(QStringLiteral("devices/newDeviceAlert"), on);
    emit newDeviceAlertChanged();
}

void DevicesController::onDiscovered(const QVector<DeviceRecord> &devices)
{
    m_store->mergeDiscovered(devices);
    m_firstScanDone = true; // 首轮已并完 → 之后 deviceAdded 才提醒
    // 离线判定：本轮快照没刷新到、且 lastSeen 超 15s 的设备置离线（宽限 15s≈3 轮 5s 热更新，
    // 避免单次探测丢包造成的在线/离线抖动）。先收集 mac 再统一标记，避免边遍历边改的再入。
    // 本机（当前仍存在的网卡 MAC）永不判离线：程序在跑，本机必然在线；它的在线态不该依赖
    // ARP/探测（开增强模式时正是这里把本机判成了掉线）。网卡被拔掉 → 不在 localMacs 里 → 照常判离线。
    const QDateTime now = QDateTime::currentDateTime();
    const QSet<QString> selfMacs = m_scanner ? m_scanner->localMacs() : QSet<QString>();
    QStringList stale;
    for (const DeviceRecord &d : m_store->devices())
        if (d.online && !selfMacs.contains(d.mac) && d.lastSeen.isValid()
            && d.lastSeen.secsTo(now) > 15)
            stale << d.mac;
    for (const QString &mac : stale)
        m_store->markOffline(mac);
    // 扫描后拓扑（含网关 MAC）已知 → 配置网关，使 gatewayReady 反映真实可用性。
    ensureGatewayConfigured();
    // 台账里开着代理、当前却没在劫持的设备（重启后 / 换了 IP）在这里补上。
    resumeProxies();
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
    if (m_scanner) {
        m_lastScan.restart();
        m_scanner->scanFull();
    }
}

void DevicesController::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active) {
        // 进页面扫一次——但**别每次切回来都重扫**。全量扫描是这个页面最重的动作（整段网段的
        // TCP 探测），来回点几次导航就会反复触发；在此期间 5s 的 liveness 热更新已经在维持
        // 在线态/流量了，30s 内切回来没必要再来一轮。
        if (!m_lastScan.isValid() || m_lastScan.elapsed() > kRescanMinIntervalMs)
            scan();
        m_livenessTimer->start();
        m_connTimer->start();
        m_clock.restart();
    } else {
        m_connTimer->stop();
        // 还有设备开着代理时，5s 的在线态热更新**不能停**：设备换 IP / 掉线再上线之后要靠它
        // 触发 resumeProxies() 把劫持重新挂上。页面一关就停，等于代理只在设备页开着时才自愈。
        if (!hasProxiedDevices())
            m_livenessTimer->stop();
    }
}

void DevicesController::select(const QString &mac)
{
    if (mac == m_selectedMac)
        return;
    m_selectedMac = mac;
    const DeviceRecord *d = m_store->find(mac);
    m_connModel.setSourceIp(d ? d->ip : QString());
    // 网关代理的连接 sourceIP=127.0.0.1,还要靠 dev-<mac> 用户名归属(与 IP 取或)。
    m_connModel.setUser(d ? DeviceStore::socksUser(d->mac) : QString());
    refreshTopDomains(true); // 换设备：立刻重查，别让详情先闪一下上一台的域名
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
        // 能否开代理：本机/网关不可，跨网段（另一张网卡那边的网络）也不可——二层端点只绑主网卡。
        m_selectedDevice["proxyable"] = d->proxyable();
        m_selectedDevice["inLanSubnet"] = d->inLanSubnet;
        // 开关为什么不能开（QML 据此给一句人话）：""=可开 / self / gateway / foreign / offline
        m_selectedDevice["proxyBlockReason"] =
            d->isSelf      ? QStringLiteral("self")
            : d->isGateway ? QStringLiteral("gateway")
            : !d->inLanSubnet ? QStringLiteral("foreign")
            : (!d->online || d->ip.isEmpty()) ? QStringLiteral("offline")
                                             : QString();

        // Top 域名 + 近 7 天用量：都来自历史库（跨重启保留），按 TTL 节流，见 refreshTopDomains。
        refreshTopDomains(false);
        m_selectedDevice["topDomains"] = m_topDomains;
        m_selectedDevice["recentDays"] = m_recentDays;
    }
    emit selectedChanged();
}

// 「常用域名」和「近 7 天用量」都来自历史库。rebuildSelected 每秒都会跑（流量聚合完就刷一次
// 详情），每秒查两次 SQL 纯属浪费——按 5s TTL 节流；换选中设备时 force=true 立刻重查，
// 否则点开一台设备会先看到上一台的数据。
void DevicesController::refreshTopDomains(bool force)
{
    if (!m_history || m_selectedMac.isEmpty()) {
        m_topDomains.clear();
        m_recentDays.clear();
        return;
    }
    if (!force && m_topDomainsMac == m_selectedMac && m_topDomainsAge.isValid()
        && m_topDomainsAge.elapsed() < kTopDomainsTtlMs)
        return;

    m_topDomainsMac = m_selectedMac;
    m_topDomainsAge.restart();

    m_topDomains.clear();
    for (const HistoryStore::DomainTotal &d : m_history->topDomains(m_selectedMac, 7, 5)) {
        QVariantMap e;
        e["host"] = d.host;
        e["bytes"] = static_cast<double>(d.bytes);
        m_topDomains.append(e);
    }
    m_recentDays.clear();
    for (const HistoryStore::DayTotal &d : m_history->dailyTraffic(m_selectedMac, 7)) {
        QVariantMap e;
        e["day"] = d.day;
        e["up"] = static_cast<double>(d.up);
        e["down"] = static_cast<double>(d.down);
        m_recentDays.append(e);
    }
}

void DevicesController::setProxyEnabled(const QString &mac, bool on)
{
    const DeviceRecord *d = m_store->find(mac);
    if (!d)
        return;
    // 关闭永远允许（离线设备也得能撤销之前开的代理）；开启才做这些校验。
    if (on) {
        if (d->isSelf || d->isGateway) {
            emit gatewayError(d->isSelf
                                  ? QStringLiteral("本机不需要（也不能）代理自己")
                                  : QStringLiteral("网关是路由器本身，劫持它会打瘫整个网络"));
            return;
        }
        if (!d->inLanSubnet) {
            emit gatewayError(QStringLiteral("该设备不在本机任何一张网卡的网段内，无法代理"
                                            "（透明网关只能劫持与本机同网段的设备）"));
            return;
        }
        if (!d->online || d->ip.isEmpty()) {
            emit gatewayError(QStringLiteral("设备已离线，等它上线后再开启代理"));
            return;
        }
    }
    const QString ip = d->ip;
    m_store->setProxyEnabled(mac, on);
    m_store->save(); // 立刻落盘，供 ConfigBuilder 从库里读出来生成网关 listener + auth/IN-USER
    if (m_core)
        m_core->rebuildConfig(); // 重生成 full.yaml + 热重载 mihomo（网关口 + 每设备身份就绪）
    if (m_gateway) {
        ensureGatewayConfigured();
        if (on) {
            QString err;
            if (m_gateway->enableDevice(mac, ip, DeviceStore::socksUser(mac), &err)) {
                m_armedIp.insert(mac, ip); // 记住劫持所用的 IP（resumeProxies 据此发现 IP 变了）
                m_resumeErr.remove(mac);
            } else {
                m_armedIp.remove(mac);
                if (!err.isEmpty())
                    emit gatewayError(err);
            }
        } else {
            m_gateway->disableDevice(mac);
            m_armedIp.remove(mac);
            m_resumeErr.remove(mac);
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
    const QString user = DeviceStore::socksUser(mac); // 网关代理连接靠它归属(sourceIP=127.0.0.1)
    m_clash->fetchConnections([this, ip, user](QJsonArray arr) {
        for (const QJsonValue &v : arr) {
            const QJsonObject meta = v.toObject().value("metadata").toObject();
            const bool mine = meta.value("sourceIP").toString() == ip
                              || (!user.isEmpty() && meta.value("inboundUser").toString() == user);
            if (mine)
                m_clash->closeConnection(v.toObject().value("id").toString());
        }
    });
}

void DevicesController::exportCsv()
{
    const QString path = QFileDialog::getSaveFileName(nullptr, QStringLiteral("导出设备列表"),
                                                      QStringLiteral("devices.csv"),
                                                      QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit gatewayError(QStringLiteral("导出失败：无法写入 ") + path);
        return;
    }
    auto q = [](const QString &s) -> QString { // CSV 转义：含逗号/引号/换行则加引号并转义引号
        if (s.contains(',') || s.contains('"') || s.contains('\n'))
            return '"' + QString(s).replace('"', "\"\"") + '"';
        return s;
    };
    QTextStream out(&f); // Qt6 QTextStream 默认 UTF-8
    out << "name,ip,mac,type,vendor,model,online,proxied,totalDown,totalUp,firstSeen\n";
    for (const DeviceRecord &d : m_store->devices()) {
        out << q(d.displayName()) << ',' << d.ip << ',' << d.mac << ','
            << DeviceStore::typeKey(d.effectiveType()) << ',' << q(d.vendor) << ',' << q(d.model)
            << ',' << (d.online ? '1' : '0') << ',' << (d.proxyEnabled ? '1' : '0') << ','
            << d.totalDown << ',' << d.totalUp << ','
            << (d.firstSeen.isValid() ? d.firstSeen.toString(Qt::ISODate) : QString()) << '\n';
    }
    f.close();
    emit csvExported(path);
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

    // ip → mac、以及 dev-<mac> 用户名 → mac 两张映射（本拍）。
    // 后者是透明网关代理设备**能看到流量速率的关键**:被代理设备的流量是
    // 设备 → 用户态栈 → SOCKS(127.0.0.1:coast-gateway) → mihomo,故 mihomo 侧连接的 sourceIP
    // 恒为 127.0.0.1(SOCKS 在本机),按 sourceIP 归类全落空;真实设备身份在 inboundUser
    // (= dev-<去冒号mac>,见 ConfigBuilder 的 per-user listener + DeviceStore::socksUser)。
    QHash<QString, QString> ipToMac;
    QHash<QString, QString> userToMac;
    QString selfMac; // 本机（第一条 isSelf 记录）——回环来源的连接归到它名下
    for (const DeviceRecord &d : m_store->devices()) {
        if (!d.ip.isEmpty())
            ipToMac.insert(d.ip, d.mac);
        const QString u = DeviceStore::socksUser(d.mac);
        if (!u.isEmpty())
            userToMac.insert(u, d.mac);
        if (d.isSelf && selfMac.isEmpty())
            selfMac = d.mac;
    }

    const bool dbg = qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG");
    int dbgByIp = 0, dbgByUser = 0, dbgByLocal = 0, dbgUnattr = 0;
    for (const QVariant &v : conns) {
        const QVariantMap m = v.toMap();
        const QString ip = m.value("sourceIP").toString();
        const QString inUser = m.value("inboundUser").toString();
        QString mac = ipToMac.value(ip);
        if (!mac.isEmpty()) {
            ++dbgByIp;
        } else { // 网关代理:sourceIP=127.0.0.1 归不到设备 → 靠 inboundUser 归属
            mac = userToMac.value(inUser);
            if (!mac.isEmpty())
                ++dbgByUser;
        }
        // 源地址是本机自己的某个网卡（回环 / TUN 的 198.18.0.1 / …）、又不是网关代理进来的
        // → 这是本机自己发出的流量，记到「本机」那台设备名下。以前这类全部落进「未归属」丢掉，
        // 于是设备列表里本机那一行的速率/今日用量恒为 0——全机器最忙的一台反而永远显示没流量。
        if (mac.isEmpty() && DeviceStore::isLocalMachineIp(ip)
            && !inUser.startsWith(QStringLiteral("dev-"))) {
            mac = selfMac;
            if (!mac.isEmpty())
                ++dbgByLocal;
        }
        if (mac.isEmpty()) {
            ++dbgUnattr;
            if (dbg && dbgUnattr <= 3) // 归不上时把原始线索打出来，否则只能看到一个「未归=N」
                std::fprintf(stderr, "[DEV] 未归属样本 sourceIP=%s inboundUser=%s selfMac=%s\n",
                             ip.toLatin1().constData(), inUser.toLatin1().constData(),
                             selfMac.toLatin1().constData()),
                    std::fflush(stderr);
            continue;
        }
        const QString id = m.value("id").toString();
        const qint64 down = m.value("download").toLongLong();
        const qint64 up = m.value("upload").toLongLong();
        seen.insert(id);
        // 「最后访问的地址」：本拍才第一次见到这个连接 id = 设备刚发起的连接。用「新建」而不是
        // 「字节最多」来定义最后访问——后者会被一条挂着的大下载长期霸屏，看不到设备在访问什么。
        // （不额外解析 metadata.start：新 id 就是最新，省掉每秒几百次 RFC3339 解析。）
        if (!connBytes.contains(id))
            m_store->setLastHost(mac, m.value("host").toString());
        const QPair<qint64, qint64> last = connBytes.value(id, {0, 0});
        const qint64 dd = down >= last.first ? down - last.first : down;   // 新连接=全量；回退=当前值
        const qint64 du = up >= last.second ? up - last.second : up;
        connBytes.insert(id, {down, up});
        auto &acc = devDelta[mac];
        acc.first += dd;
        acc.second += du;
        devConn[mac] += 1;
        // （Top 域名不再在这里攒：以前那份 QHash 只活在内存里，重启就清零。改由 HistoryStore
        //   统一记账——连接断了落库、在途的实时合并，见 refreshTopDomains。）
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
        if ((rateDown > 0 || rateUp > 0) && qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG"))
            std::fprintf(stderr, "[DEV] %s rate down=%lld up=%lld conn=%d\n",
                         mac.toLatin1().constData(), static_cast<long long>(rateDown),
                         static_cast<long long>(rateUp), devConn.value(mac)),
                std::fflush(stderr);
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

    // 有连接时才打印(避免空闲每秒刷屏):按 sourceIP / inboundUser 各归属了多少。
    if (dbg && !conns.isEmpty())
        std::fprintf(stderr, "[DEV] conns=%d 归属:byIp=%d byUser=%d byLocal=%d 未归=%d\n",
                     int(conns.size()), dbgByIp, dbgByUser, dbgByLocal, dbgUnattr),
            std::fflush(stderr);

    m_totalRateUp = totalUp;
    m_totalRateDown = totalDown;

    // 流量是每秒高频更新：统一在这里刷新一次模型/详情/概览（applyTraffic 本身不发信号）。
    refreshModel();
    rebuildSelected();
    emit overviewChanged();
}
