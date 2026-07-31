#pragma once

// 「设备」页的门面控制器（薄胶水，风格对齐 SubscriptionsController/AboutController）。
// 组装：LanScanner（发现）+ DeviceStore（台账）+ DeviceListModel（左列表）+
// DeviceConnectionsModel（右详情连接）。自持一个 1s 连接轮询（仅页面活动时开），把 /connections
// 按 sourceIP 聚合成每设备实时速率/会话/累计流量 —— 对已经过 mihomo 的设备生效（M0：主要是本机；
// M1 ARP 劫持接入后其它设备的流量也会带真实 sourceIP/inboundUser 出现）。
//
// M0 不做劫持：proxyEnabled 只写台账（供 M1 生效）；此处开关会给出「M1 才真正生效」的语义。
#include "DeviceConnectionsModel.h"
#include "DeviceListModel.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <memory>

struct AppConfig;
class DeviceStore;
class LanScanner;
class ClashService;
class CoreController;
class LanGateway;
class HistoryStore;
class ProxyConfigStore; // CoastCore 出站配置快照持有者（灰度）
class RuleEngine;        // CoastCore 分流规则引擎（灰度）
class DnsResolver;       // DNS 旁听器：学核心分配的 fake-ip → 域名（灰度）
class QTimer;

class DevicesController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(DeviceListModel *model READ model CONSTANT)
    Q_PROPERTY(DeviceConnectionsModel *connModel READ connModel CONSTANT)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(QString localIp READ localIp NOTIFY topologyChanged)
    Q_PROPERTY(QString gatewayIp READ gatewayIp NOTIFY topologyChanged)
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY overviewChanged)
    Q_PROPERTY(int onlineCount READ onlineCount NOTIFY overviewChanged)
    Q_PROPERTY(int proxiedCount READ proxiedCount NOTIFY overviewChanged)
    Q_PROPERTY(double totalRateUp READ totalRateUp NOTIFY overviewChanged)
    Q_PROPERTY(double totalRateDown READ totalRateDown NOTIFY overviewChanged)
    // 全部设备**今日**累计上/下行（概览条显示的就是这两个；实时总速率在状态页已经有了）。
    Q_PROPERTY(double totalTodayUp READ totalTodayUp NOTIFY overviewChanged)
    Q_PROPERTY(double totalTodayDown READ totalTodayDown NOTIFY overviewChanged)
    Q_PROPERTY(QString selectedMac READ selectedMac NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap selectedDevice READ selectedDevice NOTIFY selectedChanged)
    // 透明网关是否就绪（Linux 且有 CAP_NET_RAW/root 且网卡已配置）。QML 据此决定代理开关是否真正生效
    // 或显示提示。随拓扑/配置变化，走 topologyChanged 通知重取。
    Q_PROPERTY(bool gatewayReady READ gatewayReady NOTIFY topologyChanged)
    // 新设备提醒（蹭网检测）：首轮扫描后再发现的新设备 → 托盘通知。QSettings 持久化。
    Q_PROPERTY(bool newDeviceAlert READ newDeviceAlert WRITE setNewDeviceAlert NOTIFY newDeviceAlertChanged)
    // 邻居安全监视（ArpWatch）：当前是否有活动威胁（有人代理本机 / 抢我劫持的设备）——驱动设备页横幅显隐。
    Q_PROPERTY(bool underAttack READ underAttack NOTIFY securityChanged)
    // 当前活动的安全告警列表（每项 {kind,offenderMac,subjectIp,mac,name}），供横幅展开显示。
    Q_PROPERTY(QVariantList securityAlerts READ securityAlerts NOTIFY securityChanged)
    // 灰度：进程内出站（实验）。默认关 = 零行为变化（网关全走 mihomo）。设置页开关绑定它。
    Q_PROPERTY(bool coastCoreEnabled READ coastCoreEnabled WRITE setCoastCoreEnabled NOTIFY coastCoreEnabledChanged)
    Q_PROPERTY(bool coastCoreStrict READ coastCoreStrict WRITE setCoastCoreStrict NOTIFY coastCoreStrictChanged)
    // 本机 HTTP/SOCKS 入站（CoastCore 的第二个入口）。开=监听 kLocalInboundPort，关=不监听。
    Q_PROPERTY(bool localInboundEnabled READ localInboundEnabled WRITE setLocalInboundEnabled NOTIFY localInboundEnabledChanged)
    Q_PROPERTY(int localInboundPort READ localInboundPort CONSTANT)
    // 一行可读状态（「12 连接 · ↑1.2 MB ↓8.4 MB」）。见 localInboundStatus() 上方的理由。
    Q_PROPERTY(QString localInboundStatus READ localInboundStatus NOTIFY localInboundStatusChanged)

public:
    DevicesController(DeviceStore *store, ClashService *clash, CoreController *core,
                      LanGateway *gateway, HistoryStore *history, const AppConfig &config,
                      QObject *parent = nullptr);

    DeviceListModel *model() { return &m_model; }
    DeviceConnectionsModel *connModel() { return &m_connModel; }
    bool scanning() const { return m_scanning; }
    QString localIp() const;
    QString gatewayIp() const;
    int deviceCount() const;
    int onlineCount() const { return m_model.onlineCount(); }
    int proxiedCount() const { return m_model.proxiedCount(); }
    double totalRateUp() const { return static_cast<double>(m_totalRateUp); }
    double totalRateDown() const { return static_cast<double>(m_totalRateDown); }
    double totalTodayUp() const;   // 台账里所有设备 todayUp 之和
    double totalTodayDown() const; // 同上，下行
    QString selectedMac() const { return m_selectedMac; }
    QVariantMap selectedDevice() const { return m_selectedDevice; }
    bool gatewayReady() const; // Linux 网关可用性（LanGateway::isAvailable）

    // —— UI 动作 ——
    Q_INVOKABLE void scan();                       // 手动全量扫描
    Q_INVOKABLE void setActive(bool active);       // 页面显隐：控制热更新定时器 + 连接轮询
    Q_INVOKABLE void select(const QString &mac);   // 选中设备（刷新详情 + 连接过滤）
    Q_INVOKABLE void setProxyEnabled(const QString &mac, bool on);
    Q_INVOKABLE void setAlias(const QString &mac, const QString &alias);
    Q_INVOKABLE void setTypeOverride(const QString &mac, const QString &typeKey);
    Q_INVOKABLE void setPolicy(const QString &mac, const QString &modeKey, const QString &target);
    Q_INVOKABLE void closeDeviceConnections(const QString &mac); // 断开该设备全部活动连接
    Q_INVOKABLE void exportCsv();                                // 导出设备列表为 CSV（弹保存对话框）

    bool newDeviceAlert() const { return m_newDeviceAlert; }
    void setNewDeviceAlert(bool on);

    bool underAttack() const { return !m_secAlerts.isEmpty(); }
    QVariantList securityAlerts() const;
    Q_INVOKABLE void dismissSecurityAlerts(); // 用户手动关掉横幅（威胁若仍在，下次观察到会重新出现）

    // —— 灰度：进程内出站（实验；10c）——
    ~DevicesController() override; // 收本机入站（工厂是裸指针，得显式收）

    bool coastCoreEnabled() const { return m_coastCore; }
    bool coastCoreStrict() const { return m_coastStrict; }
    bool localInboundEnabled() const { return m_inboundPort > 0; }
    int localInboundPort() const { return kLocalInboundPort; }
    // ★ 本机入站的流量在别处**一点都看不到**：连接列表和流量图都来自 mihomo 的 REST，
    //   而这条路压根不经过核心。不给个读数的话，用户开了开关只能盲信。这就是那个最小可见性。
    QString localInboundStatus() const;
    // 落盘 config.yaml + 重建快照 + 把开关意图推给网关数据面。默认关时网关全走 mihomo（零行为变化）。
    Q_INVOKABLE void setCoastCoreEnabled(bool on);
    // 严格模式：判不了的连接拒绝回退核心、直接失败。只有 coastCoreEnabled 开着时才有意义。
    Q_INVOKABLE void setCoastCoreStrict(bool on);
    Q_INVOKABLE void setLocalInboundEnabled(bool on);

signals:
    void scanningChanged();
    void topologyChanged();
    void overviewChanged();
    void selectedChanged();
    void gatewayError(const QString &message); // 开代理失败（无权限/网卡等）——QML 可提示
    void newDeviceAlertChanged();
    void newDeviceFound(const QString &name);  // 首轮后发现新设备 → main 连到托盘通知
    void csvExported(const QString &path);     // 导出完成 → QML 可提示路径
    void securityChanged();                    // 安全告警集合变化 → 横幅/徽标刷新
    void securityAlertRaised(const QString &title, const QString &body); // 新威胁 → main 连托盘通知
    void coastCoreEnabledChanged();            // 灰度开关变化 → 设置页开关刷新
    void localInboundEnabledChanged();         // 本机入站开关变化 → 设置页开关刷新
    void localInboundStatusChanged();          // 本机入站读数刷新（1s 一跳，仅在它开着时）
    void coastCoreStrictChanged();

private:
    void onDiscovered(const QVector<class DeviceRecord> &devices);
    void refreshModel();          // store → 列表模型（含排序）
    void rebuildSelected();       // 重算 selectedDevice map
    void pollConnections();       // 拉 /connections → 聚合每设备流量 + 喂连接模型
    void aggregate(const QVariantList &conns);
    void ensureGatewayConfigured(); // 用当前扫描到的拓扑配置 LanGateway（每次开代理前确保）
    // 本机 v6 前缀变化 → rebuildConfig。v6 没有固定私网段，规则跟着网络走，会过期。
    void syncLanPrefixRules();
    // 把台账里「代理开着」但当前没在劫持的设备重新上劫持（幂等，每轮发现后调）。
    // 两个场景：①重启 Coast——开关是持久的，劫持是运行时的，新进程里没人重新劫持；
    // ②设备换了 IP（DHCP 续租）——按旧 IP 的劫持已经失效，得按新 IP 重上。
    void resumeProxies();
    bool hasProxiedDevices() const; // 台账里是否还有开着代理的设备

    // —— 灰度：进程内出站（10b）——
    // 读 full.yaml 的 proxies（其节点名与核心/选中节点一致，subscribe.yaml 的裸名少了「- 订阅名」后缀，
    // 对不上 selected）+ 当前模式/选中 → 组装 ProxyConfig 快照灌进 m_pcfgStore（原子换手热更新），
    // 并把规则喂给 m_ruleEngine（本单元 Rule 模式回退核心，规则仅备用）。
    void rebuildCoastCoreConfig();
    // 本机混合入站的起/停。端口 0（默认）时 start 是 no-op ⇒ 零行为变化。
    void persistLocalInbound(int port);
    // 「策略组 → 叶子」的落盘缓存：核心缺席时重放**含用户手选**的那张表，
    // 而不是退回 full.yaml 的默认结构（那会静默换掉用户选的节点）。见 .cpp 上方的说明。
    QString groupMapCachePath() const;
    void saveGroupMapCache(const QHash<QString, QString> &map) const;
    QHash<QString, QString> loadGroupMapCache() const;
    void startLocalInbound();
    void stopLocalInbound();
    // rebuildCoastCoreConfig + 重新把开关意图推给网关（保持热更新）。仅在灰度开着时才动作。
    void refreshCoastCore();
    void persistCoastCore(bool on); // 只改 config.yaml 的 coastcore 键，保留其余内容
    void persistCoastCoreStrict(bool on); // 同上，改 coastcore_strict 键

    DeviceStore *m_store = nullptr;
    ClashService *m_clash = nullptr;
    CoreController *m_core = nullptr;
    LanGateway *m_gateway = nullptr;
    HistoryStore *m_history = nullptr; // 常用域名从它查（跨重启保留）
    LanScanner *m_scanner = nullptr;
    DeviceListModel m_model;
    DeviceConnectionsModel m_connModel;

    QTimer *m_livenessTimer = nullptr; // 5s 轻量在线态刷新
    QTimer *m_connTimer = nullptr;     // 1s 连接轮询（聚合流量）
    bool m_active = false;
    bool m_scanning = false;
    bool m_connInFlight = false;
    QString m_selectedMac;
    QVariantMap m_selectedDevice;
    qint64 m_totalRateUp = 0, m_totalRateDown = 0;

    void onDeviceAdded(const QString &mac); // store.deviceAdded → 首轮后发提醒
    // 内核起停 → 劫持跟着上/撤（被劫持设备的唯一出口就是内核，见 .cpp 里的说明）。
    void onCoreRunningChanged(bool up);

public:
    // 系统电源事件（PowerWatcher）。本机一睡就不再转发，而设备的 ARP 还指着本机 → 必须在挂起前
    // 撤劫持还原，醒来再补回去。**handleSleep 必须同步做完**（调用方在系统给的挂起窗口里调它）。
    void handleSleep();
    void handleWake();

private:

    // —— 邻居安全监视（ArpWatch → LanGateway::securityAlert）——
    void onSecurityAlert(int kind, const QString &offenderMac, const QString &subjectIp,
                         const QString &subjectMac);
    void sweepSecurityAlerts();  // TTL 过期：一段时间没再观察到的威胁视为已停止，清出横幅/徽标
    void refreshContended();     // 把「争抢中」的 mac 集合喂给列表模型（打行徽标）
    // 一条活动告警。key = "kind|offenderMac|subjectIp"。
    struct SecAlert {
        int kind = 0;
        QString offenderMac, subjectIp, mac, name;
        qint64 firstMs = 0, lastMs = 0;
    };
    QHash<QString, SecAlert> m_secAlerts;    // 活动告警（横幅/徽标数据源；dismiss/TTL 会清）
    QHash<QString, qint64> m_secLastNotify;  // key → 上次托盘提醒时刻（**不**随 dismiss 清，只做去重节流）
    QTimer *m_secTimer = nullptr;            // TTL 巡检
    QElapsedTimer m_secClock;                // 单调时钟
    static constexpr int kSecTtlMs = 150000;      // 超过这么久没再观察到该威胁 → 判定已停止
    static constexpr int kSecRenotifyMs = 1800000; // 同一威胁再次弹托盘的最小间隔（30 分钟）

    // 每设备的会话累计（单调，算速率 + 落台账）。key = mac。
    struct Prev { qint64 up = 0, down = 0; qint64 elapsedMs = 0; };
    QHash<QString, Prev> m_prev;
    // 选中设备的 Top 域名（来自 HistoryStore，含在途连接）。rebuildSelected 每秒都会跑，
    // 不能每次都去查库——按 kTopDomainsTtlMs 节流，选中设备变化时立即失效重查。
    QVariantList m_topDomains;
    QVariantList m_recentDays; // 近 7 天每天的 {day,up,down}（同样来自 HistoryStore）
    QString m_topDomainsMac;
    QElapsedTimer m_topDomainsAge;
    static constexpr int kTopDomainsTtlMs = 5000;
    void refreshTopDomains(bool force);
    bool m_newDeviceAlert = true;
    bool m_firstScanDone = false;
    // 上一次已提示过的网关错误（LanGateway::deviceError）——每轮扫描都会重试 open，用它去重。
    QString m_lastGatewayErr;
    // mac → 当前劫持所用的 IP。用来发现「设备换了 IP」（旧劫持已失效，需重上）。
    QHash<QString, QString> m_armedIp;
    // mac → 上次自动恢复劫持失败的原因。resumeProxies 每轮都会重试，同一条只提示一次。
    QHash<QString, QString> m_resumeErr;
    // 内核在不在跑（CoreController::statusChanged 的第三个参数）。劫持只在它为真时上。
    bool m_coreUp = false;
    // 本会话是否已为「恢复劫持」补过一次配置重生成（full.yaml 的 coast-gateway listener/IN-USER）。
    bool m_resumeConfigSynced = false;
    // 本机 v6 前缀的上一次快照 + 是否已记过基线（见 syncLanPrefixRules）。
    QStringList m_lanPrefixes6;
    bool m_lanPrefixSynced = false;
    // 上次全量扫描的时刻：进页面时用它去抖，避免来回切导航反复触发重扫（scan() 里 restart）。
    QElapsedTimer m_lastScan;
    static constexpr int kRescanMinIntervalMs = 30000;
    // 每连接 id → 上次累计字节 (down,up)：逐连接取增量，避免连接关闭时和值回退。
    QHash<QString, QPair<qint64, qint64>> m_connBytes;
    QElapsedTimer m_clock;

    // —— 灰度：进程内出站 ——
    QString m_configDir;                          // config.yaml / full.yaml 所在（构造时从 AppConfig 取）
    bool m_coastCore = false;                     // 灰度开关（默认关 = 零行为变化）
    // 本机入站的默认端口。**不与 mihomo 的混合端口（7890）冲突**，两者并存，
    // 这样同一台机器上可以 A/B 对比两个引擎。
    static constexpr int kLocalInboundPort = 7891;
    int m_inboundPort = 0;                        // 本机混合入站端口（0=关，默认）
    int m_mixedPort = 7890;                       // mihomo 混合端口：本机入站的回退目标
    class MixedInbound *m_localInbound = nullptr; // 本机 HTTP/SOCKS 入站（CoastCore 第二个入口）
    class CoreDialerFactory *m_localInboundFactory = nullptr; // 入站不持有工厂，由本类管生命周期
    QTimer *m_inboundStatTimer = nullptr;         // 本机入站读数刷新（只在它开着时跑）
    bool m_coastStrict = false;                   // 严格模式（默认关）
    std::shared_ptr<ProxyConfigStore> m_pcfgStore; // 出站配置快照持有者（app 生命周期常驻，与网关 worker 共享）
    std::shared_ptr<RuleEngine> m_ruleEngine;   // 分流规则引擎（本单元备用；Rule 模式回退核心）
    // DNS 旁听器：交给网关装到 NetStack 上，学核心分配的 fake-ip → 域名（见 LanGateway::setCoastCore）。
    std::shared_ptr<DnsResolver> m_dnsResolver;
    static QString groupFingerprint(const QHash<QString, QString> &m); // 组映射指纹（见 .cpp）

    QString m_ccMode;      // 上次构建时的模式（Rule/Global/Direct）——变了才热更新，避免每次轮询白重建
    QString m_ccSelected;  // 上次构建时的全局选中节点名
    QString m_ccGroupFp;   // 上次构建时的「组→叶子」映射指纹（组内换节点也要触发重建）
};
