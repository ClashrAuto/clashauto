#pragma once

// 「进程内增强」的会话对象 —— 把已经各自验过的四块串成一个可开关的整体：
//   TunEndpoint(设备层) + TunSession(激活层) + NetStack(lwIP) + CoreDialerFactory(出站)
//
// 它对应 UI 上那个「增强」按钮：语义不变（还是开 TUN），但 coastcore 打开时走**我们自己的**
// TUN，而不是让 mihomo 去开。
//
// ★ 启动顺序不是随意的，改动前先读这段：
//   1. 先探物理出口并**打开** SelfRouteGuard —— 必须在任何出站建立之前。顺序反了的话，
//      TUN 已经接管路由、而出站还没被钉住，那段窗口里新建的连接会直接进环路。
//   2. 建 TUN 设备、拿**内核给的真实网卡名**（macOS 的 utun 单元号由内核分配）。
//   3. 起 NetStack，把端点接进去。
//   4. **最后**才让 TunSession 接管路由 —— 到这一步栈已经能收包了，接管的瞬间就有人处理。
//   停止时严格逆序。
//
// ★★ QUIC 系节点（hysteria2 / tuic）曾经被**直接拒绝**：它们的 socket 由 msquic 内部创建，
//   SelfRouteGuard 那条 fd 路径够不着 → 开 TUN 必然环路 → 整机断网。
//   现在的主层是 **/32 主机路由**（start() ①.5 收集代理服务器 + 系统 DNS 地址、TunSession 安装
//   经物理网关的主机路由）——协议无关，msquic 自持 socket 也覆盖；socket 选项/钉源那些是保险层。
//   证据（2026-07-31，详见 docs/mihomo-replacement-gap.md 环路一节）：Linux 真机 tcpdump A/B
//   对照证过（无 /32 时 QUIC 包 coast0:3/eth0:0，有 /32 时 0/5）；macOS route -n get A/B 证过
//   查表级；**Windows 只编过，macOS 的真 QUIC 流量也还没验**。blockedByQuicNode() 保留未删 ——
//   真出问题时把 start() 里那处 emit 改回 return false 即可立刻恢复闸门。
//
// ★★★ 协议栈是**借来的，不是自己的**（2026-07-31 修的真实线上故障，改这块前必读）——
//   lwIP 全进程只能有一份（lwip_init/ARP 表/PCB 链全局，见 NetStack.h 头注释）。本类最初直接
//   `new NetStack` 自己起一个，而所有验证都在「没有网关在跑」的环境里做（容器 / 干净测试台），
//   于是没人发现：DevicesController **每轮局域网扫描**都 ensureGatewayConfigured() → 网关工作线程
//   建栈。也就是说只要扫描跑过一轮（几乎等于「装上就会」），用户点「增强」必定拿到
//   NetStack::init() 的那句「已有一个网关协议栈实例在运行」，开关恒定打不开。
//   正确做法是**挂到已有的那张栈上当第二张网卡**（NetStack 本来就支持多 netif）：
//     · 有网关的栈 → setStackProvider 给回来那一份，TUN 只 addNic/removeNic，**绝不 delete**；
//     · 没有（headless 自测、平台没有网关数据面）→ 才自建自管（m_ownsNet=true）。
//   由此带来的两条硬约束：
//     1. 那张栈活在**网关工作线程**上，而本类活在 GUI 线程。所有碰栈的调用（addNic/addDevice/
//        removeNic/removeDevice）以及 **TUN 端点的创建/open/析构**都必须 marshal 到栈的线程上
//        （端点的 QSocketNotifier 必须在服务它的线程上创建），且 frameReceived→inputFrame 的连接
//        必须是**同线程直连**（零拷贝帧只在槽内有效，队列连接会悬垂读，见 IL2Endpoint.h）。
//     2. 出站工厂**共用栈上那一份**（网关装的 CoreDialerFactory）。借栈时本类不再自建工厂 ——
//        setOutboundFactory 取得所有权，重复设置会把网关那份删掉。分流实现本来也只该有一份。
#include <QObject>
#include <QString>
#include <QThread>

#include <functional>
#include <memory>

class IL2Endpoint;
class NetStack;
class TunSession;
class CoreDialerFactory;
class ProxyConfigStore;
class RuleEngine;

class LocalTunService : public QObject
{
    Q_OBJECT
public:
    explicit LocalTunService(QObject *parent = nullptr);
    ~LocalTunService() override;

    // 共享协议栈的来源（见文件头）。返回进程内那一份已初始化的 NetStack；返回 nullptr 且置 *err
    // 表示「拿不到」，此时本类自建一个（headless 自测 / 没有网关数据面的平台就是这条路）。
    // 正式 App 里由 QmlBridge 接成 LanGateway::acquireStack —— 它保证栈建在网关工作线程上，
    // 于是「先开网关再开 TUN」和「先开 TUN 再开网关」两种顺序拿到的都是同一份栈。
    using StackProvider = std::function<NetStack *(QString *err)>;
    void setStackProvider(StackProvider p) { m_stackProvider = std::move(p); }

    // socksFallbackPort > 0 时允许回退到 mihomo 的 SOCKS 口；0 = 严格模式（判不了就失败）。
    // 「完全替换 mihomo」的目标态是 0。
    // ★ 只有**自建**协议栈时这两个参数（store/rules/socksFallbackPort）才用来造出站工厂；借用别人的
    //   栈时沿用栈上已有的那一份（网关装的 CoreDialerFactory，同一个 store/rules，见文件头第 2 条）。
    bool start(std::shared_ptr<ProxyConfigStore> store, std::shared_ptr<RuleEngine> rules,
               int socksFallbackPort,
               QString *err);
    void stop();
    bool active() const { return m_active; }
    QString ifname() const;

    // 当前配置里有没有 QUIC 系节点。有就不能开进程内 TUN（理由见文件头）。
    // *why 填一句可直接展示给用户的说明。
    static bool blockedByQuicNode(const std::shared_ptr<ProxyConfigStore> &store, QString *why);

    // 整体自检（COAST_TUNSERVICE_SELFTEST=1）：起服务 → TUN **真的接管默认路由** →
    // 从本机发一条 HTTP 请求 → 它必须经 TUN→NetStack→进程内出站→真目标绕回来 → 停服务 →
    // 核对路由已还原、网络恢复。返回进程退出码（0=PASS）。
    //
    // ★ 只能在**容器/虚机这类可牺牲的网络命名空间**里跑：它会真的接管默认路由。
    //   跑法见 tools/tunroute/README.md。需要 root。
    // ★ 出站只装内建 DIRECT + 严格模式（fallback=0）：任何一次想回退 mihomo 都会当场失败，
    //   所以 PASS 就等于「整条链路都在进程内」。
    static int selfTest();

    // 出站探针（COAST_OUTBOUND_PROBE=1）：**完全不碰 TUN**，起本机入站 + 指定节点做出站。
    // 用来把「节点/出站本身通不通」和「TUN 那层有没有问题」分开 —— 前提没验就测组合，
    // 失败了分不清是哪一层（我已经在这上面误判过一次）。
    static int outboundProbe();

signals:
    void activeChanged();
    void logged(const QString &line);

    // 「网关已在跑 + 再开 TUN」的组合自检（COAST_COMBO_SELFTEST=1 顺序：先网关后 TUN；=2 反序）。
    // 见 GatewaySelfTest.h 的 runComboSelfTest —— 实现放在那边，因为要复用它的 TAP 端点与假 SOCKS。

private:
    void teardown(); // stop() 与 start() 失败回滚共用
    // 在**协议栈所属线程**上同步跑 fn（同线程就直接调）。所有碰 NetStack / TUN 端点的动作都要过它。
    bool runOnStackThread(const std::function<void()> &fn);
    // 栈要没了（NetStack::aboutToDestroy，直连、跑在栈的线程上）：就地摘干净自己，别留悬垂指针。
    void detachFromSharedStack();

    IL2Endpoint *m_ep = nullptr;
    // 网卡名的**本线程副本**：m_ep 属于协议栈那条线程，GUI 线程不该去读它的成员。
    // open() 之后就不再变，start() 里在栈线程上抄一份过来即可。
    QString m_ifname;
    NetStack *m_net = nullptr;
    TunSession *m_sess = nullptr;
    CoreDialerFactory *m_factory = nullptr;
    std::shared_ptr<ProxyConfigStore> m_store;
    bool m_active = false;
    bool m_guardWasEnabled = false;
    // —— 协议栈的归属（见文件头）——
    StackProvider m_stackProvider;             // 空 = 没人给共享栈 → 自建
    bool m_ownsNet = false;                    // true 才可以 delete m_net；借来的绝不能删
    QThread *m_netThread = nullptr;            // m_net 所属线程；所有栈调用都 marshal 到这里
    QMetaObject::Connection m_stackGoneConn;   // aboutToDestroy 的连接（借栈时才有）
};
