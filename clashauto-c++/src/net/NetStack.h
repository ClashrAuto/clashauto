#pragma once

// 用户态 TCP/IP 栈（基于 vendored lwIP，PIMPL 把 lwIP 头藏进 .cpp）。
//
// 职责：被劫持设备的以太帧经 LanGateway 过滤后送入 inputFrame()：
//   · IP/TCP 帧 → 交 lwIP(ethernet_input→ip4_input，accept-all 补丁使其终结发往任意公网 IP 的连接)
//     → 每条 TCP 连接经 Socks5Tcp 拨 mihomo 网关口(带每设备用户名) → 双向桥接字节。
//   · IP/UDP 帧 → 不进 lwIP，直接解析，经 Socks5Udp 转发（含 DNS）；回程手工封包发回设备。
//   · lwIP 出网(linkoutput) → 序列化成以太帧(dst=设备MAC，已由静态 ARP 表解析) → IL2Endpoint::send。
//
// **一个栈、多张网卡**：lwIP 只能有一个实例（lwip_init 全局、ARP 表全局、PCB 链全局），但一个实例
// 可以挂多个 netif。所以「同时代理有线 A 路由 + WiFi B 路由」的做法是 addNic() 一张卡一个 netif，
// 而不是开多个 NetStack。每个 netif 用**本机在该网卡上的真实 IP + 真实掩码**：
//   · 出方向 ip4_route(目的=设备IP) 靠子网匹配挑中正确的 netif（若像早期那样给 0.0.0.0/0 占位掩码，
//     两张卡都「匹配一切」，ip4_route 永远返回链表里第一张 → B 网段设备的回包会从 A 网卡发出去）；
//   · etharp_add_static_entry 内部也走 ip4_route，静态 ARP 因此自动落到正确的 netif 上。
// 每 netif 的二层端点/本机 MAC 放在 netif->state 里，C 回调从那里取上下文（不再用全局单例）。
//
// 线程：NetStack 自身不做线程化——它假定「由**单一**线程拥有并调用」（NO_SYS 单线程 lwIP：
// lwip_init/ARP 表/PCB 链全局，g_impl / ConnWatch 等哨兵也都依赖单线程前提）。全进程只有一个
// NetStack 实例，那个实例始终在某一个线程上：在正式 App 里是 LanGateway 的专用工作线程
// （见 LanGateway_linux.cpp 的线程模型），在 headless 自测里是主线程。两者从不并存 → 单线程前提恒成立。
// init() 创建的 QTimer 挂在**调用 init 的那个线程**的事件循环上周期 sys_check_timeouts()。
// 所有公开方法（init/addNic/removeNic/addDevice/removeDevice/inputFrame）都只能在这同一个线程上调用。
//
// ★★ 「一个栈」是硬约束，不是可以放开的判重（改这里之前先读完这段）——
//   曾经踩过的坑：进程内 TUN（LocalTunService）自己 new 了一个 NetStack，于是只要局域网扫描跑过一轮
//   （DevicesController 每轮扫描都 ensureGatewayConfigured → 网关 worker 建栈），用户点「增强」就恒定
//   拿到 init() 的那句「已有一个网关协议栈实例在运行」。**正确解法是 addNic()，不是放开判重**：
//   lwip_init/ARP 表/PCB 链是全局的，两个 NetStack 从构造上就不可能共存。
//   现在网关与 TUN **共用同一个栈**：谁先起谁建，后来者用 sharedInstance() 拿到它、addNic() 挂自己的
//   网卡、removeNic() 摘掉自己（**绝不能 delete 别人的栈**）。栈的出站工厂也是共用的一份 —— 分流实现
//   本来就该只有一份（复制一份就会开始漂移）。
#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class DnsResolver;
class IL2Endpoint;
class OutboundFactory;
class QHostAddress;

class NetStack final : public QObject
{
    Q_OBJECT
public:
    explicit NetStack(quint16 socksPort, QObject *parent = nullptr);
    ~NetStack() override;

    // 初始化 lwIP + catch-all TCP 监听 + 定时器泵。全进程只能有一个实例。失败置 *err。
    bool init(QString *err);

    // 进程内**唯一**那个已初始化的实例（init() 成功后登记，析构时注销；没有则 nullptr）。
    // 可从任意线程调用（内部加锁）；但返回的指针只能在 `->thread()` 上使用 —— 其余线程必须把调用
    // marshal 过去（BlockingQueuedConnection），见文件头的线程约束。
    // 用途：让「网关」和「进程内 TUN」共用同一个栈，各自 addNic()/removeNic()，而不是各建一个。
    static NetStack *sharedInstance();

    // 装一个「DNS 旁听器」：DNS 劫持的应答经我们手时交它学「核心分配的 fake-ip → 域名」，
    // 于是 accept 时能把 fake-ip 目的地**改写成域名**再交给出站（进程内出站才拨得通；回退核心时
    // 给域名也比给 IP 更利于规则匹配）。不设/设空 = 关掉该改写，行为与从前完全一致。
    // 只用它的同步方法（learnFromDnsResponse/domainForLearnedIp，内部有锁、不碰事件循环），
    // 故跨线程共享安全；本 NetStack 所属线程调用即可。
    void setDnsLearner(std::shared_ptr<DnsResolver> learner);

    // 进程内 DNS 开关：开 = 设备的 :53 查询由**我们自己**答（fake-ip 当场合成，其余转发上游），
    // 关 = 老行为（转投 mihomo 的 fake-ip DNS）。这是「网关整条数据面不再依赖 mihomo」的最后一环 ——
    // 在此之前，即便进程内出站开着，DNS 仍然恒走核心。需要 setDnsLearner 装过解析器才生效。
    void setLocalDnsEnabled(bool on);

    // 换出站工厂（取得所有权：delete 掉旧的，存下新的）。默认工厂 = 构造时按 socksPort 建的
    // Socks5OutboundFactory（全走 mihomo）。CoastCore 落地后由 CoreController 装一个 CoreDialerFactory
    //（内部包着 Socks5OutboundFactory 做回退），从而把「按目的地/设备选出站」接进来。
    // 必须在本 NetStack 所属线程上调用（与其它公开方法同一线程前提，见文件头）；只影响**之后**新建的
    // 连接/会话，在途的仍用各自已持有的出站对象。f 不可为空。
    void setOutboundFactory(OutboundFactory *f);

    // 挂一张网卡：ep 为其二层端点，localMac6/localIp/netmask 为本机在这张卡上的信息。
    // localIp/netmask 必须有效——它们决定出方向选哪个 netif（见文件头说明）。
    bool addNic(IL2Endpoint *ep, const QByteArray &localMac6, const QString &localIp,
                const QString &netmask, QString *err);
    // 摘掉一张网卡（网卡消失/重配时）。其上的设备静态 ARP 由 removeDevice 各自清理。
    void removeNic(IL2Endpoint *ep);
    bool hasNic(IL2Endpoint *ep) const;

    // 登记/注销被劫持设备：静态 ARP(ip↔mac) + 记录其 mihomo 身份用户名。
    void addDevice(const QString &ip, const QByteArray &mac6, const QString &socksUser);
    void removeDevice(const QString &ip);

    // IPv6 版：登记设备的某个 v6 地址（从它发出的实帧里学到的，一台设备可能有多个：链路本地 +
    // 一或多个全局/隐私地址，每个都调一次）。装一条 nd6 **静态邻居**项（v6↔mac，回程二层寻址靠它，
    // 见 nd6.c 的 Coast 补丁）+ 记录 mihomo 身份。from = 学到该帧的网卡端点（决定装到哪个 netif）。
    // 幂等：同址重复调用只刷新。
    void addDeviceV6(IL2Endpoint *from, const QString &ip6, const QByteArray &mac6,
                     const QString &socksUser);
    void removeDeviceV6(const QString &ip6);

    // 送入一个「已确认属于某被劫持设备」的以太帧（含 14 字节以太头）。
    // from = 收到该帧的二层端点，用来定位注入哪个 netif（也决定 UDP 回程从哪张卡发出）。
    void inputFrame(IL2Endpoint *from, const QByteArray &frame);

    // 前置声明公开（定义仍在 .cpp）：让 .cpp 内的自由函数/lwIP C 回调（在匿名命名空间里）
    // 能命名该类型。不暴露任何实现细节。
    struct Impl;
    struct Nic;

signals:
    // 析构的**第一句**就发（此刻 Impl 完好，removeNic/removeDevice 仍可安全调用；`destroyed` 太晚，
    // 那时 Impl 已经没了）。给「借用本栈挂了自己网卡」的模块（进程内 TUN）一个在**本栈所属线程上**
    // 同步摘干净自己的机会。★ 必须用 Qt::DirectConnection 接：队列连接排到时对象早已不在。
    void aboutToDestroy();

private:
    // UDP（含 DNS）不走 lwIP：手工解析设备发出的 UDP → Socks5Udp 转发；回程手工封包发回设备。
    // NAT 粒度是「每个设备源端口一条独立的 SOCKS UDP 关联」，所以回程不需要（也无法）靠
    // (fromIp,fromPort) 反查设备端口——vport 由「回包从哪条关联进来」直接给出。细节见 .cpp。
    void handleUdpFrame(Nic *nic, const QByteArray &frame, int ihl);
    // IPv6 UDP NAT：与 v4 版结构对称，复用同一套 UdpFlow/LRU/老化机制；帧封装与校验和用 v6 伪首部。
    void handleUdpFrame6(Nic *nic, const QByteArray &frame);
    // v4/v6 共用：回程 UDP 到设备。按会话记录的 v4/v6 走对应的封包/校验和。
    void onUdpResponse(const QString &victimIp, quint16 vport, const QHostAddress &fromIp,
                       quint16 fromPort, const QByteArray &payload);
    // DNS 转发：把设备的 :53 查询发给 upstream，应答**源地址伪装成设备原本查的那台 DNS**
    //（origServer:53）回封给它 —— 对设备完全透明。两种上游：
    //   · 默认 127.0.0.1:1053 = mihomo 的 fake-ip DNS（未开进程内 DNS 时的老行为）；
    //   · 进程内 DNS 开着但本地答不了（看不懂的报文/不该 fake 的域名）时 = origServer:53 直发，
    //     这条路**完全不经 mihomo**。原样中继给设备配置的 DNS 走用户态栈不通，但从宿主发是通的。
    void forwardDns(const QString &victimIp, quint16 vport, const QHostAddress &origServer,
                    const QByteArray &query, bool v6, const QHostAddress &upstream,
                    quint16 upstreamPort);

    // 进程内 DNS：自己出应答（该走代理的域名当场发 fake-ip），不再投给 mihomo。
    // 返回 true = 已处理；false = 没接管（调用方按老路转投核心）。
    bool answerDnsLocally(const QString &victimIp, quint16 vport, const QHostAddress &origServer,
                          const QByteArray &query, bool v6);

    Impl *d;
};
