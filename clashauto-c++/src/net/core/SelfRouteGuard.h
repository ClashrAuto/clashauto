#pragma once

// 自身流量排除 —— 接「增强」(进程内 TUN) 之前必须先有的东西。
//
// ★ 要解的问题：TUN 一旦接管默认路由，**本进程自己发起的出站连接也会被路由进这个 TUN**，
//   于是包又回到我们的 lwIP、又被拨一次出站……死循环。表现不是"某个网站打不开"，
//   而是整机断网 —— 比不开 TUN 更糟。所以这不是可选优化，是接线的前置条件。
//
// ★ 办法：把本进程的出站 socket 钉死在**物理默认路由网卡**上，让它的路由查表根本命中不到 TUN。
//   三个平台的机制不同，但形状一样，都必须在 **connect() 之前**作用在 fd 上：
//     · Linux  —— SO_MARK 打标 + 策略路由（`ip rule add fwmark <mark> lookup main pref 100`，
//                 TUN 的默认路由放进优先级更低的独立表）。也可以用 SO_BINDTODEVICE，但那要
//                 CAP_NET_RAW 且会绕过策略路由，这里用 mark。
//     · Windows —— IP_UNICAST_IF / IPV6_UNICAST_IF（无 SO_MARK）。**v4 的 ifindex 要网络序、
//                 v6 的要主机序**，这是微软文档里的既定坑，写反了不会报错、只是不生效。
//     · macOS  —— IP_BOUND_IF / IPV6_BOUND_IF。
//
// ★ 为什么单独成一层而不是在各出站里各写各的：现在 QTcpSocket/QUdpSocket 散在约 10 处
//   （DirectOutbound、Shadowsocks、Vless、Vmess×3、TlsClient、UtlsClient、QuicTransport、
//   Socks5Client）。逐个改必然漏一个，而漏一个的表现就是偶发死循环。所以先有收口，再谈接线。
//
// 用法：进程启动/网络变化时 refreshPhysicalInterface()；每个出站 socket 在 connect 前
//   prepareSocket()。未启用（TUN 没开）时是空操作。
//
// ★★ 已知**未覆盖**的一条路，接 TUN 之前必须解决：**QUIC / Hysteria2**。
//   它的 socket 由 msquic 内部创建（见 transport/QuicTransport.cpp），Qt 这条路根本够不着，
//   于是 prepareSocket 对它无效 —— TUN 开着时 Hy2 会走进环路。
//   解法是走 msquic 自己的接口把本地地址钉在物理出口的 IP 上（QUIC_PARAM_CONN_LOCAL_ADDRESS，
//   必须在 ConnectionStart 之前设）。在那之前，**TUN 与 Hy2 节点不能同时用**。
//
// 审计方法（改动出站时照这个查，别照 connectToHost 查）：
//   grep -rn "new QTcpSocket\|new QUdpSocket\|new QSslSocket" src/
//   按 connect 点查会漏 —— 协议出站大多经我们自己的 TlsClient/UtlsClient，保护加在那两个类
//   内部，调用处看不到 prepareSocket，容易被误判成没保护而重复加。
#include <QString>

class QAbstractSocket;

class SelfRouteGuard
{
public:
    // 当前物理默认路由网卡。ifIndex==0 表示没探到。
    struct Iface
    {
        int ifIndex = 0;
        QString name;
        bool valid() const { return ifIndex != 0; }
    };

    // 探测「现在走哪张网卡出公网」。换网（WiFi↔有线）后必须重探，否则新建连接会钉在一张
    // 已经没了的网卡上 —— 那和环路一样是整机不通。
    static Iface probePhysicalInterface();
    static Iface refreshPhysicalInterface(); // 探测并记为当前值
    static Iface currentInterface();

    // 启用/停用。停用时 applyToFd() 变成空操作 —— 没开 TUN 时不该平白改变 socket 行为。
    static void setEnabled(bool on);
    static bool enabled();

    // Linux 专用：出站打的 fwmark。要与 `ip rule ... fwmark <mark>` 一致。
    static quint32 fwmark();

    // 在 **connect() 之前**作用于 fd。family 取 AF_INET / AF_INET6。
    // 返回 false = 没能应用（*err 说明原因）。调用方**照常拨号**：没有环路保护总好过连不上，
    // 但必须把 err 记进日志——静默失效正是这类 bug 最难查的地方。
    static bool applyToFd(qintptr fd, int family, QString *err);

    // 出站拨号的**统一入口**：在 connectToHost() 之前调用它，之后照常 connectToHost。
    //
    // 解决的是一个 Qt 上的死结：选项必须在 connect() 之前作用于 fd，而 Qt 在 connectToHost()
    // 之前根本没建 socket、拿不到 fd。出路是**先 bind**（Qt 的 bind 会把 fd 建出来），
    // 拿到 fd 打完选项再 connect。这样就不必为了确定 AF_INET/AF_INET6 而先解析域名 ——
    // 绑成双栈，然后把 v4/v6 两个选项**都**打上：双栈 socket 上的 v4-mapped 流量吃 v4 那个。
    //
    // 返回 false = 没能保护（*err 说明）。调用方**照常拨号**——没有环路保护也好过连不上，
    // 但必须记日志：静默失效正是这类 bug 最难查的地方。
    static bool prepareSocket(QAbstractSocket *sock, QString *err);

    // 自检：探网卡 → 建 socket → 应用 → **读回来核对** → 再**真连一次**确认没把连通性弄坏。
    // 读回核对是关键：这些 setsockopt 写错参数时多半**不报错也不生效**，只验 rc==0 等于没验。
    // 结果写进 *report。返回 true = 全部通过。
    static bool selfTest(QString *report);
};
