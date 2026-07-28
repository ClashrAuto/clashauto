#pragma once

// 被动 ARP / NDP 冲突监视器 —— 回答用户的两个问题：
//   ①「别人在不在代理我这台电脑」（有第三方冒充网关/冒充本机做 MITM）；
//   ②「我代理的设备有没有被别的电脑抢」（有第三方也在投毒我正在劫持的 victim）。
//
// 它是 ArpSpoofer / NdpSpoofer 的**镜像**：那两个类**发**伪造帧把流量引到本机；本类**只读**，
// 把链路上观察到的每一条 ARP/NDP「IP↔MAC」声明和一张「真相表」比对，不一致就报警。抓包链路
// 本来就放行了全部 ARP(0x0806) 与全部 ICMPv6/NDP（见 L2Endpoint 的 BPF + 混杂模式，反制解毒要用），
// 所以**不新增任何抓包设施**——只是在既有的 frameReceived 里多解析一次载荷。
//
// 真相表（由 LanGateway 工作线程按网卡填）：
//   · 本机： localMac + 本机在这张卡上的 IPv4 / 全局 IPv6；
//   · 网关： gatewayIp4→gatewayMac、routerLL6→routerMac6（v6 默认路由器）；
//   · victim：正在劫持的设备 ip4/ip6 → 其真实 MAC。
//
// **铁律**：sender MAC == 本机 MAC 的帧一律忽略 —— 那是我们自己发的投毒/还原帧（含
//   answerGatewayArp 的应答、heal 帧）。不排掉就会「自己报自己」。heal 帧虽由本机发出，但其
//   sender 字段填的是**真实**网关/设备 MAC（= 真相），比对也不会误报，双保险。
//
// 去抖：同一 (kind, 冒充者MAC, 目标IP) 要在窗口内**连看 kMinHits 次**才首报（滤掉设备漫游时
//   偶发的一条 gratuitous ARP），首报后 kRealertMs 内不重报。攻击者像我们自己的投毒器一样会
//   ~1s 一发，所以 2 次/8s 的门槛既压掉瞬态、又能在 1~2s 内发现真攻击。
//
// 本类**跨平台可编译**（纯 QByteArray 解析 + Qt 信号，无系统调用），放在 BACKEND_SOURCES；
// 但只有 Linux 网关会真正实例化它。端点/网卡由外部持有，本类不拥有任何资源。
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class ArpWatch : public QObject
{
    Q_OBJECT
public:
    // kind 语义（int 传出，避免把枚举暴露给 QML 层）：
    enum Kind {
        SelfImpersonated = 0, // 有人冒充网关 / 冒充本机 → 别人可能在代理「我」
        DeviceContended  = 1, // 有人也在投毒我正在劫持的某台设备 → 我的设备被别的电脑抢
    };

    explicit ArpWatch(QObject *parent = nullptr);

    // —— 真相表配置（都在工作线程调用；重配幂等）——
    // 本机在这张卡上的身份。localGlobal6 可空（没有全局 v6 时 v6 身份盗用检测自动 no-op）。
    void setLocal(const QString &localMac, const QString &localIp4, const QString &localGlobal6);
    // 这张卡的网关（v4）+ v6 默认路由器（可空）。每张卡只有一个网关/路由器，重配直接覆盖。
    void setGateway(const QString &gatewayIp4, const QString &gatewayMac,
                    const QString &routerLinkLocal6, const QString &routerMac6);
    // 登记/注销一台被劫持设备（用于把「网关冒充」进一步判成是不是冲着我的某台设备来的、
    // 以及检测「有人冒充我的设备 IP」）。ip4 可空（只知 MAC 时）。
    void addVictim(const QString &mac, const QString &ip4);
    void removeVictim(const QString &mac);
    void addVictimV6(const QString &mac, const QString &ip6); // 从设备实帧学到其某个 v6 地址时补登记
    void clearVictims();

    // 观察一帧。非 ARP/非 NDP 立即返回（热路径极廉价）。命中冲突→去抖后 emit alert()。
    void observe(const QByteArray &frame);

signals:
    // kind 见 Kind。offenderMac = 冒充者 MAC（"aa:bb:.."）。subjectIp = 被冒充/被抢的 IP
    // （网关冒充填网关 IP；身份盗用填本机/设备 IP）。subjectMac = 被抢设备的 MAC（仅
    // DeviceContended 有值，供设备页给该行打「被争抢」徽标）；SelfImpersonated 时为空串。
    void alert(int kind, const QString &offenderMac, const QString &subjectIp,
               const QString &subjectMac);

private:
    void observeArp(const uchar *f, int len);  // f 指向以太头首字节，len = 整帧长
    void observeNdp(const uchar *f, int len);
    // 去抖 + 首报门槛；命中则 emit。subjectMac 见信号说明。
    void maybeAlert(int kind, const QByteArray &offMac6, const QString &subjectIp,
                    const QByteArray &subjectMac6);

    static QByteArray macBytes(const QString &);   // "aa:bb:.." → 6B（非法空）
    static quint32 ip4ToU32(const QString &);       // 点分 → 主机序 u32（非法/非 v4 = 0）
    static QByteArray ip6Bytes(const QString &);    // "fe80::.." → 16B（非法空）
    static QString macToStr(const uchar *p6);       // 6B → "aa:bb:.."
    // 在 NDP 选项区找第一条 type==wantType 的链路层地址选项，返回其 6B MAC（无则空）。
    static QByteArray ndpOptMac(const uchar *f, int len, int optStart, quint8 wantType);

    // 真相表。所有 IPv4 存主机序 u32；所有 MAC/IPv6 存定长 QByteArray（6/16）。
    QByteArray m_localMac;                 // 6B；空 = 未配置（observe 直接返回）
    quint32 m_localIp4 = 0;
    QByteArray m_localGlobal6;             // 16B 或空
    quint32 m_gatewayIp4 = 0;
    QByteArray m_gatewayMac;               // 6B
    QByteArray m_routerLL6;                // 16B（v6 默认路由器链路本地）
    QByteArray m_routerMac6;               // 6B
    QHash<quint32, QByteArray> m_victimV4; // ip4(u32) → mac6
    QHash<QByteArray, QByteArray> m_victimV6; // ip6(16B) → mac6
    // 反查：ip4/ip6 → 该 victim 的 MAC（DeviceContended 的 subjectMac）。就是上面两张表的 value。

    // 去抖跟踪。key = "kind|offMac|subjectIp"。
    struct Track {
        int hits = 0;
        qint64 firstMs = 0;   // 当前统计窗口起点
        qint64 lastAlertMs = 0; // 上次首/重报时刻（0 = 从未报过）
    };
    QHash<QString, Track> m_tracks;
    QElapsedTimer m_clock; // 单调时钟（ctor 里 start）

    static constexpr int kMinHits = 2;       // 首报前需连看的次数
    static constexpr int kWindowMs = 8000;   // 统计窗口
    static constexpr int kRealertMs = 60000; // 重报最小间隔
    static constexpr int kPruneMs = 300000;  // 跟踪表老化阈值
    static constexpr int kPruneAt = 256;     // 跟踪表超过这个大小才触发老化清理
};
