#pragma once

// 设备台账（configDir/devices.json）——「设备」页的持久层。
//
// 设计要点：
//  - **以 MAC 为主键**：局域网设备的 IP 会随 DHCP 漂移，MAC 才是稳定身份。扫描发现的
//    实时字段（当前 IP、在线、主机名、厂商、自动识别的类型/型号）每次刷新覆盖；
//    **用户可编辑字段**（别名、类型覆盖、代理开关、每设备策略）与累计流量长期保留。
//  - 增量落盘：任一可持久字段变化才写文件（防止 3~5s 热更新把磁盘写爆）。
//  - 无 YAML/JSON 手术：这里是纯 JSON（QJsonDocument），可放心结构化读写（不同于 *.yaml）。
//
// 本类只管「存」；发现逻辑在 LanScanner，聚合流量在 DevicesController。
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

// 设备类型（自动识别 + 用户可覆盖）。值即用于 QML 选图标 / i18n key。
enum class DeviceType {
    Unknown,
    Phone,     // 手机
    Tablet,    // 平板
    Computer,  // 电脑（PC/Mac）
    Router,    // 路由器 / 网关
    TvBox,     // 电视 / 电视盒 / 投屏
    Speaker,   // 智能音箱
    Printer,   // 打印机
    Camera,    // 摄像头
    GameConsole, // 游戏机
    Nas,       // NAS / 存储
    IoT,       // 其它智能家居 / IoT
};

// 每设备的代理策略（写进 ConfigBuilder → 生成 IN-USER 规则）。
enum class DevicePolicyMode {
    Follow,  // 跟随全局（不生成专属规则）
    Rule,    // 规则分流（走指定策略组，默认主选择组）
    Global,  // 全局：指定具体节点/策略组
    Direct,  // 强制直连
    Reject,  // 禁止上网（REJECT）
};

// 单台设备的完整记录（持久字段 + 运行时字段合一，落盘时只写持久字段）。
struct DeviceRecord {
    // —— 身份（持久）——
    QString mac;            // 主键，规范化小写冒号分隔 aa:bb:cc:dd:ee:ff
    QString alias;          // 用户备注名（优先于自动 name 显示）
    DeviceType typeOverride = DeviceType::Unknown; // 用户强制类型；Unknown=用自动识别
    QDateTime firstSeen;    // 首次发现

    // —— 策略（持久）——
    bool proxyEnabled = false;                       // 「代理网络」开关（M1 起真正生效；M0 仅存）
    DevicePolicyMode policyMode = DevicePolicyMode::Follow;
    QString policyTarget;                            // Global 模式下的目标节点/策略组名

    // —— 累计流量（持久，跨会话）——
    qint64 totalUp = 0;     // 历史累计上行字节
    qint64 totalDown = 0;   // 历史累计下行字节
    qint64 todayUp = 0;     // 今日上行
    qint64 todayDown = 0;   // 今日下行
    QString todayDate;      // 今日记账所属日期（yyyy-MM-dd），换天清零 todayUp/Down

    // —— 运行时（不落盘，扫描/聚合每拍覆盖）——
    QString ip;             // 当前 IP
    QString autoName;       // 自动识别名（主机名/friendlyName）
    QString model;          // 型号串（如 iPhone15,2 / MI TV）
    QString vendor;         // OUI 厂商名
    DeviceType autoType = DeviceType::Unknown; // 自动识别类型
    bool online = false;
    QDateTime lastSeen;
    QDateTime onlineSince;  // 本次连续在线起点（算在线时长）
    qint64 rateUp = 0;      // 实时上行速率（字节/秒，由聚合器填）
    qint64 rateDown = 0;    // 实时下行速率
    int connCount = 0;      // 当前活动连接数
    qint64 sessionUp = 0;   // 本次会话（本次程序运行内）累计
    qint64 sessionDown = 0;
    bool isSelf = false;    // 本机（任一本机网卡的 MAC）
    bool isGateway = false; // 网关（任一默认路由的网关 IP/MAC）
    // 是否位于「可劫持网段」= 用于二层收发的主物理网卡的子网。同时连两个网络时，另一张网卡
    // 那边的设备（含它那边的路由器）无法被 ARP 劫持 —— 二层端点只绑一张网卡。
    bool inLanSubnet = false;

    // 能否开「代理网络」：本机/网关不动，且必须在可劫持网段内。
    bool proxyable() const { return !isSelf && !isGateway && inLanSubnet; }

    // 展示名：别名 > 自动名 > 型号 > 厂商 > IP
    QString displayName() const;
    // 最终类型：用户覆盖优先
    DeviceType effectiveType() const
    {
        return typeOverride != DeviceType::Unknown ? typeOverride : autoType;
    }
};

class DeviceStore final : public QObject
{
    Q_OBJECT
public:
    explicit DeviceStore(const QString &configDir, QObject *parent = nullptr);

    // 全量当前设备（发现列表 + 台账合并后的视图；运行时字段来自最近扫描/聚合）。
    const QVector<DeviceRecord> &devices() const { return m_devices; }
    // 按 MAC 取（找不到返回 nullptr）。
    DeviceRecord *find(const QString &mac);
    const DeviceRecord *find(const QString &mac) const;
    const DeviceRecord *findByIp(const QString &ip) const; // 按当前 IP 反查（流量聚合用）

    // 扫描器汇报一批「发现结果」：按 MAC upsert 运行时字段（身份/在线/名称等），保留持久字段。
    // 返回是否有任何行发生（展示相关的）变化——供控制器决定是否通知模型。
    void mergeDiscovered(const QVector<DeviceRecord> &found);
    // 把某 MAC 标记离线（热更新时 ARP 表里消失）。
    void markOffline(const QString &mac);

    // —— 用户编辑（会持久化）——
    void setAlias(const QString &mac, const QString &alias);
    void setTypeOverride(const QString &mac, DeviceType type);
    void setProxyEnabled(const QString &mac, bool on);
    void setPolicy(const QString &mac, DevicePolicyMode mode, const QString &target);

    // —— 流量记账（由聚合器每拍调用）——
    // 用「本次会话累计值」更新运行时速率与会话/累计流量；delta 部分并入 today/total。
    void applyTraffic(const QString &mac, qint64 sessionUp, qint64 sessionDown,
                      qint64 rateUp, qint64 rateDown, int connCount);

    // 立刻落盘（防抖后由定时器触发；退出时也强制存一次）。
    void save();

    // 类型 ↔ 字符串（JSON 存储 + i18n key 派生）。
    static QString typeKey(DeviceType t);
    static DeviceType typeFromKey(const QString &k);
    static QString modeKey(DevicePolicyMode m);
    static DevicePolicyMode modeFromKey(const QString &k);
    static QString normalizeMac(const QString &raw); // 统一小写冒号；非法返回空
    // 每设备的 mihomo SOCKS 身份用户名（ConfigBuilder 生成 authentication/IN-USER 与
    // LanGateway 拨号必须用同一派生，否则规则/流量归属对不上）：dev-<去冒号小写 mac>。
    static QString socksUser(const QString &mac);
    static bool isLoopbackIp(const QString &ip)
    {
        return ip.startsWith(QLatin1String("127.")) || ip == QLatin1String("::1");
    }
    // 这个 sourceIP 是不是「本机自己」。**本机发出的连接在核心眼里从来不是它的局域网 IP**：
    //   · 走系统代理(127.0.0.1:7890) → sourceIP = 127.0.0.1
    //   · 开增强(TUN) → sourceIP = TUN 网卡地址（mihomo 默认 198.18.0.1，实测就是它）
    // 两种都按局域网 IP 归属不上，于是设备列表里「本机」那一行的流量/域名/今日用量恒为 0 ——
    // 全机器最忙的一台反而永远显示没流量。判据取「回环 或 本机任一网卡的地址」，把 TUN、
    // 虚拟网卡这些将来可能冒出来的出口一并覆盖住，不写死某个网段。
    // 注意调用顺序：网关代理的设备 sourceIP 也是回环，但带 inboundUser=dev-*，**必须先按用户名
    // 归属**，否则会把别的设备的流量记到本机头上。
    static bool isLocalMachineIp(const QString &ip);
    // mihomo 专用「网关」socks inbound 端口：被劫持设备的流量经此口带每设备用户名进 mihomo。
    // 独立于主混合口(7890)，让 Coast 自己的测速仍免认证走 7890。ConfigBuilder 生成此 listener，
    // LanGateway 拨号连此端口——两边必须一致。
    static constexpr quint16 kGatewayPort = 7899;

signals:
    void changed();          // 任一展示字段变化（控制器据此刷新模型）
    void deviceAdded(QString mac); // 新设备首次出现（供托盘「蹭网」通知）

private:
    void load();
    void scheduleSave();     // 防抖落盘
    int indexOf(const QString &mac) const;
    void rolloverTodayIfNeeded(DeviceRecord &d) const;

    QString m_path;
    QVector<DeviceRecord> m_devices;
    QHash<QString, int> m_index; // mac → m_devices 下标
    bool m_dirty = false;
};
