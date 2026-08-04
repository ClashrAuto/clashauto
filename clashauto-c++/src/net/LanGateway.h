#pragma once

#include <memory>

class ProxyConfigStore;
class RuleEngine;
class DnsResolver;

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

    // —— 数据面选择 ——
    // 默认 lwIP（现状）。打开 tproxy 后：ARP/NDP 劫持照旧（那是「设备什么都不用改」的来源），
    // 但劫持来的**数据帧不再进用户态**——内核转发 + nftables TPROXY 直接投给核心。
    //
    // 必须在 configure() 之前设好：configureLocal 是按当前模式决定「建不建 NetStack」的。
    // 中途改模式需要先 disableAll() 再重新 configure()（现在没有这个用法，也不打算有：
    // 模式在启动时从 AppConfig 定下来，改了要重启——这比在运行中把一半设备切到另一条路安全）。
    struct DatapathSpec {
        bool tproxy = false;
        quint16 tproxyPort = 0; // 核心的 tproxy 入站端口（ConfigBuilder 发的 coast-tproxy）
        quint16 dnsPort = 0;    // 核心的 DNS 监听端口；0 = 不劫持 DNS
        // macOS 的内核态数据面：pf 的 rdr + DIOCNATLOOK（见 PfRules）。与 tproxy 互斥 ——
        // TPROXY 是 Linux netfilter 独有的，BSD 上没有等价物。两者都为 false 时走 lwIP。
        bool pf = false;
        quint16 redirPort = 0;  // 核心的 redir 入站端口（ConfigBuilder 发的 coast-redir）
    };
    void setDatapath(const DatapathSpec &spec);

    /// 启用/停用 **CoastCore 进程内出站**（网关这条路）。
    /// 开着时网关终结出的连接不再经回环 SOCKS 拨 mihomo，而是在进程内直接出站 ——
    /// 那一跳实测占网关软件成本的 **65%**（docs/gateway-bottleneck-audit.md 第十节）。
    /// · store  = 出站配置快照来源（节点表）。为空 = 等同于关。
    /// · rules  = Rule 模式的首命中匹配引擎；可为空（那时只有 Global/Direct 判得了）。
    /// · strict = 拒绝回退核心，让「还差多少」变成明确的失败 + GatewayDiag 的 cc= 分布。
    /// 关掉时立刻回到「全走 mihomo」，与从未启用完全一致。可热切换。
    /// · dns    = DNS 旁听器：把核心分配的 fake-ip 反查成域名再拨。**不给它，域名类流量
    ///            就只能拿着假 IP 出站、必然路由不到**，进程内出站等于只对 IP 直连类生效。
    void setCoastCore(bool enabled, bool strict, std::shared_ptr<ProxyConfigStore> store,
                      std::shared_ptr<RuleEngine> rules, std::shared_ptr<DnsResolver> dns);

    // 平台是否可用（至少一张网卡的二层端点 + 协议栈就绪）。DevicesController.gatewayReady 返回它。
    bool isAvailable() const;
    // 该 IP 是否落在某张已就绪网卡的子网里（= 能不能对它开代理）。
    bool canProxy(const QString &ip) const;

    // 开始劫持某设备：ip+mac 为目标，socksUser 为其 mihomo 身份（dev-<mac 短哈希>）。
    // 成功后该设备所有 IP 流量经用户态栈拨 mihomo。失败置 *err。
    // reject=true：该设备被设为「禁网」。TCP 靠核心的 IN-USER 规则拦，但 UDP/DNS 不带用户名
    // 进核心，只能在网关层丢 —— 见 NetStack 里 DeviceInfo::reject 的论证。
    bool enableDevice(const QString &mac, const QString &ip, const QString &socksUser,
                      bool reject, QString *err);
    // 停止劫持某设备并还原其 ARP。
    void disableDevice(const QString &mac);
    // 停止全部（退出/急停）：还原所有被劫持设备的 ARP。
    void disableAll();

    /// 彻底停：还原全部 ARP/NDP + 销毁数据面对象 + **拆掉 TPROXY 的内核规则**。幂等。
    ///
    /// ★ 必须显式接到 aboutToQuit，**不能指望析构函数**：真机实测 SIGTERM(pkill) 之后 nft 表、
    ///   ip rule、route_localnet 全都还在——进程退出时 LanGateway 并没有被析构。而"规则还在、
    ///   没有进程接管 TPROXY"= 被覆盖的设备**完全断网**，且它不会自己好（要等应用重启后
    ///   removeStale 才清）。disableAll 只清空设备集合，拆不掉表与 sysctl。
    void shutdown();

    // 启动时调用：若上次异常退出留有劫持清单，先给这些设备发还原 ARP（panic-restore）。
    void recoverFromCrash();

    QStringList activeDevices() const; // 当前被劫持的 mac 列表

signals:
    void deviceError(const QString &mac, const QString &message);
    void statusChanged(); // 劫持集合变化
    // 被动 ARP/NDP 监视发现冲突（有人冒充网关/本机在代理我，或有人在抢我劫持的设备）。
    // kind：0=别人在代理我(SelfImpersonated)，1=我的设备被争抢(DeviceContended)。
    // offenderMac=冒充者 MAC；subjectIp=被冒充/被抢的 IP；subjectMac=被抢设备 MAC（kind=1 才有值）。
    void securityAlert(int kind, const QString &offenderMac, const QString &subjectIp,
                       const QString &subjectMac);
    /// 从设备自己的帧里现学到它换了 v4 地址（DHCP 续约/重连）。
    /// 上层据此更新台账并按新地址重挂劫持 —— 否则投毒仍打向旧地址，设备会静默掉回直连。
    void deviceIpChanged(const QString &mac, const QString &newIp);

private:
    class Impl;
    Impl *d;
};
