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
    bool isScanning() const { return m_scanning; }

    // 当前本机网段信息（供 UI 展示 / 网关保护）。这些是「主网卡」的——即挑出来做二层劫持的那张
    // 物理网卡；TUN(Meta/utun/wintun)、Hyper-V/WSL/VMware 网桥等虚拟网卡一律不参选（见 isVirtualIface）。
    QString localIp() const { return m_localIp; }
    QString localMac() const { return m_localMac; }
    QString gatewayIp() const { return m_gatewayIp; }
    QString gatewayMac() const { return m_arp.value(m_gatewayIp); } // 网关 MAC（本轮 ARP 表里）
    QString interfaceName() const { return m_ifaceName; }
    // 主网卡子网掩码（点分，如 "255.255.255.0"）；未知返回空。供网关做「同网段直连旁路」。
    QString localNetmask() const;
    // 本机所有网卡 MAC（含虚拟网卡）——用来判定「这台设备就是本机」，不受主网卡选择影响。
    const QSet<QString> &localMacs() const { return m_localMacs; }
    // 所有默认路由的网关 IP（多网卡各一个）——用来判定「这台设备是某个网络的路由器」。
    const QSet<QString> &gatewayIps() const { return m_gatewayIps; }
    // 该 IP 是否在主网卡子网内（只有同网段设备能被 ARP 劫持）。
    bool inPrimarySubnet(const QString &ip) const;
    // 该 IP 是否在**任意一张**物理网卡的子网内。有线接 A 路由、WiFi 接 B 路由时两个网段都算——
    // 透明网关每张卡各有一套端点/ArpSpoofer，两边设备都能劫持（见 LanGateway::NicSpec）。
    bool inAnyLanSubnet(const QString &ip) const;

    // 一张可做透明网关的物理网卡（喂给 LanGateway::configure）。
    struct NicInfo {
        QString name;       // OS 级接口名
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
    void probePort(quint32 ip, quint16 port);

    // —— ARP 表 ——
    // onDone 在输出解析完（或进程起不来）后回调，用来串起两阶段探测。
    void readArpTable(std::function<void()> onDone = {}); // QProcess arp → m_arp（ip→mac）
    void onArpOutput(const QByteArray &out);

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
        QString ip;
        QString mac;
        QString gatewayIp;          // 这张卡自己的默认网关（多网卡各不相同）
        quint32 base = 0, mask = 0; // 网段（主机序）
        // —— IPv6（Linux 尽力发现）——
        QString gatewayLL6;         // v6 默认路由器链路本地地址
        QString gatewayMac6;        // v6 路由器 MAC
        QString global6;            // 本机在该卡上的全局 v6
        QString prefix6;            // 该卡 v6 前缀
    };
    // Linux 尽力发现 IPv6 拓扑（填 m_physIfaces 的 v6 字段）；其它平台为空实现。
    void detectIpv6Topology();

    bool m_scanning = false;
    QString m_localIp, m_localMac, m_gatewayIp, m_ifaceName; // 主网卡
    quint32 m_netBase = 0, m_netMask = 0; // 主网卡网段（主机序）
    QVector<LocalIface> m_physIfaces;     // 全部物理网卡（第 0 个即主网卡）
    QSet<QString> m_localMacs;            // 本机全部网卡 MAC（含虚拟网卡）
    QSet<QString> m_gatewayIps;           // 全部默认路由网关 IP
    QHash<QString, QString> m_arp;         // ip → mac（本轮 ARP 表）
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
