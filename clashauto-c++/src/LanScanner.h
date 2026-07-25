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

#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

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

    // 当前本机网段信息（供 UI 展示 / 网关保护）。
    QString localIp() const { return m_localIp; }
    QString localMac() const { return m_localMac; }
    QString gatewayIp() const { return m_gatewayIp; }
    QString gatewayMac() const { return m_arp.value(m_gatewayIp); } // 网关 MAC（本轮 ARP 表里）
    QString interfaceName() const { return m_ifaceName; }

signals:
    // 一轮扫描（或轻量刷新）产出的设备快照（运行时字段已填，持久字段留空由 store 保留）。
    void discovered(QVector<DeviceRecord> devices);
    void scanningChanged(bool scanning);

private:
    // —— 网络拓扑 ——
    void detectLocalTopology();       // 填 m_localIp/Mac/gatewayIp/ifaceName/m_subnet*
    QVector<quint32> hostsToProbe() const; // 网段内待探测的主机 IP（网络/广播/本机除外）

    // —— 主动探测（触发系统 ARP + 端口指纹）——
    void probeArp(const QVector<quint32> &hosts); // 异步 TCP 连若干端口
    void probePort(quint32 ip, quint16 port);

    // —— ARP 表 ——
    void readArpTable();              // QProcess arp → m_arp（ip→mac）
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
    void sendNbnsQuery(quint32 ip);
    void onNbnsDatagram();
    void reverseDnsLookup(const QString &ip);

    // —— 汇总 & 分类 ——
    void assemble();                  // 合并各信号 → DeviceRecord 列表 → emit discovered
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
    bool m_scanning = false;
    QString m_localIp, m_localMac, m_gatewayIp, m_ifaceName;
    quint32 m_netBase = 0, m_netMask = 0; // 网段（主机序）
    QHash<QString, QString> m_arp;         // ip → mac（本轮 ARP 表）
    QHash<QString, Signals> m_sig;         // ip → 解析信号
    int m_pendingProbes = 0;               // 在途探测计数（归零 → 收尾）
    QTimer *m_settleTimer = nullptr;       // 探测发完后再等一小会儿收 ARP/名称，再 assemble

    QUdpSocket *m_mdns = nullptr;
    QUdpSocket *m_ssdp = nullptr;
    QUdpSocket *m_nbns = nullptr;

    void loadOui();
    QString ouiVendor(const QString &mac) const;
    QHash<QString, QString> m_oui;         // "aabbcc" → vendor（首字节小写无分隔）
    bool m_ouiLoaded = false;
};
