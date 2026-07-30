#pragma once

// 透明网关编排器 —— DevicesController 用的稳定公共 API（跨平台可编译）。
//
// 内部（仅 Linux 实现，其余平台为 no-op 桩，isAvailable()=false）：
//   ArpSpoofer（对被劫持设备伪装成网关、对网关伪装成设备 = 双向 MITM）
//     + IL2Endpoint（AF_PACKET 二层收发）
//     + NetStack（lwIP 用户态栈：把捕获到的该设备 IP 帧终结成 TCP/UDP 连接）
//     + Socks5Tcp/Udp（每条连接拨 mihomo，user=dev-<mac> → IN-USER 规则/inboundUser 归属）。
//
// 安全：只劫持 enableDevice() 显式开启的设备；disableAll() / 析构 / 崩溃看门狗必须**可靠还原
// ARP**（给被劫持设备重发「网关的真实 MAC」），否则设备断网。启动时应调用 recoverFromCrash()
// 读取上次留下的劫持清单先还原再继续。
//
// 权限：Linux 需 CAP_NET_RAW/root。isAvailable() 反映「平台支持 且 至少一张网卡能打开二层」。
//
// **多网卡**：有线接 A 路由、WiFi 接 B 路由时，两个网段的设备都可代理。每张物理网卡各有一套
// {二层端点 + ArpSpoofer}，共用同一个 NetStack（lwIP 只能有一个实例，但可挂多个 netif）。
// 开代理时按设备 IP 落在哪张卡的子网里，自动选对应的那套。
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

class ProxyConfigStore; // CoastCore 出站配置快照的持有者（不完整类型，只经 shared_ptr 透传给工作线程）
class RuleEngine;        // CoastCore 分流规则引擎（同上）
class DnsResolver;       // DNS 旁听器：学核心分配的 fake-ip → 域名（同上）

class LanGateway : public QObject
{
    Q_OBJECT
public:
    // 一张物理网卡的拓扑。mac 均为 "aa:bb:cc:dd:ee:ff"；netmask 为点分（如 "255.255.255.0"）。
    struct NicSpec {
        QString ifname;     // OS 级接口名（AF_PACKET/BPF/Npcap 绑定用）
        QString localIp;    // 本机在这张卡上的 IPv4
        QString localMac;
        QString gatewayIp;  // 这张卡的默认网关
        QString gatewayMac;
        QString netmask;    // 该卡子网掩码；空/非法则这张卡不启用（出方向没法定路由）
        // —— IPv6 拓扑（可选；缺项则这张卡的 v6 劫持自动 no-op，不影响 v4）——
        QString routerLinkLocal6; // v6 默认路由器的链路本地地址（fe80::…）——NdpSpoofer 投毒的目标
        QString routerMac6;       // v6 路由器 MAC（通常同 gatewayMac；单列以防路由器 v4/v6 用不同 NIC）
        QString localGlobal6;     // 本机在该卡上的全局 v6（仅诊断/展示用，数据路径不依赖）
        QString prefix6;          // 该卡的 v6 前缀（如 "2408:xxxx::/64"，仅诊断/展示用）
    };

    explicit LanGateway(QObject *parent = nullptr);
    ~LanGateway() override;

    // 配置全部可用物理网卡 + mihomo SOCKS 端点。每次扫描后（网段/网关变化）都可重配；
    // 已有活动劫持的网卡不会被重建（避免断流）。socksPort 通常 = config.mixedPort(7890)。
    //
    // netmask 还用于「同网段直连旁路」：被劫持设备发往「本网段内且非网关本身」的帧不进用户态栈、
    // 照常二层直达——否则设备回给本机 LAN IP 的包会被 lwIP 误终结，导致本机无法直连该设备
    // （SSH/网页/共享）。
    void configure(const QVector<NicSpec> &nics, quint16 socksPort);

    // 平台是否可用（至少一张网卡的二层端点 + 协议栈就绪）。DevicesController.gatewayReady 返回它。
    bool isAvailable() const;
    // 该 IP 是否落在某张已就绪网卡的子网里（= 能不能对它开代理）。
    bool canProxy(const QString &ip) const;

    // 开始劫持某设备：ip+mac 为目标，socksUser 为其 mihomo 身份（dev-<mac 短哈希>）。
    // 成功后该设备所有 IP 流量经用户态栈拨 mihomo。失败置 *err。
    bool enableDevice(const QString &mac, const QString &ip, const QString &socksUser, QString *err);
    // 停止劫持某设备并还原其 ARP。
    void disableDevice(const QString &mac);
    // 停止全部（退出/急停）：还原所有被劫持设备的 ARP。
    void disableAll();

    // 启动时调用：若上次异常退出留有劫持清单，先给这些设备发还原 ARP（panic-restore）。
    void recoverFromCrash();

    // 灰度：把 CoastCore 的进程内出站接进数据面（#10 的 10b/10c）。默认关时**不装** CoreDialerFactory，
    // NetStack 保持默认 Socks5OutboundFactory —— 与现状完全一致（零行为变化）。
    //   enabled=true  → 给 NetStack 装 CoreDialerFactory(store, 回退=Socks5OutboundFactory(每设备 7899))，
    //                   并 setRouter：cfg.mode() Direct→"DIRECT"、Global→cfg.selected()、Rule→""（回退 mihomo）。
    //   enabled=false → 撤回默认 Socks5OutboundFactory（全走 mihomo）。
    // store/rules 用 shared_ptr 跨线程安全传递（数据面在专用工作线程；本调用投递到该线程执行）。
    // 可重入：切节点/改模式/换订阅后重复调即热更新（配合 ProxyConfigStore::reload 的原子换手）。
    // dns：DNS 旁听器（可空）。装上后 NetStack 会把 DNS 劫持应答里学到的「核心分配的 fake-ip → 域名」
    //   用于 accept 时改写拨号目标——否则进程内出站拿到假 IP 原样拨、节点必然 i/o timeout（真机实测）。
    // strict：判不了的连接**拒绝回退核心**、直接失败（默认关）。见 CoreDialerFactory::setStrict。
    void setCoastCore(bool enabled, bool strict, std::shared_ptr<ProxyConfigStore> store,
                      std::shared_ptr<RuleEngine> rules, std::shared_ptr<DnsResolver> dns);

    QStringList activeDevices() const; // 当前被劫持的 mac 列表

signals:
    void deviceError(const QString &mac, const QString &message);
    void statusChanged(); // 劫持集合变化
    // 被动 ARP/NDP 监视发现冲突（有人冒充网关/本机在代理我，或有人在抢我劫持的设备）。
    // kind：0=别人在代理我(SelfImpersonated)，1=我的设备被争抢(DeviceContended)。
    // offenderMac=冒充者 MAC；subjectIp=被冒充/被抢的 IP；subjectMac=被抢设备 MAC（kind=1 才有值）。
    void securityAlert(int kind, const QString &offenderMac, const QString &subjectIp,
                       const QString &subjectMac);

private:
    class Impl;
    Impl *d;
};
