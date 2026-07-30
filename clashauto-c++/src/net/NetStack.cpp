#include "NetStack.h"
#include "IL2Endpoint.h"
#include "Socks5Client.h"

#include <QDebug>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QSet>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTimer>
#include <QUdpSocket>

#include "GatewayDiag.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

extern "C" {
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/memp.h"
#include "lwip/nd6.h"
#include "lwip/netif.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
}

// lwIP(NO_SYS) 需要单调毫秒时钟。
extern "C" u32_t sys_now(void)
{
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return static_cast<u32_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

namespace {

struct DeviceInfo {
    QByteArray mac6;
    QString socksUser;
};

// 一条被终结的 TCP 连接：lwIP pcb ↔ 出站隧道（现状经工厂拨 mihomo 的 Socks5Tcp）。
struct TcpConn {
    NetStack::Impl *impl = nullptr;
    struct tcp_pcb *pcb = nullptr;
    IOutboundTcp *socks = nullptr;
    QByteArray toLwip;    // socks→设备 方向待写入 lwIP 的字节（受 tcp_sndbuf 限流）
    // 上行背压：已经交给 socks、但**还没**用 tcp_recved 归还给 lwIP 的接收窗口字节数。
    // 上限天然是 TCP_WND(128 KiB)——lwIP 不会送来超过已通告窗口的数据。
    // 必须是 quint32（不是 u16_t）：TCP_WND 开窗口缩放后已经超过 65535 了。
    quint32 pendingRecved = 0;
    bool upThrottled = false;  // 上行处于「等排空」状态（高/低水位的迟滞标志）
    bool downPaused = false;   // 下行已让 socks 停止读取（toLwip 顶到高水位）
    bool socksClosed = false;  // socks 侧已关闭：等 toLwip 排空后再优雅关 lwIP 侧
    bool established = false;
    bool lwipClosed = false;
    // —— 阶段0 量化埋点（纯观测，不参与转发决策）——
    qint64 upBytes = 0;         // 设备→服务器 累计字节（喂给 socks 的量）
    qint64 downBytes = 0;       // 服务器→设备 累计字节（从 socks 收到的量）
    QElapsedTimer connectTimer; // connectTo→established 计时；established 回调里落 connectMsHist
};

} // namespace

// 一张网卡对应的 netif 上下文。指针存进 netif->state，C 回调据此拿到「该从哪个二层端点发出去」
// 和「本机在这张卡上的 MAC」——多网卡就是靠它区分的。
struct NetStack::Nic {
    struct netif nif;          // 必须是稳定地址：Nic 一律堆分配，不放进会搬家的容器里
    IL2Endpoint *ep = nullptr;
    QByteArray localMac6;
    QString localIp, netmask;
};

namespace {

// ————————— UDP NAT：为什么是「每个设备源端口一条独立 SOCKS 关联」 —————————
//
// 老实现是「一台设备一个 Socks5Udp + 一张 QHash<"dstIp:dport" → 设备源端口>」。这张表的键少了
// 源端口，于是同一台设备用**两个不同源端口打同一个目标**时，后一条 insert 直接覆盖前一条：先发
// 的那个请求的回包被封成「目的端口 = 另一个源端口」发回去，设备当然丢弃。DNS 就是典型受害者
// ——多数系统每次查询换一个源端口、还会并发几条打同一个 resolver——症状即「偶发 DNS 超时、
// 解析时快时慢」。
//
// 回程方向能用来查表的**只有** (fromIp, fromPort)：Socks5Udp 的回程头里就这两样。只要两个设备
// 源端口共用同一个 SOCKS 关联去打同一个 (dstIp,dport)，回包在 SOCKS 这一层就是**信息论意义上
// 不可区分**的，换什么键都救不回来（按 DNS 事务 ID 消歧只能救 DNS，救不了两条 QUIC 打同一个
// CDN 的 443）。所以唯一真正能工作的做法，是让「关联」本身携带这个信息：
//
//     一个设备源端口 = 一条 UdpFlow = 一个独立的 Socks5Udp（各自有自己的本地 UDP socket）。
//
// 回包从哪条关联进来，就发回哪个设备源端口，不用猜。这正是真实 NAT 的 endpoint-independent
// mapping：内网 (IP,端口) ↔ 外部映射一一对应，与目的地址无关。
//
// 代价（诚实记下）：每条流多一个 QUdpSocket + 一条到 mihomo 的 TCP 控制连接（UDP ASSOCIATE 要求
// 控制连接全程保持）。DNS 突发能在几秒内造出几十条流，所以老化必须给力，外加容量上限兜底。

struct UdpFlow;

// 侵入式 LRU 链（按 lastUsed 降序，链首最新、链尾最旧）。老化只看链尾：尾巴没过期，前面的更不
// 可能过期 → 直接停。所以「回收」常态下就是一两次整数比较，O(1) 摊还，绝不扫全表——这条路挂在
// 已有的 200ms lwIP 定时器上，不能给 GUI 线程添任何负担。
struct UdpLru {
    UdpFlow *head = nullptr;
    UdpFlow *tail = nullptr;
};

struct UdpSess; // 每设备的壳（MAC / 网卡 / 该设备的所有流）

// 一条 UDP 流 = 设备的一个源端口。
struct UdpFlow {
    UdpSess *sess = nullptr;
    IOutboundUdp *socks = nullptr;
    quint16 vport = 0;                 // 设备源端口：回程包的目的端口就是它
    bool ready = false;                // associate 完成
    struct Pending {
        quint32 dstIp = 0;      // v4 目的（v6 会话不用）
        QByteArray dst6;        // v6 目的（16 字节；v4 会话为空）
        quint16 dport = 0;
        QByteArray payload;
    };
    QList<Pending> pending;            // ready 前暂存的上行包
    QSet<quint32> peers;               // 联系过的目的 IP：做「地址限制型」来源校验
    bool coneOpen = false;             // peers 溢出后放弃校验（退化成全锥）
    qint64 lastUsed = 0;
    qint64 idleMs = 0;                 // 该流的空闲超时档位（短档/长档）
    UdpLru *lru = nullptr;             // 当前挂在哪条链上
    UdpFlow *lruPrev = nullptr;
    UdpFlow *lruNext = nullptr;
};

// 一台设备的 UDP 上下文（含 DNS）：一堆按源端口索引的流。
// v4/v6 各自一个会话（key = 源 IP 串，v4/v6 串天然不冲突）：一台双栈设备会有两个 UdpSess。
struct UdpSess {
    QByteArray mac6;
    QString victimIp;
    NetStack::Nic *nic = nullptr;      // 该设备所在网卡：回程包从这张卡、用这张卡的 MAC 发
    QHash<quint16, UdpFlow *> flows;   // 设备源端口 → 流
    bool v6 = false;                   // 该会话是 IPv6（回程走 onUdpResponse 的 v6 分支）
    QByteArray victimAddr;             // 设备地址原始字节：v4=4B / v6=16B（回程封包 dst 用）
};

// 空闲超时。选值理由：
//  · 一般 UDP 取 120s —— RFC 4787 REQ-5 规定 NAT 的 UDP 映射不得短于 2 分钟；QUIC/游戏/VPN 这类
//    长连接的保活心跳普遍按 15~30s 设计，2 分钟足够宽松，不会把正在用的流掐断。
//  · DNS/NTP/mDNS 这类「一问一答」取 15s —— 回包基本 1s 内到，15s 只是兜重传。它们又恰恰是源端口
//    爆炸的元凶（每次查询换端口），短超时才能把 socket 和 mihomo 侧的关联迅速还回去。
// 两档阈值不同 ⇒ **必须两条独立的 LRU 链**：混在一条链里，链尾就不再是「最先过期」的那条，
// O(1) 的尾部回收会被一条长档流挡住，短档形同虚设。
constexpr qint64 kUdpIdleMs = 120000;
constexpr qint64 kUdpDnsIdleMs = 15000;

// DNS 劫持目标端口：mihomo 的 dns.listen（ConfigBuilder 同步写 `dns.listen: 127.0.0.1:1053`）。
// 被劫持设备的 UDP :53 查询不再原样中继到「设备配置的 DNS（常是网关/路由器 IP，经用户态栈中继到
// 它走不通 → 名字解析时断时通）」，而是转投 mihomo 自己的 fake-ip DNS，走它的国内外分流后再回封给设备。
constexpr quint16 kDnsHijackPort = 1053;

// lwIP 定时器泵的周期，以及「UDP 流老化 + 内存池诊断」相对它的分频比（详见 NetStack::init）。
// 25ms × 8 = 200ms，老化/诊断的实际节奏与改动前一致。
constexpr int kLwipPumpIntervalMs = 25;
constexpr int kHousekeepEveryTicks = 8;

// 诊断采样里附带的 lwIP 内部量。这些只有 lwIP 头才拿得到，所以由本 TU 拼好字符串交给
// GatewayDiag（它是跨平台 TU，不该碰 lwIP）。取的都是**这条链路真正会出问题**的量：
//   · rexmit/rto —— 重传次数。发方丢帧修好之后，这一栏就是「还有没有在丢」的直接证据；
//     它和 txdrop 的区别是：txdrop 是我们自己丢的，rexmit 还包含设备侧/空口丢的。
//   · 各池的 used/max/err —— 池子耗尽是静默杀连接的（见 lwipopts.h 的 MEMP_NUM_TCP_PCB 一段），
//     max 是高水位线，能回答「离上限还有多远」，不必等到 err 涨了才发现。
// 计数器是 u16（LWIP_STATS_LARGE=0），窗口内增量用 u16 运算算，回绕一次也是对的。
QString lwipStatsLine();

// 容量兜底：句柄和 mihomo 侧的关联数都是有限资源。BT/DHT 这类应用通常共用一个源端口广撒（对本
// 方案友好），但「源端口也乱换」的极端情况必须挡住，不能让一台设备把整个进程的 fd 吃光。
constexpr int kMaxUdpFlowsPerDevice = 128;
constexpr int kMaxUdpFlowsTotal = 1024;
// peers 只为保留老代码「查不到就丢」的严格度（地址限制型 NAT）。超过上限说明这条流在广撒，
// 再记下去只是白吃内存 → 退化成全锥，不再校验来源。
constexpr int kMaxUdpPeersPerFlow = 256;
// associate 未就绪时最多暂存几个包。老代码是无上限 QList：mihomo 没起来时会一直堆。
constexpr int kMaxUdpPendingPerFlow = 16;

// 短档目的端口：53 DNS / 5353 mDNS / 5355 LLMNR / 853 DoQ / 123 NTP。
bool isShortLivedUdpPort(quint16 dport)
{
    return dport == 53 || dport == 5353 || dport == 5355 || dport == 853 || dport == 123;
}

// 单调毫秒。和 sys_now() 同一个 steady_clock，但用 64 位：老化算的是「多久没用」，
// 不想为了 u32 的 49 天回绕再套一层回绕安全的减法。
qint64 monoMs()
{
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

void lruUnlink(UdpFlow *f)
{
    if (!f->lru)
        return;
    if (f->lruPrev)
        f->lruPrev->lruNext = f->lruNext;
    else
        f->lru->head = f->lruNext;
    if (f->lruNext)
        f->lruNext->lruPrev = f->lruPrev;
    else
        f->lru->tail = f->lruPrev;
    f->lruPrev = f->lruNext = nullptr;
    f->lru = nullptr;
}

// 移到链首（顺带完成「短档→长档」的换链：先 unlink 再挂到目标链上）。
void lruPushFront(UdpLru *l, UdpFlow *f)
{
    lruUnlink(f);
    f->lru = l;
    f->lruNext = l->head;
    if (l->head)
        l->head->lruPrev = f;
    else
        l->tail = f;
    l->head = f;
}

void lruTouch(UdpFlow *f)
{
    if (f->lru)
        lruPushFront(f->lru, f);
}

} // namespace

struct NetStack::Impl {
    // 出站工厂：每条被终结的连接向它要一个新出站对象（现状 = Socks5OutboundFactory 拨 mihomo）。
    // NetStack 拥有它，dtor 里 delete。
    OutboundFactory *factory = nullptr;
    NetStack *owner = nullptr;
    struct tcp_pcb *listener = nullptr;
    QHash<IL2Endpoint *, Nic *> nics;        // 二层端点 → 该网卡的 netif 上下文
    QHash<QString, DeviceInfo> devices;      // 设备 IP（v4 或 v6 串）→ {mac,user}
    QHash<QString, Nic *> deviceV6Nic;       // 设备 v6 串 → 其所在网卡（removeDeviceV6 摘 nd6 静态项用）
    QHash<QString, UdpSess *> udp;           // 设备 IP（v4 或 v6 串）→ UDP 会话
    UdpLru udpLruShort;                      // 短档(DNS 类)流的 LRU
    UdpLru udpLruLong;                       // 长档(一般 UDP)流的 LRU
    int udpFlowCount = 0;                    // 全局流数（对上限用，省得遍历）
    QTimer *timer = nullptr;
    int pumpTick = 0;        // 泵的拍数计数器，给「老化 + 池诊断」分频用（见 init() 里的说明）
    QElapsedTimer pumpClock; // 量每一拍的真实间隔 → 迟到量 = 工作线程饱和度
    qint64 lastDiagMs = 0;   // 上次写诊断采样的墙钟时刻
    bool inited = false;

    QString userForIp(const QString &ip) const { return devices.value(ip).socksUser; }
    QByteArray macForIp(const QString &ip) const { return devices.value(ip).mac6; }
};

namespace {

// 拆掉一条流：摘 LRU、摘设备表、断信号、关 SOCKS 关联。
// 先 disconnect 再 closeSession 是必须的——closeSession 会同步 emit closed()，而我们正是在
// closed/failed 的槽里调用本函数，不断开就会自己递归进来。socks 用 deleteLater：本函数常常是
// 从 socks 自己的信号里被调用的，当场 delete 等于在信号发射途中析构发射者。
void destroyUdpFlow(NetStack::Impl *d, UdpFlow *f)
{
    if (!f)
        return;
    lruUnlink(f);
    if (f->sess)
        f->sess->flows.remove(f->vport);
    if (f->socks) {
        QObject::disconnect(f->socks, nullptr, nullptr, nullptr);
        f->socks->closeSession();
        f->socks->deleteLater();
        f->socks = nullptr;
    }
    if (d->udpFlowCount > 0)
        d->udpFlowCount--;
    delete f;
}

// 老化：只看两条链的链尾，过期就摘，没过期立刻停（链按 lastUsed 有序，链尾即最旧）。
void reapUdpFlows(NetStack::Impl *d)
{
    const qint64 now = monoMs();
    for (UdpLru *l : {&d->udpLruShort, &d->udpLruLong}) {
        while (l->tail && now - l->tail->lastUsed > l->tail->idleMs)
            destroyUdpFlow(d, l->tail);
    }
}

// 淘汰某设备最久未用的一条流。只在该设备顶到上限时才走，这时扫它自己那张最多 128 条的表，比在
// 全局 LRU 链上一路找「属于这台设备的最旧一条」更划算（后者最坏要走完 1024 条）。
void evictOldestFlowOfDevice(NetStack::Impl *d, UdpSess *s)
{
    UdpFlow *victim = nullptr;
    for (UdpFlow *f : std::as_const(s->flows)) {
        if (!victim || f->lastUsed < victim->lastUsed)
            victim = f;
    }
    if (victim)
        ++GatewayDiag::c.udpFlowsEvicted; // 撞每设备上限：可能顶掉一条正在用的流（QUIC 会莫名卡住）
    destroyUdpFlow(d, victim);
}

// 全局最久未用：两条链的链尾里挑更旧的那个，O(1)。
void evictGlobalOldestFlow(NetStack::Impl *d)
{
    UdpFlow *a = d->udpLruShort.tail;
    UdpFlow *b = d->udpLruLong.tail;
    UdpFlow *victim = !a ? b : (!b ? a : (a->lastUsed <= b->lastUsed ? a : b));
    if (victim)
        ++GatewayDiag::c.udpFlowsEvicted; // 撞全局上限，同上
    destroyUdpFlow(d, victim);
}

// 收掉一台设备的全部流并释放会话壳。
void destroyUdpSess(NetStack::Impl *d, UdpSess *s)
{
    if (!s)
        return;
    const QList<UdpFlow *> flows = s->flows.values();
    for (UdpFlow *f : flows)
        destroyUdpFlow(d, f);
    delete s;
}

} // namespace

// ———————————————————————————— 前置：C 回调 ————————————————————————————
namespace {

// lwIP 本身只能有一个实例（lwip_init 全局、ARP 表全局、PCB 链全局），所以「栈」确实是单例：
// g_impl 只用来给 TCP accept 这类**与网卡无关**的全局回调取上下文（监听 pcb、设备表、socks 端口）。
// 与网卡相关的东西（二层端点、本机 MAC）一律从 netif->state 取，不走这里——多网卡的关键。
//
// 线程前提：g_impl（以及下方的 ConnWatch / g_destroyedConn 哨兵）都是进程级全局，只在「lwIP 只跑在
// 一个线程」这个前提下成立。这个前提仍然成立——全进程只有一个 NetStack 实例（init() 里用 g_impl 判重），
// 而它始终被单一线程拥有（App 里是 LanGateway 的工作线程，自测里是主线程，二者从不并存）。绝不能出现
// 两个线程同时碰 lwIP/这些全局的情况。
NetStack::Impl *g_impl = nullptr;
bool g_debug = false;             // COAST_GATEWAY_DEBUG=1 时打诊断日志（自测/联调用）

NetStack::Nic *nicOf(struct netif *netif)
{
    return netif ? static_cast<NetStack::Nic *>(netif->state) : nullptr;
}

// pbuf 链 → QByteArray。仍有两个调用点：linkoutput（要一整帧连续字节交给二层端点）和
// lwipTcpRecv 的**多段链**慢路径。上行单段 pbuf 的快路径已经绕开它（零拷贝），别顺手删。
QByteArray pbufToBytes(struct pbuf *p)
{
    QByteArray out;
    out.reserve(p->tot_len);
    for (struct pbuf *q = p; q != nullptr; q = q->next) {
        out.append(static_cast<const char *>(q->payload), q->len);
        if (q->tot_len == q->len)
            break;
    }
    return out;
}

// netif linkoutput：lwIP 要发一帧 → 序列化 → 从**该 netif 自己的**二层端点发出
//（dst MAC 已由静态 ARP 填好）。多网卡就靠这里分流：绝不能回到某个全局端点上。
//
// ★ 恒返回 ERR_OK、**故意**忽略 send() 的返回值。这不是偷懒，但也曾经是个坑：
//   端点侧原本「内核缓冲一满就把帧丢掉并 return false」，配上这里的忽略，就成了完全无声的
//   本机→设备丢包（LINK_STATS 也是关的），只能靠 lwIP 几百毫秒后的重传兜底 —— 被代理设备的
//   现象是「访问什么都慢、偶尔打不开」。现在端点自己带积压队列 + 丢帧计数（见
//   L2Endpoint_linux.cpp / L2Endpoint_mac.cpp 的「发方」一节），send() 返回 false 已经意味着
//   「链路真的喂不进去了」，此时最合理的处置恰好就是交给 TCP 重传，所以这里保持不变。
err_t lwipLinkOutput(struct netif *netif, struct pbuf *p)
{
    NetStack::Nic *nic = nicOf(netif);
    if (nic && nic->ep) {
        const QByteArray f = pbufToBytes(p);
        if (g_debug)
            std::fprintf(stderr, "NETSTACK OUT nic=%s len=%d\n",
                         nic->localIp.toLatin1().constData(), int(f.size()));
        nic->ep->send(f);
    }
    return ERR_OK;
}

err_t lwipNetifInit(struct netif *netif)
{
    netif->name[0] = 'c';
    netif->name[1] = 't';
    netif->output = etharp_output;      // IPv4 出口经 ARP（静态表已填，不会真发 ARP 请求）
#if LWIP_IPV6
    netif->output_ip6 = ethip6_output;  // IPv6 出口经 nd6（静态邻居已填，见 nd6.c 补丁）
#endif
    netif->linkoutput = lwipLinkOutput; // 二层出口（L3 无关，v4/v6 共用）
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    if (NetStack::Nic *nic = nicOf(netif))
        std::memcpy(netif->hwaddr, nic->localMac6.constData(), 6);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET
                   | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    return ERR_OK;
}

void closeConn(TcpConn *c, bool abort);

// 把所有网卡攒着的出帧一次性交给驱动（契约与理由见 IL2Endpoint::flushTx）。
// 为什么不挑「这条连接对应的那张卡」：TcpConn 手里只有 pcb，反查 netif→Nic 要多绕一圈，而网卡
// 数是个位数、空队列的 flushTx 只是一次判空 —— 全刷一遍更简单也更不容易漏。
// **绝对安全**：flushTx 只走到 pcap_sendqueue_transmit，不发任何 Qt 信号，回不到 TcpConn 上
// （与 ConnWatch 上方那张「确认安全」清单里的 tcp_output 同类）。
void flushNicTx(NetStack::Impl *d)
{
    if (!d)
        return;
    for (auto it = d->nics.constBegin(); it != d->nics.constEnd(); ++it) {
        if (it.key())
            it.key()->flushTx();
    }
}

// ————————————————————— 背压：两个方向都必须有闸 —————————————————————
//
// 这条链路是「设备 ⇄ lwIP ⇄ Socks5Tcp(QTcpSocket) ⇄ mihomo」，两头的速率毫不相干。谁慢谁就是
// 瓶颈，而中间的队列如果没有闸，就会替瓶颈**无界地**缓冲——内存一路涨到 OOM。
//
// 【上行 设备→mihomo】lwIP 的接收窗口本来就是现成的闸门：收到设备数据后**先不**调 tcp_recved，
//   窗口就一直关小着，设备的 TCP 自己会减速。等 QTcpSocket 真把字节交给内核
//   （upstreamBytesWritten）、待发队列降下来了，再一次性把窗口还回去。
//   老代码是无条件立刻 tcp_recved(全开)：等于不停对设备喊「随便发」，而字节全堆在
//   QTcpSocket 的写缓冲里（Qt 的写缓冲没有上限）。
//
// 【下行 mihomo→设备】toLwip 是队列：只能按 tcp_sndbuf 往 lwIP 灌，灌不进去的留着。老代码任由
//   它涨，且 QTcpSocket 没设 setReadBufferSize，Qt 会一直往上读 → 设备慢（WiFi 信号差）时同样
//   无界。现在超过高水位就让 Socks5Tcp 停止读 socket（读缓冲填满 → 收窗关闭 → 压回远端），
//   降到低水位再恢复。
//
// —— 为什么不会死锁（改这块最容易写出「双方互等」）——
//  上行：只要 pendingRecved>0 且窗口没还，就必然存在「QTcpSocket 里没发完的字节」。目的地是
//    本机回环的 mihomo，内核迟早排空 → upstreamBytesWritten 必然到来 → flushRecvWindow 必然
//    把窗口还上。唯一的例外是 socket 出错，而那条路走 failed/closed → closeConn 把整条连接
//    拆掉，不存在「等一个永远不来的事件」的稳态。另外 c->socks 已经为空时**无条件**归还：
//    没有排空者了，扣着窗口只会让连接永久僵住。
//  下行：恢复读取的触发点是 pumpToLwip，而 pumpToLwip 挂在 tcp_sent 上 —— 只要设备还在 ACK，
//    tcp_sent 就会来；设备彻底不响应时 lwIP 自己重传超时 → tcp_err → 拆连接。而且恢复是
//    **排队**执行的（Socks5Tcp::setReadPaused），绝不在 lwIP 回调里同步重入。
//
// —— 水位取值 —— 高水位 64 KiB、低水位 16 KiB（¼）。
//  低水位的作用是迟滞：免得在水位线上反复 暂停/恢复 抖动（每次恢复都要过一趟事件循环，
//  抖起来纯属浪费）。上行同理：queued 掉到 16 KiB 才还窗口，一次还一大块，tcp_recved 内部的
//  TCP_WND_UPDATE_THRESHOLD（= min(TCP_WND/4, 4*TCP_MSS) = 5840 B）判据才会真的触发窗口通告。
//  ★ 这两个数**故意不跟着** lwIP 的单连接预算走。原本它们是照着 TCP_SND_BUF = TCP_WND = 60 KiB
//  取的「≈ 一个满 sndbuf」；窗口缩放把预算提到 128 KiB 之后没有同步上调，因为：
//    · 这里的队列是 QByteArray / QTcpSocket 读缓冲，落在**普通堆**上，按连接数线性膨胀
//      （2048 条 × 128 KiB = 256 MiB 的病态上限），而 lwIP 那 128 KiB 是有池子封顶的；
//    · 吞吐上也不需要 —— 下行管道深度 = lwIP 在途 128 KiB + toLwip 排队 64 KiB，
//      toLwip 只需覆盖「lwIP 腾出空间 → 我们补上」这一次事件循环的往返，64 KiB 绰绰有余。
constexpr int kUpQueueHighWater = 64 * 1024;
constexpr int kUpQueueLowWater = 16 * 1024;
constexpr int kToLwipHighWater = 64 * 1024;
constexpr int kToLwipLowWater = 16 * 1024;

// ——————————————— 「c 是不是在这次调用里被拆掉了」的通用判据 ———————————————
//
// 本文件里有一类调用**可能同步销毁 TcpConn**——不只是 lwIP 回调，Qt 的直连信号一样能做到。
// 清单（调用之后必须先查判据，才可以再碰 c）：
//   ★ Socks5Tcp::write()      established 之后落到 QTcpSocket::write；Qt 在写入途中发现对端
//                             已 RST 会 setErrorAndEmit **同步**发 errorOccurred →
//                             Socks5Tcp emit failed → 我们的 failed 槽是直连(context 同线程)
//                             → closeConn → delete c。两个重载（QByteArray / 裸指针）都算，
//                             所以上行热路径上**一个包只调一次** write（见 lwipTcpRecv）。
//   ★ Socks5Tcp::closeTunnel() 同步 emit closed() → closed 槽 → pumpToLwip → 可能 closeConn。
//   ★ pumpToLwip()            内部 tcp_write 出错会 abort；socksClosed 排空后会优雅关闭。
//   ★ closeConn()             本体。
//
// 反过来，**确认安全、不需要保护**的调用（别为它们加噪音）：
//   · tcp_recved / tcp_write / tcp_output / tcp_sndbuf —— 纯 lwIP，只会往下走到 netif
//     linkoutput → IL2Endpoint::send()，而后者是裸的 pcap_sendpacket / ::sendto，不发任何
//     Qt 信号、也不嵌套事件循环，回不到 TcpConn 上。
//   · Socks5Tcp::bytesToWrite() / isEstablished() —— 纯 getter。
//   · Socks5Tcp::setReadPaused() —— 暂停只置个标志；恢复走 Qt::QueuedConnection 补读，
//     故意**不**同步 emit dataReceived（见 Socks5Client.cpp 里的理由）。
//   · deleteLater() —— 定义上就是延后。
//
// 判据用两个全局量（NO_SYS + 全程主事件循环，单线程，够用）：
//   g_destroyedConn    最近一次被 delete 的 TcpConn 地址。只做**指针比较，绝不解引用**。
//   g_destroyedByAbort 那次销毁走的是不是 tcp_abort。这是**另一件事**，不能和「c 还在不在」
//                      混用一个标志：lwIP 要求回调里 tcp_abort 过就必须返回 ERR_ABRT
//                      （tcp_in.c 的 `aborted:` 标签只有这条路能到），而 tcp_close 不需要
//                      （lwIP 自己有 tcp_trigger_input_pcb_close 延迟释放）。优雅关闭同样
//                      会 delete c，却**不该**回 ERR_ABRT。
TcpConn *g_destroyedConn = nullptr;
bool g_destroyedByAbort = false;

// 每个 delete c 的地方都必须先过这里。
void markConnDestroyed(TcpConn *c, bool byAbort)
{
    g_destroyedConn = c;
    g_destroyedByAbort = byAbort;
}

// 把「可能销毁 c」的一段调用夹住：构造时清账，之后 alive() 判断能不能继续碰 c，
// needsAbortReturn() 判断 lwIP 回调该不该回 ERR_ABRT。
// 嵌套是安全的：清账只发生在销毁之前；真被销毁了就不会再有后续代码拿着同一个 c 去新建 watch。
class ConnWatch
{
public:
    explicit ConnWatch(TcpConn *c) : m_c(c)
    {
        g_destroyedConn = nullptr;
        g_destroyedByAbort = false;
    }
    bool alive() const { return g_destroyedConn != m_c; }
    bool needsAbortReturn() const { return g_destroyedConn == m_c && g_destroyedByAbort; }

private:
    TcpConn *m_c;
};

// 无条件把攒着的接收窗口还给 lwIP。
void giveBackRecvWindow(TcpConn *c)
{
    if (!c || !c->pcb || c->lwipClosed)
        return;
    // 先扣账再调用：tcp_recved 内部可能立刻 tcp_output → linkoutput，保持状态自洽。
    // 循环+钳位**不是**防御性冗余：tcp_recved() 的形参恒为 u16_t（打开 LWIP_WND_SCALE 也不变，
    // lwIP 把 raw API 的窗口参数一律钳在 u16 上），而 pendingRecved 的上限是 TCP_WND = 128 KiB，
    // 已经超过 0xFFFF —— 单次调用还不完，必须分多次。
    while (c->pendingRecved > 0) {
        const u16_t give = static_cast<u16_t>(qMin<quint32>(c->pendingRecved, 0xFFFFu));
        c->pendingRecved -= give;
        tcp_recved(c->pcb, give);
    }
    c->upThrottled = false;
    // tcp_recved 打开窗口后 lwIP 会立刻打一个窗口更新 ACK 出去 —— 那是**设备上行**能不能继续
    // 发的唯一许可。本函数最常见的调用点是 Socks5Tcp::upstreamBytesWritten（socket 回调），
    // 既不在收帧排空里也不在 pumpToLwip 里，不在这里收口就要等泵那一拍，等于给上行凭空加 25 ms。
    flushNicTx(c->impl);
}

// 按 SOCKS 侧的排空进度决定要不要归还接收窗口（高/低水位迟滞）。
void flushRecvWindow(TcpConn *c)
{
    if (!c || !c->pcb || c->lwipClosed || c->pendingRecved == 0)
        return;
    if (c->socks) {
        const qint64 queued = c->socks->bytesToWrite();
        const qint64 mark = c->upThrottled ? kUpQueueLowWater : kUpQueueHighWater;
        if (queued > mark) {
            if (!c->upThrottled)
                ++GatewayDiag::c.upThrottleHits; // 只数「进入节流态」的沿，不数每次评估
            c->upThrottled = true; // 继续扣着窗口，等 upstreamBytesWritten 再来评估
            return;
        }
    }
    // socks 为空（失败/已拆）时落到这里：无条件归还，见上文死锁分析。
    giveBackRecvWindow(c);
}

// 下行水位：toLwip 堆过高就让 Socks5Tcp 停止读 socket，降下来再恢复。
void updateDownstreamPause(TcpConn *c)
{
    if (!c || !c->socks)
        return;
    const int queued = c->toLwip.size();
    if (!c->downPaused && queued >= kToLwipHighWater) {
        ++GatewayDiag::c.downPauseHits;
        c->downPaused = true;
        c->socks->setReadPaused(true);
    } else if (c->downPaused && queued <= kToLwipLowWater) {
        c->downPaused = false;
        c->socks->setReadPaused(false); // 补读是排队执行的，不会在这里重入
    }
}

// 把 socks→设备方向的待写字节尽量写进 lwIP（受发送窗口限流），tcp_sent 回调后再续。
// **返回 true 表示连接已在本函数里被销毁**（等价于外层 ConnWatch 的 !alive()）：调用方不得
// 再碰 c；调用方若是 lwIP 回调，还要照 ConnWatch::needsAbortReturn() 决定是否回 ERR_ABRT。
// 本函数内部除 closeConn 外没有别的「可能销毁 c」的调用：updateDownstreamPause 只会置标志或
// 排队补读，tcp_* 全是纯 lwIP（理由见 ConnWatch 上方清单），所以不需要再夹一层 watch。
bool pumpToLwip(TcpConn *c)
{
    if (!c || !c->pcb || c->lwipClosed)
        return false;
    while (!c->toLwip.isEmpty()) {
        // u16_t 是**对的**，不是窗口缩放漏改的窄化：tcp_sndbuf(pcb) 展开成 TCPWND16(pcb->snd_buf)
        // = (u16_t)LWIP_MIN(x, 0xFFFF)，lwIP 自己就把它钳在 65535（tcp_write 的 len 形参也只有
        // u16_t，收不下更多）。TCP_SND_BUF = 128 KiB 时它报的是 65535 而不是回绕成 0 ——
        // 外面这个 while 会再转一圈把剩下的窗口用掉。
        const u16_t space = tcp_sndbuf(c->pcb);
        if (space == 0)
            break;
        const int n = qMin<int>(space, c->toLwip.size());
        const err_t e = tcp_write(c->pcb, c->toLwip.constData(), n, TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM)
            break;        // 缓冲不足，等 tcp_sent 再来
        if (e != ERR_OK) {
            closeConn(c, true);
            return true;
        }
        c->toLwip.remove(0, n);
    }
    tcp_output(c->pcb);
    // ★ flushTx 的第二个调用点：上面这一串 tcp_write + tcp_output 是「下行数据到了 → 打给设备」
    //   的主路，一次能吐出几十个段。它们由 mihomo 的 socket 回调触发，**不在**收帧排空里，
    //   所以必须在这里自己收口，否则要等泵那一拍（最多 25 ms）才发得出去。
    flushNicTx(c->impl);
    // 排空到低水位 → 恢复从 socks 读。放在这里是因为 tcp_sent 是下行唯一的「有进展」信号。
    updateDownstreamPause(c);
    // socks 早已关闭、下行残余也全交给 lwIP 了 → 现在才轮到优雅关闭本端。
    // 老代码只在「closed 到达那一刻 toLwip 恰好为空」时才关，否则 pcb 就一直挂着等设备自己超时；
    // 加了下行水位之后 toLwip 非空的概率更高，这个洞必须一起补。
    if (c->socksClosed && c->toLwip.isEmpty()) {
        closeConn(c, false);
        return true;
    }
    return false;
}

void closeConn(TcpConn *c, bool abort)
{
    if (!c)
        return;
    // 先断开 socks 的全部信号：本函数经常正是从 socks 自己的槽里进来的，而下面就要 delete c。
    // 不断开的话，closeTunnel() 同步 emit 的 closed()、以及 setReadPaused 排进事件队列的补读，
    // 都会带着一个悬垂的 c 再进来一次。（UDP 侧的 destroyUdpFlow 早就是这个套路。）
    if (c->socks)
        QObject::disconnect(c->socks, nullptr, nullptr, nullptr);
    bool aborted = false;
    if (c->pcb && !c->lwipClosed) {
        tcp_arg(c->pcb, nullptr);
        tcp_recv(c->pcb, nullptr);
        tcp_sent(c->pcb, nullptr);
        tcp_err(c->pcb, nullptr);
        if (abort) {
            ++GatewayDiag::c.tcpAborted;
            tcp_abort(c->pcb);
            aborted = true; // 若身处 lwIP 回调，调用方要据此回 ERR_ABRT
        } else {
            // 优雅关闭前**必须**把扣着的接收窗口还回去：tcp_close 一看到 rcv_wnd != TCP_WND_MAX
            // 就认定「上层没把对端的数据收完」，改发 RST 而不是 FIN（tcp_close_shutdown 里的
            // rst_on_unacked_data 分支）——设备侧会连带丢掉已经收到的下行数据。
            // 这是引入上行背压之后新出现的坑，不还窗口就会变成「下载到一半被 RST」。
            giveBackRecvWindow(c);
            if (tcp_close(c->pcb) == ERR_OK) {
                ++GatewayDiag::c.tcpClosed;
            } else {
                ++GatewayDiag::c.tcpAborted; // 优雅关不掉，退化成 RST
                tcp_abort(c->pcb);
                aborted = true;
            }
        }
        c->lwipClosed = true;
    }
    c->pcb = nullptr;
    if (c->socks) {
        // 此刻 socks 的信号已全部断开，closeTunnel() 同步 emit 的 closed() 回不到我们身上，
        // 所以这里不会递归回 closeConn。
        c->socks->closeTunnel();
        c->socks->deleteLater();
        c->socks = nullptr;
    }
    // 上面 tcp_close/tcp_abort 打出去的 FIN/RST 也要收口：本函数最常从 socks 的 failed/closed
    // 槽（socket 回调）进来，不在任何一个批量提交点上。晚 25 ms 关连接不致命，但设备侧会多挂
    // 一条半开连接，没必要。
    flushNicTx(c->impl);
    // 阶段0 埋点：连接终局 → 按累计字节落桶 + 活跃 gauge 减一（两个 delete 点都要，见 lwipTcpErr）。
    GatewayDiag::observeConnBytes(c->upBytes, c->downBytes);
    GatewayDiag::tcpConnClosed();
    markConnDestroyed(c, aborted); // 必须在 delete 之前登记，外层 ConnWatch 靠它判存活
    delete c;
}

// 设备→服务器 方向（lwIP 收到设备数据）→ 写给 socks。
err_t lwipTcpRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    Q_UNUSED(pcb);
    auto *c = static_cast<TcpConn *>(arg);
    if (!c)
        return ERR_OK;
    // 本回调里有三处「可能当场把 c 拆掉」的调用（closeConn / closeTunnel / write），全程夹住。
    ConnWatch watch(c);
    if (err != ERR_OK) {
        if (p)
            pbuf_free(p);
        closeConn(c, true);       // 之后不得再碰 c
        // 老代码这里固定回 ERR_OK，而上面刚 tcp_abort 过 → tcp_input 会继续用已释放的 pcb。
        return watch.needsAbortReturn() ? ERR_ABRT : ERR_OK;
    }
    if (p == nullptr) {           // 设备侧关闭 → 关 socks 上行
        if (c->socks)
            c->socks->closeTunnel(); // 同步 emit closed() → 我们的槽可能当场收掉 c
        return watch.needsAbortReturn() ? ERR_ABRT : ERR_OK; // 之后不得再碰 c
    }
    const u16_t len = p->tot_len;
    c->upBytes += len; // 阶段0 埋点：设备上行累计（关闭时落桶），纯整型自增
    // 窗口先记账、**不**立刻归还：归还与否交给 flushRecvWindow 按 SOCKS 侧排空进度判断。
    c->pendingRecved += len;
    if (c->socks) {
        // 设备上行的**每个**数据包都走这里，所以这条路上一次多余的堆分配+全量拷贝都不能有。
        //
        // 快路径（绝大多数）：单段 pbuf —— 一帧 ≤1514 < PBUF_POOL_BUFSIZE(1600)，收上来就是
        // 一整块连续内存。直接把 payload 指针交给 socks，省掉 pbufToBytes 的 QByteArray。
        // 判据用 tot_len==len 而不是 next==nullptr：pbuf 的 next 也可能挂着「下一个包」
        // （队列语义），只有 tot_len==len 才真的保证载荷全在这一段里。
        //
        // 慢路径：多段链（lwIP 把补齐的乱序段 pbuf_cat 起来一起上交，打开 SACK 之后更常见）
        // —— 先拼成连续字节再写。这条路保持原样，宁可多一次拷贝。
        //
        // ★★ 为什么这里**只调一次** write，而不是「对 pbuf 链逐段循环 write」：
        //    write() 是那张「可能同步 delete c」清单上的第一条（对端已 RST 时 Qt 在 write
        //    内部就把 errorOccurred 发出来 → failed 槽 → closeConn → delete c）。逐段循环的话，
        //    第一段写完 c 就可能已经析构，第二次迭代 c->socks 就是 use-after-free。
        //    维持「一次调用 + 紧跟一次 watch.alive() 判断」这个形状，重入面才只有一个点。
        //    要改成多次 write，必须每次之后都 watch.alive() 并跳出——收益远不抵风险。
        //
        // ★★ 零拷贝依赖的前提（见 Socks5Client.h 里 write(const char*, qsizetype) 的契约）：
        //    payload 指针只需在 write 调用期间有效。established 后 write 落到 QIODevice::write，
        //    字节当场被拷进 QTcpSocket 写缓冲；握手未完成时 Socks5Tcp 会深拷贝进 pending。
        //    那边一旦改成「把指针存进队列晚点再发」，这里立刻变成悬垂指针。
        if (p->tot_len == p->len)
            c->socks->write(static_cast<const char *>(p->payload), qsizetype(p->len));
        else
            c->socks->write(pbufToBytes(p));
    }
    // pbuf 必须在 write **之后**才归还：零拷贝路径上 socks 拿的就是 p->payload 里的字节。
    // 这里放在 alive() 判断之前，是因为 p 的所有权自始至终在本回调手上，与 c 死没死无关
    //（lwIP 把 pbuf 交给 recv 回调后就不再引用它，tcp_abort 也不会碰它）——
    // c 就算刚在 write 里被拆掉，这一次 pbuf_free 依然要、且只要执行一次。
    pbuf_free(p);
    if (!watch.alive())
        return watch.needsAbortReturn() ? ERR_ABRT : ERR_OK;
    flushRecvWindow(c);
    return ERR_OK;
}

err_t lwipTcpSent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    Q_UNUSED(pcb);
    Q_UNUSED(len);
    auto *c = static_cast<TcpConn *>(arg);
    if (!c)
        return ERR_OK;
    ConnWatch watch(c);
    pumpToLwip(c); // 可能当场把连接收掉（tcp_write 失败 abort / socksClosed 排空后优雅关闭）
    return watch.needsAbortReturn() ? ERR_ABRT : ERR_OK;
}

void lwipTcpErr(void *arg, err_t err)
{
    Q_UNUSED(err);
    auto *c = static_cast<TcpConn *>(arg);
    if (!c)
        return;
    c->pcb = nullptr;          // lwIP 已释放 pcb
    c->lwipClosed = true;
    if (c->socks) {
        // 同 closeConn：先断信号，否则 closeTunnel/排队补读会带着悬垂的 c 回来。
        QObject::disconnect(c->socks, nullptr, nullptr, nullptr);
        c->socks->closeTunnel();
        c->socks->deleteLater();
        c->socks = nullptr;
    }
    // 阶段0 埋点：与 closeConn 同口径 —— 连接终局落桶 + 活跃 gauge 减一。
    GatewayDiag::observeConnBytes(c->upBytes, c->downBytes);
    GatewayDiag::tcpConnClosed();
    // byAbort=false：pcb 是 lwIP 自己释放的，不是我们 tcp_abort 的；tcp_err 也没有返回值可回。
    markConnDestroyed(c, false);
    delete c;
}

// 新 SYN 被 catch-all 监听接受：local_ip/port = 设备想访问的服务器；remote = 设备。
err_t lwipTcpAccept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    Q_UNUSED(arg);
    if (err != ERR_OK || newpcb == nullptr)
        return ERR_VAL;
    if (g_impl && g_impl->listener)
        tcp_accepted(g_impl->listener);

    const QString serverIp = QString::fromLatin1(ipaddr_ntoa(&newpcb->local_ip));
    const quint16 serverPort = newpcb->local_port;
    QString victimIp = QString::fromLatin1(ipaddr_ntoa(&newpcb->remote_ip));
    // ★ v6 键规范化：lwIP 的 ip6addr_ntoa 输出**大写** hex（"2001:DB8:C0A::239"），而 addDeviceV6
    //   存进 devices 表的键来自 QHostAddress::toString()（**小写**，"2001:db8:c0a::239"）。不统一
    //   会导致 userForIp 落空 → v6 连接以空 user 拨 7899（每设备 listener 需 dev-<mac> 认证）→ 认证失败、
    //   且 /connections 无法按 inboundUser 归属到设备。经 QHostAddress 往返统一成小写规范形（v4 是恒等，
    //   无副作用）。放在冷路径的 accept 里，开销可忽略。
    if (victimIp.contains(QLatin1Char(':'))) {
        const QString canon = QHostAddress(victimIp).toString();
        if (!canon.isEmpty())
            victimIp = canon;
    }
    const QString user = g_impl ? g_impl->userForIp(victimIp) : QString();
    if (g_debug)
        std::fprintf(stderr, "NETSTACK ACCEPT server=%s:%u victim=%s user=%s\n",
                     serverIp.toLatin1().constData(), serverPort,
                     victimIp.toLatin1().constData(), user.toLatin1().constData());

    ++GatewayDiag::c.tcpAccepted;
    GatewayDiag::tcpConnOpened(); // 阶段0 埋点：活跃连接 gauge +1（+ 刷本窗口峰值）
    auto *c = new TcpConn;
    c->impl = g_impl;
    c->pcb = newpcb;
    c->socks = g_impl->factory->createTcp(g_impl->owner);

    tcp_arg(newpcb, c);
    tcp_recv(newpcb, lwipTcpRecv);
    tcp_sent(newpcb, lwipTcpSent);
    tcp_err(newpcb, lwipTcpErr);
    tcp_nagle_disable(newpcb);

    // 下面这些槽都是**直连**（context 是 NetStack，同一个线程），所以它们跑在信号发射者的栈上。
    // 凡是会销毁 c 的调用（pumpToLwip / closeConn）一律放在**最后一句**，槽返回后就没人再碰 c
    // 了——这样才不必在每个槽里再夹一层 ConnWatch。改这几个 lambda 时务必保持这个形状。
    QObject::connect(c->socks, &IOutboundTcp::established, g_impl->owner, [c]() {
        c->established = true;
        // 阶段0 埋点：出站建连耗时落桶（connectTo→established；Socks5 即环回握手 RTT）。
        // connectTimer 在下方 connectTo 之前 start()，established 只发一次，落桶一次。
        GatewayDiag::observeConnectMs(c->connectTimer.elapsed());
        // 握手期扣下的窗口在这里重新评估一次：pending 刚被冲进 socket，水位变了。
        // flushRecvWindow 不会销毁 c（只有 getter + 纯 lwIP 调用），故无需保护。
        flushRecvWindow(c);
    });
    QObject::connect(c->socks, &IOutboundTcp::upstreamBytesWritten, g_impl->owner, [c](qint64) {
        // 上行真的排空了一点 → 按量把 lwIP 的接收窗口还回去。这是上行唯一的推进点，
        // 也正是「不会死锁」的依据：只要还扣着窗口，就一定还有没发完的字节在等这个信号。
        flushRecvWindow(c);
    });
    QObject::connect(c->socks, &IOutboundTcp::dataReceived, g_impl->owner, [c](const QByteArray &d) {
        c->downBytes += d.size(); // 阶段0 埋点：下行累计（关闭时落桶），纯整型自增
        c->toLwip.append(d);
        pumpToLwip(c); // ★ 可能销毁 c —— 必须是最后一句
    });
    QObject::connect(c->socks, &IOutboundTcp::failed, g_impl->owner, [c](const QString &) {
        ++GatewayDiag::c.socksFailed; // 拨出站失败：认证/端口/核心没起来，都汇到这一栏
        closeConn(c, true); // ★ 必然销毁 c —— 必须是最后一句
    });
    QObject::connect(c->socks, &IOutboundTcp::closed, g_impl->owner, [c]() {
        // socks 关闭：先记账，把剩余下行写完之后再优雅关闭 lwIP 侧（收口在 pumpToLwip 里，
        // 排空后自己会 closeConn）。
        c->socksClosed = true;
        pumpToLwip(c); // ★ 可能销毁 c —— 必须是最后一句
    });

    c->connectTimer.start(); // 阶段0 埋点：建连计时起点（必须紧邻 connectTo 之前）
    c->socks->connectTo(serverIp, serverPort, user);
    return ERR_OK;
}

// ————————————— lwIP 内存池诊断：把 LWIP_STATS 真正读出来 —————————————
//
// lwipopts.h 打开了 MEM_STATS/MEMP_STATS，但统计只是被「存下来」——池子耗尽时 lwIP 既不打日志
// 也不向上层报错（tcp_alloc 甚至会悄悄杀掉一条活着的连接），所以必须有人主动去读，否则等于没开。
//
// 成本控制（这条路挂在 200ms 的 lwIP 定时器上，绝不能给 GUI 线程添负担）：
//  · 常态 = 对下面这张表（9 项）各做一次 u16 比较，没变化立刻 return，零字符串分配；
//  · 只有 err 计数**相对上次上报**发生变化才考虑打日志，且两次上报至少隔 30s ——
//    池子一旦见底会每 200ms 都有新失败，不节流就是刷屏；
//  · 被节流吃掉的那一次**不更新基线**，攒着的失败会在下一个窗口一并报出来，不会被吞。
// 注意 LWIP_STATS_LARGE=0 → 计数器是 u16_t 会回绕，所以判据是「与上次不相等」而非「变大了」。
struct MempWatch {
    memp_t pool;
    const char *name;
};
constexpr MempWatch kMempWatch[] = {
    {MEMP_TCP_PCB, "TCP_PCB(并发连接)"},
    {MEMP_TCP_PCB_LISTEN, "TCP_PCB_LISTEN"},
    {MEMP_TCP_SEG, "TCP_SEG(发送/乱序段)"},
    {MEMP_PBUF, "PBUF(壳)"},
    {MEMP_PBUF_POOL, "PBUF_POOL(入站帧)"},
    {MEMP_UDP_PCB, "UDP_PCB"},
    {MEMP_REASSDATA, "REASSDATA(分片重组)"},
#if ARP_QUEUEING
    // 注意：lwIP 2.x 里 ARP_QUEUEING 默认是 0，此时 memp_std.h 根本不会生成 MEMP_ARP_QUEUE
    //（lwipopts.h 里的 MEMP_NUM_ARP_QUEUE 也就是个死配置）。跟着开关走，别硬写。
    {MEMP_ARP_QUEUE, "ARP_QUEUE"},
#endif
    {MEMP_SYS_TIMEOUT, "SYS_TIMEOUT"},
};
constexpr int kMempWatchCount = int(sizeof(kMempWatch) / sizeof(kMempWatch[0]));
constexpr qint64 kPoolReportMinIntervalMs = 30000;

void pollLwipPoolStats()
{
    static u16_t lastErr[kMempWatchCount] = {};
    static u16_t lastHeapErr = 0;
    static qint64 lastReportMs = -kPoolReportMinIntervalMs;

    bool changed = (lwip_stats.mem.err != lastHeapErr);
    for (int i = 0; !changed && i < kMempWatchCount; ++i) {
        const struct stats_mem *m = lwip_stats.memp[kMempWatch[i].pool];
        if (m && m->err != lastErr[i])
            changed = true;
    }
    if (!changed)
        return; // 绝大多数 tick 在这里就结束了
    const qint64 now = monoMs();
    if (now - lastReportMs < kPoolReportMinIntervalMs)
        return; // 节流：基线不动，攒到下个窗口一起报
    lastReportMs = now;

    QString detail;
    for (int i = 0; i < kMempWatchCount; ++i) {
        const struct stats_mem *m = lwip_stats.memp[kMempWatch[i].pool];
        if (!m)
            continue;
        if (m->err != lastErr[i]) {
            detail += QStringLiteral("%1 分配失败(err=%2 用量=%3 高水位=%4); ")
                          .arg(QLatin1String(kMempWatch[i].name))
                          .arg(uint(m->err))
                          .arg(uint(m->used))
                          .arg(uint(m->max));
            lastErr[i] = m->err;
        }
    }
    if (lwip_stats.mem.err != lastHeapErr) {
        detail += QStringLiteral("堆(mem.c) 分配失败(err=%1 用量=%2 高水位=%3 容量=%4); ")
                      .arg(uint(lwip_stats.mem.err))
                      .arg(uint(lwip_stats.mem.used))
                      .arg(uint(lwip_stats.mem.max))
                      .arg(uint(lwip_stats.mem.avail));
        lastHeapErr = lwip_stats.mem.err;
    }
    qWarning().noquote() << "NetStack: lwIP 内存吃紧 ——" << detail
                         << "(TCP_PCB 耗尽会静默丢 SYN、甚至挤掉活连接；其余多为降速。"
                            "上限见 lwipopts.h)";
}

// 诊断采样的 lwIP 那一段（声明见本文件上方常量区的注释）。
QString lwipStatsLine()
{
    QString out;

#if TCP_STATS
    // ★ lwIP **没有**重传计数器：struct stats_proto 只有 xmit/recv/fw/drop/chkerr/lenerr/
    //   memerr/rterr/proterr/opterr/err/cachehit 这几个，tcp_out.c 的三个重传入口
    //   （tcp_rexmit / tcp_rexmit_fast / tcp_rexmit_rto_commit）一个计数都不加。
    //   所以这里报的是实际存在的量，别指望从中直接读出「重传了几次」：
    //     · xmit/recv —— 本窗口收发的 TCP 段数。xmit 明显大于设备侧应有的量 = 在重传，
    //       这是目前能拿到的最接近的信号（粗，但零成本）。
    //     · drop —— 入站被丢的段（校验/序号/无 pcb 等）。
    //     · memerr —— 分配不到 pbuf/seg，直接对应 lwipopts.h 里那几个池的容量。
    //   真要精确的 rexmit 计数，得往 vendored lwIP 打一个 Coast 补丁（tcp_out.c 三处 +
    //   一个自有计数器），和已有的 accept-all / nd6 静态邻居补丁同一路数 —— 留给以后。
    // 上一窗口的快照。u16 计数器（LWIP_STATS_LARGE=0）的增量用 u16 运算，回绕一次结果依然正确。
    static u16_t lastXmit = 0, lastRecv = 0, lastDrop = 0, lastMemerr = 0;
    const u16_t xmit = lwip_stats.tcp.xmit, recv = lwip_stats.tcp.recv;
    const u16_t drop = lwip_stats.tcp.drop, memerr = lwip_stats.tcp.memerr;
    out += QStringLiteral("tcpXmit=%1 tcpRecv=%2 tcpDrop=%3 tcpMemerr=%4")
               .arg(uint(u16_t(xmit - lastXmit)))
               .arg(uint(u16_t(recv - lastRecv)))
               .arg(uint(u16_t(drop - lastDrop)))
               .arg(uint(u16_t(memerr - lastMemerr)));
    lastXmit = xmit;
    lastRecv = recv;
    lastDrop = drop;
    lastMemerr = memerr;
#endif

    // 池子只报「用量/高水位/失败数」三元组，且只报有意义的那几个（全报会把行撑爆）。
    struct { memp_t pool; const char *name; } kPools[] = {
        {MEMP_TCP_PCB, "pcb"},
        {MEMP_TCP_SEG, "seg"},
        {MEMP_PBUF_POOL, "pbuf"},
    };
    for (const auto &p : kPools) {
        const struct stats_mem *m = lwip_stats.memp[p.pool];
        if (!m)
            continue;
        out += QStringLiteral(" %1=%2/%3/%4")
                   .arg(QLatin1String(p.name))
                   .arg(uint(m->used))
                   .arg(uint(m->max))   // 高水位：离 lwipopts.h 里的上限还有多远
                   .arg(uint(m->err));  // 非 0 = 撞过上限，已经在静默降级了
    }
    out += QStringLiteral(" heap=%1/%2/%3")
               .arg(uint(lwip_stats.mem.used))
               .arg(uint(lwip_stats.mem.max))
               .arg(uint(lwip_stats.mem.err));
    return out;
}

// —— UDP 手工封包辅助 ——
quint16 ipChecksum(const uchar *data, int len)
{
    quint32 sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += (quint32(data[i]) << 8) | data[i + 1];
    if (len & 1)
        sum += quint32(data[len - 1]) << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<quint16>(~sum);
}

// 手工封 IPv6/UDP/以太 回程包发给设备（v4 版 onUdpResponse 尾段的 v6 对应物）。
// s->victimAddr 是设备 16 字节 v6 地址，fromIp 是服务器（回包源）。UDP 校验和在 v6 里**强制**，
// 用 v6 伪首部计算；结果为 0 时按 RFC 2460 置 0xFFFF。
void sendUdpResponse6(UdpSess *s, quint16 vport, const QHostAddress &fromIp, quint16 fromPort,
                      const QByteArray &payload)
{
    if (!s->nic || !s->nic->ep || s->victimAddr.size() != 16)
        return;
    const Q_IPV6ADDR src6 = fromIp.toIPv6Address(); // 服务器地址（网络序）
    const uchar *dst6 = reinterpret_cast<const uchar *>(s->victimAddr.constData());
    const int udpLen = 8 + payload.size();
    const int ipLen = 40 + udpLen;

    QByteArray frame(14 + ipLen, char(0));
    uchar *b = reinterpret_cast<uchar *>(frame.data());
    // 以太头
    std::memcpy(b, s->mac6.constData(), 6);               // dst = 设备
    std::memcpy(b + 6, s->nic->localMac6.constData(), 6); // src = 本机在该卡上的 MAC
    b[12] = 0x86;
    b[13] = 0xDD;
    // IPv6 头
    uchar *ip = b + 14;
    ip[0] = 0x60;
    ip[4] = (udpLen >> 8) & 0xFF; // payload length
    ip[5] = udpLen & 0xFF;
    ip[6] = 17;                   // next header = UDP
    ip[7] = 64;                   // hop limit
    std::memcpy(ip + 8, src6.c, 16);
    std::memcpy(ip + 24, dst6, 16);
    // UDP 头
    uchar *u = ip + 40;
    u[0] = (fromPort >> 8) & 0xFF;
    u[1] = fromPort & 0xFF;
    u[2] = (vport >> 8) & 0xFF;
    u[3] = vport & 0xFF;
    u[4] = (udpLen >> 8) & 0xFF;
    u[5] = udpLen & 0xFF;
    u[6] = 0;
    u[7] = 0;
    std::memcpy(u + 8, payload.constData(), payload.size());
    // 校验和（v6 伪首部：src(16)+dst(16)+upper-len(4)+zero(3)+nexthdr(1=17)）
    QByteArray pseudo;
    pseudo.reserve(40 + udpLen);
    pseudo.append(reinterpret_cast<const char *>(src6.c), 16);
    pseudo.append(reinterpret_cast<const char *>(dst6), 16);
    pseudo.append(char((udpLen >> 24) & 0xFF));
    pseudo.append(char((udpLen >> 16) & 0xFF));
    pseudo.append(char((udpLen >> 8) & 0xFF));
    pseudo.append(char(udpLen & 0xFF));
    pseudo.append(char(0));
    pseudo.append(char(0));
    pseudo.append(char(0));
    pseudo.append(char(17));
    pseudo.append(reinterpret_cast<const char *>(u), udpLen);
    quint16 uck = ipChecksum(reinterpret_cast<const uchar *>(pseudo.constData()), pseudo.size());
    if (uck == 0)
        uck = 0xFFFF;
    u[6] = (uck >> 8) & 0xFF;
    u[7] = uck & 0xFF;

    s->nic->ep->send(frame);
    // UDP/DNS 回程是**单帧**且延迟敏感，而它由 socks 的 UDP 回调触发 —— 既不在收帧排空里、
    // 也不在 pumpToLwip 里，不在这里收口就要等泵那一拍（最多 25 ms）才发出去，DNS 会直接慢一档。
    s->nic->ep->flushTx();
}

// 手工封 IPv4/UDP/以太 回程包发给设备（从 onUdpResponse 抽出，DNS 劫持回程也复用）。
// src = 服务器(fromIp:fromPort)，dst = 设备(s->victimIp:vport)。不依赖 UdpFlow，只需要 UdpSess 的
// mac6/nic/victimIp —— 所以 DNS 劫持（不建 flow）也能直接调它把 mihomo 的应答封回设备。
void sendUdpResponse4(UdpSess *s, quint16 vport, const QHostAddress &fromIp, quint16 fromPort,
                      const QByteArray &payload)
{
    if (!s->nic || !s->nic->ep)
        return;
    const quint32 srcIp = fromIp.toIPv4Address();                    // 服务器
    const quint32 dstIp = QHostAddress(s->victimIp).toIPv4Address(); // 设备
    const int udpLen = 8 + payload.size();
    const int ipLen = 20 + udpLen;

    QByteArray frame(14 + ipLen, char(0));
    uchar *b = reinterpret_cast<uchar *>(frame.data());
    std::memcpy(b, s->mac6.constData(), 6);               // dst = 设备
    std::memcpy(b + 6, s->nic->localMac6.constData(), 6); // src = 本机在该卡上的 MAC
    b[12] = 0x08; b[13] = 0x00;
    uchar *ip = b + 14;
    ip[0] = 0x45; ip[1] = 0x00;
    ip[2] = (ipLen >> 8) & 0xFF; ip[3] = ipLen & 0xFF;
    ip[4] = 0; ip[5] = 0;
    ip[6] = 0x40; ip[7] = 0; // DF
    ip[8] = 64;  ip[9] = 17; // ttl, proto=UDP
    ip[10] = 0;  ip[11] = 0;
    ip[12] = (srcIp >> 24) & 0xFF; ip[13] = (srcIp >> 16) & 0xFF;
    ip[14] = (srcIp >> 8) & 0xFF;  ip[15] = srcIp & 0xFF;
    ip[16] = (dstIp >> 24) & 0xFF; ip[17] = (dstIp >> 16) & 0xFF;
    ip[18] = (dstIp >> 8) & 0xFF;  ip[19] = dstIp & 0xFF;
    const quint16 ipck = ipChecksum(ip, 20);
    ip[10] = (ipck >> 8) & 0xFF; ip[11] = ipck & 0xFF;
    uchar *u = ip + 20;
    u[0] = (fromPort >> 8) & 0xFF; u[1] = fromPort & 0xFF;
    u[2] = (vport >> 8) & 0xFF;    u[3] = vport & 0xFF;
    u[4] = (udpLen >> 8) & 0xFF;   u[5] = udpLen & 0xFF;
    u[6] = 0; u[7] = 0;
    std::memcpy(u + 8, payload.constData(), payload.size());
    QByteArray pseudo;
    pseudo.append(char((srcIp >> 24) & 0xFF)); pseudo.append(char((srcIp >> 16) & 0xFF));
    pseudo.append(char((srcIp >> 8) & 0xFF));  pseudo.append(char(srcIp & 0xFF));
    pseudo.append(char((dstIp >> 24) & 0xFF)); pseudo.append(char((dstIp >> 16) & 0xFF));
    pseudo.append(char((dstIp >> 8) & 0xFF));  pseudo.append(char(dstIp & 0xFF));
    pseudo.append(char(0)); pseudo.append(char(17));
    pseudo.append(char((udpLen >> 8) & 0xFF)); pseudo.append(char(udpLen & 0xFF));
    pseudo.append(reinterpret_cast<const char *>(u), udpLen);
    quint16 uck = ipChecksum(reinterpret_cast<const uchar *>(pseudo.constData()), pseudo.size());
    if (uck == 0)
        uck = 0xFFFF;
    u[6] = (uck >> 8) & 0xFF; u[7] = uck & 0xFF;

    s->nic->ep->send(frame);
    // UDP/DNS 回程是**单帧**且延迟敏感，而它由 socks 的 UDP 回调触发 —— 既不在收帧排空里、
    // 也不在 pumpToLwip 里，不在这里收口就要等泵那一拍（最多 25 ms）才发出去，DNS 会直接慢一档。
    s->nic->ep->flushTx();
}

} // namespace

// ———————————————————————————— NetStack ————————————————————————————
NetStack::NetStack(quint16 socksPort, QObject *parent)
    : QObject(parent), d(new Impl)
{
    // 默认出站 = 拨 mihomo 混合端口。CoastCore 落地后这里可换成按节点选实现的工厂。
    d->factory = new Socks5OutboundFactory(socksPort);
    d->owner = this;
}

NetStack::~NetStack()
{
    // 诊断日志收尾：写掉最后一个采样窗口 + 一条停机标记。放在这里而不是 LanGateway::disableAll，
    // 是因为**本析构在工作线程上跑**（见 LanGateway_linux.cpp 的线程模型），与所有 sample() 调用
    // 同线程 —— GatewayDiag 的单线程前提得以保持。此刻 d->timer 还没被 delete d 干掉，但我们已经
    // 不再回事件循环了，不会有第二个 sample 并发进来。
    // 注意：进程被直接杀掉时（见 main_qml.cpp 里关于 aboutToQuit 跑不到的说明）这里不会执行，
    // 最多丢最后一个未满 10s 的窗口 —— 可以接受，不为它加复杂度。
    if (d->inited)
        GatewayDiag::flush(lwipStatsLine(), "stop");

    for (UdpSess *s : std::as_const(d->udp))
        destroyUdpSess(d, s);
    d->udp.clear();
    for (Nic *n : d->nics) {
        if (d->inited)
            netif_remove(&n->nif);
        delete n;
    }
    if (g_impl == d)
        g_impl = nullptr;
    delete d->factory;
    delete d;
}

void NetStack::setOutboundFactory(OutboundFactory *f)
{
    if (!f || f == d->factory)
        return;
    // 取得所有权：先 delete 旧的再存新的。在途连接/会话早已各自持有从旧工厂造出的出站对象
    //（不是持有工厂本身），所以换工厂不影响它们；只有此后新建的连接才向新工厂要出站。
    delete d->factory;
    d->factory = f;
}

bool NetStack::init(QString *err)
{
    if (d->inited)
        return true;
    if (g_impl && g_impl != d) {
        if (err)
            *err = QStringLiteral("已有一个网关协议栈实例在运行");
        return false;
    }
    g_impl = d;
    g_debug = qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG");

    lwip_init();

    // catch-all TCP 监听：绑**双栈任意 IP**（IP_ANY_TYPE）+ 端口 0（配合 tcp_in.c 补丁通配任意目的
    // 端口）。监听是全局的、与网卡无关：哪张卡进来的 v4/v6 SYN 都命中它，accept 里再按设备 IP 查身份。
    // ★ 必须用 tcp_new_ip_type(IPADDR_TYPE_ANY) + IP_ANY_TYPE，不能用旧的 tcp_new()+IP_ADDR_ANY：
    //   后者的 pcb 类型是 IPv4，tcp_in.c 的监听匹配里 v6 SYN 走不到「ANY_TYPE 通配」那条分支，会被
    //   IP 版本精确匹配挡掉 → v6 SYN 无人 accept。ANY_TYPE 让同一个监听器同时收 v4/v6。
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        g_impl = nullptr;
        if (err)
            *err = QStringLiteral("tcp_new 失败");
        return false;
    }
    tcp_bind(pcb, IP_ANY_TYPE, 0);
    d->listener = tcp_listen_with_backlog(pcb, TCP_DEFAULT_LISTEN_BACKLOG);
    if (!d->listener) {
        tcp_close(pcb);
        g_impl = nullptr;
        if (err)
            *err = QStringLiteral("tcp_listen 失败");
        return false;
    }
    d->listener->local_port = 0; // 通配任意目的端口
    tcp_accept(d->listener, lwipTcpAccept);

    // lwIP 定时器泵（TCP 重传/超时/ARP 老化）+ UDP 流老化 + 内存池诊断。
    //
    // ★ 泵的周期必须**明显细于** lwIP 自己的 TCP 定时器周期（TCP_TMR_INTERVAL，见 lwipopts.h），
    //   否则每一拍都要等下一次泵才跑得到，实际周期被拉长到「泵周期与 TCP 周期的公倍数量级」。
    //   老值 200ms 配 lwIP 默认的 250ms 就是这个毛病：tcp_fasttmr（延迟 ACK）实际约 400ms 一次、
    //   tcp_slowtmr（**重传**）约 600ms 一次 —— 本机→设备方向一旦丢一帧，恢复要大半秒起步。
    //   被代理设备的表现就是「访问什么都慢、偶尔打不开」，且与目标在国内还是国外无关。
    //   现在 TCP_TMR_INTERVAL 降到 100ms、泵降到 25ms（1/4 周期，留足抖动余量）。
    // 成本：空转一拍就是「读一次时钟 + 比一次链表头」，40 次/秒可以忽略；且这个定时器只在网关
    //   开着时存在，跑在 LanGateway 的工作线程上，不碰 GUI 线程。
    // 老化和诊断**不跟着提频**：它们本来就是 200ms 一次的量级，没必要 8 倍频，按拍数分频即可。
    d->timer = new QTimer(this);
    d->timer->setInterval(kLwipPumpIntervalMs);
    // ★ 必须显式 PreciseTimer —— 默认的 Qt::CoarseTimer 在 **Windows** 上把这个泵毁掉两次：
    //   1) 粒度：CoarseTimer 走 SetTimer/WM_TIMER，受系统时钟节拍（默认 15.6ms）约束，25ms 实际
    //      变成 31.25ms。真机 gateway-diag.log 里 6033 个采样窗口有 5933 个泵周期正好是 31ms
    //      —— 不是负载，是纯粹的量化误差，空闲时也一样。
    //   2) 饥饿：WM_TIMER 是**队列空时才合成**的最低优先级消息。数据面忙起来（Npcap 收帧事件 +
    //      上百条到 mihomo 的 socket 通知挤满消息队列）时它会被无限期推后 —— 同一份日志里，设备
    //      下行 >600 帧/秒的窗口泵周期滑到 45ms、26% 的拍迟到 2 倍以上、最坏一次迟到 631ms。
    //      泵一停就是 lwIP 的重传/延迟 ACK 全停，对外表现是所有被代理设备同时卡住半秒。
    //   PreciseTimer 在 Qt 的 Windows 事件分发器里走 timeSetEvent（多媒体定时器）：既不受 15.6ms
    //   节拍限制，也不再是 WM_TIMER，因而不被消息队列里的收帧/socket 事件饿死。
    //   linux/mac 上两种类型都是 timerfd/kqueue，本行无副作用。
    d->timer->setTimerType(Qt::PreciseTimer);
    d->pumpClock.start();
    connect(d->timer, &QTimer::timeout, this, [this] {
        // ★ 泵的迟到量 = 工作线程的饱和度。这一拍本该 kLwipPumpIntervalMs 之后就到，迟到多少就
        //   说明上一拍的活（收帧 → lwIP → SOCKS 读写）占了多少额外时间。这是**唯一**能把
        //   「链路丢包」和「本机算不过来」分开的指标：前者只涨 txdrop/rxdrop，后者只涨这里。
        const qint64 elapsed = d->pumpClock.restart();
        const qint64 lag = elapsed - kLwipPumpIntervalMs;
        ++GatewayDiag::c.pumpTicks;
        GatewayDiag::observePumpLag(lag); // 阶段0 埋点：每拍迟到量分布（含早到，归 <=0 桶）
        if (elapsed > 2 * kLwipPumpIntervalMs) {
            ++GatewayDiag::c.pumpLateTicks;
            if (lag > GatewayDiag::c.pumpMaxLagMs)
                GatewayDiag::c.pumpMaxLagMs = lag;
        }

        // ★ 收帧排空兜底（Windows 才有实际动作，见 IL2Endpoint::drainNow 的注释）。
        //   放在 sys_check_timeouts() **之前**：先把已经到达的帧喂进 lwIP，再跑它的定时器 ——
        //   否则新到的 ACK 要多等一整拍才被 lwIP 看到，等于白排空。
        //   先快照 key 再遍历：排空会同步触发 frameReceived → inputFrame，虽然帧处理路径不会
        //   增删网卡，但直接在 QHash 上边遍历边回调是自找麻烦。网卡数是个位数，这份拷贝可忽略。
        if (!d->nics.isEmpty()) {
            const QList<IL2Endpoint *> eps = d->nics.keys();
            for (IL2Endpoint *ep : eps) {
                if (ep && d->nics.contains(ep)) // 上一张卡的回调万一把这张摘了，这里挡一下
                    ep->drainNow();
            }
        }

        sys_check_timeouts();
        // ★ flushTx 的第三个调用点，同时是**兜底**：重传、延迟 ACK、ARP/NDP 周期投毒这些帧
        //   既不在收帧排空里、也不在 pumpToLwip 里，只有这里能收口。它也保证了「任何漏掉
        //   flush 的出帧路径最迟 25 ms 也会被发出去」——加新的出帧路径时这条兜底是安全网，
        //   但别拿它当主路（25 ms 的延迟对 TCP 自时钟是致命的）。
        flushNicTx(d);
        if (++d->pumpTick >= kHousekeepEveryTicks) {
            d->pumpTick = 0;
            reapUdpFlows(d);
            pollLwipPoolStats();
        }
        // 诊断采样：按墙钟判间隔（不按拍数——泵一旦迟到，拍数和真实时间就对不上了）。
        if (GatewayDiag::enabled()) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - d->lastDiagMs >= GatewayDiag::sampleIntervalMs()) {
                d->lastDiagMs = now;
                GatewayDiag::sample(lwipStatsLine());
            }
        }
    });
    d->timer->start();

    d->inited = true;
    return true;
}

bool NetStack::addNic(IL2Endpoint *ep, const QByteArray &localMac6, const QString &localIp,
                      const QString &netmask, QString *err)
{
    if (!d->inited) {
        if (err)
            *err = QStringLiteral("协议栈未初始化");
        return false;
    }
    if (!ep || localMac6.size() != 6) {
        if (err)
            *err = QStringLiteral("网卡端点或本机 MAC 非法");
        return false;
    }
    if (d->nics.contains(ep))
        return true;

    // netif 的 IP/掩码用**本机在这张卡上的真实地址**，不能再用早期的 0.0.0.1 // 0.0.0.0 占位：
    //  · ip4_route 会跳过 IP 为 0.0.0.0 的 netif（回包找不到出口而被丢），所以 IP 必须非零；
    //  · 掩码若给 0.0.0.0，该 netif「匹配一切」，多网卡时 ip4_route 永远返回链表第一张 →
    //    B 网段设备的回包会从 A 网卡发出去。用真实掩码，出方向按子网各归各的。
    ip4_addr_t nip, nmask, ngw;
    if (!ip4addr_aton(localIp.toLatin1().constData(), &nip) || ip4_addr_isany_val(nip)) {
        if (err)
            *err = QStringLiteral("本机 IP 非法: ") + localIp;
        return false;
    }
    if (!ip4addr_aton(netmask.toLatin1().constData(), &nmask) || ip4_addr_isany_val(nmask)) {
        if (err)
            *err = QStringLiteral("子网掩码非法: ") + netmask;
        return false;
    }
    ip4_addr_set_zero(&ngw);

    auto *nic = new Nic;
    nic->ep = ep;
    nic->localMac6 = localMac6;
    nic->localIp = localIp;
    nic->netmask = netmask;
    // state 必须在 netif_add 之前就绪：lwipNetifInit 里要用它填 hwaddr。
    if (netif_add(&nic->nif, &nip, &nmask, &ngw, nic, lwipNetifInit, ethernet_input) == nullptr) {
        delete nic;
        if (err)
            *err = QStringLiteral("netif_add 失败");
        return false;
    }
    if (d->nics.isEmpty())
        netif_set_default(&nic->nif); // ip4_route 无匹配时的兜底出口
    netif_set_up(&nic->nif);
    netif_set_link_up(&nic->nif);
#if LWIP_IPV6
    // 给这张卡一个 IPv6 链路本地地址（本机 MAC 派生）。它只当我们发 NS/NA 的源地址与 nd6 内部
    // 一致性用；不配任何全局 v6——被劫持设备的 v6 目的是「任意公网地址」，由 ip6.c 的 accept-all
    // 补丁接管，回程由 nd6 静态邻居项（addDeviceV6）直连设备 MAC，都不依赖本机有没有全局 v6。
    // DAD 已在 lwipopts 关（LWIP_IPV6_DUP_DETECT_ATTEMPTS=0）→ 该地址即刻 PREFERRED，无需等待。
    // 不设 ip6_autoconfig_enabled：LWIP_IPV6_AUTOCONFIG=0 时该字段根本不存在，且我们本就不做 SLAAC。
    netif_create_ip6_linklocal_address(&nic->nif, 1);
#endif
    d->nics.insert(ep, nic);
    return true;
}

void NetStack::removeNic(IL2Endpoint *ep)
{
    Nic *nic = d->nics.take(ep);
    if (!nic)
        return;
    if (d->inited)
        netif_remove(&nic->nif);
    // 该卡上的 UDP 会话失去出口，一并收掉（设备重新发包会重建）。
    const QStringList victims = d->udp.keys();
    for (const QString &ip : victims) {
        UdpSess *s = d->udp.value(ip);
        if (s && s->nic == nic) {
            d->udp.remove(ip);
            destroyUdpSess(d, s);
        }
    }
    // 指向本卡的 v6 静态邻居索引也清掉（netif_remove 已把 nd6 项随 netif 带走；这里只是别让
    // deviceV6Nic 留下悬垂的 Nic*，否则之后 removeDeviceV6 会 use-after-free）。
    const QStringList v6keys = d->deviceV6Nic.keys();
    for (const QString &ip6 : v6keys) {
        if (d->deviceV6Nic.value(ip6) == nic)
            d->deviceV6Nic.remove(ip6);
    }
    delete nic;
}

bool NetStack::hasNic(IL2Endpoint *ep) const
{
    return d->nics.contains(ep);
}

void NetStack::addDevice(const QString &ip, const QByteArray &mac6, const QString &socksUser)
{
    if (ip.isEmpty() || mac6.size() != 6)
        return;
    d->devices.insert(ip, DeviceInfo{mac6, socksUser});
    if (d->inited) {
        // 预置静态 ARP：lwIP 回包给设备时直接用其 MAC，不发 ARP 请求。
        ip4_addr_t a;
        if (ip4addr_aton(ip.toLatin1().constData(), &a)) {
            struct eth_addr e;
            std::memcpy(e.addr, mac6.constData(), 6);
            etharp_add_static_entry(&a, &e);
        }
    }
}

void NetStack::removeDevice(const QString &ip)
{
    d->devices.remove(ip);
    if (auto *s = d->udp.take(ip))
        destroyUdpSess(d, s);
    if (d->inited) {
        ip4_addr_t a;
        if (ip4addr_aton(ip.toLatin1().constData(), &a))
            etharp_remove_static_entry(&a);
    }
}

void NetStack::addDeviceV6(IL2Endpoint *from, const QString &ip6, const QByteArray &mac6,
                           const QString &socksUser)
{
    if (ip6.isEmpty() || mac6.size() != 6)
        return;
    // 与 v4 共用 devices 表：key 是 v6 地址串，和 v4 的点分串天然不冲突。lwipTcpAccept 里
    // userForIp(victimIp) 用的就是这张表，v6 连接的 victimIp 是 v6 串，正好命中。
    d->devices.insert(ip6, DeviceInfo{mac6, socksUser});
    if (!d->inited)
        return;
    Nic *nic = d->nics.value(from);
    if (!nic)
        return;
#if LWIP_IPV6
    ip6_addr_t a;
    if (!ip6addr_aton(ip6.toLatin1().constData(), &a))
        return;
    // 预置静态邻居：lwIP 回包给设备时直接用其 MAC，把该 v6 目的当 on-link，不发 NS（见 nd6.c 补丁）。
    // LWIP_IPV6_SCOPES=0 → 无需给地址 assign zone。
    nd6_add_static_neighbor_entry(&a, &nic->nif,
                                  reinterpret_cast<const u8_t *>(mac6.constData()));
    d->deviceV6Nic.insert(ip6, nic);
#endif
}

void NetStack::removeDeviceV6(const QString &ip6)
{
    d->devices.remove(ip6);
    if (auto *s = d->udp.take(ip6))
        destroyUdpSess(d, s);
    Nic *nic = d->deviceV6Nic.take(ip6);
#if LWIP_IPV6
    if (d->inited && nic) {
        ip6_addr_t a;
        if (ip6addr_aton(ip6.toLatin1().constData(), &a))
            nd6_remove_static_neighbor_entry(&a, &nic->nif);
    }
#else
    Q_UNUSED(nic);
#endif
}

void NetStack::inputFrame(IL2Endpoint *from, const QByteArray &frame)
{
    if (!d->inited || frame.size() < 14)
        return;
    Nic *nic = d->nics.value(from);
    if (!nic)
        return; // 帧来自一张没挂上来的卡（正在重配/已摘除）
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    const quint16 ethType = (quint16(f[12]) << 8) | f[13];
    if (g_debug) {
        const int proto = (frame.size() >= 24) ? f[14 + 9] : -1;
        std::fprintf(stderr, "NETSTACK IN nic=%s len=%d eth=%04x proto=%d\n",
                     nic->localIp.toLatin1().constData(), int(frame.size()), ethType, proto);
    }

    // ARP/NDP 等不喂 lwIP（投毒由 Arp/NdpSpoofer 负责，避免 lwIP 误答）。LanGateway 已在上游把
    // ARP 与 ICMPv6-NDP(NS/NA/RS/RA) 截走不喂进来；这里只按 ethertype 分 v4/v6 两条数据路径。
    if (ethType == 0x86DD) { // IPv6
        if (frame.size() < 14 + 40)
            return;
        // NDP 从不带扩展头；这里只看紧邻的 next header。有扩展头的（罕见）会当作「非 UDP」喂给
        // lwIP，lwIP 处理不了就丢——首个版本可接受（TCP 被 MSS 钳住不分片、UDP 无扩展头）。
        const quint8 nexthdr = f[14 + 6];
        if (nexthdr == 17) { // UDP：手工拦截转发（含 DNS）
            handleUdpFrame6(nic, frame);
            return;
        }
        // TCP/ICMPv6-echo 等交给 lwIP（accept-all v6 补丁把无主单播收到 inp 上）。
        struct pbuf *p6 = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(frame.size()), PBUF_POOL);
        if (!p6)
            return;
        pbuf_take(p6, frame.constData(), static_cast<u16_t>(frame.size()));
        if (nic->nif.input(p6, &nic->nif) != ERR_OK)
            pbuf_free(p6);
        return;
    }
    if (ethType != 0x0800) // 只剩 IPv4
        return;
    if (frame.size() < 14 + 20)
        return;
    const uchar *ip = f + 14;
    const int ihl = (ip[0] & 0x0F) * 4;
    const quint8 proto = ip[9];

    if (proto == 17) { // UDP：手工拦截转发（含 DNS）
        handleUdpFrame(nic, frame, ihl);
        return;
    }
    // TCP/ICMP 等交给 lwIP —— 注入**收到它的那个 netif**（accept-all 补丁会把无主单播收到 inp 上）。
    struct pbuf *p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(frame.size()), PBUF_POOL);
    if (!p)
        return;
    pbuf_take(p, frame.constData(), static_cast<u16_t>(frame.size()));
    if (nic->nif.input(p, &nic->nif) != ERR_OK)
        pbuf_free(p);
}

// ———————————————————————————— UDP 拦截 ————————————————————————————
void NetStack::handleUdpFrame(Nic *nic, const QByteArray &frame, int ihl)
{
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    const uchar *ip = f + 14;
    if (14 + ihl + 8 > frame.size())
        return;
    const QString srcIp = QString("%1.%2.%3.%4").arg(ip[12]).arg(ip[13]).arg(ip[14]).arg(ip[15]);
    const quint32 dstIpV4 = (quint32(ip[16]) << 24) | (quint32(ip[17]) << 16)
                          | (quint32(ip[18]) << 8) | quint32(ip[19]);
    const uchar *udp = ip + ihl;
    const quint16 sport = (quint16(udp[0]) << 8) | udp[1];
    const quint16 dport = (quint16(udp[2]) << 8) | udp[3];
    const int ulen = (quint16(udp[4]) << 8) | udp[5];
    if (ulen < 8 || 14 + ihl + ulen > frame.size())
        return;
    // ★ 这里必须是**深拷贝**：payload 会被塞进 flow->pending 存到 SOCKS 握手完成之后，活得比本次
    //   调用久；而 frame 在 Linux 后端是 QByteArray::fromRawData 指向 TPACKET_v3 收环的视图，
    //   本函数一返回那块 mmap 内存就还给内核、随时被新帧覆写（契约见 IL2Endpoint::frameReceived）。
    //   mid() 在 Qt6 里只有 pos==0 && len==size 的「Full」分支才返回浅拷贝，这里起点恒 ≥ 14+20+8=42，
    //   走的是 sliced() 深拷贝分支 —— 成立，但别把起点改成 0。
    const QByteArray payload = frame.mid(14 + ihl + 8, ulen - 8);

    const DeviceInfo dev = d->devices.value(srcIp);
    if (dev.mac6.size() != 6)
        return;

    UdpSess *s = d->udp.value(srcIp);
    if (!s) {
        s = new UdpSess;
        s->mac6 = dev.mac6;
        s->victimIp = srcIp;
        s->nic = nic; // 回程包要从这张卡发、用这张卡的本机 MAC 当源
        d->udp.insert(srcIp, s);
    }

    // ★ DNS 劫持：设备的 :53 查询转投 mihomo 的 fake-ip DNS（见 kDnsHijackPort / hijackDns），
    //   不建 UdpFlow、不原样中继到「设备配置的 DNS」（常是网关/路由器 IP，经用户态栈中继到它走不通
    //   → 名字解析时断时通）。回封的源地址伪装成设备原本查询的那个 DNS（dstIpV4:53），设备才收。
    if (dport == 53) {
        hijackDns(srcIp, sport, QHostAddress(dstIpV4), payload, false);
        return;
    }

    // 一个设备源端口一条流（= 一个独立 Socks5Udp）。回程靠「从哪条关联进来」定位设备端口，
    // 不再靠 (dstIp,dport) 反查——那正是老代码串包的根因，见文件上方的方案说明。
    UdpFlow *flow = s->flows.value(sport);
    if (!flow) {
        reapUdpFlows(d); // 建流前顺手回收：突发 DNS 场景下这一步就够把上一波流还回去了
        if (s->flows.size() >= kMaxUdpFlowsPerDevice)
            evictOldestFlowOfDevice(d, s);
        while (d->udpFlowCount >= kMaxUdpFlowsTotal
               && (d->udpLruShort.tail || d->udpLruLong.tail))
            evictGlobalOldestFlow(d);

        ++GatewayDiag::c.udpFlowsCreated;
        flow = new UdpFlow;
        flow->sess = s;
        flow->vport = sport;
        flow->idleMs = isShortLivedUdpPort(dport) ? kUdpDnsIdleMs : kUdpIdleMs;
        flow->socks = d->factory->createUdp(this);
        s->flows.insert(sport, flow);
        d->udpFlowCount++;

        UdpFlow *nf = flow;
        connect(nf->socks, &IOutboundUdp::ready, this, [nf]() {
            nf->ready = true;
            for (const UdpFlow::Pending &pk : std::as_const(nf->pending))
                nf->socks->sendTo(QHostAddress(pk.dstIp), pk.dport, pk.payload);
            nf->pending.clear();
        });
        connect(nf->socks, &IOutboundUdp::datagramReceived, this,
                [this, srcIp, sport](const QHostAddress &fromIp, quint16 fromPort,
                                     const QByteArray &data) {
                    // 捕值不捕流指针：即便这条流已经被老化/淘汰掉，槽里也只是查表落空，
                    // 绝不会踩到悬垂指针。
                    onUdpResponse(srcIp, sport, fromIp, fromPort, data);
                });
        // 关联建不起来（mihomo 没起来）或控制连接掉线（mihomo 重启）→ 立刻收掉这条流，别让这个
        // 源端口一直黑洞到老化为止；设备下次发包会重建。destroyUdpFlow 走的是 disconnect +
        // deleteLater，在被删对象自己的信号里调用是安全的。
        connect(nf->socks, &IOutboundUdp::failed, this, [this, nf](const QString &) {
            destroyUdpFlow(d, nf);
        });
        connect(nf->socks, &IOutboundUdp::closed, this, [this, nf]() { destroyUdpFlow(d, nf); });

        nf->socks->associate(dev.socksUser);
    }

    // 续命 + 换档：同一个源端口只要打过一次非 DNS 目的，就永久升到长档（降回去会把一条正在用的
    // 长连接按 15s 掐掉）。lruPushFront 内部先 unlink，换链是顺带完成的。
    if (!isShortLivedUdpPort(dport))
        flow->idleMs = kUdpIdleMs;
    flow->lastUsed = monoMs();
    lruPushFront(flow->idleMs == kUdpDnsIdleMs ? &d->udpLruShort : &d->udpLruLong, flow);

    if (!flow->coneOpen) {
        flow->peers.insert(dstIpV4);
        if (flow->peers.size() > kMaxUdpPeersPerFlow) { // 广撒型流量：放弃来源校验，别再吃内存
            flow->coneOpen = true;
            flow->peers.clear();
        }
    }

    if (flow->ready) {
        flow->socks->sendTo(QHostAddress(dstIpV4), dport, payload);
    } else if (flow->pending.size() < kMaxUdpPendingPerFlow) {
        flow->pending.append(UdpFlow::Pending{dstIpV4, QByteArray(), dport, payload});
    }
}

// ———————————————————————————— UDP 拦截（IPv6）————————————————————————————
// 与 v4 版 handleUdpFrame 结构对称，复用同一套 UdpFlow/LRU/老化/上限机制；只有地址宽度、封包、
// 校验和不同。会话按设备 v6 源地址串建（与 v4 会话不冲突）。
void NetStack::handleUdpFrame6(Nic *nic, const QByteArray &frame)
{
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    const uchar *ip = f + 14;                 // IPv6 头（40 字节固定）
    const uchar *udp = ip + 40;
    if (14 + 40 + 8 > frame.size())
        return;
    Q_IPV6ADDR sraw, draw;
    std::memcpy(sraw.c, ip + 8, 16);          // 设备源地址
    std::memcpy(draw.c, ip + 24, 16);         // 目的（公网服务器）
    const QHostAddress srcAddr(sraw);
    const QHostAddress dstAddr(draw);
    const QString srcIp = srcAddr.toString();
    const quint16 sport = (quint16(udp[0]) << 8) | udp[1];
    const quint16 dport = (quint16(udp[2]) << 8) | udp[3];
    const int ulen = (quint16(udp[4]) << 8) | udp[5]; // v6 UDP 头的 length 含 8 字节头
    if (ulen < 8 || 14 + 40 + ulen > frame.size())
        return;
    // ★ 深拷贝，理由同 v4 版（frame 是 Linux TPACKET_v3 收环视图，返回即失效）。
    const QByteArray payload = frame.mid(14 + 40 + 8, ulen - 8);

    const DeviceInfo dev = d->devices.value(srcIp);
    if (dev.mac6.size() != 6)
        return;

    UdpSess *s = d->udp.value(srcIp);
    if (!s) {
        s = new UdpSess;
        s->mac6 = dev.mac6;
        s->victimIp = srcIp;
        s->nic = nic;
        s->v6 = true;
        s->victimAddr = QByteArray(reinterpret_cast<const char *>(sraw.c), 16);
        d->udp.insert(srcIp, s);
    }

    // DNS 劫持（v6，与 v4 同）：设备 :53 转投 mihomo 的 DNS，回封源伪装成设备查询的那个 v6 DNS。
    if (dport == 53) {
        hijackDns(srcIp, sport, dstAddr, payload, true);
        return;
    }

    UdpFlow *flow = s->flows.value(sport);
    if (!flow) {
        reapUdpFlows(d);
        if (s->flows.size() >= kMaxUdpFlowsPerDevice)
            evictOldestFlowOfDevice(d, s);
        while (d->udpFlowCount >= kMaxUdpFlowsTotal
               && (d->udpLruShort.tail || d->udpLruLong.tail))
            evictGlobalOldestFlow(d);

        ++GatewayDiag::c.udpFlowsCreated;
        flow = new UdpFlow;
        flow->sess = s;
        flow->vport = sport;
        flow->idleMs = isShortLivedUdpPort(dport) ? kUdpDnsIdleMs : kUdpIdleMs;
        // v6 的来源校验：peers 用的是 QSet<quint32>（v4 地址），装不下 v6。直接退化成全锥
        //（不校验来源），记录在案的取舍——v6 UDP（QUIC/DNS64 等）以此为代价换实现简单。
        flow->coneOpen = true;
        flow->socks = d->factory->createUdp(this);
        s->flows.insert(sport, flow);
        d->udpFlowCount++;

        UdpFlow *nf = flow;
        connect(nf->socks, &IOutboundUdp::ready, this, [nf]() {
            nf->ready = true;
            for (const UdpFlow::Pending &pk : std::as_const(nf->pending)) {
                Q_IPV6ADDR d6;
                std::memcpy(d6.c, pk.dst6.constData(), 16);
                nf->socks->sendTo(QHostAddress(d6), pk.dport, pk.payload);
            }
            nf->pending.clear();
        });
        connect(nf->socks, &IOutboundUdp::datagramReceived, this,
                [this, srcIp, sport](const QHostAddress &fromIp, quint16 fromPort,
                                     const QByteArray &data) {
                    onUdpResponse(srcIp, sport, fromIp, fromPort, data);
                });
        connect(nf->socks, &IOutboundUdp::failed, this, [this, nf](const QString &) {
            destroyUdpFlow(d, nf);
        });
        connect(nf->socks, &IOutboundUdp::closed, this, [this, nf]() { destroyUdpFlow(d, nf); });

        nf->socks->associate(dev.socksUser);
    }

    if (!isShortLivedUdpPort(dport))
        flow->idleMs = kUdpIdleMs;
    flow->lastUsed = monoMs();
    lruPushFront(flow->idleMs == kUdpDnsIdleMs ? &d->udpLruShort : &d->udpLruLong, flow);

    if (flow->ready) {
        flow->socks->sendTo(dstAddr, dport, payload);
    } else if (flow->pending.size() < kMaxUdpPendingPerFlow) {
        UdpFlow::Pending pk;
        pk.dport = dport;
        pk.payload = payload;
        pk.dst6 = QByteArray(reinterpret_cast<const char *>(draw.c), 16);
        flow->pending.append(pk);
    }
}

void NetStack::onUdpResponse(const QString &victimIp, quint16 vport, const QHostAddress &fromIp,
                             quint16 fromPort, const QByteArray &payload)
{
    UdpSess *s = d->udp.value(victimIp);
    if (!s || !s->nic)
        return;
    UdpFlow *f = s->flows.value(vport);
    if (!f)
        return; // 流已被老化/淘汰：迟到的回包直接丢
    // 来源校验只比对 IP、不比对端口：保留老代码「没见过的来源就丢」的严格度（地址限制型 NAT），
    // 又不至于误杀 TFTP/部分 STUN 那种「换个端口回你」的服务器。v6 会话 coneOpen 恒真，跳过。
    if (!f->coneOpen && !f->peers.contains(fromIp.toIPv4Address()))
        return;
    if (s->v6) {
        // 下行续命（同 v4 分支）：QUIC 大文件下载这类上行稀疏 ACK 的流别被误判空闲。
        f->lastUsed = monoMs();
        lruTouch(f);
        sendUdpResponse6(s, vport, fromIp, fromPort, payload);
        return;
    }
    // 下行也续命：QUIC 大文件下载这类「上行只有稀疏 ACK」的流，光靠上行续命可能被误判为空闲。
    f->lastUsed = monoMs();
    lruTouch(f);
    // v4 回封（逻辑已抽到 sendUdpResponse4，与 DNS 劫持回程共用）。
    sendUdpResponse4(s, vport, fromIp, fromPort, payload);
}

// DNS 劫持：把设备的一条 DNS 查询发给 mihomo 的 DNS(127.0.0.1:kDnsHijackPort)，应答原样回封给设备、
// 源地址伪装成设备原本查询的那个 DNS 服务器(origServer:53)——对设备完全透明，于是设备拿到的是 mihomo
// 的 fake-ip 结果，随后连 fake-ip 又经网关回到 mihomo，按国内外分流。用一次性 QUdpSocket（一问一答，
// 5s 兜底回收），不建 UdpFlow。应答到达时按 victimIp **重新查** UdpSess（防设备中途被摘除留下的悬垂）。
void NetStack::hijackDns(const QString &victimIp, quint16 vport, const QHostAddress &origServer,
                         const QByteArray &query, bool v6)
{
    ++GatewayDiag::c.dnsHijacked;
    // 「有没有等到应答」要在两个 lambda 之间共享，而它们的生命周期都挂在 ds 上 —— 用 shared_ptr
    // 而不是捕获裸 bool 的引用：5s 那条定时器可能在 readyRead 之后才跑，栈上的东西早没了。
    auto answered = std::make_shared<bool>(false);
    auto *ds = new QUdpSocket(this);
    connect(ds, &QUdpSocket::readyRead, this, [this, ds, victimIp, vport, origServer, v6, answered]() {
        *answered = true;
        while (ds->hasPendingDatagrams()) {
            QByteArray resp;
            resp.resize(int(ds->pendingDatagramSize()));
            const qint64 n = ds->readDatagram(resp.data(), resp.size());
            if (n < 0)
                break;
            resp.truncate(int(n));
            UdpSess *s = d->udp.value(victimIp);
            if (s && s->nic) {
                if (v6)
                    sendUdpResponse6(s, vport, origServer, 53, resp);
                else
                    sendUdpResponse4(s, vport, origServer, 53, resp);
            }
        }
        ds->deleteLater(); // 一问一答即弃
    });
    ds->writeDatagram(query, QHostAddress(QStringLiteral("127.0.0.1")), kDnsHijackPort);
    // 无应答兜底回收（mihomo 没起 / 解析失败）：5s 后无论如何删掉 socket，防泄漏。
    // dnsNoReply 涨 = 名字解析在核心那一侧就断了，这时再怎么查网关的数据面都是白费——
    // 有这一栏才分得清「解析不出来」和「解析出来了但连不上」。
    QTimer::singleShot(5000, ds, [ds, answered] {
        if (!*answered)
            ++GatewayDiag::c.dnsNoReply;
        ds->deleteLater();
    });
}
