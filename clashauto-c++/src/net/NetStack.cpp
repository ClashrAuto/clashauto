#include "NetStack.h"
#include "IL2Endpoint.h"
#include "Socks5Client.h"

#include <QDebug>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QSet>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

extern "C" {
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/memp.h"
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

// 一条被终结的 TCP 连接：lwIP pcb ↔ 到 mihomo 的 Socks5Tcp。
struct TcpConn {
    NetStack::Impl *impl = nullptr;
    struct tcp_pcb *pcb = nullptr;
    Socks5Tcp *socks = nullptr;
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
    Socks5Udp *socks = nullptr;
    quint16 vport = 0;                 // 设备源端口：回程包的目的端口就是它
    bool ready = false;                // associate 完成
    struct Pending {
        quint32 dstIp = 0;
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
struct UdpSess {
    QByteArray mac6;
    QString victimIp;
    NetStack::Nic *nic = nullptr;      // 该设备所在网卡：回程包从这张卡、用这张卡的 MAC 发
    QHash<quint16, UdpFlow *> flows;   // 设备源端口 → 流
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
    quint16 socksPort = 0;
    NetStack *owner = nullptr;
    struct tcp_pcb *listener = nullptr;
    QHash<IL2Endpoint *, Nic *> nics;        // 二层端点 → 该网卡的 netif 上下文
    QHash<QString, DeviceInfo> devices;      // 设备 IP → {mac,user}
    QHash<QString, UdpSess *> udp;           // 设备 IP → UDP 会话
    UdpLru udpLruShort;                      // 短档(DNS 类)流的 LRU
    UdpLru udpLruLong;                       // 长档(一般 UDP)流的 LRU
    int udpFlowCount = 0;                    // 全局流数（对上限用，省得遍历）
    QTimer *timer = nullptr;
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
    destroyUdpFlow(d, victim);
}

// 全局最久未用：两条链的链尾里挑更旧的那个，O(1)。
void evictGlobalOldestFlow(NetStack::Impl *d)
{
    UdpFlow *a = d->udpLruShort.tail;
    UdpFlow *b = d->udpLruLong.tail;
    UdpFlow *victim = !a ? b : (!b ? a : (a->lastUsed <= b->lastUsed ? a : b));
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
    netif->output = etharp_output;      // IP 出口经 ARP（静态表已填，不会真发 ARP 请求）
    netif->linkoutput = lwipLinkOutput; // 二层出口
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    if (NetStack::Nic *nic = nicOf(netif))
        std::memcpy(netif->hwaddr, nic->localMac6.constData(), 6);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET
                   | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    return ERR_OK;
}

void closeConn(TcpConn *c, bool abort);

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
            tcp_abort(c->pcb);
            aborted = true; // 若身处 lwIP 回调，调用方要据此回 ERR_ABRT
        } else {
            // 优雅关闭前**必须**把扣着的接收窗口还回去：tcp_close 一看到 rcv_wnd != TCP_WND_MAX
            // 就认定「上层没把对端的数据收完」，改发 RST 而不是 FIN（tcp_close_shutdown 里的
            // rst_on_unacked_data 分支）——设备侧会连带丢掉已经收到的下行数据。
            // 这是引入上行背压之后新出现的坑，不还窗口就会变成「下载到一半被 RST」。
            giveBackRecvWindow(c);
            if (tcp_close(c->pcb) != ERR_OK) {
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
    const QString victimIp = QString::fromLatin1(ipaddr_ntoa(&newpcb->remote_ip));
    const QString user = g_impl ? g_impl->userForIp(victimIp) : QString();
    if (g_debug)
        std::fprintf(stderr, "NETSTACK ACCEPT server=%s:%u victim=%s user=%s\n",
                     serverIp.toLatin1().constData(), serverPort,
                     victimIp.toLatin1().constData(), user.toLatin1().constData());

    auto *c = new TcpConn;
    c->impl = g_impl;
    c->pcb = newpcb;
    c->socks = new Socks5Tcp(g_impl->owner);

    tcp_arg(newpcb, c);
    tcp_recv(newpcb, lwipTcpRecv);
    tcp_sent(newpcb, lwipTcpSent);
    tcp_err(newpcb, lwipTcpErr);
    tcp_nagle_disable(newpcb);

    // 下面这些槽都是**直连**（context 是 NetStack，同一个线程），所以它们跑在信号发射者的栈上。
    // 凡是会销毁 c 的调用（pumpToLwip / closeConn）一律放在**最后一句**，槽返回后就没人再碰 c
    // 了——这样才不必在每个槽里再夹一层 ConnWatch。改这几个 lambda 时务必保持这个形状。
    QObject::connect(c->socks, &Socks5Tcp::established, g_impl->owner, [c]() {
        c->established = true;
        // 握手期扣下的窗口在这里重新评估一次：pending 刚被冲进 socket，水位变了。
        // flushRecvWindow 不会销毁 c（只有 getter + 纯 lwIP 调用），故无需保护。
        flushRecvWindow(c);
    });
    QObject::connect(c->socks, &Socks5Tcp::upstreamBytesWritten, g_impl->owner, [c](qint64) {
        // 上行真的排空了一点 → 按量把 lwIP 的接收窗口还回去。这是上行唯一的推进点，
        // 也正是「不会死锁」的依据：只要还扣着窗口，就一定还有没发完的字节在等这个信号。
        flushRecvWindow(c);
    });
    QObject::connect(c->socks, &Socks5Tcp::dataReceived, g_impl->owner, [c](const QByteArray &d) {
        c->toLwip.append(d);
        pumpToLwip(c); // ★ 可能销毁 c —— 必须是最后一句
    });
    QObject::connect(c->socks, &Socks5Tcp::failed, g_impl->owner, [c](const QString &) {
        closeConn(c, true); // ★ 必然销毁 c —— 必须是最后一句
    });
    QObject::connect(c->socks, &Socks5Tcp::closed, g_impl->owner, [c]() {
        // socks 关闭：先记账，把剩余下行写完之后再优雅关闭 lwIP 侧（收口在 pumpToLwip 里，
        // 排空后自己会 closeConn）。
        c->socksClosed = true;
        pumpToLwip(c); // ★ 可能销毁 c —— 必须是最后一句
    });

    c->socks->connectTo(g_impl->socksPort, serverIp, serverPort, user);
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

} // namespace

// ———————————————————————————— NetStack ————————————————————————————
NetStack::NetStack(quint16 socksPort, QObject *parent)
    : QObject(parent), d(new Impl)
{
    d->socksPort = socksPort;
    d->owner = this;
}

NetStack::~NetStack()
{
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
    delete d;
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

    // catch-all TCP 监听：绑任意 IP + 端口 0（配合 tcp_in.c 补丁通配任意目的端口）。
    // 监听是全局的、与网卡无关：哪张卡进来的 SYN 都命中它，accept 里再按设备 IP 查身份。
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        g_impl = nullptr;
        if (err)
            *err = QStringLiteral("tcp_new 失败");
        return false;
    }
    tcp_bind(pcb, IP_ADDR_ANY, 0); // 绑任意 IP + 端口 0（配合 tcp_in.c 补丁通配任意目的端口）
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
    // 老化和诊断都搭这趟车而不是各自再开定时器：reapUdpFlows 只看两条 LRU 链的链尾，
    // pollLwipPoolStats 只做十来次 u16 比较，200ms 一次的代价可以忽略；
    // 绝不能在这里扫全表——GUI 线程刚治完卡顿。
    d->timer = new QTimer(this);
    d->timer->setInterval(200);
    connect(d->timer, &QTimer::timeout, this, [this] {
        sys_check_timeouts();
        reapUdpFlows(d);
        pollLwipPoolStats();
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

    // 仅处理 IPv4；ARP 等不喂 lwIP（ARP 投毒由 ArpSpoofer 负责，避免 lwIP 误答）。
    if (ethType != 0x0800)
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

        flow = new UdpFlow;
        flow->sess = s;
        flow->vport = sport;
        flow->idleMs = isShortLivedUdpPort(dport) ? kUdpDnsIdleMs : kUdpIdleMs;
        flow->socks = new Socks5Udp(this);
        s->flows.insert(sport, flow);
        d->udpFlowCount++;

        UdpFlow *nf = flow;
        connect(nf->socks, &Socks5Udp::ready, this, [nf]() {
            nf->ready = true;
            for (const UdpFlow::Pending &pk : std::as_const(nf->pending))
                nf->socks->sendTo(QHostAddress(pk.dstIp), pk.dport, pk.payload);
            nf->pending.clear();
        });
        connect(nf->socks, &Socks5Udp::datagramReceived, this,
                [this, srcIp, sport](const QHostAddress &fromIp, quint16 fromPort,
                                     const QByteArray &data) {
                    // 捕值不捕流指针：即便这条流已经被老化/淘汰掉，槽里也只是查表落空，
                    // 绝不会踩到悬垂指针。
                    onUdpResponse(srcIp, sport, fromIp, fromPort, data);
                });
        // 关联建不起来（mihomo 没起来）或控制连接掉线（mihomo 重启）→ 立刻收掉这条流，别让这个
        // 源端口一直黑洞到老化为止；设备下次发包会重建。destroyUdpFlow 走的是 disconnect +
        // deleteLater，在被删对象自己的信号里调用是安全的。
        connect(nf->socks, &Socks5Udp::failed, this, [this, nf](const QString &) {
            destroyUdpFlow(d, nf);
        });
        connect(nf->socks, &Socks5Udp::closed, this, [this, nf]() { destroyUdpFlow(d, nf); });

        nf->socks->associate(d->socksPort, dev.socksUser);
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
        flow->pending.append(UdpFlow::Pending{dstIpV4, dport, payload});
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
    // 又不至于误杀 TFTP/部分 STUN 那种「换个端口回你」的服务器。
    if (!f->coneOpen && !f->peers.contains(fromIp.toIPv4Address()))
        return;
    // 下行也续命：QUIC 大文件下载这类「上行只有稀疏 ACK」的流，光靠上行续命可能被误判为空闲。
    f->lastUsed = monoMs();
    lruTouch(f);

    // 手工封 UDP/IP/以太 回程包发给设备。
    const quint32 srcIp = fromIp.toIPv4Address();     // 服务器
    QHostAddress va(victimIp);
    const quint32 dstIp = va.toIPv4Address();          // 设备
    const int udpLen = 8 + payload.size();
    const int ipLen = 20 + udpLen;

    QByteArray frame(14 + ipLen, char(0));
    uchar *b = reinterpret_cast<uchar *>(frame.data());
    // 以太头
    std::memcpy(b, s->mac6.constData(), 6);                 // dst = 设备
    std::memcpy(b + 6, s->nic->localMac6.constData(), 6);   // src = 本机在该设备那张卡上的 MAC
    b[12] = 0x08; b[13] = 0x00;
    // IP 头
    uchar *ip = b + 14;
    ip[0] = 0x45; ip[1] = 0x00;
    ip[2] = (ipLen >> 8) & 0xFF; ip[3] = ipLen & 0xFF;
    ip[4] = 0; ip[5] = 0;
    ip[6] = 0x40; ip[7] = 0; // DF
    ip[8] = 64;  ip[9] = 17; // ttl, proto=UDP
    ip[10] = 0;  ip[11] = 0; // checksum 占位
    ip[12] = (srcIp >> 24) & 0xFF; ip[13] = (srcIp >> 16) & 0xFF;
    ip[14] = (srcIp >> 8) & 0xFF;  ip[15] = srcIp & 0xFF;
    ip[16] = (dstIp >> 24) & 0xFF; ip[17] = (dstIp >> 16) & 0xFF;
    ip[18] = (dstIp >> 8) & 0xFF;  ip[19] = dstIp & 0xFF;
    const quint16 ipck = ipChecksum(ip, 20);
    ip[10] = (ipck >> 8) & 0xFF; ip[11] = ipck & 0xFF;
    // UDP 头
    uchar *u = ip + 20;
    u[0] = (fromPort >> 8) & 0xFF; u[1] = fromPort & 0xFF;   // sport = 服务器端口
    u[2] = (vport >> 8) & 0xFF;    u[3] = vport & 0xFF;      // dport = 设备源端口
    u[4] = (udpLen >> 8) & 0xFF;   u[5] = udpLen & 0xFF;
    u[6] = 0; u[7] = 0; // checksum
    std::memcpy(u + 8, payload.constData(), payload.size());
    // UDP 校验和（含伪首部）
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

    if (s->nic->ep)
        s->nic->ep->send(frame);
}
