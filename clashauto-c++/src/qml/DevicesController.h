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
#include <QVariantMap>
#include <QVector>

class DeviceStore;
class LanScanner;
class ClashService;
class CoreController;
class LanGateway;
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
    Q_PROPERTY(QString selectedMac READ selectedMac NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap selectedDevice READ selectedDevice NOTIFY selectedChanged)
    // 透明网关是否就绪（Linux 且有 CAP_NET_RAW/root 且网卡已配置）。QML 据此决定代理开关是否真正生效
    // 或显示提示。随拓扑/配置变化，走 topologyChanged 通知重取。
    Q_PROPERTY(bool gatewayReady READ gatewayReady NOTIFY topologyChanged)
    // 新设备提醒（蹭网检测）：首轮扫描后再发现的新设备 → 托盘通知。QSettings 持久化。
    Q_PROPERTY(bool newDeviceAlert READ newDeviceAlert WRITE setNewDeviceAlert NOTIFY newDeviceAlertChanged)

public:
    DevicesController(DeviceStore *store, ClashService *clash, CoreController *core,
                      LanGateway *gateway, QObject *parent = nullptr);

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

signals:
    void scanningChanged();
    void topologyChanged();
    void overviewChanged();
    void selectedChanged();
    void gatewayError(const QString &message); // 开代理失败（无权限/网卡等）——QML 可提示
    void newDeviceAlertChanged();
    void newDeviceFound(const QString &name);  // 首轮后发现新设备 → main 连到托盘通知
    void csvExported(const QString &path);     // 导出完成 → QML 可提示路径

private:
    void onDiscovered(const QVector<class DeviceRecord> &devices);
    void refreshModel();          // store → 列表模型（含排序）
    void rebuildSelected();       // 重算 selectedDevice map
    void pollConnections();       // 拉 /connections → 聚合每设备流量 + 喂连接模型
    void aggregate(const QVariantList &conns);
    void ensureGatewayConfigured(); // 用当前扫描到的拓扑配置 LanGateway（每次开代理前确保）

    DeviceStore *m_store = nullptr;
    ClashService *m_clash = nullptr;
    CoreController *m_core = nullptr;
    LanGateway *m_gateway = nullptr;
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

    // 每设备的会话累计（单调，算速率 + 落台账）。key = mac。
    struct Prev { qint64 up = 0, down = 0; qint64 elapsedMs = 0; };
    QHash<QString, Prev> m_prev;
    // 每设备 域名→累计字节（Top 域名排行）。key = mac。
    QHash<QString, QHash<QString, qint64>> m_devDomains;
    bool m_newDeviceAlert = true;
    bool m_firstScanDone = false;
    // 上一次已提示过的网关错误（LanGateway::deviceError）——每轮扫描都会重试 open，用它去重。
    QString m_lastGatewayErr;
    // 上次全量扫描的时刻：进页面时用它去抖，避免来回切导航反复触发重扫（scan() 里 restart）。
    QElapsedTimer m_lastScan;
    static constexpr int kRescanMinIntervalMs = 30000;
    // 每连接 id → 上次累计字节 (down,up)：逐连接取增量，避免连接关闭时和值回退。
    QHash<QString, QPair<qint64, qint64>> m_connBytes;
    QElapsedTimer m_clock;
};
