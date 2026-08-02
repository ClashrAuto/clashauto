#pragma once

// Linux 透明网关的**内核态转发规则**（nftables TPROXY + 策略路由）。
//
// 这是什么、为什么要有它 —— 先说清楚它和现有 lwIP 数据面的关系：
//   现在的网关是「ARP 劫持 → AF_PACKET 把整帧收进用户态 → lwIP 终结 TCP → SOCKS5 交给核心」。
//   ARP 劫持这一层是产品的价值所在（被代理设备什么都不用改），**保留**；换掉的只是它后面那半截：
//   让**内核**转发，用 nftables 的 TPROXY 把流量交给核心，lwIP 整条旁路。
//
//   为什么值得换 —— 树莓派 5 上同机同靶实测（详见提交信息）：
//     数据面                吞吐            CPU
//     lwIP（现状）          0.89 Gbps       82%，且**全部压在一个工作线程上**
//     TPROXY                13.1 Gbps       73%（4 核里的 0.73 核）
//   0.93 核/Gbps × 单线程 = lwIP 这条路无论换什么网卡都卡在 1 Gbps 出头；TPROXY 下核心两端都是
//   真 socket，Go 的 io.Copy 走 splice(2) 内核内零拷贝，同样 0.85 Gbps 时核心只用 2% 的 CPU。
//   UDP 两条路都必须逐包过用户态（splice 是流语义），实测 0.31 → 0.66 Gbps，只快一倍。
//
// 本类**只管规则**：装、拆、按设备增删。不碰 ARP 劫持，也不决定何时启用——那是 LanGateway 的事。
// 现阶段默认不启用，只有自测钩子会用它（见 COAST_TPROXY_SELFTEST）。
//
// —— 三件必须做对的事，每一件都是实测踩出来的 ——
//
// 1) **FORWARD 默认策略**。装了 Docker 的机器上 `iptables -L FORWARD` 是 `policy DROP`，而网关机
//    装 Docker 是极常见的组合。现在的 lwIP 方案完全不受影响（它从不走内核转发），改用 TPROXY 后
//    不自己放行就会**整机哑掉，且现象是「部分设备完全不通」**——最难查的那种。
//    更隐蔽的是 br_netfilter：`bridge-nf-call-iptables=1` 时**同网桥的二层流量也走 FORWARD**，
//    于是连"同网段两台设备互访"都会被 DROP。两处都要放行。
//
// 2) **崩溃清理**。ARP 劫持崩了还能靠 GatewayPanic 裸发几帧还原回去；而 nft 规则、ip rule、
//    路由表**是内核持久状态，进程死了它们还在**，没有"发个包就能恢复"这回事。规则还在、却没有
//    进程接收 TPROXY 的流量 = 被覆盖的设备**完全断网**（实测：删掉 nft 表立刻恢复）。
//    所以表名写死、每一步都幂等，并且**启动时先无条件清一遍**（removeStale）。
//
// 3) **DNS 用 redirect 而不是 tproxy**。TPROXY 要求目标 socket 带 IP_TRANSPARENT，核心的 DNS
//    监听是普通 UDP server；而 DNS 本来也不需要保留原目的地址（解析器照答不误）。所以 53 端口
//    走 nat 的 redirect，其余走 tproxy。实测有效：被接管设备 `dig @1.1.1.1` 返回核心的 fake-ip。
//
// 依赖：nft(nftables) 与 ip(iproute2)。缺任何一个 install() 返回 false 并给出原因，调用方据此
// 回退到 lwIP 数据面——**不可用时必须回退，不能半开着**（半开 = 设备断网）。
// 权限：需要 root/CAP_NET_ADMIN。

#include <QString>
#include <QStringList>
#include <QtGlobal>

class TproxyRules
{
public:
    struct Spec {
        quint16 tproxyPort = 0; // 核心的 tproxy 入站端口（TCP+UDP 都投到这里）
        quint16 dnsPort = 0;    // 核心的 DNS 监听端口；0 = 不劫持 DNS
        quint32 fwmark = 0x1;   // TPROXY 打的标记，配合下面的策略路由把包留在本机
        int routeTable = 0x63;  // 策略路由表号（99）。只放一条 `local default dev lo`
    };

    TproxyRules() = default;
    ~TproxyRules();
    TproxyRules(const TproxyRules &) = delete;
    TproxyRules &operator=(const TproxyRules &) = delete;

    /// 环境是否具备（nft / ip 在不在）。不具备时 install() 必然失败，调用方可提前判定并回退。
    static bool available(QString *why = nullptr);

    /// 无条件清掉本类可能留下的一切（nft 表 / ip rule / 路由表）。**幂等**，没装过也能安全调用。
    /// 启动时必须调一次：上一次进程若是被 kill -9 打死的，这些内核状态还在，而它们会让被覆盖的
    /// 设备完全断网。
    static void removeStale();

    /// 装上规则。失败时 *err 给出原因，且**保证不留半成品**（内部失败即回滚）。
    bool install(const Spec &spec, QString *err = nullptr);

    /// 更新「被代理设备」集合（nft set 的元素）。ip 为设备的 IPv4 文本地址。
    /// 只有集合里的设备走 TPROXY，其余照常由内核转发（等价于「没开代理的设备」）。
    /// 运行时增删即时生效，不需要重装规则、更不需要重启核心——实测过。
    /// macs 是同一批设备的 MAC（形如 aa:bb:cc:dd:ee:ff）。**必须一起给**：v6 兜底丢弃链按 MAC
    /// 匹配（tproxy 下不学设备的 v6 地址，MAC 才是稳定的抓手），漏了它 = IPv6 绕过策略。
    bool syncDevices(const QStringList &ipv4, const QStringList &macs, QString *err = nullptr);

    /// 更新「局域网隔离」集合：policy=reject 的设备 IP，以及本机各网卡的网段。
    ///
    /// 语义与 lwIP 那条一致——**被隔离设备不能主动发起到同网段，别人仍能访问它**——但换成
    /// conntrack 判定（ct state new 才丢）。比 lwIP 现在那套无状态启发式准，而且 **UDP 也能按
    /// 方向正确放行**：LanGateway_linux.cpp 里明说过 UDP 只能全丢，代价是对端也用不了被隔离
    /// 设备上的 mDNS/SSDP 之类服务；conntrack 没有这个代价。
    ///
    /// 前提是这类流量真的会经过本机——靠 ArpSpoofer::answerIsolationArp 的抢答（tproxy 下二层
    /// 端点照常在跑，这一层没变）。
    bool syncIsolation(const QStringList &isolatedIpv4, const QStringList &lanCidrs,
                       QString *err = nullptr);

    /// 拆除。幂等。析构时自动调用。
    void remove();

    bool isInstalled() const { return m_installed; }
    const Spec &spec() const { return m_spec; }

    /// 供自测/诊断：当前 nft 表的文本（未安装时为空）。
    static QString dumpRuleset();

private:
    bool runIp(const QStringList &args, QString *err = nullptr);
    bool applyNft(const QString &script, QString *err = nullptr);
    bool ensureForwardAccept(QString *err = nullptr);

    Spec m_spec;
    bool m_installed = false;
    // ip_forward 原值，remove() 时还原（我们只在需要时打开，不擅自改变用户的系统设置）。
    QString m_savedIpForward;
    // route_localnet 原值（只有开了 DNS 劫持才会动它；见 install/remove 里的说明）。
    QString m_savedRouteLocalnet;
};

// —— 自测钩子（COAST_TPROXY_SELFTEST=1）——
// 装规则 → 核对内核里确实有了 → 增删设备 → 拆 → 核对拆干净。全程不需要真设备、不改路由。
// 返回进程退出码（0 = PASS）。非 Linux 恒返回 0 并打印跳过。
int runTproxyRulesSelfTest();
