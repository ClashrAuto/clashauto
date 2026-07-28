#pragma once

// LatencyProbe —— 状态页「延迟」卡的四个数：直连 / 到路由 / DNS / 到当前代理。
//
// 三件事决定了这里的实现方式：
//
// 1) **不用 ICMP。** ping 在 Windows 要 IcmpSendEcho、在 Linux/mac 要 root 或 CAP_NET_RAW，
//    三个平台三套代码，还可能被防火墙整段丢掉。这里一律用 **TCP 握手 RTT**：QTcpSocket 连上去，
//    从 connectToHost 到 connected 的耗时就是一个 RTT。端口关着也没关系——RST 是对端内核直接
//    回的，和 SYN-ACK 走同样的路，所以 ConnectionRefused **同样是一次有效测量**（对路由器尤其
//    重要：家用路由多半只开 80/443，甚至全关）。只有超时/网络不可达才算失败。
//
// 2) **直连必须真的直连。** 每个探测 socket 都显式 setProxy(NoProxy)：Qt 的默认代理是应用级的，
//    开着系统代理时不设的话，测到的是「到代理服务器」的延迟，那这张卡就没意义了。
//
// 3) **到代理的延迟不自己测。** 核心已经在测了（/proxies 的 history），自己再去连一遍节点既慢
//    又可能触发对端风控。直接取 ClashService 每秒推上来的、当前选中节点的 delay。
//
// 四个数各自独立刷新、互不阻塞；任何一项失败就把它置 -1（QML 显示「—」），不影响其余三项。
#include <QElapsedTimer>
#include <QObject>
#include <QString>

class ClashService;
class QTimer;
class QDnsLookup;

class LatencyProbe final : public QObject
{
    Q_OBJECT
    // 四个延迟，单位 ms。-1 = 未知/失败（QML 显示「—」），0 = 还没测过。
    Q_PROPERTY(int directMs READ directMs NOTIFY changed)   // 直连公网（TCP 握手到公共 DNS 的 53 口）
    Q_PROPERTY(int routerMs READ routerMs NOTIFY changed)   // 到本网段网关
    Q_PROPERTY(int dnsMs READ dnsMs NOTIFY changed)         // 一次真实域名解析的耗时
    Q_PROPERTY(int proxyMs READ proxyMs NOTIFY changed)     // 当前选中节点（取自核心的测速结果）
    Q_PROPERTY(QString proxyName READ proxyName NOTIFY changed) // 当前节点名（卡片副标题显示）
    Q_PROPERTY(bool probing READ probing NOTIFY changed)

public:
    explicit LatencyProbe(ClashService *clash, QObject *parent = nullptr);

    int directMs() const { return m_direct; }
    int routerMs() const { return m_router; }
    int dnsMs() const { return m_dns; }
    int proxyMs() const { return m_proxy; }
    QString proxyName() const { return m_proxyName; }
    bool probing() const { return m_pending > 0; }

    // 网关 IP（由 DevicesController 的拓扑变化推过来；空 = 不测路由那一项）。
    void setGatewayIp(const QString &ip);

public slots:
    // 立刻测一轮（页面可见时定时调用，也接卡片上的手动刷新）。
    void probe();
    // 页面显隐：只有状态页可见时才定时测——这卡片在别的页面上没人看，没必要每 10s 建四个 socket。
    void setActive(bool active);

signals:
    void changed();

private:
    void probeTcp(const QString &host, quint16 port, int *slot);
    void probeDns();
    void finishOne();

    ClashService *m_clash = nullptr;
    QTimer *m_timer = nullptr;
    QString m_gatewayIp;
    int m_direct = 0;
    int m_router = 0;
    int m_dns = 0;
    int m_proxy = 0;
    QString m_proxyName;
    int m_pending = 0;      // 在途探测数（都回来了才算测完一轮）
    int m_dnsNameIndex = 0; // 轮换解析目标，尽量绕开本地 DNS 缓存

    static constexpr int kIntervalMs = 10000; // 10s 一轮：够实时，又不至于一直在建连接
    static constexpr int kTimeoutMs = 3000;   // 单项超时：超过就当这条路不通
};
