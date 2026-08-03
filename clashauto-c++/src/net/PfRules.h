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
//   · pf anchor 可独立加载/清空：`pfctl -a com.apple/coast -f 文件` 写入、`-F all` 清掉，
//     **不碰用户 /etc/pf.conf 的主规则集**（直接 -f 主规则集会把系统启动时加的规则冲掉，
//     pfctl 自己会警告）；
//   · ★ anchor **必须挂在 `com.apple/` 下**：主规则集只引用了 `com.apple/*` 这一组通配挂载点，
//     顶层 anchor 装了也永远不会被求值（且 pfctl 不报任何错）。对照实验见 PfRules.cpp kAnchor；
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
//   场景设计的，且它的 **darwin 实现（listener/redir/tcp_darwin.go）自己就打开 /dev/pf 做
//   DIOCNATLOOK**，与本类的 lookupOriginalDest 是同一套机制。所以 macOS 这条路是
//   「PfRules 装 rdr 规则 + 核心开 redir listener」，与 Linux 的「TproxyRules + tproxy
//   listener」完全对称，ConfigBuilder 只需按平台写不同的 listener type。
//
// ★★★ **核心必须以 root 运行，否则 redir 静默失效** —— 这是 macOS 这条路最要命的一个点。
//   /dev/pf 的权限是 `crw------- root:wheel`，非 root 打不开。而 mihomo 的 darwin redir
//   在拿不到原始目的地时**不报错、不打日志**，只是 accept 之后把连接丢掉：
//     · 设备侧表现：TCP 握手成功（因为 accept 了），随后 http=000；
//     · 核心日志：**一行都没有**（连接根本没进入路由匹配）；
//     · pf 侧一切正常：rdr 规则 Packets 在涨、状态表里能看到
//       `127.0.0.1:17897 <- 93.184.216.34:80 <- 192.168.20.203`。
//   三边"看起来都对"，唯独不通 —— 真机上为此绕了很久，试了 rdr 目标改 LAN IP、listener 改
//   IPv4/IPv6 等一堆方向，全是错的。**先确认核心是不是 root。**
//   真机对照（同一套 pf 规则，只改核心的运行用户）：
//     非 root：http=000，核心日志无任何记录
//     root   ：http=200 559B，核心日志
//              `[TCP] 192.168.20.203:41056 --> 93.184.216.34:80 match Match using DIRECT`
//              —— 原始目的地被正确还原（不是被改写后的 127.0.0.1:17897）。
//   产品影响：macOS 上 CoreController 必须经特权 helper 以 root 启核心（helper 已存在，
//   见 MacHelperClient / helper/），不能沿用普通用户启动那条路。
//
// ★ 另一个真机确认的平台行为：**rdr 到 127.0.0.1 对"来自其它主机的转发流量"是可行的**
//   （上面 root 那次就是 -> 127.0.0.1 port 17897 成功的）。网上常见「macOS 不允许外部流量
//   rdr 到 loopback」的说法在本机 13.7.8 上不成立 —— 那类报告多半也是踩了 root 这个坑。
//
// ── ★ 端到端实测（2026-08-03，macOS 13.7.8 + Windows 真机做被接管设备）───────────
//   终于用一台**不被别的网关代理**的真机（Windows .51，Pi 的劫持名单里没有它）跑通了整条路：
//   COAST_GATEWAY_TESTDEV=192.168.20.51 让 App 自己发现并接管它（App 维护 pf 表，手工
//   `-T add` 是徒劳的 —— syncDevices 每轮 `-T replace` 会刷成"已挂载设备集合"）。
//
//   **功能正确**：核心侧看到
//       inbound=Redir  192.168.20.51:54143 → :443  host=mp.weixin.qq.com  chain=DIRECT
//     —— Redir 入站、源是被接管设备、**原始目的地被正确还原成域名**（DIOCNATLOOK 通路正常）。
//     退出后 ARP 还原（Windows 的网关 MAC 回到真路由 70-A7-41-A4-19-7B）、rdr 与 forwarding 清零。
//
//   ★★ 吞吐：**这台 Mac 是 Wi-Fi，测不出数据面的真实代价**（`networksetup` 确认
//      `Hardware Port: Wi-Fi`）。多流对照（同一设备、同一个局域网 HTTP 源，48MB 文件）：
//
//        （单流，各自的墙钟；多流数字见下面那条「夹具不可信」的说明，此处不引用）
//        Windows 直连     88.9 MB/s
//        Mac 做端点       27.2 MB/s        ← 这台 Mac 的空口上限
//        经 Mac pf 网关    8.4 MB/s
//
//      Wi-Fi 是**半双工共享介质**，做网关时每个字节要过两次空口（设备→Mac、Mac→服务器），
//      所以它的理论转发上限约是端点上限的一半 ≈ 13.6 MB/s —— 实测 8.4 是其 62%。
//      加压时核心 CPU **0.00 核（0%）**、App 0%，en0 计数是载荷的 2×（370MB/30s vs 6.2MB/s），
//      都与"瓶颈在空口、不在我们"一致。
//
//      ★ **更正**：先前据"直连 109.6 MB/s vs 经网关 6.2 MB/s"写过"约 1/18、吞吐不通"，
//        那是拿 Wi-Fi 网关去比有线 2.5G 基线，参照系错了。真实差距是相对这台机器自身
//        转发上限的约 2×，而不是 18×。
//      ★ 剩下那 ~1.6× 尚未定位（Wi-Fi 竞争/重传、多一跳回环拷贝都可能），但**在 Mac 接到
//        有线之前没法把它和空口分开**。要评估 pf 数据面能不能替掉 lwIP、能上多少吞吐，
//        前提是找一台**有线**的 mac；本机这条链路上得到的任何吞吐数字都只反映 Wi-Fi。
//
//      ★★ **夹具警告：别用「每个并发任务各自计时、取最大值」算总吞吐。** 我用 PowerShell
//        Start-Job 那样量过，得出「Windows 直连 4 流 202.5 MB/s ≈ 1.7 Gbps、局域网是 2.5G 级」
//        —— 假的。job 启动是错开的，各自的秒表都短于真实墙钟，总字节除以 max(各自秒数)
//        会把吞吐算高。iperf3 权威复核：**239↔Pi 单流 941 Mb/s、四流 SUM 也是 941 Mb/s、
//        零重传**，`ethtool` 也确认 Pi 的 eth0 就是 1000Mb/s —— 局域网是**千兆**，不是 2.5G。
//        要量并发总吞吐，用 iperf3 -P，或在**父进程**里量总墙钟，别信子任务自己的秒表。
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

    /// 把 anchor 里的规则原样读回（含 table 内容）。自测与排查用；非 macOS 返回空串。
    static QString dumpAnchor();

    /// 固定 anchor 名，当前是 **"com.apple/coast"**。**别用带 pid/随机串的名字**：崩溃后要靠
    /// 这个名字把陈旧规则删掉。**也别改成顶层名字**——顶层 anchor 不被 macOS 默认 /etc/pf.conf
    /// 引用，规则装得进去却永远不会被求值（真机对照实验见 PfRules.cpp 里 kAnchor 处）。
    static const char *anchorName();

    // ── ★ 人工核对 pf 状态时的三个坑（真机上连续踩过，都会让你误判成"代码没生效"）──
    //  ① **pfctl 把规则列表写到 stderr**。`pfctl -a coast -s nat 2>/dev/null | grep rdr`
    //     恒为空 —— 必须 `2>&1`。本类内部用 QProcess::MergedChannels，不受影响。
    //  ② **`grep -c "^rdr"` 匹配不上**：输出里 rdr 行带前导空白/分节标题。用 `grep -c rdr`。
    //  ③ **`-T replace` 打印 `no changes.` 不是失败**，而是"内容已与目标一致"。判成败要看
    //     `-T show` 的实际内容，别看这句话。
    //  另：sudo 凭据过期时 pfctl 的输出会混进 "a password is required"，同样表现为"读到空"。
    //  这三条曾让我一度以为 install/syncDevices 没生效、差点去改本来正确的代码。

private:
    Spec m_spec;
    bool m_installed = false;
};

/// pf 规则层 headless 自测（COAST_PF_SELFTEST=1）：装载 → 核对规则与挂载点 → 增删设备 → 拆除
/// → 核对拆干净。需要 root（或已就绪的免密 helper）。
/// ★ **不产生真实流量、不接管任何设备**：表里只放 192.0.2.0/24（RFC 5737 文档用网段），
///   rdr 绑 lo0，所以在一台正在使用的机器上跑也是安全的。与 Linux 侧 TPROXY 自测同一套约定。
int runPfRulesSelfTest();
