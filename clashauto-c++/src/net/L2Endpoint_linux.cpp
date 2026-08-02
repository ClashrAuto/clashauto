// IL2Endpoint 的 Linux 后端 —— AF_PACKET(SOCK_RAW) 二层收发。
//
// 为什么 AF_PACKET：透明网关要在以太层「窃听 + 注入」被劫持设备的完整帧（含 14 字节以太头），
// 普通 socket 拿不到二层。绑定到指定网卡后，内核把该口收到的每个帧原样交给我们（ETH_P_ALL），
// 我们再决定终结/转发；发帧则由调用方自填 dst/src MAC + ethertype，内核不改。
//
// 为什么用 QSocketNotifier 而非线程轮询：契约要求 frameReceived() 在 Qt 事件循环线程发出，
// 且不得阻塞。把 fd 设成非阻塞 + Notifier(Read)，就绪回调里一次把内核那边攒下的帧全吃掉——
// 零轮询、零额外线程。
//
// ——————————————————— 收方：当前走逐帧 recvfrom；mmap 环形缓冲**已实现但关着** ————————————
//
// ★ 结论先行：环的代码完整保留在下面（`COAST_ENABLE_RX_RING`），但**默认关**，理由是真机量出来的
//   ——TPACKET_v3 的块超时给单条往返加 ~15ms，TPACKET_v2 的定长槽装不下 GRO 合并的大帧、把
//   **上行打到不足 1 MB/s**。逐帧 recvfrom 是唯一全维度正确的，代价只有 ~8% 连接速率。
//   完整的三方对照数据与机理见下面 `COAST_ENABLE_RX_RING` 那段注释 —— 想把环打开的人务必先读它。
//
// 下面这段是当初做环的动机，仍然成立（只是被上面那两个坑压过）：
// 老写法是「每帧一次 recvfrom + 每帧一个 QByteArray」。83k pps 量级下，两笔开销都和整个用户态
// 协议栈处理同量级：
//   · syscall：每帧一次 recvfrom ≈ 一次进出内核（现在还叠着 spectre/meltdown 的缓解代价）；
//   · 堆分配：每帧一个引用计数块 + memcpy。
// 换成 AF_PACKET 的 TPACKET_v2 收环之后：
//   · 内核把帧直接码进一块和我们共享的 mmap 区（定长槽、逐帧就绪）。一次 poll 唤醒把当前所有
//     就绪槽全吃掉 → **一次唤醒一批**（其实连 syscall 都没有：读环是纯内存访问，唯一的系统
//     调用是 Qt 事件循环那次 poll），而不是每帧一次 recvfrom。
//   · 为什么不用看起来更"新"的 TPACKET_v3：它按**块**交付，轻载下每个包要等块超时（在
//     CONFIG_HZ=250 的内核上被取整到 4ms），实测给单条往返凭空加了 ~15ms。详见下方 RX 环参数
//     那一段的实测账。
//   · 帧内容就在环里，用 QByteArray::fromRawData 指过去，**入站方向零堆分配、零拷贝**。
//     这依赖 IL2Endpoint::frameReceived 的零拷贝契约（帧仅在槽内有效）+ 必须是**直连**，
//     见 IL2Endpoint.h 的信号注释；跨线程队列连接会让指针在投递后失效。
//
// **回退路径**：建环要一大块连续内核内存。setsockopt(PACKET_VERSION)
// / setsockopt(PACKET_RX_RING) / mmap 任何一步失败，都原样退回逐帧 recvfrom（drainSocket），
// 功能完全不变、只是少了这层优化。老到连头文件都不认识 TPACKET_v2 的构建环境，则整段代码被
// COAST_HAVE_TPACKET_V2 编译期摘掉。CI 要出 x64 和 arm64 两个 Linux 包、用户内核版本不可控，
// 这条回退不能省。
//
// ——————————————————— 发方：加大 SO_SNDBUF + 积压队列（别再静默丢帧）———————————————————
//
// 收方做到了零拷贝批处理，发方却长期是「一次 ::sendto，失败就 return false」——而这个 false
// 一路无人理会：NetStack 的 lwipLinkOutput 忽略返回值、恒回 ERR_OK，于是 lwIP 认为帧已发出。
// 结果是**本机→设备方向的静默丢包**，而且完全不可见（LINK_STATS 也是关的）。
//
// sendto 什么时候会失败：AF_PACKET 的发方走 sock_alloc_send_skb，按 SO_SNDBUF 记账，且记的是
// skb 的 truesize（比帧长大不少）。默认 SO_SNDBUF 取自 net.core.wmem_default（多数发行版
// 212992 B），折算下来只排得下几十个满长帧 —— lwIP 一条连接的拥塞窗口就能填满（TCP_SND_BUF
// 是 128 KiB），网卡/qdisc 一旦短时拥塞（WiFi 客户端慢、千兆口突发）就立刻 ENOBUFS/EAGAIN。
//
// 丢了之后的代价被 lwIP 的定时器粒度放大：重传由 tcp_slowtmr 驱动，恢复要几百毫秒起步。
// 表现就是「被代理设备访问什么都慢、偶尔打不开」——**与目标在国内还是国外无关**，因为丢的是
// 本机→设备这条所有流量共用的腿。
//
// 所以发方补两层：
//   1) SO_SNDBUF 抬到 kTxSndBufBytes（优先 SO_SNDBUFFORCE，网关本就以 root 跑，可越过
//      net.core.wmem_max 的钳制）；
//   2) 仍然 EAGAIN/ENOBUFS 时把帧**排进积压队列**，挂 QSocketNotifier(Write) 等可写再发，
//      而不是丢掉。队列有字节上限，真顶满了才丢并计数（那时链路确实喂不进去，继续囤只会变成
//      缓冲膨胀，交给 lwIP 重传更划算）。
//   3) 丢帧（发方队列溢出 / 收方内核环溢出）一律计数并节流打日志 —— 这类故障不可见才是最贵的。
//
// 权限：需要 CAP_NET_RAW / root，否则 socket() 直接 EPERM。open() 失败时置 *err 并清理半开 fd。
//
// 本文件在所有平台都参与编译：非 Linux 时只保留一个返回 nullptr 的工厂（LanGateway 据此判不可用）。
#include "IL2Endpoint.h"

#if defined(Q_OS_LINUX)

#include "GatewayDiag.h" // 数据面计数（热路径上就是一条 inc，见该头文件的开销说明）

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>     // mmap/munmap（RX 环）
#include <net/if.h>
#include <net/ethernet.h> // ETH_P_ALL / ETH_ALEN（内部再引 <linux/if_ether.h>，有 UAPI 守卫防冲突）
// ★ 内核 UAPI 头放在 glibc 网络头之后引，且用 <linux/if_packet.h> 而**不是** glibc 的
//   <netpacket/packet.h>：两者都定义 struct sockaddr_ll / struct packet_mreq，彼此没有 UAPI 守卫，
//   同时包含会直接「redefinition of struct sockaddr_ll」。TPACKET 环的那套结构
//   （tpacket_req / tpacket2_hdr）只有内核头里有，所以只能选它——
//   sockaddr_ll 与 PACKET_* 常量它同样提供，是 netpacket/packet.h 的超集。
//   （libpcap 的 Linux 后端同样只包含 <linux/if_packet.h>。）
#include <linux/if_packet.h>
#include <linux/filter.h> // sock_filter/sock_fprog + BPF_STMT/BPF_JUMP（SO_ATTACH_FILTER 的 cBPF）
#include <arpa/inet.h>    // htons
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <deque>
#include <vector>

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QSocketNotifier>
#include <QVector>

// SOL_PACKET 正常由 glibc 的 <sys/socket.h>(bits/socket.h) 提供；个别精简 libc 没有，兜一个。
#ifndef SOL_PACKET
#define SOL_PACKET 263
#endif

// ★★ RX 环当前**关着**，走逐帧 recvfrom —— 这是量出来的选择，不是没做完。
//
// 三种收包方式在同一台子上逐一实测（树莓派网关，被接管设备跑批量传输 + 连接速率两类负载）：
//
//                     上行(设备→外网)   下行     连接速率    轻载单条延迟
//   TPACKET_v3          正常           正常     1245/s      16.1 ms   ← 块超时，延迟不可接受
//   TPACKET_v2        **0.5~2 MB/s**   114MB/s  1192/s       2.09 ms  ← 上行崩了
//   逐帧 recvfrom       118.6 MB/s     115MB/s  1095/s       2.18 ms  ← 全维度正确
//
// **V2 为什么会把上行打瘫**：它的槽是**定长**的（tp_frame_size），而 Linux 的 **GRO 默认开着**
// （`ethtool -k eth0` 里 generic-receive-offload: on），入站方向内核会把多个 MTU 段**合并成
// 远超 MTU 的大帧**再交给 ptype_all —— 真机抓到的就是 7300 字节（5×1460）的帧。这种帧塞不进
// 2048 的槽，直接丢。设备侧的表现是：大段全丢、只有小段过得去，于是它按 RTO 退避重传
// （抓包实测 +201ms、+408ms），吞吐塌到不足 1 MB/s。而 V3 是变长紧凑排布、块 64 KiB，装得下，
// 所以这个坑只在 V2 上有。**下行完全正常**，因为那是发方，不经收环 —— 只测下行会漏掉它。
//
// 那为什么不把槽放大到能装 GRO 帧？GRO 帧上限是 64 KiB，槽要按最坏情况定长 ⇒ 同样的 1 MiB
// 预算只剩 16 个槽，缓冲深度反而不够；试过 16 KiB 的折中，实测直接把进程搞崩（未深究）。
//
// 逐帧 recvfrom 没有定长槽这回事，GRO 大帧原样收得到，全维度正确，只损失 ~8% 连接速率
// （1192 → 1095/s）—— 而那 8% 换的是「批量上传能不能用」。要把环加回来，先解决
// 「定长槽 vs GRO 大帧」这个矛盾，并且**必须同时测上行**（别再只测下行和连接速率）。
#define COAST_ENABLE_RX_RING 0

#if COAST_ENABLE_RX_RING && defined(TP_STATUS_COPY) && defined(PACKET_VERSION) && defined(PACKET_RX_RING)
#define COAST_HAVE_TPACKET_V2 1
#endif

namespace {

// 把 errno 格式化进 *err（"<what>: <strerror>"）。必须在触发错误的系统调用之后、
// 任何可能改写 errno 的调用（如 ::close）之前调用。
void setSysError(QString *err, const char *what)
{
    if (err)
        *err = QString::fromLatin1(what) + QStringLiteral(": ")
             + QString::fromLocal8Bit(std::strerror(errno));
}

// 挂一段「全丢」cBPF（单条 ret #0）。
// 用途是堵住一个经典竞态窗口：socket(AF_PACKET, …, ETH_P_ALL) **返回的那一刻**这条 socket 就已经
// 挂进内核的 ptype_all 了，而此时还没 bind——也就是说**全机所有网卡**的每一帧都在往它上面灌，
// 一直灌到我们把 ring 建好、调用方再调 setSourceMacFilter() 装上真正的源 MAC 过滤为止。
// 先在 socket() 之后立刻挂 ret #0，等 bind + ring 都就位再 SO_DETACH_FILTER 放开，于是：
//   · 窗口期压根没有帧进来 ⇒ 不需要「装过滤器前先 drain 掉旧帧」那一步；
//   · open() 的对外语义一个字没变（返回后仍是「什么都收」，直到调用方设过滤）。
void attachDropAllFilter(int fd)
{
    sock_filter code[] = { BPF_STMT(BPF_RET | BPF_K, 0) };
    struct sock_fprog prog;
    prog.len = 1;
    prog.filter = code;
    ::setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)); // 失败也无妨，纯优化
}

void detachFilter(int fd)
{
    int dummy = 0; // SO_DETACH_FILTER 忽略 optval，但内核要求给一个合法的指针/长度
    ::setsockopt(fd, SOL_SOCKET, SO_DETACH_FILTER, &dummy, sizeof(dummy));
}

#ifdef COAST_HAVE_TPACKET_V2
// ——————————————————— RX 环参数（每张网卡一份；内存账见下）———————————————————
//
// tp_block_size = 64 KiB
//   硬约束：内核要求它页对齐（PAGE_ALIGNED）。取「PAGE_SIZE 的 2 的幂倍」是因为内核用
//   __get_free_pages(order) 分配每个块，order = get_order(block_size)——不是 2 的幂就会向上取到
//   整个 order，尾巴白扔。64 KiB = 16 × 4K 页 = order-4，是「够大」和「连续页好凑」的折中
//   （order 越大越容易因内存碎片分配失败；内核有 vzalloc 兜底但没必要去踩）。
//   它同时也是单帧长度上限：内核的 max_frame_len = 块大小 − 块头，64 KiB 远大于巨帧 9018，
//   所以抓包不会被截断——和原来 recvfrom 到 64 KiB 缓冲的行为一致。
// tp_block_nr = 16 → 每张网卡 64 KiB × 16 = **1 MiB**
//   账：1 Gbps 满线速 ≈ 125 MB/s，1 MiB 环 ≈ 8 ms 的缓冲深度；我们每次 notifier 唤醒就把所有
//   就绪块吃干净，8 ms 足够盖住一次事件循环抖动。参考：老路径靠的是默认 SO_RCVBUF（约 208 KB），
//   所以这已经是 5 倍的余量。
//   为什么不敢再大：这块是内核**预分配并常驻**的物理页（建环时就全部分配好、mmap 进本进程），
//   不像 lwIP 那些 BSS 池是惰性提交的。家用场景最多同时代理 2~3 张卡 ⇒ 常驻 2~3 MiB 封顶，
//   这个量级可以接受；取 8 MiB/卡 就变成几十兆常驻，对一个桌面客户端不合适。
// tp_frame_size = 2048
//   V2 是**定长槽**：这个值就是单帧的容量上限（含 TPACKET2_HDRLEN + 以太头 + 载荷）。
//   必须 ≥ TPACKET2_HDRLEN、必须 TPACKET_ALIGNMENT(16) 对齐、且要能整除 block_size。
//   取 2048 → 每块 32 个槽，足以装下 1514 的标准帧（巨帧会被截到 2048，本链路不用巨帧）。
// tp_frame_nr 必须**精确**等于 (block_size / frame_size) × block_nr，差一个内核就直接 EINVAL。
constexpr unsigned kRingBlockSize = 64u * 1024u;
constexpr unsigned kRingBlockNr = 16u;
constexpr unsigned kRingFrameSize = 2048u;
constexpr unsigned kRingFrameNr = (kRingBlockSize / kRingFrameSize) * kRingBlockNr; // = 512
constexpr std::size_t kRingBytes = std::size_t(kRingBlockSize) * kRingBlockNr;      // = 1 MiB

// 每块能放几个定长槽。V2 的帧**不跨块**，所以第 i 帧的地址是
//   块号 = i / kRingFramesPerBlock，块内偏移 = (i % kRingFramesPerBlock) * kRingFrameSize
constexpr unsigned kRingFramesPerBlock = kRingBlockSize / kRingFrameSize; // = 32

// ★ **为什么是 V2 而不是 V3** —— 真机实测的延迟账，别再"升级"回去。
//
// V3 按**块**交付：帧先码进当前块，只有块填满、或块开启超过 `tp_retire_blk_tov` 之后，内核才把
// 整块标成 TP_STATUS_USER 并唤醒 poll。轻载下块永远填不满，于是每个包都要等那个超时。
// 而这个超时**取不小**：内核用 `msecs_to_jiffies` 换算，`CONFIG_HZ=250` 的发行版内核（树莓派
// 官方内核正是）会把 1ms 取整成 1 个 jiffy = **4ms**，实际观测到 4~8ms。
//
// 真机量到的代价（树莓派当网关，被劫持设备单条连接一来一回，tcpdump 打时间戳对齐三条腿）：
//   · 设备 SYN → 我们回 SYN-ACK：**4.885 ms**
//   · 设备的 GET 到达 → 我们开始拨 SOCKS：**7.70 ms**
//   · 而这之后的全链路（SOCKS 握手 + 拨靶机 + 请求 + 响应 + 回程）总共只要 **1.16 ms**
//   两段空白都是"帧已经到网卡了，但内核还没通知我们"。单条往返 16.1ms，其中 ~15ms 是这个。
// 关掉环退回逐帧 recvfrom 之后：**16.1ms → 2.18ms（7.4 倍）**，但吞吐掉 11%（1245→1095 conn/s）
// —— 说明批处理本身是有价值的，该去掉的只是"按块交付"这个语义。
//
// V2 逐帧就绪：内核每写完一帧就置该槽的 TP_STATUS_USER 并唤醒 poll，**没有 tov 这回事**，
// 同时仍然是 mmap 零拷贝 + 一次唤醒吃干净所有就绪槽的批处理。代价是槽位定长（每帧固定占
// kRingFrameSize，小包浪费），而我们本来就是 64 KiB×16 = 1 MiB 的固定预算，没有实际损失。
#endif // COAST_HAVE_TPACKET_V2

// —— 发方参数（理由见文件头「发方」一节）——
//
// ★ 定容量的判据是「盖住突发」，**不是**「越大越好」。这两个数就是排队时延的上限，而排队时延
//   必须明显小于 lwIP 的重传下限（lwipopts.h 里算过：TCP_TMR_INTERVAL=100ms 时 rto_min ≈ 0.6s）。
//   排得比 0.6s 还久，对端已经重传过一轮了我们才把陈旧帧发出去 —— 那是拿丢包换缓冲膨胀，白折腾。
//   ★ 注意内核会把 SO_SNDBUF **翻倍**记账（sock_setsockopt: sk_sndbuf = val * 2），算账时别忘。
//   按「网卡把队列排空的速率」折算，下面这组取值的实际缓冲是 内核 2 MiB + 用户态 1 MiB = 3 MiB：
//     千兆有线   3 MiB ≈  25 ms
//     百兆/WiFi  3 MiB ≈ 250 ms   ← 本机自己接在 WiFi 上的最坏情形，仍安全落在 0.6 s 以内
//   相对原来那个默认 208 KB 有 15 倍余量，足以吃下「lwIP 一次 tcp_output 把整个拥塞窗口刷出来」
//   这种突发。要再往上加之前，先回头看一眼上面那条「必须短于 rto_min」的判据。
constexpr int kTxSndBufBytes = 1 * 1024 * 1024; // 内核记 2 MiB
// 收方缓冲。关掉 RX 环之后帧走 socket 接收队列，默认的 208 KB 在多设备高 pps 下会溢出
// （真机四设备 6698 帧/s 时 rxdrop 一个窗口 666）。取 4 MiB，内核翻倍记账 ⇒ 实际 8 MiB。
constexpr int kRxRcvBufBytes = 4 * 1024 * 1024;
// 用户态积压：内核那层满了之后再兜一层。这层的时延同样计进上面的总账。
constexpr qint64 kTxBacklogMaxBytes = 1 * 1024 * 1024;
// 丢帧日志节流：链路一旦喂不进去就是每帧都丢，不节流会刷屏。
constexpr qint64 kDropReportMinIntervalMs = 30000;
// 内核收方丢包计数（PACKET_STATISTICS）的采样间隔。读一次即清零，故按固定间隔累加。
constexpr qint64 kRxStatsPollIntervalMs = 5000;

// Linux 二层端点：一张网卡一个 AF_PACKET 原始套接字。
// 注意：本类不加 Q_OBJECT——它不声明新信号/槽，只重写基类虚函数并 emit 基类的 frameReceived，
// 且用 functor 版 connect 绑定 Notifier，因此无需 moc（避免在 .cpp 里 #include ".moc"）。
class LinuxL2Endpoint final : public IL2Endpoint
{
public:
    explicit LinuxL2Endpoint(QObject *parent) : IL2Endpoint(parent) {}
    ~LinuxL2Endpoint() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        if (isOpen())
            close();

        // 网卡名转 C 字符串（接口名是 ASCII，latin1 足够）。
        const QByteArray name = ifname.toLatin1();

        int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd < 0) {
            setSysError(err, "socket(AF_PACKET)");
            return false;
        }
        // ★ 第一件事就是堵住「未 bind 期间收全机所有网卡」的窗口，见 attachDropAllFilter 注释。
        attachDropAllFilter(fd);

        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        std::strncpy(ifr.ifr_name, name.constData(), IFNAMSIZ - 1);
        // memset 已保证末尾 '\0'，strncpy 只写到 IFNAMSIZ-1，安全截断。

        // 接口索引（bind 用）。
        if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
            setSysError(err, "ioctl(SIOCGIFINDEX)");
            ::close(fd);
            return false;
        }
        const int idx = ifr.ifr_ifindex;

        // 本机 MAC（伪造 ARP/以太源地址用）。SIOCGIFHWADDR 把 6 字节放在 ifr_hwaddr.sa_data[0..5]。
        if (::ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
            setSysError(err, "ioctl(SIOCGIFHWADDR)");
            ::close(fd);
            return false;
        }
        const QByteArray mac(ifr.ifr_hwaddr.sa_data, 6);

        // MTU（用户态栈分片参考）。取不到不算致命——回落 1500（契约）。
        int mtu = 1500;
        if (::ioctl(fd, SIOCGIFMTU, &ifr) >= 0)
            mtu = ifr.ifr_mtu;

        // 绑定到该网卡，只收/发这张口的帧。
        struct sockaddr_ll sll;
        std::memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = htons(ETH_P_ALL);
        sll.sll_ifindex = idx;
        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll)) < 0) {
            setSysError(err, "bind(sockaddr_ll)");
            ::close(fd);
            return false;
        }

        // ★ 混杂模式（IPv6 网关的硬要求）：IPv6 的邻居发现 NS/NA/RS/RA 走的是**组播**（solicited-node
        //   / all-nodes / all-routers），不像 IPv4 ARP 那样是广播。网卡硬件默认只收「本机 MAC + 广播 +
        //   已订阅的组播组」，会把被劫持设备发出的组播 NS、真路由器周期广播的 RA 直接在硬件层丢掉——
        //   于是 NdpSpoofer 既抢答不到设备的 NS（首解析/老化后重解析时无法把「路由器在本机」钉住），
        //   也反制不到路由器的 RA。开混杂让这些组播 NDP 帧进到本 socket，再交由下面的 cBPF 在内核态
        //   按「源 MAC=victim 或 ethertype=ARP/ICMPv6-NDP」筛掉无关帧（userspace 只拿到该拿的）。
        //   用 PACKET_ADD_MEMBERSHIP(PACKET_MR_PROMISC) 而非 SIOCSIFFLAGS：随 socket 关闭内核自动撤销，
        //   进程崩溃也不会把网卡永久留在混杂态。IPv4 侧本不需要（ARP 是广播），开着无害。
        struct packet_mreq mreq;
        std::memset(&mreq, 0, sizeof(mreq));
        mreq.mr_ifindex = idx;
        mreq.mr_type = PACKET_MR_PROMISC;
        ::setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)); // 失败无妨，BPF 仍在

        // ★ 抬高发送缓冲（理由见文件头「发方」一节）。先试 SO_SNDBUFFORCE：它需要 CAP_NET_ADMIN，
        //   而网关本来就以 root 跑，好处是**越过 net.core.wmem_max 的钳制**（发行版默认常是
        //   212992，普通 SO_SNDBUF 会被悄悄砍回去，看起来设了其实没设）。没权限再退回 SO_SNDBUF，
        //   由内核钳到系统上限。两者都失败也不致命——积压队列还兜着一层。
        //   SO_SNDBUFFORCE 的数值是**按架构定义**的，不能硬写常量兜底；头文件没有就只走普通路径。
        int snd = kTxSndBufBytes;
        bool sndSet = false;
#ifdef SO_SNDBUFFORCE
        sndSet = ::setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &snd, sizeof(snd)) == 0;
#endif
        if (!sndSet)
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));

        // ★ 收方缓冲同样要抬 —— **关掉 RX 环之后这条才成为必须**。
        //
        //   有环的时候，帧落在那 1 MiB 的 mmap 环里，`SO_RCVBUF` 根本不参与；现在走的是逐帧
        //   recvfrom（见文件头那段选型账），帧先进 socket 的接收队列，而它的默认容量取自
        //   `net.core.rmem_default` —— 多数发行版就是 **212992（208 KB）**，按满长帧算只排得下
        //   一百四十来个。事件循环被别的活占住一小会儿就溢出，内核直接丢帧（`tp_drops`）。
        //
        //   丢在这里是最难查的一类：设备侧只看到 TCP 重传变多、变慢，而我们自己
        //   `txdrop/tcpAbort/socksFail` 全是 0，看起来一切正常。
        //   真机实测（树莓派网关，**四台设备同时打**、6698 帧/s）：`rxdrop` 一个窗口涨到 666；
        //   单设备负载下则是 0~2 —— 所以这个坑只有多设备/高 pps 才现形，单设备测不出来。
        //
        //   取 4 MiB（内核同样翻倍记账 ⇒ 实际 8 MiB），与原先那 1 MiB 环相比有充足余量；
        //   和发方一样优先 `SO_RCVBUFFORCE` 越过 `net.core.rmem_max` 的钳制。
        int rcv = kRxRcvBufBytes;
        bool rcvSet = false;
#ifdef SO_RCVBUFFORCE
        rcvSet = ::setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof(rcv)) == 0;
#endif
        if (!rcvSet)
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));

        // 非阻塞：回退路径（drainSocket）靠它循环 recv 到 EAGAIN，绝不在无数据时阻塞事件循环。
        // 发方同样依赖它：满了要拿到 EAGAIN/ENOBUFS 才能转去排队，而不是把工作线程堵死。
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            flags = 0;
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        m_fd = fd;
        m_ifIndex = idx;
        m_localMac = mac;
        m_mtu = mtu;

        // 建 RX 环（TPACKET_v2，逐帧就绪）。失败不是错误——静默回退到逐帧 recvfrom（m_ring 保持 nullptr）。
        setupRxRing();

        // 环（或回退路径）已就位，放开临时的「全丢」过滤器，恢复 open() 的对外语义。
        detachFilter(m_fd);

        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        // Qt6 的 activated 只剩 (QSocketDescriptor, Type) 一个重载，取地址无歧义；
        // 用零参 lambda 转发（尾随实参可丢弃）。
        connect(m_notifier, &QSocketNotifier::activated, this, [this]() { onReadable(); });

        // 写就绪通知器：常态**关闭**（fd 绝大多数时候可写，开着就是每轮事件循环空转一次），
        // 只在 enqueueTx 排进第一帧时打开、drainTx 排空后关掉。
        m_txNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
        m_txNotifier->setEnabled(false);
        connect(m_txNotifier, &QSocketNotifier::activated, this, [this]() { drainTx(); });
        return true;
    }

    void close() override
    {
        if (m_notifier) {
            delete m_notifier; // 先删 Notifier，停止对 fd 的监听，再拆环、关 fd
            m_notifier = nullptr;
        }
        if (m_txNotifier) {
            delete m_txNotifier;
            m_txNotifier = nullptr;
        }
        m_txQueue.clear();
        m_txQueuedBytes = 0;
#ifdef COAST_HAVE_TPACKET_V2
        if (m_ring) {
            // 先 munmap 再 close：环页由内核持有，socket 关闭时才真正释放；反过来做会在
            // 已释放的映射上留一个悬空 VMA 的窗口。不需要显式发「销毁环」的 setsockopt——
            // ::close(fd) 会把 rx_ring 一并拆掉。
            ::munmap(m_ring, kRingBytes);
            m_ring = nullptr;
            m_frameIdx = 0;
        }
#endif
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        m_ifIndex = -1;
        m_localMac.clear();
        m_mtu = 1500;
    }

    bool isOpen() const override { return m_fd >= 0; }

    // ——— 崩溃兜底通道（IL2Endpoint::panicSender 的契约）———
    // 只做一次 sendto，无分配、无锁、无 Qt —— 可以在 SIGSEGV 的处理器里调用。
    // 上下文是本对象里的一个成员（地址稳定），装着 fd 与 ifIndex。
    static void panicSendImpl(void *ctx, const unsigned char *data, int len)
    {
        const PanicCtx *c = static_cast<const PanicCtx *>(ctx);
        if (!c || c->fd < 0 || !data || len < 14)
            return;
        struct sockaddr_ll sll;
        std::memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = c->ifIndex;
        sll.sll_halen = 6;
        std::memcpy(sll.sll_addr, data, 6); // 目的 MAC = 帧头前 6 字节
        (void)::sendto(c->fd, data, static_cast<size_t>(len), 0,
                       reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll));
    }
    RawSendFn panicSender() const override { return &panicSendImpl; }
    void *panicContext() override
    {
        m_panicCtx.fd = m_fd;
        m_panicCtx.ifIndex = m_ifIndex;
        return &m_panicCtx;
    }

    // 发一帧。缓冲满时**排队重试**而不是丢（理由见文件头「发方」一节）。
    // 返回 false 只在「参数非法 / 端点没开 / 队列也满了」——调用方（lwipLinkOutput）历来忽略它，
    // 这里也不指望它去处理；真正的可观测性在 m_txDropped 的节流日志上。
    bool send(const QByteArray &frame) override
    {
        if (m_fd < 0 || frame.size() < 14) // 至少要够以太头
            return false;

        // 队列非空 = 内核正堵着，必须排到队尾，否则新帧插队会把同一条 TCP 流的段序打乱
        // （乱序在对端表现为丢包重传，等于自己给自己制造损伤）。
        if (!m_txQueue.empty())
            return enqueueTx(frame);

        const int r = rawSend(frame);
        if (r > 0)
            return true;
        if (r == 0)
            return enqueueTx(frame); // EAGAIN/ENOBUFS：缓冲满，等可写
        return false;                // 其它 errno：这一帧真发不出去（网卡 down 等）
    }

    QByteArray localMac() const override { return m_localMac; }
    int ifIndex() const override { return m_ifIndex; }
    int mtu() const override { return m_mtu; }

    // 内核态源 MAC 过滤：SO_ATTACH_FILTER 给这条 AF_PACKET socket 挂一段手写 cBPF。裸 socket 不带
    // libpcap，故自己生成指令（结构很简单：比对以太头 offset 6..11 的 6 字节源 MAC）。
    // 过滤只作用于本 socket 的**收**，不影响 sendto；也不影响内核给本机正常协议栈的投递。契约见头文件。
    //
    // 与 RX 环共存：SO_ATTACH_FILTER 是在 packet_rcv/tpacket_rcv **之前**由 sk_filter 执行的
    // （run_filter() 在 tpacket_rcv 入口，早于「往环里码字节」这一步），所以过滤器和环是正交的两层，
    // 装环前装、装环后装都行，被过滤掉的帧根本不会占环里的空间。本类的装载顺序是：
    //   socket() → 挂 ret #0（堵住未 bind 窗口）→ bind → 建环 → 解开 ret #0 → 之后由调用方
    //   随时重设真正的源 MAC 过滤。
    // 因此不存在「过滤器装上之前 socket 已经收了一堆包、需要 drain」的经典坑（那个窗口里是全丢的）。
    //
    // cBPF 布局（N 个 MAC 的「或」）：每个 MAC 4 条指令块，末尾两条 ret：
    //   [base+0] ld  [8]                 ; A = 以太头 offset 8 的 4 字节 = src[2..5]（大端）
    //   [base+1] jeq #last4, jt0, jf→失败 ; 不等 → 跳下一个 MAC 块（或 reject）
    //   [base+2] ldh [6]                 ; A = offset 6 的 2 字节 = src[0..1]
    //   [base+3] jeq #first2, jt→accept, jf→失败
    //   ...
    //   [4N]     ret #0xffffffff          ; accept：整帧收上来
    //   [4N+1]   ret #0                   ; reject：丢弃
    // 拆成 [8] 的 word + [6] 的 half 是 tcpdump "ether src" 的经典写法（一次比 4 字节 + 一次比 2 字节）。
    bool setSourceMacFilter(const QVector<QByteArray> &macs) override
    {
        if (m_fd < 0)
            return false;

        QVector<QByteArray> valid;
        valid.reserve(macs.size());
        for (const QByteArray &m : macs)
            if (m.size() == 6)
                valid.append(m);

        std::vector<sock_filter> code;
        {
            // ★ 没有被劫持设备时**不再全丢**：仍放行 ARP + NDP，让被动监视器 ArpWatch 能看见「有人
            //   冒充网关/本机在代理我」——这类检测不需要正在劫持任何设备（idle 也要能报警）。数据帧
            //   照旧一个不进用户态。下面的通用布局在 n=0 时**恰好退化成「只放行 ARP/NDP」**，与非空
            //   时末尾的放行分支逐字节一致，所以不再特判空集——直接走通用路径，复用已验证的偏移算法。
            //   代价：idle 时每条广播 ARP / 组播 NDP 会进一次用户态（低频帧，近零成本）。
            // cBPF 跳转偏移是 8bit——设备极多会越界。真实场景不会有这么多被劫持设备；超限就不装内核
            // 过滤（返回 false），交用户态兜底。
            if (valid.size() > 60)
                return false;
            const int n = valid.size();
            // 末尾追加两条放行分支（都在所有 victim 源 MAC 失配之后，各自最终跳到 accept/reject）：
            //  · ethertype=ARP(0x0806)：捕获真网关广播的 who-has（携带「网关在真 MAC」会把设备解毒），
            //    LanGateway 据此 reassertNow。
            //  · ethertype=IPv6(0x86DD) 且 next-header=ICMPv6(58)：捕获 IPv6 邻居发现（NS/NA/RS/RA）。
            //    IPv6 无广播，路由器的 RA、设备的 NS 全是**组播**——靠上面的混杂让它们进 socket，这里放行，
            //    NdpSpoofer 才能抢答设备 NS、反制路由器 RA（v4 侧 ARP 分支的 v6 对应物）。低频帧，全收近零成本。
            // 指令布局（n 个 victim 块之后）：
            //   [4n]   arpLd : ldh [12]              ; A = ethertype
            //   [4n+1] jeq 0x0806 → accept，否则落下
            //   [4n+2] jeq 0x86DD → 落下查 next-header，否则 → reject
            //   [4n+3] ldb [20]                      ; A = IPv6 next header（14 以太 + 6）
            //   [4n+4] jeq 58(ICMPv6) → accept，否则 → reject
            //   [4n+5] accept: ret 0xffffffff
            //   [4n+6] reject: ret 0
            const int arpLd  = 4 * n;      // ldh [12]
            const int accept = 4 * n + 5;  // ret 0xffffffff
            const int reject = 4 * n + 6;  // ret 0
            for (int i = 0; i < n; ++i) {
                const auto *b = reinterpret_cast<const unsigned char *>(valid[i].constData());
                const quint16 first2 = (quint16(b[0]) << 8) | b[1];
                const quint32 last4 = (quint32(b[2]) << 24) | (quint32(b[3]) << 16)
                                    | (quint32(b[4]) << 8) | quint32(b[5]);
                const int base = 4 * i;
                // 本块失配后的去处：不是最后一个 MAC → 下一块；最后一个 → ARP/NDP 判定块。
                const int failIdx = (i < n - 1) ? (4 * (i + 1)) : arpLd;
                code.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 8));
                code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, last4, 0,
                                        static_cast<__u8>(failIdx - (base + 1) - 1)));
                code.push_back(BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 6));
                code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, first2,
                                        static_cast<__u8>(accept - (base + 3) - 1),
                                        static_cast<__u8>(failIdx - (base + 3) - 1)));
            }
            code.push_back(BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12));            // [4n]   A = ethertype
            code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0806,
                                    static_cast<__u8>(accept - (4 * n + 1) - 1), 0)); // [4n+1] ARP→accept 否则落下
            code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x86DD,
                                    0, static_cast<__u8>(reject - (4 * n + 2) - 1))); // [4n+2] IPv6→查下条 否则→reject
            code.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 20));            // [4n+3] A = IPv6 next header
            code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 58,
                                    static_cast<__u8>(accept - (4 * n + 4) - 1),
                                    static_cast<__u8>(reject - (4 * n + 4) - 1)));    // [4n+4] ICMPv6→accept 否则→reject
            code.push_back(BPF_STMT(BPF_RET | BPF_K, 0xFFFFFFFFu));            // [4n+5] accept：整帧
            code.push_back(BPF_STMT(BPF_RET | BPF_K, 0));                      // [4n+6] reject
        }

        struct sock_fprog prog;
        prog.len = static_cast<unsigned short>(code.size());
        prog.filter = code.data();
        // SO_ATTACH_FILTER 会**替换**已有过滤器（内核 rcu 换指针），故动态重设直接再调一次即可。
        // 注意：换过滤器前已排进环/接收队列的旧帧不会被追溯过滤——那几帧仍由用户态过滤丢掉，无害。
        if (::setsockopt(m_fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) < 0)
            return false;
        return true;
    }

private:
    // —————————————————————————— 发方 ——————————————————————————
    // 裸发一帧。返回 1 = 已发出；0 = 内核缓冲满（EAGAIN/ENOBUFS/EINTR，可重试）；-1 = 真错误。
    int rawSend(const QByteArray &frame)
    {
        // 发送地址结构：内核按 sll_ifindex 选口，sll_halen/sll_addr 给目的 MAC（= 帧头前 6 字节）。
        // 注意发方**不走环**（我们只装了 RX_RING，没装 TX_RING）——sendto 语义一如既往。
        struct sockaddr_ll sll;
        std::memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = m_ifIndex;
        sll.sll_halen = 6;
        std::memcpy(sll.sll_addr, frame.constData(), 6); // dst MAC

        for (;;) {
            const ssize_t n = ::sendto(m_fd, frame.constData(),
                                       static_cast<size_t>(frame.size()), 0,
                                       reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll));
            if (n == static_cast<ssize_t>(frame.size())) {
                ++GatewayDiag::c.txFrames;
                GatewayDiag::c.txBytes += frame.size();
                return 1;
            }
            if (n >= 0)
                return -1; // 部分发送：数据报 socket 上不该发生，当错误处理
            if (errno == EINTR)
                continue;  // 被信号打断，原样重来（不消耗队列名额）
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
                return 0;
            return -1;
        }
    }

    // 排进积压队列，并确保写就绪通知器是开着的。队列满则丢最新的一帧并计数。
    // 丢**新**而不是丢旧：旧帧已经排在同一条流的前面，丢它会造成空洞，让对端进入更贵的
    // 乱序/重传恢复；丢队尾等价于「链路此刻的容量就到这了」，对 TCP 是最温和的信号。
    bool enqueueTx(const QByteArray &frame)
    {
        if (m_txQueuedBytes + frame.size() > kTxBacklogMaxBytes) {
            ++GatewayDiag::c.txDropped;
            reportDropsThrottled();
            return false;
        }
        ++GatewayDiag::c.txDeferred; // 还没丢，但内核已经喂不进去了——拥塞的**早期**信号
        // ★ 这里存的是 QByteArray 的**隐式共享副本**，不是深拷贝——对 fromRawData 造出来的视图
        //   等于存了个裸指针。所以 send() 的入参契约是「必须是自有内存的 QByteArray」。
        //   现状成立：唯一的调用方 lwipLinkOutput 传的是 pbufToBytes() 新分配的缓冲；
        //   ArpSpoofer/NdpSpoofer 传的也是自己拼出来的。将来若有人想把收环里的帧直接回注，
        //   必须在这之前 deep-copy（QByteArray(f.constData(), f.size())）。
        m_txQueue.push_back(frame);
        m_txQueuedBytes += frame.size();
        if (m_txQueuedBytes > GatewayDiag::c.txBacklogPeak)
            GatewayDiag::c.txBacklogPeak = m_txQueuedBytes; // 本窗口高水位（采样后清零）
        if (m_txNotifier && !m_txNotifier->isEnabled())
            m_txNotifier->setEnabled(true);
        return true;
    }

    // fd 可写 → 把积压尽量灌进内核；空了就关掉写通知器（不关会在可写时空转烧 CPU）。
    void drainTx()
    {
        while (!m_txQueue.empty()) {
            const QByteArray &f = m_txQueue.front();
            const int r = rawSend(f);
            if (r == 0)
                return; // 又满了，等下一次可写
            if (r < 0) {
                ++GatewayDiag::c.txDropped; // 发不出去的帧不能永远堵住队头
                reportDropsThrottled();
            }
            m_txQueuedBytes -= m_txQueue.front().size();
            m_txQueue.pop_front();
        }
        if (m_txNotifier)
            m_txNotifier->setEnabled(false);
    }

    // 收方内核丢包计数。PACKET_STATISTICS 是「读即清零」，所以按固定间隔采样累加。
    // tp_drops 涨 = 环被写满、内核来不及交付（用户态处理不过来 / 事件循环被别的活堵住）——
    // 这是设备**上行**方向的静默丢包，和发方的 m_txDropped 互为镜像。
    void pollRxDrops(qint64 nowMs)
    {
        if (nowMs - m_lastRxStatsMs < kRxStatsPollIntervalMs)
            return;
        m_lastRxStatsMs = nowMs;
        struct tpacket_stats st;
        std::memset(&st, 0, sizeof(st));
        socklen_t len = sizeof(st);
        if (::getsockopt(m_fd, SOL_PACKET, PACKET_STATISTICS, &st, &len) < 0)
            return;
        if (st.tp_drops == 0)
            return;
        GatewayDiag::c.rxKernelDrops += st.tp_drops;
        reportDropsThrottled();
    }

    // 两个方向的丢包共用一条节流日志。丢包不可见是这条链路最贵的故障模式（lwIP 只会在几百
    // 毫秒后重传，用户看到的只是「慢」），所以宁可吵一点也要报出来。
    void reportDropsThrottled()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastDropReportMs < kDropReportMinIntervalMs)
            return; // 节流：基线不动，攒到下个窗口一起报
        m_lastDropReportMs = now;
        qWarning().noquote()
            << "L2Endpoint: 二层丢帧 —— 发方积压溢出" << GatewayDiag::c.txDropped
            << "帧, 内核收方丢弃" << GatewayDiag::c.rxKernelDrops
            << "帧 (当前积压" << m_txQueuedBytes
            << "字节)。发方涨 = 网卡/链路喂不进去(WiFi 慢客户端、qdisc 拥塞)；"
               "收方涨 = 用户态处理不过来。两者都会让被代理设备「什么都慢」。";
    }

    // Notifier 就绪回调的统一入口：走环或走回退路径，并做一层重入保护。
    // 为什么要防重入：槽里会一路跑到 lwIP / SOCKS，理论上不该转事件循环，但万一某天有人在下游
    // 引入了嵌套事件循环，level-triggered 的 QSocketNotifier 会在同一个 fd 上再次触发，
    // 于是同一个块被走两遍、还给内核两次——那是内存被内核就地覆写却仍在读的悬垂场景。一个 bool 的代价。
    void onReadable()
    {
        if (m_inDrain)
            return;
        m_inDrain = true;
        ++GatewayDiag::c.rxWakes; // 与 rxFrames 之比 = 收环的批处理效率（掉到 1 = 退化成逐帧唤醒）
        pollRxDrops(QDateTime::currentMSecsSinceEpoch()); // 内部自带 5s 间隔，常态直接 return
#ifdef COAST_HAVE_TPACKET_V2
        if (m_ring)
            drainRing();
        else
            drainSocket();
#else
        drainSocket();
#endif
        m_inDrain = false;
    }

    // 回退路径：fd 可读时把内核缓冲里所有帧一次性抽干（每帧一次 recv、一次 QByteArray）。
    // 只在 RX 环没建起来时才会被用到。
    void drainSocket()
    {
        char buf[65536]; // 单帧上限，容得下巨帧
        for (;;) {
            const ssize_t n = ::recv(m_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                ++GatewayDiag::c.rxFrames;
                GatewayDiag::c.rxBytes += n;
                emit frameReceived(QByteArray(buf, static_cast<int>(n)));
            } else if (n < 0) {
                if (errno == EINTR)
                    continue;               // 被信号打断，重试
                break;                      // EAGAIN/EWOULDBLOCK 或其它错误：结束本轮
            } else {
                break;                      // n == 0
            }
        }
    }

#ifdef COAST_HAVE_TPACKET_V2
    // 建 TPACKET_v2 RX 环。任何一步失败都返回 false ⇒ 调用方保持 m_ring == nullptr ⇒ 走 drainSocket。
    bool setupRxRing()
    {
        int ver = TPACKET_V2;
        if (::setsockopt(m_fd, SOL_PACKET, PACKET_VERSION, &ver, sizeof(ver)) < 0)
            return false; // 内核不支持 V2 → 回退

        struct tpacket_req req;
        std::memset(&req, 0, sizeof(req));
        req.tp_block_size = kRingBlockSize;
        req.tp_block_nr = kRingBlockNr;
        req.tp_frame_size = kRingFrameSize;
        req.tp_frame_nr = kRingFrameNr;
        if (::setsockopt(m_fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0)
            return false; // 参数被拒 / 连续内存不足（ENOMEM）→ 回退

        void *p = ::mmap(nullptr, kRingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
        if (p == MAP_FAILED) {
            // ★ 必须把已经建好的环拆掉再回退。装了 RX_RING 的 socket，收方**只往环里投**
            //   （内核把 prot_hook.func 从 packet_rcv 换成了 tpacket_rcv），普通接收队列永远是空的
            //   —— 留着一个没人 mmap 的环，drainSocket 会一帧都读不到，网关直接哑掉。
            //   传全 0 的 req 即为「销毁环」；此时 po->mapped 为 0，不会 EBUSY。
            struct tpacket_req zero;
            std::memset(&zero, 0, sizeof(zero));
            ::setsockopt(m_fd, SOL_PACKET, PACKET_RX_RING, &zero, sizeof(zero));
            return false;
        }

        m_ring = static_cast<unsigned char *>(p);
        m_frameIdx = 0;
        return true;
    }

    // 收方主路径：一次唤醒把所有「已就绪」的槽处理干净。V2 是**逐帧**就绪，槽定长顺序排布。
    //
    // 索引同步（写错就要么丢包、要么空转烧 CPU，务必看懂）：
    //   内核和我们各自维护一个帧游标，都从 0 开始、都严格 +1 取模 kRingFrameNr 地推进。内核的
    //   poll 就绪条件正是「队首槽的 tp_status != TP_STATUS_KERNEL」——和这里 break 的判据是
    //   同一个字段同一个槽。所以只要我们「每消费一槽就恰好推进一格、且绝不跳过」，
    //   level-triggered 的 QSocketNotifier 就不会出现「说就绪但我们什么都没吃到」的空转。
    void drainRing()
    {
        unsigned consumed = 0;
        for (;;) {
            if (!m_ring)
                return; // 上一槽里发生了 close()（重配/摘卡）：环已 munmap，立刻停手
            // V2 的帧不跨块：先定位块，再在块内按定长槽取。
            const unsigned blk = m_frameIdx / kRingFramesPerBlock;
            const unsigned slot = m_frameIdx % kRingFramesPerBlock;
            unsigned char *frame = m_ring + std::size_t(blk) * kRingBlockSize
                                   + std::size_t(slot) * kRingFrameSize;
            const unsigned char *frameEnd = frame + kRingFrameSize;
            auto *th = reinterpret_cast<struct tpacket2_hdr *>(frame);

            // acquire：内核是「先写满帧内容，再置 tp_status」；我们必须反过来「先读到状态，
            // 再读内容」，中间隔一次 acquire 屏障。arm64 这类弱序架构上少了它就可能读到还没写完的帧
            // （x86 上是空操作，但 CI 要出 arm64 包，不能省）。
            const __u32 status = __atomic_load_n(&th->tp_status, __ATOMIC_ACQUIRE);
            if ((status & TP_STATUS_USER) == 0)
                break; // 队首槽还归内核 → 本轮结束

            const unsigned char *data = frame + th->tp_mac; // tp_mac = 相对本帧头的以太头偏移
            const __u32 caplen = th->tp_snaplen;            // 实际抓到的字节数（不是原始 tp_len）
            // 越界自保：正常情况下内核给的偏移一定落在槽内，这几步比较是防「槽被写坏」时
            // 我们拿着野指针去 emit（那可是直接读内核给的共享内存）。都写成「差值 vs 长度」
            // 而不是「指针 + 长度」，免得 caplen 是垃圾值时指针加法先溢出。
            if (data >= frame && data < frameEnd && caplen > 0
                && caplen <= std::size_t(frameEnd - data)) {
                // ★ 零拷贝：QByteArray 直接指向环内存，不分配、不 memcpy。
                //   合法性依赖两条契约（见 IL2Endpoint.h）：帧仅在槽内有效；连接必须是直连。
                //   本槽要到下面 __atomic_store_n 把它还给内核之后才可能被覆写，而那一步在
                //   emit 返回之后——所以槽执行期间指针一定有效。
                ++GatewayDiag::c.rxFrames;
                GatewayDiag::c.rxBytes += caplen;
                emit frameReceived(QByteArray::fromRawData(
                    reinterpret_cast<const char *>(data), static_cast<qsizetype>(caplen)));
            }
            if (!m_ring)
                return; // 槽里发生了 close()：环已 munmap，连下面还槽的写都不能做

            // release：保证上面对帧内容的读全部完成之后，才把槽还给内核——内核拿到 TP_STATUS_KERNEL
            // 会立刻开始往这槽里写新帧。写错时机 = 一边读一边被覆写。
            __atomic_store_n(&th->tp_status, TP_STATUS_KERNEL, __ATOMIC_RELEASE);
            m_frameIdx = (m_frameIdx + 1) % kRingFrameNr;
            ++consumed;
        }

        if (consumed == 0) {
            // 说「可读」却一个块都没吃到。理论上不该发生（判据同源，见上），但有一个现实来源：
            // packet_poll 里 OR 了 datagram_poll，socket 上挂着未取走的 sk_err（比如网卡被拔 →
            // ENETDOWN）会让 poll 一直返回 POLLERR，而 Qt 的 Read 通知器把它也当就绪。
            // 老的 recvfrom 路径每次读会顺手 sock_error() 把它取走清零，改成环之后就没人清了 ——
            // 那正好会变成事件循环空转烧 CPU（这个仓库刚为「GUI 线程空转」做过一次优化，不能再引入一个）。
            // getsockopt(SO_ERROR) 的内核实现就是 sock_error()（读并清零），是不碰接收队列的清错法：
            // 装了 RX_RING 的 socket 收方只走环，不该再 recv/read。
            int soErr = 0;
            socklen_t len = sizeof(soErr);
            ::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &soErr, &len);
        }
    }
#else
    bool setupRxRing() { return false; } // 头文件不认识 TPACKET_v2：永远走 drainSocket
#endif // COAST_HAVE_TPACKET_V2

    int m_fd = -1;
    int m_ifIndex = -1;
    // 崩溃兜底上下文：地址必须稳定（GatewayPanic 长期持有它的指针），所以放成员而不是临时量。
    struct PanicCtx {
        int fd = -1;
        int ifIndex = -1;
    };
    PanicCtx m_panicCtx;
    int m_mtu = 1500;
    QByteArray m_localMac;
    QSocketNotifier *m_notifier = nullptr;
    bool m_inDrain = false;
    // 发方积压（见文件头「发方」一节）。写通知器**常态是关的**，只在队列非空期间打开。
    QSocketNotifier *m_txNotifier = nullptr;
    std::deque<QByteArray> m_txQueue;
    qint64 m_txQueuedBytes = 0;
    // 丢帧的**累计**数不在这里——放 GatewayDiag::c 里，好让它一起进采样日志（多网卡时是合计值）。
    qint64 m_lastDropReportMs = -kDropReportMinIntervalMs;
    qint64 m_lastRxStatsMs = 0;
#ifdef COAST_HAVE_TPACKET_V2
    unsigned char *m_ring = nullptr; // mmap 起点（= 第 0 块）；nullptr = 没建成环，走回退路径
    unsigned m_frameIdx = 0;         // 下一个要消费的槽号，必须和内核队首严格同步
#endif
};

} // namespace

IL2Endpoint *createL2Endpoint(QObject *parent)
{
    return new LinuxL2Endpoint(parent);
}

#endif // Q_OS_LINUX（非 Linux：本 TU 为空，工厂由 L2Endpoint_mac.cpp / L2Endpoint_win.cpp 提供）
