#pragma once

// macOS 透明网关的**数据面规则**：pf 的 rdr-to + /dev/pf 的 DIOCNATLOOK 取原始目的地。
// 与 Linux 侧的 TproxyRules 一一对应（那边是 nftables TPROXY + fwmark 策略路由）。
//
// ── 为什么是 pf rdr 而不是 TPROXY ────────────────────────────────────────────
// TPROXY 是 Linux netfilter 独有的。BSD/macOS 上没有等价物，透明代理的标准做法是：
//   ① pf 用 `rdr-to` 把**转发流量**重定向到本机某个端口；
//   ② 目的地址在重定向时已被改写，代理进程必须回头问 pf「这条连接原本要去哪」——
//      即 open("/dev/pf") + ioctl(DIOCNATLOOK, struct pfioc_natlook)。
// Linux 的 TPROXY 不改地址（靠 fwmark + 策略路由把包留在本机），所以那边不需要这一步。
//
// ★ pf 的 rdr **只作用于转发流量，不影响运行 pf 的机器自身的出站流量**。
//   这条常被当成限制（mitmproxy 文档专门提醒"拦不到本机流量"），但对**透明网关**恰恰是
//   我们要的语义：只接管别的设备，不动 Mac 自己 —— 省掉了 Linux 那边"本机地址旁路"的一堆判断。
//
// ★★ 为什么不用 divert-to（OpenBSD 官方推荐的"更现代"做法）：**macOS 的 pf 根本不支持它。**
//   OpenBSD 文档明确建议新代理改用 divert-to —— 它不需要访问特权 /dev/pf，原始目的地直接用
//   getsockname(2) 就能取到，比 DIOCNATLOOK 干净得多。但 macOS 的 pf 是 OpenBSD 4.x 时代的
//   分支，divert-to 是之后才加的。真机实测（macOS 13.7.8）：
//       pass in on en0 ... divert-to 127.0.0.1 port 17898  →  pfctl: syntax error, 规则未加载
//       rdr pass on en0  ... -> 127.0.0.1 port 17898       →  加载成功（1 条规则）
//       man 5 pf.conf 里 "divert-to" 出现 0 次
//   所以 rdr + DIOCNATLOOK 是 macOS 上**唯一**可行的路，那条"更现代"的建议在这里用不了。
//   别再被搜索结果带偏去试 divert-to。
//
// ── 真机验证过的前提（macOS 13.7.8，2026-08-03）─────────────────────────────
//   · /dev/pf 存在（crw------- root:wheel），需 root 打开；
//   · DIOCNATLOOK 通路正常：用一条不存在的连接查询返回 ENOENT(2)，说明 ioctl 号与结构体
//     布局都正确（若布局错会是 EINVAL 或读到垃圾）；
//   · pf anchor 可独立加载/清空：`pfctl -a coast -f -` 写入、`pfctl -a coast -F all` 清掉，
//     **不碰用户 /etc/pf.conf 的主规则集**（直接 -f 主规则集会把系统启动时加的规则冲掉，
//     pfctl 自己会警告）；
//   · net.inet.ip.forwarding / net.inet6.ip6.forwarding 可读写（默认 0，需要我们打开并在
//     退出时还原 —— 与 Linux 侧 sysctl 存档同理，见 TproxyRules 的 /run 存档机制）。
//
// ── ★ struct pfioc_natlook 的布局：**必须用 Apple XNU 的定义，不能照抄 OpenBSD** ──
//   macOS 的端口字段是 `union pf_state_xport { u_int16_t port; u_int16_t call_id;
//   u_int32_t spi; }`（4 字节），而 OpenBSD 是裸的 u_int16_t（2 字节）。照 OpenBSD 写会
//   让后面所有字段错位，ioctl 读回垃圾地址。定义取自
//   apple-oss-distributions/xnu 的 bsd/net/pfvar.h：
//       struct pfioc_natlook {
//           struct pf_addr saddr, daddr, rsaddr, rdaddr;
//           union pf_state_xport sxport, dxport, rsxport, rdxport;
//           sa_family_t af; u_int8_t proto, proto_variant, direction;
//       };
//       #define DIOCNATLOOK _IOWR('D', 23, struct pfioc_natlook)
//       enum { PF_INOUT, PF_IN, PF_OUT };
//   Apple **不在 SDK 里导出 net/pfvar.h**（实测 MacOSX.sdk 与 /usr/include 都没有），
//   所以这些定义只能在我们自己的 .cpp 里按上面原样重声明。改 macOS 大版本后请重新核对。
//
// ── 与 Linux 侧的对应关系 ───────────────────────────────────────────────────
//   TproxyRules::install/remove   ←→  PfRules::install/remove
//   nft set proxied（被接管设备）  ←→  pf table <coast_proxied>
//   tproxy ip to :7898            ←→  rdr-to 127.0.0.1 port 7898
//   fwmark + ip rule + table 99   ←→  （不需要：rdr 已把包送到本机监听）
//   listener type: tproxy         ←→  listener type: **redir**（核心已支持，见下）
//
// ★ 核心侧**不需要改**：mihomo 的 `redir` 入站就是为「目的地已被 REDIRECT/rdr 改写」这类
//   场景设计的，官方文档写明它适用于 Linux 与 **macOS**；实测本仓库随包的 coast 1.10.0
//   二进制里也确实带 `redir`。所以 macOS 这条路是「PfRules 装 rdr 规则 + 核心开 redir
//   listener」，与 Linux 的「TproxyRules 装 tproxy 规则 + 核心开 tproxy listener」完全对称，
//   ConfigBuilder 只需按平台写不同的 listener type。
//   （曾一度以为「核心只有 Linux TPROXY 语义、macOS 拿不到原始目的地」是阻塞项 —— 查证后
//     证伪，记在这里免得下次再绕。）
//
// ★ 真正待做的下一步：实现 install/remove（写 anchor + 开关 forwarding + sysctl 存档还原）、
//   ConfigBuilder 按平台产出 redir listener、以及 LanGateway 的 macOS 实现接上
//   （目前 LanGateway 只有 _linux 与 _stub 两份，macOS 走 stub）。

#include <QString>
#include <QStringList>

class PfRules
{
public:
    /// 环境是否具备（pfctl 在不在、/dev/pf 能不能开）。不具备时 install 必然失败。
    static bool available(QString *why = nullptr);

    struct Spec {
        quint16 redirPort = 0;   ///< 核心的 redir 入站端口（rdr 的目的地）
        quint16 dnsPort = 0;     ///< 核心的 DNS 监听端口；0 = 不劫持 DNS
        QStringList ifnames;     ///< 网关接管的网卡名（rdr 规则按网卡限定）
    };

    ~PfRules();
    PfRules() = default;
    PfRules(const PfRules &) = delete;
    PfRules &operator=(const PfRules &) = delete;

    /// 装载 pf 规则（anchor 内的 table + rdr）并打开转发。失败时**不留半成品**（内部会 remove）。
    bool install(const Spec &spec, QString *err = nullptr);
    /// 拆除本实例装的一切（anchor 规则 + sysctl 还原）。幂等。
    void remove();
    bool isInstalled() const { return m_installed; }

    /// 把「当前应当被接管的设备 IPv4 全集」同步进 pf table。**整体替换**，不做增量 diff：
    /// 调用方给的就是全集，逐个 diff 既容易漏，也会在中间态出现「刚删又加回」的抖动。
    bool syncDevices(const QStringList &ipv4, QString *err = nullptr);

    /// 无条件清掉本类可能留下的一切（anchor 规则 + sysctl 还原）。**幂等**。
    /// 启动时必须调一次：上次若被 kill -9，anchor 与 forwarding 还留在系统里。
    static void removeStale();

    /// 查一条已被 rdr 重定向的连接的**原始目的地**（open /dev/pf + ioctl DIOCNATLOOK）。
    /// clientIp/clientPort = 连接的源（被代理设备）；proxyIp/proxyPort = 它被重定向到的本机监听。
    /// 成功时写出 origIp/origPort（重定向前的真实目的地）。需要 root。
    ///
    /// ★ 方向用 PF_OUT 而不是 PF_IN：pf 记的是**重定向之后**那条状态的方向，包在 rdr 之后
    ///   是从本机出去到 127.0.0.1 的。填 PF_IN 不会报错，只会恒 ENOENT —— 极易被误判成
    ///   "rdr 规则没生效"，排查时先核对这里。
    static bool lookupOriginalDest(const QString &clientIp, quint16 clientPort,
                                   const QString &proxyIp, quint16 proxyPort, QString *origIp,
                                   quint16 *origPort, QString *err = nullptr);

    /// 固定 anchor 名。**别用带 pid/随机串的名字**：崩溃后要靠这个名字把陈旧规则删掉。
    static const char *anchorName();

private:
    Spec m_spec;
    bool m_installed = false;
};
