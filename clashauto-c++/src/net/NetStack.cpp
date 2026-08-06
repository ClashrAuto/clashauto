#include "NetStack.h"
#include "IL2Endpoint.h"
#include "IOutbound.h"
#include "core/DnsResolver.h"
#include "core/DnsMessage.h"
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

// TCP 数据面 = Rust(smoltcp)，经 C ABI 调用（rust/coaststack，头见 src/net/coaststack.h）。
// ★ 这个 TU 现在**只在 Windows 编**（见根 CMakeLists.txt）：Linux 走 TPROXY、macOS 走 pf rdr，
//   它们不需要用户态栈；非 Windows 由 NetStack_stub.cpp 顶上（init() 直接失败）。
extern "C" {
#include "coaststack.h"
}

#ifndef COAST_HAVE_RUST_STACK
// lwIP 已被彻底移除（见 docs/lwip-alternatives.md 的「彻底移除 lwIP 的执行计划」），
// 用户态栈只剩 smoltcp 这一条 —— 关掉 COAST_RUST 就没有任何 TCP 数据面可用了，
// 与其编出一个运行期才发现是空壳的二进制，不如在这里直接停下。
#  error "NetStack.cpp 需要 Rust 数据面：请开启 CMake 选项 COAST_RUST（默认 ON）"
#endif

namespace {

struct DeviceInfo {
    QByteArray mac6;
    QString socksUser;
    // 「禁网」设备。TCP 那条腿靠核心的 `IN-USER,<user>,REJECT` 规则就够了（连接带着用户名进
    // SOCKS），但 **UDP 和 DNS 走的不是 SOCKS 那条路**：DNS 被劫持直投核心的 :1053，
    // 普通 UDP 走 associate，两者到了核心都没有「入站用户」可供规则区分 ——
    // mihomo 的 DNS 监听更是根本没有这个概念。真机实测（设备设为 reject）：
    //   TCP 0 字节✓ / IPv6 000✓ / **DNS 有 44 字节应答✗ / UDP 有 9 字节回包✗**
    // 也就是被禁的设备照样能解析域名、照样能收发任意 UDP（够跑非 443 口的 QUIC、
    // 游戏流量甚至隧道）。这两条只能在**网关这一层**按设备身份拦，所以标记带到这里。
    bool reject = false;
};

// 一条经 smoltcp 终结的 TCP 连接 ↔ 到 mihomo 的 Socks5Tcp。
//
// ★ 这里曾经还有一个 lwIP 版的 `TcpConn`，连同 ~60 行 `ConnWatch`/`g_destroyedConn`/
//   `markConnDestroyed` 的重入防护。它们存在只因 lwIP 的回调可能**同步销毁**上下文、
//   且销毁后必须回 ERR_ABRT 给 tcp_in.c。coaststack 是 poll 模型（回调在 coast_stack_poll
//   内部发出，且引擎保证发回调时不持有自身借用），这一整类问题结构性地消失了 ——
//   这是换栈最大的**结构性**收益，比性能数字更值钱。
struct SmolConn {
    NetStack::Impl *impl = nullptr;
    quint64 id = 0;              // coaststack 的连接 id（0 恒无效）
    // 最后一次**有字节流动**的时刻。只做观测用（诊断行里的 idle5m/idleMaxS），
    // 不据此回收 —— RFC 5382 REQ-5 要求已建立的 TCP 映射不短于 2 小时 4 分，按空闲砍会把
    // 推送/IMAP 这类合法长连接掐断。有了这两个数才分得清「92 条是真在用」还是「僵尸堆积」。
    qint64 lastActive = 0;
    // ★ 出站是**接口**，不是具体实现：Socks5Tcp（拨 mihomo）与进程内协议出站都实现它。
    //   由 Impl::outFactory 创建 —— 换出站不需要动桥接这一侧的任何一行。
    IOutboundTcp *socks = nullptr;
    // 下行待写队列。★ 消费端用**偏移游标** `toStackOff`，不用 `remove(0,n)` ——
    //   remove 每次都要 memmove 剩余部分，而拥塞时（设备窗口小 → coast_conn_sndbuf 只给一点
    //   → 反复部分写）它退化成 O(n²)。实测（COAST_GW_BENCH_DIR=down + DEVWND）：
    //     设备窗口 65535 → 19.5 ns/字节
    //     设备窗口  1024 → **309 ns/字节（16 倍）**、38.6 核/Gbps
    //   而这恰好发生在"设备侧慢"——最不该雪上加霜的场景（弱信号 Wi-Fi 设备下载）。
    QByteArray toStack;
    int toStackOff = 0;          // toStack 里已经写进栈的前缀长度
    // 上次**成功把下行字节灌进栈**的时刻。这与 lastActive 不是一回事：lastActive 记的是
    // 「socks 送来数据」，而一旦下行水位顶满、我们停止从 socks 读，它就永远停在那一刻 ——
    // 于是「设备还在不在取数」这件事没有任何字段能回答。清扫器要的正是后者。
    qint64 lastDrainMs = 0;
    int pendingDown() const { return toStack.size() - toStackOff; }
    quint32 pendingRecved = 0;   // 已交给我们、但还没 coast_conn_recved 归还的窗口字节
    bool upThrottled = false;
    bool downPaused = false;
    bool socksClosed = false;
    bool established = false;
    bool stackClosed = false;
};

} // namespace

// 一张网卡的上下文：出帧回调据 nic id 反查到它，取「该从哪个二层端点发出去」和
// 「本机在这张卡上的 MAC」——多网卡就是靠它区分的。
// （lwIP 时代这里还内嵌一个 `struct netif`，要求地址稳定；smoltcp 每张卡一个独立
//   Interface 实例、由 Rust 侧按 nic id 持有，C++ 这边只剩纯数据。）
struct NetStack::Nic {
    IL2Endpoint *ep = nullptr;
    QByteArray localMac6;
    QString localIp, netmask;
    // ★ 这张卡专属的出站工厂：拨**这张卡对应的**核心入站口。
    //   「从哪张网卡出去」在核心里是 listener 的属性（interface-name），所以每张卡一个入站、
    //   拨错口 = 走错网卡。为空 = 沿用 Impl::outFactory（单网卡，与以前一致）。
    OutboundFactory *outFactory = nullptr;
    quint16 socksPort = 0;
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
    // 出站是**接口**：Socks5Udp（经 mihomo 的 UDP associate）与进程内 UDP 出站都实现它。
    // 由 Impl::outFactory 创建 —— 与 TCP 侧同一个工厂，所以「换出站」两条腿一起换。
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
// 合成 fake-ip 应答的 TTL。取小值：fake-ip 与域名的映射是**本进程内**的短期约定，
// 设备缓存太久会在我们重启/换池之后拿着一个已经没人认的假 IP 去连。
constexpr int kFakeIpTtlSec = 60;

// 定时器泵的周期，以及「UDP 流老化」相对它的分频比（详见 NetStack::init）。
// 25ms × 8 = 200ms，老化的实际节奏与 lwIP 时代一致。
constexpr int kPumpIntervalMs = 25;
constexpr int kHousekeepEveryTicks = 8;

// 诊断采样里附带的栈内部量。只有 coaststack 才拿得到，所以由本 TU 拼好字符串交给
// GatewayDiag（它是跨平台 TU，不该碰数据面）。取的都是**这条链路真正会出问题**的量。
// ★ 换栈顺带补上了一件 lwIP 做不到的事：**真正的重传计数**。lwIP 的 struct stats_proto
//   根本没有这个字段（tcp_out.c 那三个重传入口一个计数都不加），当年只能拿 xmit 总数
//   当近似；现在 retransmits 是引擎自己数的实数。
QString smolStatsLine(NetStack::Impl *d);

// 容量兜底：句柄和 mihomo 侧的关联数都是有限资源。BT/DHT 这类应用通常共用一个源端口广撒（对本
// 方案友好），但「源端口也乱换」的极端情况必须挡住，不能让一台设备把整个进程的 fd 吃光。
constexpr int kMaxUdpFlowsPerDevice = 128;
constexpr int kMaxUdpFlowsTotal = 1024;
// 顶到上限时「多久没用才算可以淘汰」。比这更近用过的一律不动 —— 详见 evictOldestFlowOfDevice
// 那段：顶掉活流会让整条 UDP 抖到 0% 成功率。取 1s：正常交互流（QUIC/游戏/RTP）的包间隔远小于它，
// 而真正闲下来的流 1 秒不发包也就等着老化了，早淘汰一点无害。
constexpr qint64 kUdpEvictMinIdleMs = 1000;
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

// 单调毫秒。老化算的是「多久没用」，用 64 位免得为 u32 的 49 天回绕再套一层回绕安全的减法。
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
    // 出站工厂。默认拨 mihomo 的 SOCKS5（= 接线前的行为，零变化）；
    // 上层可用 setOutboundFactory() 换成 CoreDialerFactory 走进程内出站。
    OutboundFactory *outFactory = nullptr;
    // DNS 旁听器（可空）：把核心分配的 fake-ip 反查回域名，供 accept 改写拨号目标。
    // 见 setDnsLearner —— 没有它，域名类流量只能拿着假 IP 去拨，进程内出站必然路由不到。
    std::shared_ptr<DnsResolver> dnsLearner;
    // 进程内 DNS 开关（见 setLocalDnsEnabled）。关 = 老行为：:53 全部转投 mihomo。
    bool localDns = false;
    Socks5OutboundFactory *ownedDefault = nullptr; // 默认工厂，由本对象持有
    // 换过口的旧工厂（见 setNicSocksPort）。留到析构才清 —— 立刻 delete 会踩到还握着它
    // 所造出站对象的活连接。
    QVector<OutboundFactory *> retiredFactories;
    QHash<IL2Endpoint *, Nic *> nics;        // 二层端点 → 该网卡的上下文
    QHash<QString, DeviceInfo> devices;      // 设备 IP（v4 或 v6 串）→ {mac,user}
    QHash<QString, UdpSess *> udp;           // 设备 IP（v4 或 v6 串）→ UDP 会话
    UdpLru udpLruShort;                      // 短档(DNS 类)流的 LRU
    UdpLru udpLruLong;                       // 长档(一般 UDP)流的 LRU
    int udpFlowCount = 0;                    // 全局流数（对上限用，省得遍历）
    // ★ DNS 劫持的**唯一**一个 socket（原来是「每条查询新建一个 QUdpSocket」）。
    //   旧写法在 DNS 洪水下的代价是实测出来的：每条查询 = 1 个 QUdpSocket + 2 个 connect +
    //   1 个 shared_ptr + 1 个 5s singleShot，全压在跑 lwIP 泵的那个工作线程上 ——
    //   经 coast 3047qps/91.5% 成功，而直打 mihomo :1053 是 3693qps/100%，且洪水期并发 TCP
    //   从 ~1200 掉到 ~150 conn/s（**一台设备的 DNS 能拖垮所有设备**）。
    //   现在改成一条常驻 socket + **事务 ID 多路复用**：给每条查询换上我们自己分配的 txid，
    //   回来时按 txid 查上下文、把原始 txid 还原回去再回封给设备（正是 DNS 代理的标准做法）。
    QUdpSocket *dnsSock = nullptr;
    quint16 dnsNextId = 0;
    struct DnsPending {
        QString victimIp;
        quint16 vport = 0;
        QHostAddress origServer; // 设备原本查的那个 DNS，回封时要伪装成它
        bool v6 = false;
        quint16 origId = 0;      // 设备自己的事务 ID，回封前必须还原
        qint64 sentMs = 0;
    };
    QHash<quint16, DnsPending> dnsPending;
    // ———— smoltcp 数据面 ————
    // 只接管 **TCP**：UDP/DNS 那 700 行（UdpFlow/UdpLru/hijackDns/onUdpResponse）与它无关，
    // 那部分从来就没走过任何用户态 TCP 栈，删 lwIP 时一行都没动。
    CoastStack *smol = nullptr;
    QHash<quint64, SmolConn *> smolConns;      // coast conn id → 桥接上下文
    QHash<IL2Endpoint *, quint32> nicIds;      // 二层端点 → coaststack 的 nic id
    QHash<quint32, Nic *> nicById;             // 反查（出帧回调要据 nic id 找端点）
    quint32 nextNicId = 1;
    QElapsedTimer smolClock;                   // 给 coast_stack_poll 的单调毫秒
    // ———— 合并式 poll（见 schedulePoll）————
    // pollScheduled：本轮事件循环已经排过一次 poll，别重复排。
    // pollCount：诊断用，进 smolStatsLine 的 polls= 一栏 —— 它是"提频到底生效没有"的唯一证据。
    bool pollScheduled = false;
    quint64 pollCount = 0;
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
// DNS 在途查询的兜底超时。原来是每条查询挂一个 5s singleShot，现在统一在这里扫 ——
// 少掉「每查询一个定时器」正是去掉 DNS 洪水开销的一部分。
constexpr qint64 kDnsPendingTimeoutMs = 5000;

void reapUdpFlows(NetStack::Impl *d)
{
    const qint64 now = monoMs();
    for (UdpLru *l : {&d->udpLruShort, &d->udpLruLong}) {
        while (l->tail && now - l->tail->lastUsed > l->tail->idleMs)
            destroyUdpFlow(d, l->tail);
    }
    // 超时未应答的 DNS 查询：清掉上下文并记 dnsNoReply（语义与旧的 5s 定时器完全一致 ——
    // 这一栏涨 = 解析在**核心那一侧**就断了，用来把「解析不出来」和「解析出来但连不上」分开）。
    // 200ms 一扫，最坏比 5s 晚一拍，无所谓。
    for (auto it = d->dnsPending.begin(); it != d->dnsPending.end();) {
        if (now - it.value().sentMs > kDnsPendingTimeoutMs) {
            ++GatewayDiag::c.dnsNoReply;
            it = d->dnsPending.erase(it);
        } else {
            ++it;
        }
    }
}

// 淘汰某设备最久未用的一条流 —— **但只淘汰确已空闲的**。返回 false = 一条都不该动。
//
// ★ 为什么不能"满了就顶掉最旧的那条"（LRU），这是实测出来的：设备的并发 UDP 流一旦超过上限，
//   纯 LRU 会**剧烈抖动并把整条 UDP 打瘫**，不是优雅降级。真机（树莓派网关，被接管设备开
//   200 个源端口、上限 128）：10 秒窗口里 `udpNew=24088 udpEvict=23962` —— 每秒建/拆 2400 条流，
//   而每条流都要新建一个 UDP socket **外加一条到 mihomo 的 SOCKS 控制连接**；
//   同时 `rxdrop=6732`、泵开始迟到，**应用侧成功率 0.0%**（发 38400 收 4）。
//   机理是经典的 LRU 抖动：被顶掉的那条流下一个包又要重建，重建又顶掉另一条正在用的，
//   **停摆自我维持**。原注释写的「可能顶掉一条正在用的流（QUIC 会莫名卡住）」低估了后果。
//
//   所以准入策略改成：**宁可拒收新流，也不动正在用的流**。空闲判据取 kUdpEvictMinIdleMs——
//   比它更近用过的就算"活着"。这样最坏情况是「前 128 条流照常工作，第 129 条起的目的地收不到
//   包」：QUIC 会回落 TCP、DNS 会重试，而已经建立的会话一条都不受影响。被拒的次数记进
//   `udpRefuse=`，长期不为零就说明该调大上限，而不是让它静默抖动。
bool evictOldestFlowOfDevice(NetStack::Impl *d, UdpSess *s)
{
    UdpFlow *victim = nullptr;
    for (UdpFlow *f : std::as_const(s->flows)) {
        if (!victim || f->lastUsed < victim->lastUsed)
            victim = f;
    }
    if (!victim)
        return false;
    if (monoMs() - victim->lastUsed < kUdpEvictMinIdleMs)
        return false; // 最旧的那条也还活着 ⇒ 这台设备真有这么多并发流，别动它们
    ++GatewayDiag::c.udpFlowsEvicted;
    destroyUdpFlow(d, victim);
    return true;
}

// 全局最久未用：两条链的链尾里挑更旧的那个，O(1)。同样只淘汰确已空闲的（理由见上）。
bool evictGlobalOldestFlow(NetStack::Impl *d)
{
    UdpFlow *a = d->udpLruShort.tail;
    UdpFlow *b = d->udpLruLong.tail;
    UdpFlow *victim = !a ? b : (!b ? a : (a->lastUsed <= b->lastUsed ? a : b));
    if (!victim)
        return false;
    if (monoMs() - victim->lastUsed < kUdpEvictMinIdleMs)
        return false;
    ++GatewayDiag::c.udpFlowsEvicted; // 撞全局上限，同上
    destroyUdpFlow(d, victim);
    return true;
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

// 线程前提：数据面只跑在一个线程上。全进程可以有多个 NetStack 实例（网关一个、进程内 TUN
// 一个……），但**每个实例都必须由单一线程拥有并调用** —— coaststack.h 的单线程契约是按实例的。
// 在 App 里那个线程是 LanGateway 的工作线程，自测里是主线程，二者从不并存。
//
// ★ 这里曾经有一个进程级的 g_impl 单例。它是 lwIP 的遗产（lwip_init 全局、ARP 表全局、
//   PCB 链全局），随 lwIP 一起删掉了 —— 顺带消掉了「网关占着栈时点『增强』被拒」那个真实故障。
bool g_debug = false;             // COAST_GATEWAY_DEBUG=1 时打诊断日志（自测/联调用）

// ———————————— 归因用的分级短路（COAST_GW_BENCH_STAGE）————————————
//
// ★ 只为**测量**存在，生产恒为 0，代价是每帧一次 int 比较。
//   动机：`COAST_GW_THROUGHPUT` 量出上行是 ~17 ns/字节、几乎全是每字节成本，
//   但那 8 项每字节的活各占多少**没人量过**（见 docs/gateway-bottleneck-audit.md 第九节）。
//   照直觉去改是本仓库反复栽过的错法，所以先做归因：逐级砍掉一段，看 CPU 掉多少。
//     0 = 全路径（默认，生产）
//     1 = 构造 QByteArray 但**不写 socket**（只砍掉 写缓冲 + 内核回环 + 对端读）
//     2 = 连 QByteArray 都不构造（再砍掉这一次分配+拷贝）
//     3 = 不 poll（再砍掉校验和验证 + 收环拷贝 + peek/to_vec + 事件派发）
//     4 = 连 coast_stack_input 都不调（再砍掉 frame.to_vec + portmap 改写）
//   相邻两级之差 = 被砍掉那一段的成本。
//
//   ★ 1 与 2 必须分开：第一版把它们并成一级，于是"回环那一跳"里混进了一次
//     QByteArray 分配+拷贝，占比会被高估。分开之后 0→1 才是干净的"进程边界"成本。
int g_benchStage = 0;

// 把所有网卡攒着的出帧一次性交给驱动（契约与理由见 IL2Endpoint::flushTx）。
// 为什么不挑「这条连接对应的那张卡」：网卡数是个位数、空队列的 flushTx 只是一次判空 ——
// 全刷一遍更简单也更不容易漏。
// **绝对安全**：flushTx 只走到 pcap_sendqueue_transmit，不发任何 Qt 信号，回不到连接上。
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
// 这条链路是「设备 ⇄ smoltcp ⇄ Socks5Tcp(QTcpSocket) ⇄ mihomo」，两头的速率毫不相干。谁慢谁就是
// 瓶颈，而中间的队列如果没有闸，就会替瓶颈**无界地**缓冲——内存一路涨到 OOM。
//
// 【上行 设备→mihomo】栈的接收窗口本来就是现成的闸门：收到设备数据后**先不**调
//   coast_conn_recved，窗口就一直关小着，设备的 TCP 自己会减速。等 QTcpSocket 真把字节交给
//   内核（upstreamBytesWritten）、待发队列降下来了，再一次性把窗口还回去。
//   反例（lwIP 时代的老代码）是无条件立刻全开窗口：等于不停对设备喊「随便发」，而字节全堆在
//   QTcpSocket 的写缓冲里（Qt 的写缓冲没有上限）。
//
// 【下行 mihomo→设备】toStack 是队列：只能按 coast_conn_sndbuf 往栈里灌，灌不进去的留着。
//   且 QTcpSocket 没设 setReadBufferSize，Qt 会一直往上读 → 设备慢（WiFi 信号差）时同样无界。
//   所以超过高水位就让 Socks5Tcp 停止读 socket（读缓冲填满 → 收窗关闭 → 压回远端），
//   降到低水位再恢复。
//
// —— 为什么不会死锁（改这块最容易写出「双方互等」）——
//  上行：只要 pendingRecved>0 且窗口没还，就必然存在「QTcpSocket 里没发完的字节」。目的地是
//    本机回环的 mihomo，内核迟早排空 → upstreamBytesWritten 必然到来 → smolFlushRecvWindow
//    必然把窗口还上。唯一的例外是 socket 出错，而那条路走 failed/closed → smolDestroyConn 把
//    整条连接拆掉，不存在「等一个永远不来的事件」的稳态。另外 c->socks 已经为空时**无条件**
//    归还：没有排空者了，扣着窗口只会让连接永久僵住。
//  下行：恢复读取的触发点是 smolPumpToStack，而它挂在 conn_sent 回调上 —— 只要设备还在 ACK，
//    conn_sent 就会来；设备彻底不响应时栈自己重传超时 → conn_closed → 拆连接。而且恢复是
//    **排队**执行的（Socks5Tcp::setReadPaused），绝不在栈回调里同步重入。
//
// —— 水位取值 —— 高水位 64 KiB、低水位 16 KiB（¼）。
//  低水位的作用是迟滞：免得在水位线上反复 暂停/恢复 抖动（每次恢复都要过一趟事件循环，
//  抖起来纯属浪费）。上行同理：queued 掉到 16 KiB 才还窗口，一次还一大块。
//  ★ 这两个数**故意不跟着**栈的单连接预算走：这里的队列是 QByteArray / QTcpSocket 读缓冲，
//    落在**普通堆**上、按连接数线性膨胀（2048 条 × 128 KiB = 256 MiB 的病态上限）。
//    吞吐上也不需要更大 —— toStack 只需覆盖「栈腾出空间 → 我们补上」这一次事件循环的往返。
//  （名字里的 kToLwip* 是 lwIP 时代留下的，换栈时**故意没改**：它们的含义、取值、以及上面
//    这段论证一个字都没变，改名只会让 git blame 断掉。）
constexpr int kUpQueueHighWater = 64 * 1024;
constexpr int kUpQueueLowWater = 16 * 1024;
constexpr int kToLwipHighWater = 64 * 1024;
constexpr int kToLwipLowWater = 16 * 1024;

// ————————————— 诊断：把 coaststack 的计数器读出来 —————————————
//
// 对应 lwIP 时代的 lwipStatsLine() + pollLwipPoolStats()。那一版要盯 9 个内存池的
// used/max/err，还得配一套 30s 节流的告警 —— 因为 lwIP 的池子耗尽是**静默**的：既不打日志
// 也不向上层报错，tcp_alloc 甚至会悄悄杀掉一条活着的连接，不主动去读就等于没开统计。
// smoltcp 没有固定池（连接表是 Vec、缓冲区堆分配），「池见底 → 静默杀连接」这一整类故障
// 结构性地不存在，所以这里只报真正有用的量，那两百行监视代码一并作废。
//
// 保留的两条**必须有等价物**（换栈计划里的硬要求）：
//   · refuse     —— 对应 lwIP 的 prioKill：资源不够导致「无声故障」的唯一可见信号。
//   · pollGapMax —— 对应 fastGapMax：泵的最坏间隔。平均值会把毛刺抹平，而延迟 ACK/重传
//     等的就是下一拍，**最坏间隔才决定性**。瞬时量，读完清零（引擎侧清）。
// ★ rexmit 是 lwIP 从来给不出的（struct stats_proto 根本没有重传字段，tcp_out.c 那三个
//   重传入口一个计数都不加），当年只能拿 xmit 总数当近似 —— 换栈顺手补上了真数。
QString smolStatsLine(NetStack::Impl *d)
{
    if (!d || !d->smol)
        return QString();
    CoastStats s {};
    coast_stack_stats(d->smol, &s);

    // 累计量报**窗口增量**（与 GatewayDiag 其余字段一致），瞬时量原样报。
    static CoastStats prev {};
    // polls= 是「提频到底生效没有」的唯一证据：只挂定时器时它恒等于窗口内的拍数
    // （10s 窗口 = 400）；接上 schedulePoll 之后应随流量显著高于它。
    const quint64 polls = d->pollCount;
    d->pollCount = 0;
    QString out = QStringLiteral("polls=%8 stackRx=%1 stackTx=%2 stackDrop=%3 rexmit=%4 refuse=%5"
                                 " conns=%6 pollGapMax=%7ms rxOvf=%9")
                      .arg(s.rx_frames - prev.rx_frames)
                      .arg(s.tx_frames - prev.tx_frames)
                      .arg(s.rx_dropped - prev.rx_dropped)
                      .arg(s.retransmits - prev.retransmits)
                      .arg(s.conns_refused - prev.conns_refused)
                      .arg(s.conns_active)
                      .arg(s.poll_max_gap_ms)
                      .arg(polls)
                      .arg(s.rx_overflow - prev.rx_overflow);

    // ★ 僵尸判据。`conns=` 只说「有多少条」，说不了「它们还活着吗」。真机上见过 92 条挂着
    //   不掉、而设备几乎不发包的情形 —— 光看 conns 分不出是「设备真开着这么多」还是
    //   「对端早没了、两侧都没人关」。idle5m 就是后者的数量，idleMaxS 是最老那条的年纪。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    int idle5m = 0;
    qint64 idleMaxMs = 0;
    for (SmolConn *c : d->smolConns) {
        if (!c || c->lastActive == 0)
            continue;
        const qint64 idle = nowMs - c->lastActive;
        idleMaxMs = qMax(idleMaxMs, idle);
        if (idle > 5 * 60 * 1000)
            ++idle5m;
    }
    out += QStringLiteral(" idle5m=%1 idleMaxS=%2").arg(idle5m).arg(idleMaxMs / 1000);

    prev = s;
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
    // 也不在 smolPumpToStack 里，不在这里收口就要等泵那一拍（最多 25 ms）才发出去，DNS 会直接慢一档。
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
    // 也不在 smolPumpToStack 里，不在这里收口就要等泵那一拍（最多 25 ms）才发出去，DNS 会直接慢一档。
    s->nic->ep->flushTx();
}

} // namespace


// ═══════════════════════ smoltcp 数据面桥接 ═══════════════════════
//
// 这里**没有**任何「回调里对象可能被同步销毁」的防护，是刻意的：coaststack 是 poll 模型 ——
// 回调在 coast_stack_poll 内部同步发出，且引擎保证发回调时不持有自身借用，所以回调里可以
// 安全地反过来调 coast_*（含当场 abort）。lwIP 那边为此写了 ~60 行 ConnWatch/g_destroyedConn/
// needsAbortReturn，还得在每个 lwIP 回调的返回值上区分 ERR_ABRT 与 ERR_OK；漏一处的后果是
// tcp_receive 对已释放的 pcb 撞断言 → **整个进程 abort、所有被代理设备一起断网**（真机复现过）。
// 这一整类问题随 lwIP 一起没了。
namespace {

void smolDestroyConn(SmolConn *c, bool abortIt);

// ———————————————————— 推进协议栈 + 收口出帧 ————————————————————
void pollStack(NetStack::Impl *d)
{
    if (!d || !d->smol || g_benchStage >= 3)
        return;
    ++d->pollCount;
    coast_stack_poll(d->smol, static_cast<quint64>(d->smolClock.elapsed()));
    flushNicTx(d);
}

// 排一次「本轮事件循环结束时」的 poll。
//
// ★ 这是整条数据面上最要紧的一处。coast_stack_input() / coast_conn_send() / coast_conn_recved()
//   **都只动缓冲、不推进协议栈** —— 真正解析、生成 ACK、吐出帧的只有 coast_stack_poll()。
//   而它原来只挂在 25 ms 的定时器上，于是整条数据面被量化到 40 Hz：
//     · 每个 RTT 平白多 ~25 ms（TLS 握手要过好几个来回）
//     · 单连接吞吐 = 窗口 ÷ 25 ms ≈ **21 Mb/s**，上下行都是（实测三种模型收敛，
//       见 docs/gateway-bottleneck-audit.md）
//   这不是 lwIP 时代就有的毛病：lwIP 的 netif->input() 是**同步**的，喂一帧当场走完
//   ip_input → tcp_input → 回调，没有任何量化。换栈时把同步模型换成 poll 模型，
//   却把 poll 留在了定时器上 —— 这是换栈引入的回归。
//
// 为什么不在 inputFrame 里直接 poll：一次 poll 要遍历全部连接（pump_conns/drain_tx），
// 逐帧调在 65k pps 下就是每秒几万次全表扫描。排一个 queued 调用，可以把**本轮事件循环里
// 到达的所有帧、所有 socket 回调**合并成一次 poll —— 既拿到亚毫秒延迟，又保持每轮一次的成本。
//
// 生命周期：queued 调用投给 d->owner（NetStack 本身）。QObject 析构时 Qt 会把投给它的
// 未决事件一并移除；而 ~NetStack 里 `delete d` 与 ~QObject 之间不会回事件循环，
// 所以不存在「d 已删但回调还在路上」的窗口。
void schedulePoll(NetStack::Impl *d)
{
    if (!d || !d->smol || d->pollScheduled)
        return;
    d->pollScheduled = true;
    QMetaObject::invokeMethod(
        d->owner,
        [d] {
            d->pollScheduled = false;
            pollStack(d);
        },
        Qt::QueuedConnection);
}

// 上行背压：与 lwIP 侧同一套高低水位迟滞（kUpQueueHighWater/kUpQueueLowWater），
// 保证换栈前后设备侧观感一致。
void smolFlushRecvWindow(SmolConn *c)
{
    if (!c || !c->impl || !c->impl->smol || c->pendingRecved == 0)
        return;
    // socks 没了 → 无条件归还，否则窗口永远关着 = 设备卡死（lwIP 侧同款兜底）
    //
    // ★ 「没了」必须**同时包含「连接已关」**，不能只判指针空 —— 这里原先写的是 `c->socks`，
    //   而 c->socks 只在 smolDestroyConn 里才被置空；上游正常关闭走的是 closed 信号，那里
    //   只置 socksClosed、指针照旧非空。于是这条兜底**从来没有在它专门为之而写的场景里生效过**。
    //
    //   漏掉的后果是一条会永久滞留的连接：上游关闭时若 socket 写缓冲里还压着超过水位的字节
    //   （对端在我们把请求发完之前就关了 —— HTTP keep-alive 复用的竞争里很常见），这里会照旧
    //   return 扣住窗口；而那些字节**永远不可能再写出去**（连接已经没了），于是 pendingRecved
    //   永远不为 0 → smolPumpToStack 末尾的 coast_conn_close 因 peeked != 0 恒返回
    //   COAST_ERR_STATE → stackClosed 永远是 false。更糟的是此后**没有任何事件会再驱动重试**：
    //   socket 已关，不会再有 dataReceived / upstreamBytesWritten；pendingDown() 是 0，也不会
    //   有 conn_sent。连接就此挂死，**设备侧收不到 FIN**，它以为这条连接还能用，下次 keep-alive
    //   复用就打进黑洞，直到应用自己超时。
    //
    //   真机佐证（2026-08-05，四台被代理设备）：栈里 conns 稳定在 230~305，而同一时刻核心侧的
    //   设备连接只有 ~50 —— 四倍多的差额挂在我们这一侧。
    //
    //   归还窗口在这里是安全的：连接马上就要关（调用方是 flush-then-close），设备即便再多发
    //   一点也只是被丢弃，而这远好过让它永远等一个不会到来的 FIN。
    const bool socksGone = !c->socks || c->socksClosed;
    const qint64 queued = socksGone ? 0 : c->socks->bytesToWrite();
    const qint64 limit = c->upThrottled ? kUpQueueLowWater : kUpQueueHighWater;
    if (!socksGone && queued > limit) {
        if (!c->upThrottled) {
            c->upThrottled = true;
            ++GatewayDiag::c.upThrottleHits;
        }
        return; // 扣住窗口 —— 这正是闸门
    }
    c->upThrottled = false;
    const quint32 give = c->pendingRecved;
    c->pendingRecved = 0;
    coast_conn_recved(c->impl->smol, c->id, give);
    // 还窗口只是改了缓冲状态；**窗口更新 ACK 要等 poll 才发得出去**，
    // 而它正是设备上行能不能继续发的唯一许可 —— 不排这一次就要等定时器那一拍。
    schedulePoll(c->impl);
}

// 下行：把 socks 来的字节写进栈（受发送缓冲余量限流），并按水位暂停/恢复读取。
// 返回 true = 连接已被销毁（调用方必须立刻停手）。
bool smolPumpToStack(SmolConn *c)
{
    if (!c || !c->impl || !c->impl->smol)
        return false;
    bool wroteAny = false;
    while (c->pendingDown() > 0) {
        const int room = coast_conn_sndbuf(c->impl->smol, c->id);
        if (room < 0) {
            smolDestroyConn(c, true);
            return true;
        }
        if (room == 0)
            break; // 等 conn_sent 回调再续
        const int n = qMin<int>(room, c->pendingDown());
        const int wrote =
            coast_conn_send(c->impl->smol, c->id,
                            reinterpret_cast<const uint8_t *>(c->toStack.constData())
                                + c->toStackOff,
                            static_cast<size_t>(n));
        if (wrote < 0) {
            smolDestroyConn(c, true);
            return true;
        }
        if (wrote == 0)
            break;
        c->toStackOff += wrote;
        wroteAny = true;
    }
    if (wroteAny)
        c->lastDrainMs = QDateTime::currentMSecsSinceEpoch();
    // 排空即清零；否则等前缀攒够一批再压缩一次 —— 摊还 O(1)，且内存不会无界增长。
    if (c->pendingDown() <= 0) {
        c->toStack.clear();
        c->toStackOff = 0;
    } else if (c->toStackOff >= kToLwipHighWater) {
        c->toStack.remove(0, c->toStackOff);
        c->toStackOff = 0;
    }
    // coast_conn_send 只是把字节塞进发送缓冲，**成帧要等 poll**。
    // 不排这一次，下行数据最多躺 25 ms 才上线。
    if (wroteAny)
        schedulePoll(c->impl);

    // 下行水位：顶到高水位让 socks 停读，回落到低水位再放开（与 lwIP 侧同参数）
    if (c->socks) {
        const int q = c->pendingDown();
        if (!c->downPaused && q >= kToLwipHighWater) {
            c->downPaused = true;
            ++GatewayDiag::c.downPauseHits;
            c->socks->setReadPaused(true);
        } else if (c->downPaused && q <= kToLwipLowWater) {
            c->downPaused = false;
            c->socks->setReadPaused(false);
        }
    }

    // socks 已关且下行排空 → 优雅关。
    // ★ 必须**先把窗口还满**：coast_conn_close 在窗口未还满时返回 COAST_ERR_STATE。
    //   lwIP 那边同样的情形是静默退化成 RST（设备"下载到一半被断"），这里是显式失败，
    //   所以这两步的顺序不能反。
    if (c->socksClosed && c->pendingDown() <= 0 && !c->stackClosed) {
        smolFlushRecvWindow(c);
        if (coast_conn_close(c->impl->smol, c->id) == COAST_OK) {
            c->stackClosed = true;
            ++GatewayDiag::c.tcpClosed;
        }
    }
    return false;
}

void smolDestroyConn(SmolConn *c, bool abortIt)
{
    if (!c || !c->impl)
        return;
    NetStack::Impl *d = c->impl;
    d->smolConns.remove(c->id);
    if (c->socks) {
        // 先断信号：closeTunnel 会同步 emit closed()，带着已死的 c 回来（lwIP 侧同款教训）
        QObject::disconnect(c->socks, nullptr, nullptr, nullptr);
        c->socks->closeTunnel();
        c->socks->deleteLater();
        c->socks = nullptr;
    }
    if (d->smol && !c->stackClosed) {
        if (abortIt) {
            coast_conn_abort(d->smol, c->id);
            ++GatewayDiag::c.tcpAborted;
        } else {
            coast_conn_close(d->smol, c->id);
            ++GatewayDiag::c.tcpClosed;
        }
    }
    delete c;
}

// 定期清扫「上游已关、却再没人来关它」的连接。
//
// ★ 这**不是**空闲回收器，空闲也**不是**判据 —— 见 SmolConn::lastActive 的论证：RFC 5382
//   REQ-5 要求已建立的 TCP 映射不短于 2 小时 4 分，按空闲砍会掐断推送/IMAP 这类合法长连接。
//   这里只碰 `socksClosed` 的连接，也就是**上游已经确认关闭、不可能再有字节**的那些。
//
// ★ 为什么需要它：优雅关的判定写在 smolPumpToStack 里，而那个函数只被两件事驱动 ——
//   socks 有新数据、或设备回 ACK。socks 关闭那一刻若 `pendingDown() > 0`（下行还没灌进栈），
//   就只能等设备 ACK 来推；设备若从此不再 ACK（弱信号、休眠、走开了），这条连接就
//   **永远没人再看它一眼**，coast_conn_close 永远发不出去。
//   实测：一台机器 50 分钟里 conns 从 0 单调涨到 254，而核心侧同时只有 46 条 —— 那 208 条
//   就是这么漏的，每条占 RX_BUF+TX_BUF = 256 KiB。堆满之后新连接建不起来，症状是
//   **「单流下载正常，一上并发就全灭」**（测速软件和 fast.com 正是几十条并发，
//   所以它们「根本测不了」，而看视频却没事）。
// 上游已关但下行排不空 → 宽限这么久再中止。可用 COAST_GW_DEADGRACE_MS 覆盖：
// 自测要把它压到毫秒级，否则一条用例得干等一分钟（同 COAST_GW_TXBATCH 那套做法）。
qint64 deadConnGraceMs()
{
    static const qint64 v = qEnvironmentVariableIsSet("COAST_GW_DEADGRACE_MS")
            ? qgetenv("COAST_GW_DEADGRACE_MS").toLongLong()
            : 60 * 1000;
    return v > 0 ? v : 60 * 1000;
}

void reapDeadTcpConns(NetStack::Impl *d)
{
    if (!d || !d->smol || d->smolConns.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // ★ 先收集再销毁：smolDestroyConn 会 d->smolConns.remove()，边遍历边删是未定义行为。
    QList<SmolConn *> drained, stuck;
    if (qEnvironmentVariableIsSet("COAST_REAP_DEBUG")) {
        std::fprintf(stderr, "[reap] 扫描 %d 条:", int(d->smolConns.size()));
        for (SmolConn *c : std::as_const(d->smolConns))
            std::fprintf(stderr,
                         " {id=%llu socksClosed=%d stackClosed=%d pend=%d idle=%lldms drainAge=%lldms}",
                         (unsigned long long)c->id, int(c->socksClosed), int(c->stackClosed),
                         c->pendingDown(), (long long)(c->lastActive ? now - c->lastActive : -1),
                         (long long)(c->lastDrainMs ? now - c->lastDrainMs : -1));
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
    for (SmolConn *c : std::as_const(d->smolConns)) {
        if (!c || c->stackClosed)
            continue;
        // ★ 判据**不能**用 socksClosed：设备不排空 → downPaused → 我们停止从 socks 读 →
        //   对端的 FIN 永远排在未读数据后面进不来，于是恰恰是会泄漏的那批连接，
        //   socksClosed 永远是 false（自测实测 socksClosed=0 / pend=65536 / idle 2.7s 仍在涨）。
        //   真正的不变量是：**我们攥着送不出去的字节，而设备已经很久没取了**。
        //   这不违反 RFC 5382 —— 对端只要还在 ACK，lastDrainMs 就一直在走；
        //   整整一分钟一个字节都推不进去的连接，TCP 自己的重传也快放弃了。
        if (c->pendingDown() > 0 && c->lastDrainMs > 0
            && now - c->lastDrainMs > deadConnGraceMs()) {
            stuck.append(c);
            continue;
        }
        if (!c->socksClosed)
            continue;
        if (c->pendingDown() <= 0)
            drained.append(c);
    }
    // 下行已排空 → 走与 smolPumpToStack 里**同一套**优雅关：必须先还满窗口，
    // 否则 coast_conn_close 以 COAST_ERR_STATE 失败（顺序不能反）。
    for (SmolConn *c : drained) {
        smolFlushRecvWindow(c);
        if (coast_conn_close(d->smol, c->id) == COAST_OK) {
            c->stackClosed = true;
            ++GatewayDiag::c.tcpClosed;
            ++GatewayDiag::c.tcpReaped;
        }
    }
    // 上游早没了、设备也不再取数 → 只能 RST 收场。留着它既占 256 KiB，
    // 也占着一个再不会有人读的 socket。
    for (SmolConn *c : stuck) {
        ++GatewayDiag::c.tcpReaped;
        smolDestroyConn(c, true);
    }
}

QString addrToString(const CoastAddr *a)
{
    if (!a)
        return QString();
    if (a->is_v6) {
        Q_IPV6ADDR raw;
        memcpy(raw.c, a->bytes, 16);
        return QHostAddress(raw).toString();
    }
    quint32 v4 = (quint32(a->bytes[0]) << 24) | (quint32(a->bytes[1]) << 16)
        | (quint32(a->bytes[2]) << 8) | quint32(a->bytes[3]);
    return QHostAddress(v4).toString();
}

// ———————————————————————— coaststack 的五个回调 ————————————————————————

void smolOutFrame(void *user, CoastNicId nic, const uint8_t *frame, size_t len)
{
    auto *d = static_cast<NetStack::Impl *>(user);
    if (!d || !frame || len < 14)
        return;
    NetStack::Nic *n = d->nicById.value(nic, nullptr);
    if (!n || !n->ep)
        return;
    // 必须是自有内存的 QByteArray：端点可能把它排进积压队列（IL2Endpoint.h 的硬契约）
    n->ep->send(QByteArray(reinterpret_cast<const char *>(frame), static_cast<int>(len)));
}

// 这条连接该用哪个出站工厂：
//   CoastCore 装了全局工厂 → 用它（它自己按设备绑网卡，不经核心）；
//   否则优先**这张卡专属**的（拨该卡对应的核心入站，出口网卡由那个入站的 interface-name 决定）；
//   都没有 → 全局默认（单网卡，与以前一致）。
OutboundFactory *outFactoryFor(NetStack::Impl *d, NetStack::Nic *n)
{
    if (!d)
        return nullptr;
    // ★ 判据是**意图**（这个工厂自己管绑卡吗），不是对象身份。
    //   以前写的是 `outFactory != ownedDefault`，而关闭 CoastCore 的分支会装一个全新的
    //   Socks5OutboundFactory —— 它同样 != ownedDefault，于是下面两条永远走不到，所有
    //   设备一律拨基准端口。副卡上的设备就拨到一个没人监听的口上，socksFail 100%。
    if (d->outFactory && d->outFactory->bindsInterfaceItself())
        return d->outFactory; // CoastCore：进程内出站，自己管绑卡
    if (n && n->outFactory)
        return n->outFactory; // 这张卡专属：拨该卡对应的核心入站
    return d->outFactory;
}

bool smolConnNew(void *user, CoastConnId id, CoastNicId nic, const CoastAddr *src, uint16_t sport,
                 const CoastAddr *dst, uint16_t dport)
{
    Q_UNUSED(sport);
    auto *d = static_cast<NetStack::Impl *>(user);
    if (!d || !src || !dst)
        return false;

    const QString victimIp = addrToString(src); // 连接对端 = 被劫持设备
    const QString serverIp = addrToString(dst); // 设备原本想访问的公网地址
    const QString socksUser = d->userForIp(victimIp);
    if (socksUser.isEmpty()) {
        // 不认识的设备：与 lwIP 侧一致，拒绝而不是拿空用户名去拨（那会认证失败）
        return false;
    }

    auto *c = new SmolConn;
    c->impl = d;
    c->id = id;
    // ★ 经工厂创建：默认工厂返回 Socks5Tcp（拨 mihomo），换成 CoreDialerFactory 就走进程内出站。
    OutboundFactory *fac = outFactoryFor(d, d->nicById.value(nic, nullptr));
    c->socks = fac ? fac->createTcp(d->owner) : nullptr;
    if (!c->socks) {
        d->smolConns.remove(id);
        delete c;
        return false;
    }
    c->lastActive = QDateTime::currentMSecsSinceEpoch();
    d->smolConns.insert(id, c);
    ++GatewayDiag::c.tcpAccepted;

    QObject::connect(c->socks, &IOutboundTcp::established, d->owner, [c]() {
        c->established = true;
        smolFlushRecvWindow(c);
    });
    QObject::connect(c->socks, &IOutboundTcp::upstreamBytesWritten, d->owner,
                     [c]() { smolFlushRecvWindow(c); });
    QObject::connect(c->socks, &IOutboundTcp::dataReceived, d->owner, [c](const QByteArray &b) {
        c->lastActive = QDateTime::currentMSecsSinceEpoch();
        c->toStack.append(b);
        smolPumpToStack(c);
    });
    QObject::connect(c->socks, &IOutboundTcp::failed, d->owner, [c](const QString &why) {
        ++GatewayDiag::c.socksFailed;
        // ★ **原因不能丢**，而且必须写进 gateway-diag.log。以前这里是 `[c](const QString &)`
        //   把原因整个扔掉，于是 `socksFail=17` 这个数字之外一无所有 —— 真机上排查「副卡
        //   设备一条连接都没有」时，日志里没有任何东西指向「拨的是 7899、那儿没人听」。
        //   也**不能用 qWarning**：GUI 子系统的程序在 Windows 上那是 OutputDebugString，
        //   文件里同样看不到，等于白写。限频在 note 里做。
        GatewayDiag::note("socksFail", QStringLiteral("出站拨号失败 — %1").arg(why));
        smolDestroyConn(c, true);
    });
    QObject::connect(c->socks, &IOutboundTcp::closed, d->owner, [c]() {
        c->socksClosed = true;
        smolPumpToStack(c);
    });

    // ★ fake-ip → 域名改写。设备连的是核心分配的**假 IP**，出站若原样拨它必然路由不到
    //   （节点侧 i/o timeout）。旁听 DNS 应答学到的映射能把它还原成域名：进程内出站按域名
    //   拨得通；即便回退核心，给域名也比给假 IP 更利于规则匹配。
    //   学不到（映射过期/被清/根本不是 fake-ip）就照原样用 IP —— 与没有这段时完全一致。
    QString dialHost = serverIp;
    if (d->dnsLearner) {
        const QHostAddress dstAddr(serverIp);
        if (!dstAddr.isNull() && DnsResolver::isFakeIp(dstAddr)) {
            // 两张不同的表都要问：domainForFake 是我们自己分配的（进程内 DNS 模式），
            // domainForLearnedIp 是从核心的应答里旁听学来的。同时开着时两者都可能命中。
            QString learned = d->dnsLearner->domainForFake(dstAddr);
            if (learned.isEmpty())
                learned = d->dnsLearner->domainForLearnedIp(dstAddr);
            if (!learned.isEmpty()) {
                dialHost = learned;
                ++GatewayDiag::c.dnsFakeIpResolved;
            }
        }
    }
    c->socks->connectTo(dialHost, dport, socksUser);
    return true;
}

void smolConnData(void *user, CoastConnId id, const uint8_t *data, size_t len)
{
    auto *d = static_cast<NetStack::Impl *>(user);
    if (!d || !data || len == 0)
        return;
    SmolConn *c = d->smolConns.value(id, nullptr);
    if (!c)
        return;
    // ★ 先记账、后落地、最后才可能归还 —— 与 lwIP 侧同一顺序。
    //   记账必须在前：write() 可能同步走到 upstreamBytesWritten → flushRecvWindow。
    c->pendingRecved += static_cast<quint32>(len);
    if (c->socks && g_benchStage < 2) {
        // STAGE=1：仍然付这次分配+拷贝，但不交给 socket ——
        // 这样 0→1 之差才是纯粹的"跨进程边界"成本，不含 QByteArray 那一份。
        QByteArray b(reinterpret_cast<const char *>(data), static_cast<int>(len));
        if (g_benchStage < 1)
            c->socks->write(b);
    }
    smolFlushRecvWindow(c);
}

void smolConnSent(void *user, CoastConnId id, uint32_t n)
{
    Q_UNUSED(n);
    auto *d = static_cast<NetStack::Impl *>(user);
    if (!d)
        return;
    if (SmolConn *c = d->smolConns.value(id, nullptr))
        smolPumpToStack(c);
}

void smolConnClosed(void *user, CoastConnId id, bool isAbort)
{
    auto *d = static_cast<NetStack::Impl *>(user);
    if (!d)
        return;
    if (SmolConn *c = d->smolConns.value(id, nullptr)) {
        c->stackClosed = true; // 栈侧已经没了，别再回头调 coast_conn_*
        smolDestroyConn(c, false);
    }
    if (isAbort)
        ++GatewayDiag::c.tcpAborted;
}

} // namespace

// ———————————————————————————— NetStack ————————————————————————————
NetStack::NetStack(quint16 socksPort, QObject *parent)
    : QObject(parent), d(new Impl)
{
    d->socksPort = socksPort;
    d->owner = this;
    // 默认出站 = 拨 mihomo 的 SOCKS5。**必须在这里就位**：init() 之前 addDevice/inputFrame
    // 理论上进不来，但留一个空工厂等于把"没设工厂"变成运行期空指针，不如默认就是对的。
    d->ownedDefault = new Socks5OutboundFactory(socksPort);
    d->outFactory = d->ownedDefault;
}

NetStack::~NetStack()
{
    qDeleteAll(d->retiredFactories);
    d->retiredFactories.clear();

    // 诊断日志收尾：写掉最后一个采样窗口 + 一条停机标记。放在这里而不是 LanGateway::disableAll，
    // 是因为**本析构在工作线程上跑**（见 LanGateway_linux.cpp 的线程模型），与所有 sample() 调用
    // 同线程 —— GatewayDiag 的单线程前提得以保持。此刻 d->timer 还没被 delete d 干掉，但我们已经
    // 不再回事件循环了，不会有第二个 sample 并发进来。
    // 注意：进程被直接杀掉时（见 main_qml.cpp 里关于 aboutToQuit 跑不到的说明）这里不会执行，
    // 最多丢最后一个未满 10s 的窗口 —— 可以接受，不为它加复杂度。
    if (d->inited)
        GatewayDiag::flush(smolStatsLine(d), "stop");

    for (UdpSess *s : std::as_const(d->udp))
        destroyUdpSess(d, s);
    d->udp.clear();
    // ★ 顺序：先拆桥接连接 → 再释放栈 → 最后删 Nic。三步都不能换位：
    //   · coast_stack_free 只是 Drop 整个引擎，**不发** conn_closed 回调（ffi.rs 就是一句
    //     Box::from_raw + drop）。所以 SmolConn 和它挂着的 Socks5Tcp 必须由这里主动收掉，
    //     否则每次关网关都泄漏一批连接对象。
    //   · 拆连接要回头调 coast_conn_abort，所以必须在 free **之前**；
    //   · 出帧回调要按 nic id 查 Nic*，所以 Nic 必须活到 free **之后**。
    const QList<SmolConn *> conns = d->smolConns.values();
    for (SmolConn *c : conns)
        smolDestroyConn(c, true); // 进程/网关要停了，没有可优雅关闭的对端
    d->smolConns.clear();
    if (d->smol) {
        coast_stack_free(d->smol);
        d->smol = nullptr;
    }
    for (Nic *n : d->nics)
        delete n;
    if (d->outFactory && d->outFactory != d->ownedDefault)
        delete d->outFactory;
    delete d->ownedDefault;
    delete d;
}

void NetStack::setDnsLearner(std::shared_ptr<DnsResolver> learner)
{
    d->dnsLearner = std::move(learner);
}

void coastSetBenchStage(int stage)
{
    g_benchStage = stage;
    if (stage > 0)
        qWarning("[NetStack] ★ 归因短路已开启 STAGE=%d —— 这不是正常模式", stage);
}

void NetStack::setOutboundFactory(OutboundFactory *f)
{
    // ★ **取得所有权**：旧工厂在这里 delete。调用方每次换出站都 new 一个新的，
    //   不必自己管旧的 —— 否则每次热切换（改设置/换模式）都漏一个工厂。
    //   传 nullptr = 回到默认的 SOCKS5（拨 mihomo）。
    if (f == d->outFactory)
        return;
    OutboundFactory *old = d->outFactory;
    d->outFactory = f ? f : static_cast<OutboundFactory *>(d->ownedDefault);
    if (old && old != d->ownedDefault)
        delete old;
}

const char *NetStack::activeTcpStack() const
{
    // lwIP 移除后只剩一条路，但这个方法**不删**：自测和诊断日志都据它证明"跑的是哪条数据面"，
    // 而 init() 失败时 d->smol 为空 —— 那时报 "none" 比报一个不存在的栈诚实。
    return d->smol ? "smoltcp" : "none";
}

bool NetStack::init(QString *err)
{
    if (d->inited)
        return true;
    // ★ 这里曾经有一条「全进程只能有一个 NetStack」的判重。**随 lwIP 一起作废了。**
    //   当年它不是防御性代码而是硬约束：lwip_init() 是全局的，NO_SYS 下整个进程只有一份
    //   协议栈、ARP 表与 PCB 链，两个 NetStack 从构造上不可能共存。
    //   代价是真实用户故障：局域网网关占着栈时点「增强」（进程内 TUN）必然被拒
    //   —— 报的就是那句"已有一个网关协议栈实例在运行"。
    //   smoltcp 每个实例自带独立的 Interface/SocketSet（coast_stack_new），没有任何全局状态，
    //   所以这条约束**不再需要**。诊断行也已改成按实例取（smolStatsLine(d)），不再走全局。
    //   ⇒ 换栈顺带把那个故障的根因消掉了。
    g_debug = qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG");


    // ———— 建栈 ————
    // catch-all（接住发往**任意** IP:port 的 SYN）由 Rust 侧实现：smoltcp 的 any_ip + 一层
    // phy 端口改写 shim。lwIP 时代这件事要在 vendored 源码上打三个补丁（ip4.c/ip6.c 的
    // accept-all + tcp_in.c 的通配端口），补丁没了，监听器/backlog/`local_port = 0` 这一整段
    // 也跟着没了 —— 引擎内部自带监听，这里只管建。
    CoastCallbacks cb {};
    cb.out_frame = smolOutFrame;
    cb.conn_new = smolConnNew;
    cb.conn_data = smolConnData;
    cb.conn_sent = smolConnSent;
    cb.conn_closed = smolConnClosed;
    d->smol = coast_stack_new(&cb, d);
    if (!d->smol) {
        if (err)
            *err = QStringLiteral("coaststack 初始化失败");
        return false;
    }
    d->smolClock.start();
    // ★ 归因用的关校验和开关**必须在这里**（coast_stack_new 之后、addNic 之前）：
    //   smoltcp 的 Interface::new 会把 device capabilities 拷进 InterfaceInner 缓存，
    //   建完网卡再改是无效的。第一版放在 addNic 之后，于是开关看着开了、实际没生效，
    //   据此量出的「校验和不要钱」是假结论 —— 靠 Rust 侧那条证伪测试才发现。
    if (qEnvironmentVariableIsSet("COAST_GW_BENCH_NOCKSUM")) {
        coast_stack_debug_skip_rx_checksum(d->smol, true);
        qWarning("[NetStack] ★ 收包校验和验证已关闭 —— 仅供测量，绝不可用于生产");
    }
    qInfo("[NetStack] TCP 数据面 = smoltcp (%s)", coast_stack_version());

    // 栈的定时器泵（TCP 重传/延迟 ACK/TIME_WAIT 回收）+ 收帧排空兜底 + UDP 流老化。
    //
    // ★ 25ms 这个值是 lwIP 时代量出来的，换栈后**保持不变**（引擎的 poll 语义与 lwIP 的
    //   sys_check_timeouts 同类：都是"到点才干活，没到点空转"）。当年的教训值得留着：
    //   泵周期必须**明显细于**栈自己的 TCP 定时器周期，否则每一拍都要等下一次泵才跑得到，
    //   实际周期被拉长到两者的公倍数量级 —— 老值 200ms 配 lwIP 默认的 250ms 就是这个毛病：
    //   延迟 ACK 实际约 400ms 一次、**重传**约 600ms 一次，本机→设备方向一旦丢一帧，恢复要
    //   大半秒起步。被代理设备的表现是「访问什么都慢、偶尔打不开」，且与目标在国内还是国外无关。
    //   coast_stack_poll 会返回建议的下次延迟，将来要自适应就从那里接（现在固定周期够用）。
    // 成本：空转一拍就是「读一次时钟 + 比一次链表头」，40 次/秒可以忽略；且这个定时器只在网关
    //   开着时存在，跑在 LanGateway 的工作线程上，不碰 GUI 线程。
    // 老化**不跟着提频**：它本来就是 200ms 一次的量级，没必要 8 倍频，按拍数分频即可。
    d->timer = new QTimer(this);
    d->timer->setInterval(kPumpIntervalMs);
    // ★ 必须显式 PreciseTimer —— 默认的 Qt::CoarseTimer 在 **Windows** 上把这个泵毁掉两次：
    //   1) 粒度：CoarseTimer 走 SetTimer/WM_TIMER，受系统时钟节拍（默认 15.6ms）约束，25ms 实际
    //      变成 31.25ms。真机 gateway-diag.log 里 6033 个采样窗口有 5933 个泵周期正好是 31ms
    //      —— 不是负载，是纯粹的量化误差，空闲时也一样。
    //   2) 饥饿：WM_TIMER 是**队列空时才合成**的最低优先级消息。数据面忙起来（Npcap 收帧事件 +
    //      上百条到 mihomo 的 socket 通知挤满消息队列）时它会被无限期推后 —— 同一份日志里，设备
    //      下行 >600 帧/秒的窗口泵周期滑到 45ms、26% 的拍迟到 2 倍以上、最坏一次迟到 631ms。
    //      泵一停就是栈的重传/延迟 ACK 全停，对外表现是所有被代理设备同时卡住半秒。
    //   PreciseTimer 在 Qt 的 Windows 事件分发器里走 timeSetEvent（多媒体定时器）：既不受 15.6ms
    //   节拍限制，也不再是 WM_TIMER，因而不被消息队列里的收帧/socket 事件饿死。
    //   linux/mac 上两种类型都是 timerfd/kqueue，本行无副作用。
    d->timer->setTimerType(Qt::PreciseTimer);
    d->pumpClock.start();
    connect(d->timer, &QTimer::timeout, this, [this] {
        // ★ 泵的迟到量 = 工作线程的饱和度。这一拍本该 kPumpIntervalMs 之后就到，迟到多少就
        //   说明上一拍的活（收帧 → 栈 → SOCKS 读写）占了多少额外时间。这是**唯一**能把
        //   「链路丢包」和「本机算不过来」分开的指标：前者只涨 txdrop/rxdrop，后者只涨这里。
        const qint64 elapsed = d->pumpClock.restart();
        const qint64 lag = elapsed - kPumpIntervalMs;
        ++GatewayDiag::c.pumpTicks;
        if (elapsed > 2 * kPumpIntervalMs) {
            ++GatewayDiag::c.pumpLateTicks;
            if (lag > GatewayDiag::c.pumpMaxLagMs)
                GatewayDiag::c.pumpMaxLagMs = lag;
        }

        // ★ 收帧排空兜底（Windows 才有实际动作，见 IL2Endpoint::drainNow 的注释）。
        //   放在 coast_stack_poll() **之前**：先把已经到达的帧喂进栈，再跑它的定时器 ——
        //   否则新到的 ACK 要多等一整拍才被栈看到，等于白排空。
        //   先快照 key 再遍历：排空会同步触发 frameReceived → inputFrame，虽然帧处理路径不会
        //   增删网卡，但直接在 QHash 上边遍历边回调是自找麻烦。网卡数是个位数，这份拷贝可忽略。
        if (!d->nics.isEmpty()) {
            const QList<IL2Endpoint *> eps = d->nics.keys();
            for (IL2Endpoint *ep : eps) {
                if (ep && d->nics.contains(ep)) // 上一张卡的回调万一把这张摘了，这里挡一下
                    ep->drainNow();
            }
        }

        // ★ 这里是**定时器兜底**，不再是主路。
        //   主路是 schedulePoll()：喂帧 / 写下行 / 还窗口都会排一次「本轮事件循环末尾」的
        //   poll，延迟亚毫秒。本拍只负责**没有外部事件驱动**的活：重传、延迟 ACK、
        //   TIME_WAIT 回收、ARP/NDP 周期投毒 —— 它们只能靠时间推进。
        //   同时它是安全网：任何漏排 schedulePoll 的新路径，最迟 25 ms 也会被这里收掉。
        pollStack(d);
        if (++d->pumpTick >= kHousekeepEveryTicks) {
            d->pumpTick = 0;
            reapUdpFlows(d);
            reapDeadTcpConns(d); // TCP 侧的同款兜底：优雅关只在泵里判，泵不来就漏
        }
        // 诊断采样：按墙钟判间隔（不按拍数——泵一旦迟到，拍数和真实时间就对不上了）。
        if (GatewayDiag::enabled()) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - d->lastDiagMs >= GatewayDiag::sampleIntervalMs()) {
                d->lastDiagMs = now;
                GatewayDiag::sample(smolStatsLine(d));
            }
        }
    });
    d->timer->start();

    d->inited = true;
    return true;
}

bool NetStack::setNicSocksPort(IL2Endpoint *ep, quint16 socksPort)
{
    if (!ep)
        return false;
    Nic *nic = d->nics.value(ep, nullptr);
    if (!nic || nic->socksPort == socksPort)
        return nic != nullptr; // 没这张卡 = 失败；没变化 = 什么都不用做
    nic->socksPort = socksPort;
    // ★ 旧工厂**不能立刻 delete**：可能还有连接握着它造出来的出站对象。挪进墓地，
    //   随 Impl 一起销毁。换口只发生在网卡增减时（人手插拔/禁用），墓地长不大。
    if (nic->outFactory)
        d->retiredFactories.append(nic->outFactory);
    nic->outFactory = (socksPort != 0 && socksPort != d->socksPort)
                              ? new Socks5OutboundFactory(socksPort)
                              : nullptr;
    return true;
}

bool NetStack::addNic(IL2Endpoint *ep, const QByteArray &localMac6, const QString &localIp,
                      const QString &netmask, quint16 socksPort, QString *err)
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

    // 用**本机在这张卡上的真实地址/掩码**，不能给 0.0.0.1 // 0.0.0.0 这类占位：栈据它判断
    // 「哪些目的是本网段的」，掩码给错会让多网卡场景下 B 网段设备的回包从 A 网卡发出去。
    // 校验放在这里而不是丢给 Rust：QHostAddress 的报错能直接告诉用户是哪一个字段不合法。
    const QHostAddress ipAddr(localIp);
    if (ipAddr.protocol() != QAbstractSocket::IPv4Protocol || ipAddr.toIPv4Address() == 0) {
        if (err)
            *err = QStringLiteral("本机 IP 非法: ") + localIp;
        return false;
    }
    const QHostAddress maskAddr(netmask);
    if (maskAddr.protocol() != QAbstractSocket::IPv4Protocol || maskAddr.toIPv4Address() == 0) {
        if (err)
            *err = QStringLiteral("子网掩码非法: ") + netmask;
        return false;
    }

    auto *nic = new Nic;
    nic->socksPort = socksPort;
    // 只有「给了口、且和全局那个不同」时才建专属工厂；否则沿用全局的（省一个对象，也让
    // 单网卡路径与以前逐字相同）。CoastCore 装了自己的工厂时由 outFactoryFor 让它优先。
    if (socksPort != 0 && socksPort != d->socksPort)
        nic->outFactory = new Socks5OutboundFactory(socksPort);
    nic->ep = ep;
    nic->localMac6 = localMac6;
    nic->localIp = localIp;
    nic->netmask = netmask;

    const quint32 ipHost = ipAddr.toIPv4Address();   // QHostAddress 给的是**主机序**
    CoastAddr a {};
    a.is_v6 = false;
    a.bytes[0] = uchar((ipHost >> 24) & 0xFF);       // CoastAddr.bytes 是网络序
    a.bytes[1] = uchar((ipHost >> 16) & 0xFF);
    a.bytes[2] = uchar((ipHost >> 8) & 0xFF);
    a.bytes[3] = uchar(ipHost & 0xFF);
    // 掩码 → 前缀长度。数的是**前导 1 的个数**，遇到第一个非全 1 字节就停 —— 不连续的掩码
    // （255.0.255.0 这种病态输入）按其前缀部分处理，不会数出个虚高的值。
    quint8 prefix = 0;
    {
        const quint32 m = maskAddr.toIPv4Address();
        for (int bit = 31; bit >= 0 && ((m >> bit) & 1u); --bit)
            ++prefix;
    }

    const quint32 nid = d->nextNicId++;
    if (coast_stack_add_nic(d->smol, nid,
                            reinterpret_cast<const uint8_t *>(nic->localMac6.constData()),
                            &a, prefix) != COAST_OK) {
        if (err)
            *err = QStringLiteral("coaststack add_nic 失败");
        delete nic;
        return false;
    }
    d->nics.insert(ep, nic);
    d->nicIds.insert(ep, nid);
    d->nicById.insert(nid, nic);
    return true;
}

void NetStack::removeNic(IL2Endpoint *ep)
{
    Nic *nic = d->nics.take(ep);
    if (!nic)
        return;
    // 先从栈里摘掉这张卡：它会连带拆掉卡上的连接（发 conn_closed → smolDestroyConn），
    // 而那些回调要按 nic id 查 nicById，所以反查表得留到摘完再清。
    const quint32 nid = d->nicIds.take(ep);
    if (d->smol && nid)
        coast_stack_remove_nic(d->smol, nid);
    d->nicById.remove(nid);
    delete nic->outFactory;
    nic->outFactory = nullptr;
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

void NetStack::addDevice(const QString &ip, const QByteArray &mac6, const QString &socksUser,
                         bool reject)
{
    if (ip.isEmpty() || mac6.size() != 6)
        return;
    d->devices.insert(ip, DeviceInfo{mac6, socksUser, reject});
    // ★ 这里曾经还要 etharp_add_static_entry 预置一条静态 ARP，否则 lwIP 回包给设备时会去
    //   发 ARP 请求（而 ARP 帧被投毒器截走、根本进不了栈，请求永远等不到应答 → 回程黑洞）。
    //   coaststack 改成**从设备发来的帧里自学 (src MAC, src IP)** 并合成一条 ARP 应答喂给
    //   引擎（engine.rs 的 learn_neighbor），所以这里不用也不该再注入 —— 少一份要和真实
    //   拓扑保持同步的状态。v6 同理，见 addDeviceV6。
}

// ★ 关掉所有「设备侧 = ip」的 TCP 连接。**换址/移除设备时必须调**，否则旧地址的连接成孤儿：
//   设备换到新 IP 后再也不对它们发包、靶机回包又发往旧地址收不到，于是这些连接实质永久占着
//   （lwIP 时代的 established 超时是 24 小时；真机实测 40 条连接换址后静置 60s，PCB 卡在 43
//   纹丝不动 —— 反复换址 DHCP 续约 / Wi-Fi 漫游就是慢性泄漏）。换栈不改变这个必要性。
// ip 可以是 v4 或 v6 串，两族都必须处理：catch-all 是双栈的，设备的 v6 TCP 连接同样会成孤儿。
static void closeDeviceConns(NetStack::Impl *d, const QString &ip)
{
    if (!d || !d->smol)
        return;
    const QHostAddress a(ip);
    CoastAddr dev {};
    if (a.protocol() == QAbstractSocket::IPv6Protocol) {
        dev.is_v6 = true;
        const Q_IPV6ADDR raw = a.toIPv6Address();
        memcpy(dev.bytes, raw.c, 16);
    } else if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 v4 = a.toIPv4Address();
        dev.bytes[0] = uchar((v4 >> 24) & 0xFF);
        dev.bytes[1] = uchar((v4 >> 16) & 0xFF);
        dev.bytes[2] = uchar((v4 >> 8) & 0xFF);
        dev.bytes[3] = uchar(v4 & 0xFF);
    } else {
        return; // 解析不出来的地址串：什么都不做，别拿一个全零地址去批量关连接
    }
    // 引擎内部按设备地址整批关，每关一条发一次 conn_closed → smolDestroyConn 收桥接侧。
    // lwIP 那边这件事要遍历 tcp_active_pcbs（还得引 priv 头），且**每关一条就得重扫一遍链表**
    // ——因为 closeConn 会同步触发别的连接的槽、连锁 delete 掉已经收集好的裸指针，先收集后批关
    // 是 use-after-free。回调模型没有这个问题，那段 O(n²) 的重扫循环一并作废。
    coast_stack_close_device_conns(d->smol, &dev);
}

void NetStack::removeDevice(const QString &ip)
{
    d->devices.remove(ip);
    closeDeviceConns(d, ip); // 先把该 IP 的 TCP 连接关掉，别让它们成孤儿泄漏
    if (auto *s = d->udp.take(ip))
        destroyUdpSess(d, s);
}

void NetStack::addDeviceV6(IL2Endpoint *from, const QString &ip6, const QByteArray &mac6,
                           const QString &socksUser, bool reject)
{
    Q_UNUSED(from); // 邻居/路由由引擎从实帧自学，不再需要「装到哪个 netif 上」
    if (ip6.isEmpty() || mac6.size() != 6)
        return;
    // 与 v4 共用 devices 表：key 是 v6 地址串，和 v4 的点分串天然不冲突。smolConnNew 里
    // userForIp(victimIp) 用的就是这张表，v6 连接的 victimIp 是 v6 串，正好命中。
    d->devices.insert(ip6, DeviceInfo{mac6, socksUser, reject});
    // ★ v6 这边曾经要 nd6_add_static_neighbor_entry 预置静态邻居。现在由引擎自学，而且它做的
    //   **不止**是邻居：还要给设备的全局 v6 加一条 /128 直连路由。这条是靠 smoltcp 的 net_trace
    //   抓出来的 —— 邻居注入一直是好的，坏的是路由：设备的全局 v6 相对本机唯一的 v6 地址
    //   （EUI-64 链路本地）是 off-link，回包走默认路由、下一跳解析到本机自己的链路本地，
    //   于是永远只发 NS。在此之前靠猜试了四种邻居注入方式，全是白费。
}

void NetStack::removeDeviceV6(const QString &ip6)
{
    d->devices.remove(ip6);
    closeDeviceConns(d, ip6); // 同 v4：关掉该 v6 地址的 TCP 连接，别让它成孤儿
    if (auto *s = d->udp.take(ip6))
        destroyUdpSess(d, s);
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

    // ARP/NDP 不进栈（投毒由 Arp/NdpSpoofer 负责，避免栈误答）。LanGateway 已在上游把
    // ARP 与 ICMPv6-NDP(NS/NA/RS/RA) 截走不喂进来；这里只按 ethertype 分 v4/v6 两条数据路径。
    //
    // ★ 分流只有三个去向：**UDP → 手工 NAT，TCP → 栈，其余一律丢**。
    //   lwIP 时代还有第三条「非 UDP 一股脑喂进去，让 lwIP 自己挑」的兜底，顺带回应 ICMP echo。
    //   现在没有了：coaststack 只实现 TCP（coaststack.h「职责边界」一节），喂别的协议进去只会
    //   让它 rx_dropped++。丢掉 ICMP 的后果是被劫持设备**ping 不通公网**（它的 ping 本来也只是
    //   由我们代答、并不真的探测远端可达性，误导性大于价值）；真要恢复，该做的是在这里手工
    //   构造 echo reply，而不是把一个通用栈塞回数据面。
    if (ethType == 0x86DD) { // IPv6
        if (frame.size() < 14 + 40)
            return;
        // NDP 从不带扩展头；这里只看紧邻的 next header。带扩展头的（罕见）next header 不是 6/17，
        // 会落到下面的丢弃分支 —— 可接受（TCP 被 MSS 钳住不分片、UDP 无扩展头）。
        const quint8 nexthdr = f[14 + 6];
        if (nexthdr == 17) { // UDP：手工拦截转发（含 DNS）
            handleUdpFrame6(nic, frame);
            return;
        }
        if (nexthdr != 6)
            return;
    } else if (ethType == 0x0800) { // IPv4
        if (frame.size() < 14 + 20)
            return;
        const uchar *ip = f + 14;
        const quint8 proto = ip[9];
        if (proto == 17) { // UDP：手工拦截转发（含 DNS）
            handleUdpFrame(nic, frame, (ip[0] & 0x0F) * 4);
            return;
        }
        if (proto != 6)
            return;
    } else {
        return; // 非 IP
    }

    // 到这里必定是 v4 或 v6 的 TCP 帧 —— 交给栈。
    if (g_benchStage >= 4)
        return; // 归因用：连喂帧都不做，量的是夹具自身的底噪
    const quint32 nid = d->nicIds.value(from, 0);
    if (nid) {
        coast_stack_input(d->smol, nid, reinterpret_cast<const uint8_t *>(frame.constData()),
                          static_cast<size_t>(frame.size()));
        // ★ 喂帧只是**入队**，解析与回 ACK 都要等 poll。排一次「本轮事件循环末尾」的 poll，
        //   把这一批帧合并成一次处理 —— 延迟从最坏 25 ms 降到亚毫秒，成本仍是每轮一次。
        schedulePoll(d);
    }
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
    // ★ 「禁网」设备的 UDP（含被劫持的 :53）在这里就丢掉 —— 到了核心已经分不出是谁了。
    if (dev.reject) {
        ++GatewayDiag::c.udpFlowsRefused;
        return;
    }
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
        // 进程内 DNS 开着就当场答（fake-ip）；判不了/没开则交回老路（转投 mihomo）。
        if (!answerDnsLocally(srcIp, sport, QHostAddress(dstIpV4), payload, false))
            hijackDns(srcIp, sport, QHostAddress(dstIpV4), payload, false);
        return;
    }

    // 一个设备源端口一条流（= 一个独立 Socks5Udp）。回程靠「从哪条关联进来」定位设备端口，
    // 不再靠 (dstIp,dport) 反查——那正是老代码串包的根因，见文件上方的方案说明。
    UdpFlow *flow = s->flows.value(sport);
    if (!flow) {
        reapUdpFlows(d); // 建流前顺手回收：突发 DNS 场景下这一步就够把上一波流还回去了
        // 顶到上限时只淘汰**确已空闲**的流；一条都腾不出来就拒收这条新流（丢这个包），
        // 绝不顶掉正在用的 —— 那会让整条 UDP 抖到 0%，见 evictOldestFlowOfDevice 的说明。
        if (s->flows.size() >= kMaxUdpFlowsPerDevice && !evictOldestFlowOfDevice(d, s)) {
            ++GatewayDiag::c.udpFlowsRefused;
            return;
        }
        while (d->udpFlowCount >= kMaxUdpFlowsTotal
               && (d->udpLruShort.tail || d->udpLruLong.tail)) {
            if (!evictGlobalOldestFlow(d)) {
                ++GatewayDiag::c.udpFlowsRefused;
                return;
            }
        }

        ++GatewayDiag::c.udpFlowsCreated;
        flow = new UdpFlow;
        flow->sess = s;
        flow->vport = sport;
        flow->idleMs = isShortLivedUdpPort(dport) ? kUdpDnsIdleMs : kUdpIdleMs;
        // 经工厂创建：默认返回 Socks5Udp（拨 mihomo），装了 CoreDialerFactory 就是进程内 UDP 出站。
        {
            OutboundFactory *fac = outFactoryFor(d, s->nic);
            flow->socks = fac ? fac->createUdp(this) : nullptr;
        }
        if (!flow->socks) {
            // 工厂拒绝（协议没有 UDP 出站实现且严格模式）→ 这条流建不起来，丢包即可（UDP 语义）
            ++GatewayDiag::c.udpFlowsRefused;
            s->flows.remove(sport);
            d->udpFlowCount--;
            delete flow;
            return;
        }
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
    // 同 v4：禁网设备的 UDP 在网关层丢弃（见 DeviceInfo::reject 的论证）。
    if (dev.reject) {
        ++GatewayDiag::c.udpFlowsRefused;
        return;
    }
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
        // 同 v4：先试进程内当场答，判不了/没开再交回老路。
        if (!answerDnsLocally(srcIp, sport, dstAddr, payload, true))
            hijackDns(srcIp, sport, dstAddr, payload, true);
        return;
    }

    UdpFlow *flow = s->flows.value(sport);
    if (!flow) {
        reapUdpFlows(d);
        // 顶到上限时只淘汰**确已空闲**的流；一条都腾不出来就拒收这条新流（丢这个包），
        // 绝不顶掉正在用的 —— 那会让整条 UDP 抖到 0%，见 evictOldestFlowOfDevice 的说明。
        if (s->flows.size() >= kMaxUdpFlowsPerDevice && !evictOldestFlowOfDevice(d, s)) {
            ++GatewayDiag::c.udpFlowsRefused;
            return;
        }
        while (d->udpFlowCount >= kMaxUdpFlowsTotal
               && (d->udpLruShort.tail || d->udpLruLong.tail)) {
            if (!evictGlobalOldestFlow(d)) {
                ++GatewayDiag::c.udpFlowsRefused;
                return;
            }
        }

        ++GatewayDiag::c.udpFlowsCreated;
        flow = new UdpFlow;
        flow->sess = s;
        flow->vport = sport;
        flow->idleMs = isShortLivedUdpPort(dport) ? kUdpDnsIdleMs : kUdpIdleMs;
        // v6 的来源校验：peers 用的是 QSet<quint32>（v4 地址），装不下 v6。直接退化成全锥
        //（不校验来源），记录在案的取舍——v6 UDP（QUIC/DNS64 等）以此为代价换实现简单。
        flow->coneOpen = true;
        // 经工厂创建：默认返回 Socks5Udp（拨 mihomo），装了 CoreDialerFactory 就是进程内 UDP 出站。
        {
            OutboundFactory *fac = outFactoryFor(d, s->nic);
            flow->socks = fac ? fac->createUdp(this) : nullptr;
        }
        if (!flow->socks) {
            // 工厂拒绝（协议没有 UDP 出站实现且严格模式）→ 这条流建不起来，丢包即可（UDP 语义）
            ++GatewayDiag::c.udpFlowsRefused;
            s->flows.remove(sport);
            d->udpFlowCount--;
            delete flow;
            return;
        }
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
    if (query.size() < 2) // 连事务 ID 都没有，不是合法 DNS 报文
        return;

    // 首次用到时建那条常驻 socket，并**只**接一次 readyRead。
    if (!d->dnsSock) {
        d->dnsSock = new QUdpSocket(this);
        // 不 bind 具体端口：writeDatagram 会自动绑一个临时端口，回包也从那里进来。
        connect(d->dnsSock, &QUdpSocket::readyRead, this, [this]() { onDnsResponse(); });
    }

    // 分配一个当前没被占用的 txid。空间 65536，而 5s 内的在途量级最多上万，找得到空位；
    // 万一真的绕了一整圈都占着（异常情况），就放弃这条查询而不是覆盖掉别人的上下文。
    quint16 id = 0;
    bool got = false;
    for (int i = 0; i < 65536; ++i) {
        const quint16 cand = d->dnsNextId++;
        if (!d->dnsPending.contains(cand)) {
            id = cand;
            got = true;
            break;
        }
    }
    if (!got) {
        // 单独一栏：这条查询**没发出去**，与「发出去了但核心没回」是两回事，处置也相反。
        ++GatewayDiag::c.dnsNoId;
        return;
    }

    Impl::DnsPending p;
    p.victimIp = victimIp;
    p.vport = vport;
    p.origServer = origServer;
    p.v6 = v6;
    p.origId = (quint8(query[0]) << 8) | quint8(query[1]);
    p.sentMs = monoMs();
    d->dnsPending.insert(id, p);

    // 换上我们的 txid 再发给 mihomo（只动前两字节，其余原样透传）。
    QByteArray q = query;
    q[0] = char((id >> 8) & 0xFF);
    q[1] = char(id & 0xFF);
    d->dnsSock->writeDatagram(q, QHostAddress(QStringLiteral("127.0.0.1")), kDnsHijackPort);
}

// mihomo 的 DNS 应答回来了：按 txid 找回上下文，还原设备原始 txid，回封给设备。
// 一次 readyRead 可能攒了多条，全部排空 —— 洪水下这里是热路径，别一条一条来。
void NetStack::onDnsResponse()
{
    while (d->dnsSock && d->dnsSock->hasPendingDatagrams()) {
        QByteArray resp;
        resp.resize(int(d->dnsSock->pendingDatagramSize()));
        const qint64 n = d->dnsSock->readDatagram(resp.data(), resp.size());
        if (n < 0)
            break;
        resp.truncate(int(n));
        if (resp.size() < 2)
            continue;
        const quint16 id = (quint8(resp[0]) << 8) | quint8(resp[1]);
        const auto it = d->dnsPending.constFind(id);
        if (it == d->dnsPending.constEnd())
            continue; // 迟到的重复应答 / 已被超时回收：丢掉即可
        const Impl::DnsPending p = it.value();
        d->dnsPending.erase(d->dnsPending.find(id));
        // 还原设备自己的事务 ID —— 否则设备的解析器认不出这是它那条查询的应答。
        resp[0] = char((p.origId >> 8) & 0xFF);
        resp[1] = char(p.origId & 0xFF);
        // 设备可能中途被摘除，按 victimIp **重新查**会话（防悬垂），与旧实现一致。
        UdpSess *s = d->udp.value(p.victimIp);
        if (s && s->nic) {
            if (p.v6)
                sendUdpResponse6(s, p.vport, p.origServer, 53, resp);
            else
                sendUdpResponse4(s, p.vport, p.origServer, 53, resp);
        }
    }
}

// ———————————————— 进程内 DNS：本地当场合成 fake-ip 应答 ————————————————
//
// 返回 true = 已经处理完（答了或明确决定不答），调用方不要再走 hijackDns。
// 返回 false = 没开 / 判不了 → 交给老路（转投 mihomo 的 fake-ip DNS）。
//
// ★ 与上游那版的**唯一实质差异**：不自带 forwardDns。上游对"不该 fake"的查询是
//   每条新建一个 QUdpSocket + 一个 5s singleShot 直发上游；而 v1.1 早在 DNS 洪水那次
//   把这套换成了**共享 socket + 事务 ID 多路复用**（见 Impl::dnsSock / dnsPending 的说明：
//   旧写法在洪水下把一台设备的 DNS 变成了拖垮所有设备的放大器）。
//   所以这里判不了就**返回 false 交回 hijackDns**，复用那条已经加固过的路径 ——
//   照抄上游等于把修好的坑重新挖开。
bool NetStack::answerDnsLocally(const QString &victimIp, quint16 vport,
                                const QHostAddress &origServer, const QByteArray &query, bool v6)
{
    if (!d->localDns || !d->dnsLearner)
        return false; // 没开 / 没装解析器 → 老路

    const coastcore::DnsQuestion q = coastcore::parseDnsQuestion(query);
    // 只对「公网域名的 A/AAAA、IN 类」合成假 IP。其余（PTR/SRV/内网名/非 IN 类…）一律交回老路：
    // 合成一个我们答不准的记录，比让它照旧走核心糟得多。
    const bool fakeable = q.ok && q.qclass == 1
            && (q.qtype == coastcore::kDnsTypeA || q.qtype == coastcore::kDnsTypeAAAA);
    if (!fakeable)
        return false;

    QByteArray resp;
    if (q.qtype == coastcore::kDnsTypeA) {
        const QHostAddress fake = d->dnsLearner->fakeFor(q.qname);
        if (fake.isNull())
            return false; // 池满等异常 → 保守交回老路
        resp = coastcore::buildDnsAnswerA(query, q, fake, kFakeIpTtlSec);
    } else {
        // AAAA 一律回 NODATA（不是 NXDOMAIN）：fake-ip 池是 v4 的，给 AAAA 回空答案
        // 会让双栈设备走 v4 —— 正是我们要的。回 NXDOMAIN 则会让它认为整个域名不存在。
        resp = coastcore::buildDnsNoData(query, q, kFakeIpTtlSec);
    }
    if (resp.isEmpty())
        return false; // 合成失败（理论上不会）→ 保守交回老路

    UdpSess *s = d->udp.value(victimIp);
    if (!s || !s->nic)
        return true; // 设备刚被摘掉：查询丢弃即可（UDP 语义），别再往下走
    ++GatewayDiag::c.dnsLocalFake;
    // 回封的源地址必须伪装成**设备原本查的那台 DNS**，否则设备不认这个应答。
    if (v6)
        sendUdpResponse6(s, vport, origServer, 53, resp);
    else
        sendUdpResponse4(s, vport, origServer, 53, resp);
    return true;
}

void NetStack::setLocalDnsEnabled(bool on)
{
    // 开着时设备的 :53 由我们当场答（fake-ip），不再转投 mihomo 的 fake-ip DNS。
    // 这是「网关整条数据面不依赖核心」的最后一环 —— 在此之前即便出站走了进程内，
    // DNS 仍然恒走核心，开关**名不副实**。关掉立刻回到老行为，可热切换。
    d->localDns = on;
}
