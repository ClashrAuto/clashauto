#pragma once

// 局域网设备发现引擎（M0，全程无需提权）。
//
// 工作流（一轮全量扫描）：
//   1) 从 QNetworkInterface 取本机活动 IPv4 + 掩码，算出要扫的 /24（或更小）网段与网关。
//   2) 主动探测：对网段每个 IP 异步 TCP connect 若干常见端口（80/443/445/22/62078...）——
//      目的不是真连上，而是**逼本机系统对每个存活 IP 做 ARP 解析**（M0 不发原始 ARP，靠系统）。
//      同时组播/广播发出 mDNS(5353) / SSDP(1900) / NetBIOS(137) 查询。
//   3) 读系统 ARP 表（QProcess `arp`，跨平台正则抽 IP+MAC）→ 得到 IP↔MAC 清单（存活集合）。
//   4) 收集名称/型号：mDNS PTR/SRV/TXT、SSDP LOCATION XML、NetBIOS 节点名、反向 DNS PTR。
//   5) OUI 厂商（MAC 前 24 位查内嵌表）。
//   6) 综合信号（OUI + 服务集 + 开放端口 + 主机名）推断设备类型。
//   7) 汇总成 QVector<DeviceRecord> 经 discovered() 交给控制器（→ DeviceStore 合并）。
//
// 热更新：全量扫描较重（手动/定时 ~5min）；控制器另用 refreshLiveness() 轻量重读 ARP 表 +
// 定向探测已知设备，仅更新在线态（3~5s）。名称解析监听持续开着，随到随更新。
#include "DeviceStore.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

class QUdpSocket;
class QTimer;

class LanScanner final : public QObject
{
    Q_OBJECT
public:
    explicit LanScanner(QObject *parent = nullptr);
    ~LanScanner() override;

    // 启动一轮全量扫描（异步）。忙时忽略重复调用。
    void scanFull();
    // 轻量在线态刷新：只重读 ARP 表 + 定向探测传入的已知 IP，更新在线/IP，不做名称解析。
    void refreshLiveness(const QStringList &knownIps);

    // 当前本机网段信息（供 UI 展示 / 网关保护）。这些是「主网卡」的——即挑出来做二层劫持的那张
    // 物理网卡；TUN(Meta/utun/wintun)、Hyper-V/WSL/VMware 网桥等虚拟网卡一律不参选（见 isVirtualIface）。
    QString localIp() const { return m_localIp; }
    QString localMac() const { return m_localMac; }
    QString gatewayIp() const { return m_gatewayIp; }
    // 主网卡的网关 MAC（本轮 ARP 表里，**只认主网卡那张表**——见 m_arpByIf 的说明）。
    QString gatewayMac() const;
    // 本机所有网卡 MAC（含虚拟网卡）——用来判定「这台设备就是本机」，不受主网卡选择影响。
    const QSet<QString> &localMacs() const { return m_localMacs; }
    // 该 IP 是否在**任意一张**物理网卡的子网内。有线接 A 路由、WiFi 接 B 路由时两个网段都算——
    // 透明网关每张卡各有一套端点/ArpSpoofer，两边设备都能劫持（见 LanGateway::NicSpec）。
    bool inAnyLanSubnet(const QString &ip) const;

    // 一张可做透明网关的物理网卡（喂给 LanGateway::configure）。
    struct NicInfo {
        QString name;       // OS 级接口名
        // 适配器**友好名**（Windows: "以太网 2"/"WLAN"；Linux/mac: 与 name 相同）。
        // ★ 这一项是给核心的 `interface-name` 用的，不是展示字段：mihomo 的
        //   `iface.ResolveInterface()` 是拿 Go 的 net.Interface.Name 做**精确 map 查找**，
        //   Windows 上那正是适配器 FriendlyName —— 给 LUID 名（name 的值，形如
        //   "ethernet_32775"）或 GUID 都查不到，返回 ErrIfaceNotFound，于是那个出站的
        //   **每一次拨号都失败且没有回退**。所以写配置前必须确认这一项非空且能反查得到。
        QString friendlyName;
        QString ip;         // 本机在该卡上的 IPv4
        QString mac;
        QString netmask;    // 点分
        QString gatewayIp;  // 该卡的默认网关
        QString gatewayMac; // 本轮 ARP 表里查到的网关 MAC（未解析出来则为空）
        // —— IPv6（Linux 尽力发现；mac/win 目前留空 → 该卡 v6 劫持自动 no-op）——
        QString routerLinkLocal6; // v6 默认路由器链路本地地址（`ip -6 route show default` 的 via）
        QString routerMac6;       // v6 路由器 MAC（`ip -6 neigh` 查 routerLinkLocal6）
        QString localGlobal6;     // 本机在该卡上的全局 v6（诊断/展示）
        QString prefix6;          // 该卡 v6 前缀（诊断/展示，如 "2408:xxxx::/64"）
    };
    // 全部可用物理网卡（第 0 个是主网卡）。缺 IP/MAC/掩码的会被剔除。
    QVector<NicInfo> physicalNics() const;
    // 全部网关的 MAC（按 m_gatewayIps 在本轮 ARP 表里查）——路由器可能有多个 IP，认 MAC 更稳。
    QSet<QString> gatewayMacs() const;

    /// ARP 表解析自测（`COAST_ARPPARSE_SELFTEST=1`，见 main_qml.cpp）。三端都能跑：解析器按
    /// 运行期参数分派而不是 #ifdef，所以 Linux 的 CI 也覆盖得到 Windows 那一种形态。
    ///
    /// ★ 为什么这一段值得一个自测：它是**文本解析**，而它的产物决定「每张网卡的网关 MAC」。
    ///   解析一旦错，enableDevice 会因为网关 MAC 为空而拒绝接管 —— 表现是所有设备都代理不了，
    ///   却没有任何崩溃或报错指向这里。用例里的样本全是真机 `arp -a`/`arp -n`/`arp -an` 原文。
    /// 通过返回 true，失败把差异打到 stdout 后返回 false。
    static bool runArpParseSelfTest();

    /// **实时**拓扑转储（`COAST_TOPO_DUMP=1`，见 main_qml.cpp）：重算一次网卡/路由拓扑 + 读一次
    /// 系统 ARP 表，然后把「这台机器上我们究竟看到了什么」交回调用方。
    ///
    /// ★ 为什么需要它：这一带的判断（谁是主网卡、每张卡的网关是谁、那个网关的 MAC 从哪张卡的
    ///   ARP 表里取）全都发生在进程内部，出了错只表现为「某张卡上的设备代理不了」。自测能验
    ///   解析器，验不了**这台真实机器上的解析结果**。有了它，真机排查的第一步从"猜"变成"看"。
    /// 异步（读 ARP 表要起子进程），报告经 onDone 交回。
    void probeTopology(std::function<void(QString)> onDone);

signals:
    // 一轮扫描（或轻量刷新）产出的设备快照（运行时字段已填，持久字段留空由 store 保留）。
    void discovered(QVector<DeviceRecord> devices);
    void scanningChanged(bool scanning);

private:
    // —— 网络拓扑 ——
    // 带缓存的入口：force=true（扫描）必重算，否则 kTopologyTtlMs 内复用上次结果。
    // refreshLiveness 每 5s 调一次，而枚举网卡 + 查路由表在 GUI 线程上是实打实的开销。
    void ensureTopology(bool force);
    void detectLocalTopology();       // 填 m_physIfaces/m_localMacs/m_gatewayIps + 主网卡那几个字段
    QVector<quint32> hostsToProbe() const; // 各物理网卡网段内待探测的主机 IP（网络/广播/本机除外）
    // 本机每张物理网卡各产出一条「本机」记录，附加到快照里。**每轮快照都必须带上**，
    // 否则控制器的 15s 陈旧判定会把本机判成掉线（开 TUN 时曾因此显示本机离线）。
    void appendSelfRecords(QVector<DeviceRecord> &out) const;

    // —— 主动探测（触发系统 ARP + 端口指纹）——
    // 探测分两阶段跑：
    //   ① 全网段每台主机只连 kArpPort 一个端口 —— 唯一目的是逼系统对存活主机做 ARP 解析；
    //   ② 读完 ARP 表后，只给「真的存在」的主机补其余指纹端口 + 反向 DNS。
    // 空 IP 不再被连 10 次，/24 的作业量从 2530 降到约 330。
    void runProbes(QVector<QPair<quint32, quint16>> jobs, std::function<void()> onDone);
    void startFingerprintPhase();

    // —— ARP 表 ——
    // onDone 在输出解析完（或进程起不来）后回调，用来串起两阶段探测。
    void readArpTable(std::function<void()> onDone = {}); // QProcess arp → m_arpByIf/m_arp
    void onArpOutput(const QByteArray &out);
    // 把 m_arpByIf 压平成 m_arp（同一 IP 出现在多张卡上时留任意一条——扁平表的消费方只关心
    // 「这个 IP 在不在、MAC 是什么」，网卡归属由下面 arpMacFor 那条精确路径负责）。
    void rebuildFlatArp();
    // （arpMacFor/arpKeyFor 声明在 LocalIface 之后 —— 类内的形参类型必须先声明。）

    // —— 名称/型号解析器（QUdpSocket，持续监听）——
    void setupMdns();
    void sendMdnsQueries();
    void onMdnsDatagram();
    void setupSsdp();
    void sendSsdpQuery();
    void onSsdpDatagram();
    void fetchSsdpLocation(const QString &url, const QString &ip);
    void setupNbns();
    void sendNbnsQueriesSliced(const QVector<quint32> &hosts); // 分片发，别一次占住 GUI 线程
    void sendNbnsQuery(quint32 ip);
    void onNbnsDatagram();
    void reverseDnsLookup(const QString &ip);

    // —— 汇总 & 分类 ——
    void assemble();                  // 合并各信号 → DeviceRecord 列表 → emit discovered（终版）
    // 出一版快照。final=false 是阶段①后的抢先快照（在线设备已知、名称待补），不结束扫描态。
    void emitSnapshot(bool final);
    DeviceType classify(const QString &mac, const QString &name, const QString &model,
                        const QString &vendor, const QSet<quint16> &ports,
                        const QSet<QString> &services) const;

    static quint32 ipToU32(const QHostAddress &a) { return a.toIPv4Address(); }
    static QString u32ToIp(quint32 v) { return QHostAddress(v).toString(); }

    // —— 每设备累积的解析信号（按 IP 归集，最后按 MAC 合并）——
    struct Signals {
        QString mac;
        QString hostname;    // 主机名（NetBIOS / 反向 DNS）
        QString friendly;    // mDNS 实例名 / SSDP friendlyName
        QString model;       // 型号
        QString vendor;      // OUI
        QSet<quint16> ports; // 探测到开放的端口
        QSet<QString> services; // mDNS 服务类型集（_airplay/_googlecast/_ipp/_smb...）
        bool alive = false;
    };
    Signals &sig(const QString &ip);   // 取/建某 IP 的信号槽

    // —— 状态 ——
    // 一张本机物理网卡（可作为二层劫持网卡的候选）。
    struct LocalIface {
        QString name;   // OS 级接口名（AF_PACKET/BPF/Npcap 绑定用）
        QString friendlyName;       // 适配器友好名（核心的 interface-name 要它，见 NicInfo）
        QString ip;
        QString mac;
        QString gatewayIp;          // 这张卡自己的默认网关（多网卡各不相同）
        quint32 gatewayMetric = 0;  // 该默认路由的**有效跃点数**（路由 metric + 接口 metric）。
                                    // 主网卡就按它最小者选——不是按枚举顺序。
        quint32 base = 0, mask = 0; // 网段（主机序）
        // —— IPv6（Linux 尽力发现）——
        QString gatewayLL6;         // v6 默认路由器链路本地地址
        QString gatewayMac6;        // v6 路由器 MAC
        QString global6;            // 本机在该卡上的全局 v6
        QString prefix6;            // 该卡 v6 前缀
    };
    // 某张卡自己那张 ARP 表里的 ip → mac。**绝不回落到别的卡**——那正是这次要修的东西。
    // 唯一的回落是「归不到任何接口」那一桶（解析降级时的保命路径，见 parseArpTable 与实现处）。
    QString arpMacFor(const LocalIface &f, const QString &ip) const;
    // 该卡在 m_arpByIf 里的键。Windows 用本机在该卡上的 IPv4（`arp -a` 的分段头给的就是它），
    // 其余平台用接口名（`arp -n`/`arp -an` 每行自带）。见 parseArpTable。
    static QString arpKeyFor(const LocalIface &f);
    // Linux 尽力发现 IPv6 拓扑（填 m_physIfaces 的 v6 字段）；其它平台为空实现。
    void detectIpv6Topology();

    bool m_scanning = false;
    QString m_localIp, m_localMac, m_gatewayIp; // 主网卡
    // ★ 这里**不再**留「主网卡网段」的副本（原 m_netBase/m_netMask + interfaceName/localNetmask/
    //   inPrimarySubnet）。多网卡支持落地后，网段判定一律走 m_physIfaces 里每张卡自己那份
    //   （inAnyLanSubnet / NicSpec.netmask）；再留一份只描述主网卡的副本，等于给「有线接 A 路由、
    //   WiFi 接 B 路由」这类场景埋一个只对其中一张卡成立的判据。
    QVector<LocalIface> m_physIfaces;     // 全部物理网卡（第 0 个即主网卡）
    QSet<QString> m_localMacs;            // 本机全部网卡 MAC（含虚拟网卡）
    QSet<QString> m_gatewayIps;           // 全部默认路由网关 IP
    // ★ ARP 表必须**按接口分开存**。系统 `arp -a` 的输出本来就是按接口分段的，以前被拍平成
    //   一张 ip→mac 全局表 —— 同时接两个路由器、而两台路由器又都用出厂默认网段（192.168.1.1
    //   这种）时，后读到的那条直接覆盖先读到的，于是**一张卡拿到另一台路由器的 MAC**。
    //   后果不是投毒失效（投毒帧声明的是"网关 IP 在本机 MAC"，与此无关），而是：
    //     · 反制触发器失灵 —— LanGateway 判「这帧是不是真网关自己发的」靠的就是拿源 MAC 比
    //       NicSpec::gatewayMac（LanGateway_linux.cpp 的 reassertNow 那处）。比不上就永远不
    //       反制，路由器每广播一次 ARP 就把设备解毒一次 ⇒ 教科书式的"时通时不通"；
    //     · 还原（heal）会把**错的** MAC 装回设备 ⇒ 关代理后黑洞到缓存老化。
    //   键的口径见 arpKeyFor。
    QHash<QString, QHash<QString, QString>> m_arpByIf; // 接口键 → (ip → mac)
    QHash<QString, QString> m_arp;         // 上表压平（ip → mac），给不关心网卡归属的消费方
    QHash<QString, Signals> m_sig;         // ip → 解析信号
    QTimer *m_settleTimer = nullptr;       // 探测发完后再等一小会儿收 ARP/名称，再 assemble
    QElapsedTimer m_topologyAge;           // 上次 detectLocalTopology 的时刻（配 ensureTopology 用）
    static constexpr int kTopologyTtlMs = 30000;

    QUdpSocket *m_mdns = nullptr;
    QUdpSocket *m_ssdp = nullptr;
    QUdpSocket *m_nbns = nullptr;

    void loadOui();
    QString ouiVendor(const QString &mac) const;
    QHash<QString, QString> m_oui;         // "aabbcc" → vendor（首字节小写无分隔）
    bool m_ouiLoaded = false;
};
