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
//   现已改为经 msquic 的 QUIC_PARAM_CONN_LOCAL_ADDRESS 把本地地址钉在物理出口 IP 上
//   （QuicTransport.cpp，须在 ConnectionStart 之前设），闸门已撤，改为 start() 时打一条告警。
//   证据边界要记清楚：目前只有「编译 + 打包后 msquic 可加载」（CI 三平台 QUIC self-test），
//   **没有「TUN 开着时 Hy2 真的不环路」的真机证据**。blockedByQuicNode() 保留未删 ——
//   真出问题时把 start() 里那处 emit 改回 return false 即可立刻恢复闸门。
#include <QObject>
#include <QString>

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

    // socksFallbackPort > 0 时允许回退到 mihomo 的 SOCKS 口；0 = 严格模式（判不了就失败）。
    // 「完全替换 mihomo」的目标态是 0。
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

private:
    void teardown(); // stop() 与 start() 失败回滚共用

    IL2Endpoint *m_ep = nullptr;
    NetStack *m_net = nullptr;
    TunSession *m_sess = nullptr;
    CoreDialerFactory *m_factory = nullptr;
    std::shared_ptr<ProxyConfigStore> m_store;
    bool m_active = false;
    bool m_guardWasEnabled = false;
};
