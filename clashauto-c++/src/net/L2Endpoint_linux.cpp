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
// ——————————————————— 收方：TPACKET_v3 mmap 环形缓冲（本文件的性能核心）———————————————————
//
// 老写法是「每帧一次 recvfrom + 每帧一个 QByteArray」。83k pps 量级下，两笔开销都和整个用户态
// 协议栈处理同量级：
//   · syscall：每帧一次 recvfrom ≈ 一次进出内核（现在还叠着 spectre/meltdown 的缓解代价）；
//   · 堆分配：每帧一个引用计数块 + memcpy。
// 换成 AF_PACKET 的 TPACKET_v3 收环之后：
//   · 内核把帧直接码进一块和我们共享的 mmap 区，按「块(block)」为单位成批交付。一次 poll 唤醒
//     处理一整块里的所有帧 → **每块一次 syscall**（其实连 syscall 都没有：读环是纯内存访问，
//     唯一的系统调用是 Qt 事件循环那次 poll），而不是每帧一次。
//   · 帧内容就在环里，用 QByteArray::fromRawData 指过去，**入站方向零堆分配、零拷贝**。
//     这依赖 IL2Endpoint::frameReceived 的零拷贝契约（帧仅在槽内有效）+ 必须是**直连**，
//     见 IL2Endpoint.h 的信号注释；跨线程队列连接会让指针在投递后失效。
//
// **回退路径**：TPACKET_v3 要内核 ≥ 3.2，建环还要一大块连续内核内存。setsockopt(PACKET_VERSION)
// / setsockopt(PACKET_RX_RING) / mmap 任何一步失败，都原样退回逐帧 recvfrom（drainSocket），
// 功能完全不变、只是少了这层优化。老到连头文件都不认识 TPACKET_v3 的构建环境，则整段代码被
// COAST_HAVE_TPACKET_V3 编译期摘掉。CI 要出 x64 和 arm64 两个 Linux 包、用户内核版本不可控，
// 这条回退不能省。
//
// 权限：需要 CAP_NET_RAW / root，否则 socket() 直接 EPERM。open() 失败时置 *err 并清理半开 fd。
//
// 本文件在所有平台都参与编译：非 Linux 时只保留一个返回 nullptr 的工厂（LanGateway 据此判不可用）。
#include "IL2Endpoint.h"

#if defined(Q_OS_LINUX)

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>     // mmap/munmap（RX 环）
#include <net/if.h>
#include <net/ethernet.h> // ETH_P_ALL / ETH_ALEN（内部再引 <linux/if_ether.h>，有 UAPI 守卫防冲突）
// ★ 内核 UAPI 头放在 glibc 网络头之后引，且用 <linux/if_packet.h> 而**不是** glibc 的
//   <netpacket/packet.h>：两者都定义 struct sockaddr_ll / struct packet_mreq，彼此没有 UAPI 守卫，
//   同时包含会直接「redefinition of struct sockaddr_ll」。TPACKET_v3 的那套结构
//   （tpacket_req3 / tpacket_block_desc / tpacket3_hdr）只有内核头里有，所以只能选它——
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
#include <vector>

#include <QByteArray>
#include <QSocketNotifier>
#include <QVector>

// SOL_PACKET 正常由 glibc 的 <sys/socket.h>(bits/socket.h) 提供；个别精简 libc 没有，兜一个。
#ifndef SOL_PACKET
#define SOL_PACKET 263
#endif

// TPACKET_v3 的编译期可用性探针。
// TPACKET_V3 本身是 enum 常量（无法 #ifdef），而 TP_STATUS_BLK_TMO 这个宏是和 TPACKET_v3 同一个
// 内核提交（3.2, "af-packet: TPACKET_V3 flexible buffer implementation"）引入、且是 V3 专用语义的，
// 拿它当「这套内核头懂 V3」的探针最贴切。PACKET_VERSION/PACKET_RX_RING 再兜一层。
#if defined(TP_STATUS_BLK_TMO) && defined(PACKET_VERSION) && defined(PACKET_RX_RING)
#define COAST_HAVE_TPACKET_V3 1
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

#ifdef COAST_HAVE_TPACKET_V3
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
//   V3 里帧是**变长紧凑排布**的，这个值运行期不限制实际抓包长度，只参与 setsockopt 的入参校验：
//   必须 ≥ TPACKET3_HDRLEN(=68)、必须 TPACKET_ALIGNMENT(16) 对齐、且要能整除 block_size。
//   取 2048 → 每块 32 个「名义槽」。
// tp_frame_nr 必须**精确**等于 (block_size / frame_size) × block_nr，差一个内核就直接 EINVAL。
constexpr unsigned kRingBlockSize = 64u * 1024u;
constexpr unsigned kRingBlockNr = 16u;
constexpr unsigned kRingFrameSize = 2048u;
constexpr unsigned kRingFrameNr = (kRingBlockSize / kRingFrameSize) * kRingBlockNr; // = 512
constexpr std::size_t kRingBytes = std::size_t(kRingBlockSize) * kRingBlockNr;      // = 1 MiB

// tp_retire_blk_tov（毫秒）—— **这是延迟旋钮，必须取小**。
//
// V3 的交付语义：帧先码进「当前块」，只有块**填满**、或者块开启后超过 tp_retire_blk_tov，内核才
// 把整块标成 TP_STATUS_USER 并唤醒 poll。也就是说块没填满时，帧就静静躺在内核里不通知我们——
// 这个超时直接变成轻载时每个包的额外延迟（计时从块开启算起，随机到达的包平均摊到 tov/2）。
// 取 1：这是能取到的最小非零值（0 = 让内核按块大小/链路速率自己算，那会算出几十毫秒，不能要）。
// 注意内核用 msecs_to_jiffies 换算，CONFIG_HZ=250 的发行版内核会取整到 1 个 jiffy = 4 ms。
//
// 权衡说清楚：
//   · 重载（正是我们要优化的场景）下块几秒钟填满好多个，走的是「满」而不是超时，这点延迟根本
//     不出现，拿到的是「每块一次唤醒」的全部收益；
//   · 轻载下多出 0~4 ms。对交互流量（TCP ACK / SSH / 游戏）是可感知的，但相对于这条链路本身
//     的代价（用户态 lwIP 终结 + SOCKS 出网，RTT 本来就是几十毫秒）属于可接受的量级。
//   · 如果将来实测这点延迟不可接受，替代方案是退回 TPACKET_V2：它逐帧就绪、没有 tov 这回事，
//     同样能拿到 mmap 零拷贝 + 批处理，代价是槽位定长（每帧固定占 tp_frame_size）内存利用率差。
constexpr unsigned kRingRetireTovMs = 1u;
#endif // COAST_HAVE_TPACKET_V3

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

        // 非阻塞：回退路径（drainSocket）靠它循环 recv 到 EAGAIN，绝不在无数据时阻塞事件循环。
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            flags = 0;
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        m_fd = fd;
        m_ifIndex = idx;
        m_localMac = mac;
        m_mtu = mtu;

        // 建 RX 环。失败不是错误——静默回退到逐帧 recvfrom（m_ring 保持 nullptr）。
        setupRxRing();

        // 环（或回退路径）已就位，放开临时的「全丢」过滤器，恢复 open() 的对外语义。
        detachFilter(m_fd);

        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        // Qt6 的 activated 只剩 (QSocketDescriptor, Type) 一个重载，取地址无歧义；
        // 用零参 lambda 转发（尾随实参可丢弃）。
        connect(m_notifier, &QSocketNotifier::activated, this, [this]() { onReadable(); });
        return true;
    }

    void close() override
    {
        if (m_notifier) {
            delete m_notifier; // 先删 Notifier，停止对 fd 的监听，再拆环、关 fd
            m_notifier = nullptr;
        }
#ifdef COAST_HAVE_TPACKET_V3
        if (m_ring) {
            // 先 munmap 再 close：环页由内核持有，socket 关闭时才真正释放；反过来做会在
            // 已释放的映射上留一个悬空 VMA 的窗口。不需要显式发「销毁环」的 setsockopt——
            // ::close(fd) 会把 rx_ring 一并拆掉。
            ::munmap(m_ring, kRingBytes);
            m_ring = nullptr;
            m_blockIdx = 0;
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

    bool send(const QByteArray &frame) override
    {
        if (m_fd < 0 || frame.size() < 14) // 至少要够以太头
            return false;

        // 发送地址结构：内核按 sll_ifindex 选口，sll_halen/sll_addr 给目的 MAC（= 帧头前 6 字节）。
        // 注意发方**不走环**（我们只装了 RX_RING，没装 TX_RING）——sendto 语义一如既往。
        struct sockaddr_ll sll;
        std::memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = m_ifIndex;
        sll.sll_halen = 6;
        std::memcpy(sll.sll_addr, frame.constData(), 6); // dst MAC

        const ssize_t n = ::sendto(m_fd, frame.constData(), static_cast<size_t>(frame.size()), 0,
                                   reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll));
        return n == static_cast<ssize_t>(frame.size());
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
        if (valid.isEmpty()) {
            // 没有被劫持设备：直接全丢（ret 0）。收方无它用，越早在内核丢越省 CPU。
            code.push_back(BPF_STMT(BPF_RET | BPF_K, 0));
        } else {
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
    // Notifier 就绪回调的统一入口：走环或走回退路径，并做一层重入保护。
    // 为什么要防重入：槽里会一路跑到 lwIP / SOCKS，理论上不该转事件循环，但万一某天有人在下游
    // 引入了嵌套事件循环，level-triggered 的 QSocketNotifier 会在同一个 fd 上再次触发，
    // 于是同一个块被走两遍、还给内核两次——那是内存被内核就地覆写却仍在读的悬垂场景。一个 bool 的代价。
    void onReadable()
    {
        if (m_inDrain)
            return;
        m_inDrain = true;
#ifdef COAST_HAVE_TPACKET_V3
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

#ifdef COAST_HAVE_TPACKET_V3
    // 建 TPACKET_v3 RX 环。任何一步失败都返回 false ⇒ 调用方保持 m_ring == nullptr ⇒ 走 drainSocket。
    bool setupRxRing()
    {
        int ver = TPACKET_V3;
        if (::setsockopt(m_fd, SOL_PACKET, PACKET_VERSION, &ver, sizeof(ver)) < 0)
            return false; // 内核 < 3.2 / 不支持 V3 → 回退

        struct tpacket_req3 req;
        std::memset(&req, 0, sizeof(req)); // tp_sizeof_priv / tp_feature_req_word 保持 0
        req.tp_block_size = kRingBlockSize;
        req.tp_block_nr = kRingBlockNr;
        req.tp_frame_size = kRingFrameSize;
        req.tp_frame_nr = kRingFrameNr;
        req.tp_retire_blk_tov = kRingRetireTovMs;
        if (::setsockopt(m_fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0)
            return false; // 参数被拒 / 连续内存不足（ENOMEM）→ 回退

        void *p = ::mmap(nullptr, kRingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
        if (p == MAP_FAILED) {
            // ★ 必须把已经建好的环拆掉再回退。装了 RX_RING 的 socket，收方**只往环里投**
            //   （内核把 prot_hook.func 从 packet_rcv 换成了 tpacket_rcv），普通接收队列永远是空的
            //   —— 留着一个没人 mmap 的环，drainSocket 会一帧都读不到，网关直接哑掉。
            //   传全 0 的 req3 即为「销毁环」（内核在 tp_block_nr==0 分支要求 tp_frame_nr 也是 0）；
            //   此时 po->mapped 为 0，不会 EBUSY。
            struct tpacket_req3 zero;
            std::memset(&zero, 0, sizeof(zero));
            ::setsockopt(m_fd, SOL_PACKET, PACKET_RX_RING, &zero, sizeof(zero));
            return false;
        }

        m_ring = static_cast<unsigned char *>(p);
        m_blockIdx = 0;
        return true;
    }

    // 收方主路径：一次唤醒把所有「已就绪」的块处理干净，每块内部顺着 tp_next_offset 链表走帧。
    //
    // 索引同步（写错就要么丢包、要么空转烧 CPU，务必看懂）：
    //   内核和我们各自维护一个块游标，都从 0 开始、都严格 +1 取模 kRingBlockNr 地推进。内核的
    //   poll 就绪条件正是「队首块的 block_status != TP_STATUS_KERNEL」——和这里 break 的判据是
    //   同一个字段同一个块。所以只要我们「每消费一块就恰好推进一格、且绝不跳过」，
    //   level-triggered 的 QSocketNotifier 就不会出现「说就绪但我们什么都没吃到」的空转。
    void drainRing()
    {
        unsigned consumed = 0;
        for (;;) {
            if (!m_ring)
                return; // 上一块的槽里发生了 close()（重配/摘卡）：环已 munmap，立刻停手
            unsigned char *blkStart = m_ring + std::size_t(m_blockIdx) * kRingBlockSize;
            const unsigned char *blkEnd = blkStart + kRingBlockSize;
            auto *bd = reinterpret_cast<struct tpacket_block_desc *>(blkStart);

            // acquire：内核是「先写满帧内容，再置 block_status」；我们必须反过来「先读到状态，
            // 再读内容」，中间隔一次 acquire 屏障。arm64 这类弱序架构上少了它就可能读到还没写完的帧
            // （x86 上是空操作，但 CI 要出 arm64 包，不能省）。
            const __u32 status = __atomic_load_n(&bd->hdr.bh1.block_status, __ATOMIC_ACQUIRE);
            if ((status & TP_STATUS_USER) == 0)
                break; // 队首块还归内核 → 本轮结束

            const __u32 num = bd->hdr.bh1.num_pkts;
            const unsigned char *p = blkStart + bd->hdr.bh1.offset_to_first_pkt;
            for (__u32 i = 0; i < num; ++i) {
                if (!m_ring)
                    return; // 槽里发生了 close()：环已 munmap，连下面还块的写都不能做
                // 越界自保：正常情况下内核给的偏移一定落在块内，这几步比较是防「块被写坏」时
                // 我们拿着野指针去 emit（那可是直接读内核给的共享内存）。都写成「差值 vs 长度」
                // 而不是「指针 + 长度」，免得 caplen/next 是垃圾值时指针加法先溢出。
                if (p < blkStart || p >= blkEnd
                    || std::size_t(blkEnd - p) < sizeof(struct tpacket3_hdr))
                    break;
                const auto *th = reinterpret_cast<const struct tpacket3_hdr *>(p);
                const unsigned char *data = p + th->tp_mac; // tp_mac = 相对本帧头的以太头偏移
                const __u32 caplen = th->tp_snaplen;        // 实际抓到的字节数（不是原始 tp_len）
                if (data < blkStart || data >= blkEnd)
                    break;
                if (caplen > 0 && caplen <= std::size_t(blkEnd - data)) {
                    // ★ 零拷贝：QByteArray 直接指向环内存，不分配、不 memcpy。
                    //   合法性依赖两条契约（见 IL2Endpoint.h）：帧仅在槽内有效；连接必须是直连。
                    //   本块要到下面 __atomic_store_n 把它还给内核之后才可能被覆写，而那一步在
                    //   本循环结束之后——所以槽执行期间指针一定有效。
                    emit frameReceived(QByteArray::fromRawData(
                        reinterpret_cast<const char *>(data), static_cast<qsizetype>(caplen)));
                }
                const __u32 next = th->tp_next_offset;
                if (next == 0)
                    break; // 链表尾（正常由 num 终止；这里防 next==0 导致原地死循环）
                p += next;
            }

            // release：保证上面对帧内容的读全部完成之后，才把块还给内核——内核拿到 TP_STATUS_KERNEL
            // 会立刻开始往这块里写新帧。写错时机 = 一边读一边被覆写。
            __atomic_store_n(&bd->hdr.bh1.block_status, TP_STATUS_KERNEL, __ATOMIC_RELEASE);
            m_blockIdx = (m_blockIdx + 1) % kRingBlockNr;
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
    bool setupRxRing() { return false; } // 头文件不认识 TPACKET_v3：永远走 drainSocket
#endif // COAST_HAVE_TPACKET_V3

    int m_fd = -1;
    int m_ifIndex = -1;
    int m_mtu = 1500;
    QByteArray m_localMac;
    QSocketNotifier *m_notifier = nullptr;
    bool m_inDrain = false;
#ifdef COAST_HAVE_TPACKET_V3
    unsigned char *m_ring = nullptr; // mmap 起点（= 第 0 块）；nullptr = 没建成环，走回退路径
    unsigned m_blockIdx = 0;         // 下一个要消费的块号，必须和内核队首严格同步
#endif
};

} // namespace

IL2Endpoint *createL2Endpoint(QObject *parent)
{
    return new LinuxL2Endpoint(parent);
}

#endif // Q_OS_LINUX（非 Linux：本 TU 为空，工厂由 L2Endpoint_mac.cpp / L2Endpoint_win.cpp 提供）
