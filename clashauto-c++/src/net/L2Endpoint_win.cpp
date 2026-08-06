// Windows 二层端点（Npcap）—— 透明网关在 Windows 上的 IL2Endpoint 实现。
// 用 Npcap(WinPcap 兼容 API)打开网卡、读/写完整以太帧；用 QWinEventNotifier 挂 pcap 事件句柄
// 融入 Qt 事件循环。
//
// 运行前提：目标机需安装 **Npcap 驱动**（含 WinPcap 兼容模式）。未装/无权限 → open 失败 →
// LanGateway.isAvailable()=false（优雅降级）。编译需 **Npcap SDK**（CI 里下载，见 release.yml）。
//
// 头文件次序：winsock2.h 必须在 windows.h/Qt 之前，否则与 winsock.h 冲突。故本文件用编译器内建
// 宏 _WIN32 作外层守卫（不依赖 Qt 的 Q_OS_WIN），并把 winsock2 放最前。
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <pcap.h>

#include "GatewayDiag.h" // 数据面收发帧计数（与 linux/mac 端点对称；将来诊断版发布后 win 也有数据）
#include "IL2Endpoint.h"
#include "NpcapStatus.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>
#include <QWinEventNotifier>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace {
// COAST_GATEWAY_DEBUG=1 时把二层收发的关键节点打到 stderr。GUI 程序看不到 stderr，但
// `Coast.exe 2> gw.txt` 能把它重定向到文件——排查「设备流量为 0」用它定位断点。
bool dbgOn()
{
    static const bool on = qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG");
    return on;
}
void dbg(const char *fmt, ...)
{
    if (!dbgOn())
        return;
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[WINL2] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(ap);
}
} // namespace

namespace {

// —— wpcap.dll 运行时绑定 ——
// 本文件**不与 wpcap 导入库链接**，全部 pcap_* 都在首次用到时 GetProcAddress 拿。
// 理由：Npcap 是可选依赖，没装的机器上 Coast 必须照常启动（只是网关不可用）。原先靠 MSVC 的
// /DELAYLOAD 实现这一点，但那段是 if(MSVC) 守卫的——MinGW 构建会把 wpcap.dll 写进导入表，
// 没装 Npcap 的机器上进程直接 0xC0000135 起不来（本地开发构建长期如此）。运行时绑定对两种
// 工具链都成立，也省掉 delayimp 依赖。
// 函数指针类型一律用 decltype(&pcap_xxx) 从 pcap.h 推出来，签名不可能写歪。
struct PcapApi
{
    decltype(&pcap_open_live) open_live = nullptr;
    decltype(&pcap_close) close = nullptr;
    decltype(&pcap_setmintocopy) setmintocopy = nullptr;
    decltype(&pcap_getevent) getevent = nullptr;
    decltype(&pcap_sendpacket) sendpacket = nullptr;
    decltype(&pcap_next_ex) next_ex = nullptr;
    decltype(&pcap_stats) stats = nullptr;
    decltype(&pcap_compile) compile = nullptr;
    decltype(&pcap_setfilter) setfilter = nullptr;
    decltype(&pcap_freecode) freecode = nullptr;
    decltype(&pcap_setbuff) setbuff = nullptr; // 可选：加大驱动内核收缓冲（缺了不降级）
    // 可选：批量发送队列（WinPcap 兼容 API，Npcap 全都导出）。缺任何一个 → 退回逐帧 sendpacket。
    decltype(&pcap_sendqueue_alloc) sq_alloc = nullptr;
    decltype(&pcap_sendqueue_destroy) sq_destroy = nullptr;
    decltype(&pcap_sendqueue_queue) sq_queue = nullptr;
    decltype(&pcap_sendqueue_transmit) sq_transmit = nullptr;
};

// 驱动内核收缓冲字节数。pcap_open_live 用的是 WinPcap/Npcap 的默认值（历史上 1 MiB 级），
// 十几台设备一起下载时**收方**方向很容易在内核里溢出——溢出即静默丢设备发来的帧，lwIP 那条
// TCP 只能等对端重传，表现就是「时好时坏 / 极慢」。加大到 4 MiB，与当年 lwipopts.h 里 PBUF_POOL
// （入站帧缓冲）同量级。代价是驱动占的非分页内存，家用几十台设备的规模可接受。
// 注意这是**收**缓冲；发方(pcap_sendpacket)是同步写驱动 TX 缓冲，没有对应的用户态旋钮，也没有
// linux/mac 那种 EAGAIN+可写事件的流控模型，故 win 侧不做「发方积压队列」（做不了也不需要）。
constexpr int kRxKernelBufBytes = 4 * 1024 * 1024;
// 发方丢帧告警节流（与 linux/mac 端点一致）。
constexpr qint64 kDropReportMinIntervalMs = 30000;

// —————————————————————— 批量发送（本文件最重要的一处性能改动）——————————————————————
// 背景与证据见 IL2Endpoint::flushTx 的注释：逐帧 pcap_sendpacket 实测约 306 µs/帧的工作线程
// 墙钟，且让 WiFi 驱动无从做 A-MPDU 聚合。改成把一批帧灌进 pcap_send_queue 一次提交：
// NPF 的 NPF_BufferedWrite 在**内核里**遍历这批包、逐个 NdisFSendNetBufferLists 发出去，
// 最后只等**一次**全部完成——用户/内核切换从 N 次变 1 次，空口上驱动也终于同时看得到多个包。
//
// 一批发多少**不设固定上限**，由 TxWorker 的队列深度自然决定（忙时攒得多、闲时一帧就发）。
// 固定批次在 WiFi 上有个坑：万一聚合没如期发生，64 帧 × 306 µs 就是 20 ms 的块状阻塞，比逐帧
// 还糟。自调节没有这个失效模式。队列容量 256 KiB ≈ 170 个满帧，灌满就先提交一次、剩下的下一轮。
constexpr u_int kTxQueueBytes = 256 * 1024;

// 应急开关：COAST_GW_TXBATCH=0 → 不开发送线程、不建发送队列，完全退回「工作线程逐帧同步
// pcap_sendpacket」，即本轮改动之前的行为。留它是因为这条路只能在真机上验证，也方便用同一个
// 构建做 A/B（对比两次的 usPerTx / fpb / tx 帧率峰值）。
bool txThreadEnabled()
{
    static const bool on = qEnvironmentVariable("COAST_GW_TXTHREAD") != QLatin1String("0");
    return on;
}
bool txBatchForcedOff()
{
    static const bool off = qEnvironmentVariable("COAST_GW_TXBATCH") == QLatin1String("0");
    return off;
}

// 收帧线程的应急开关：COAST_GW_RXTHREAD=0 → 回到 QWinEventNotifier + 工作线程 drain()
// （本轮改动之前的行为）。与 TX 的开关分开，两个方向可以各自 A/B。
bool rxThreadEnabled()
{
    static const bool on = qEnvironmentVariable("COAST_GW_RXTHREAD") != QLatin1String("0");
    return on;
}

// 单调微秒时钟。只服务诊断计数，进程级一个实例。
qint64 nowUs()
{
    static QElapsedTimer t = [] {
        QElapsedTimer x;
        x.start();
        return x;
    }();
    return t.nsecsElapsed() / 1000;
}

// 队列上限（帧）。驱动真卡住时不能让内存无限涨；撞上限就丢，交 TCP 重传兜底。
// 4096 帧 ≈ 6 MB 满帧，按实测 306 µs/帧算够驱动消化 1.2 秒 —— 到这个深度还没排空，
// 说明网卡已经出问题了，继续攒只会让延迟更离谱。
constexpr int kTxQueueMaxFrames = 4096;

// ————————————————————————— 发送线程（本文件的第二处性能改动）—————————————————————————
// 为什么必须**离开工作线程**：
//   pcap_sendpacket 那 306 µs/帧（实测，见 flushTx 注释）几乎全是驱动里等空口发送完成的时间，
//   不是 CPU。它挂在工作线程上的后果是灾难性的 —— 峰值 1263 帧/秒 × 306 µs = 39% 的线程墙钟，
//   而这根线程还要跑收帧、lwIP、上百条到 mihomo 的 socket、ARP 投毒。于是：
//     · 泵周期从 25 ms 塌到 45–120 ms（实测），lwIP 的重传/延迟 ACK 节奏跟着变粗；
//     · Npcap 收帧通知器在 Qt 的 Windows 事件分发里本来就排在所有 socket 消息之后，
//       线程一忙，设备的 ACK 要多躺几十毫秒才被读走，直接压死对端的发送窗口；
//     · 整个日志跨多天从未突破 1263 帧/秒 —— 那是**帧率**天花板，正是「每帧一次阻塞系统调用」
//       的特征。
//   把发送整个挪走之后，工作线程上就只剩纯 CPU 的活（lwIP + SOCKS 转发），不再有任何阻塞点。
//
// 批次是**自调节**的，不需要调参：线程忙着提交时新帧在队列里自然攒起来，下一轮一次全取走；
// 空闲时队列里只有一帧，就单帧发出去（延迟最优）。所以既拿到了驱动侧的聚合收益（一次
// sendqueue 提交让 WiFi 驱动同时看到多帧、能做 A-MPDU），又不会像固定批次那样在低负载时
// 平白引入排队延迟 —— 之前那版固定 64 帧的批次在 WiFi 上有「聚合没发生就变成 20 ms 块状
// 阻塞」的风险，自调节把这个风险一起消掉了。
//
// 专用发送句柄：pcap_t 不是线程安全的，绝不能和收帧共用一个。第二个句柄开不出来就整个
// 降级回「工作线程同步发送」（= 本改动之前的行为），不冒险。
// —— Packet.dll（Npcap 底层 API）的三个符号 —— 定义放在 TxWorker 之前（它 submit 要用）。
// 加载逻辑在下面 packetApi()（挨着 pcap 的运行时绑定）。
struct PacketApi
{
    void *(*openAdapter)(const char *) = nullptr;
    void (*closeAdapter)(void *) = nullptr;
    int (*sendPackets)(void *, void *, unsigned long, int) = nullptr;
    // PacketSetLoopbackBehavior —— 可选（老版本 Packet.dll 可能没有），拿不到就跳过。
    int (*setLoopback)(void *, unsigned int) = nullptr;
};
#define COAST_NPF_DISABLE_LOOPBACK 1u

class TxWorker final : public QThread
{
public:
    // adapter 非空 → 用 PacketSendPackets 批量提交（快 53 倍，见 PacketApi 注释）；
    // 空 → 退回逐帧 pcap_sendpacket（COAST_GW_TXBATCH=0 / 没装 Packet.dll / 打不开时）。
    TxWorker(const PcapApi *api, pcap_t *pcap, void *adapter, const PacketApi *pkt)
        : m_api(api), m_pcap(pcap), m_adapter(adapter), m_pkt(pkt)
    {
    }

    // —— 以下三个由**工作线程**调用 ——
    // 入队一帧。QByteArray 是隐式共享 + 原子引用计数，跨线程传所有权安全（只读不改）。
    bool post(const QByteArray &frame)
    {
        QMutexLocker lk(&m_mutex);
        if (m_pending.size() >= kTxQueueMaxFrames) {
            ++m_stDropped;
            return false;
        }
        m_pending.append(frame);
        m_cv.wakeOne();
        return true;
    }
    void kick()
    {
        QMutexLocker lk(&m_mutex);
        m_cv.wakeOne();
    }
    // 把发送线程攒下的统计取回来，由工作线程折进 GatewayDiag —— 这样 GatewayDiag::c
    // 依然只被工作线程碰，头文件里那条「单线程前提」的约定不用破。
    void takeStats(qint64 *frames, qint64 *bytes, qint64 *dropped, qint64 *sendUs, qint64 *batches)
    {
        QMutexLocker lk(&m_mutex);
        *frames = m_stFrames;   m_stFrames = 0;
        *bytes = m_stBytes;     m_stBytes = 0;
        *dropped = m_stDropped; m_stDropped = 0;
        *sendUs = m_stSendUs;   m_stSendUs = 0;
        *batches = m_stBatches; m_stBatches = 0;
    }
    // 停机：**必须排空再退**。ArpSpoofer 的「还原 ARP」就在最后那批里，丢了等于把被代理
    // 设备的网关永久指向一台已经不转发的机器（彻底断网直到缓存老化）。
    void stopAndJoin()
    {
        {
            QMutexLocker lk(&m_mutex);
            m_stop = true;
            m_cv.wakeOne();
        }
        wait();
    }

protected:
    void run() override
    {
        QVector<QByteArray> batch;
        for (;;) {
            {
                QMutexLocker lk(&m_mutex);
                while (!m_stop && m_pending.isEmpty())
                    m_cv.wait(&m_mutex);
                if (m_pending.isEmpty() && m_stop)
                    return; // 只有队列真空了才退（stopAndJoin 的契约）
                batch.swap(m_pending);
                m_pending.clear();
            }
            submit(batch); // 不持锁：提交要几百微秒到几毫秒，持锁会把工作线程堵在 post 上
            batch.clear();
        }
    }

private:
    void submit(const QVector<QByteArray> &frames)
    {
        const qint64 t0 = nowUs();
        qint64 okFrames = 0, okBytes = 0, dropped = 0, batches = 0;

        if (m_adapter && m_pkt) {
            // 一次 PacketSendPackets 提交多帧。缓冲格式与 WinPcap send queue 一致：连续的
            // (pcap_pkthdr + 帧数据)，无对齐填充 —— PacketSendPackets 就是逐个按 caplen 走过去的。
            constexpr int kCap = 256 * 1024; // 单次提交上限，太大内核会拒
            int i = 0;
            while (i < frames.size()) {
                m_buf.resize(0);
                int n = 0;
                qint64 bytes = 0;
                while (i < frames.size()) {
                    const QByteArray &f = frames.at(i);
                    const int need = int(sizeof(pcap_pkthdr)) + f.size();
                    if (!m_buf.isEmpty() && m_buf.size() + need > kCap)
                        break; // 这批满了，先发掉，剩下的下一轮
                    pcap_pkthdr hdr;
                    std::memset(&hdr, 0, sizeof hdr);
                    hdr.caplen = bpf_u_int32(f.size());
                    hdr.len = hdr.caplen;
                    m_buf.append(reinterpret_cast<const char *>(&hdr), sizeof hdr);
                    m_buf.append(f);
                    ++n;
                    bytes += f.size();
                    ++i;
                }
                const int sent = m_pkt->sendPackets(m_adapter, m_buf.data(),
                                                    static_cast<unsigned long>(m_buf.size()), 0);
                ++batches;
                if (sent >= m_buf.size()) { // 返回值是已提交字节数（含 header）
                    okFrames += n;
                    okBytes += bytes;
                } else {
                    dropped += n; // 短发：断在第几帧无从得知，整批记丢，交 TCP 重传
                }
            }
        } else {
            for (const QByteArray &f : frames) {
                if (sendOneRaw(f)) { ++okFrames; okBytes += f.size(); }
                else ++dropped;
                ++batches;
            }
        }

        const qint64 us = nowUs() - t0;
        QMutexLocker lk(&m_mutex);
        m_stFrames += okFrames;
        m_stBytes += okBytes;
        m_stDropped += dropped;
        m_stSendUs += us;
        m_stBatches += batches;
    }
    bool sendOneRaw(const QByteArray &f)
    {
        return m_api->sendpacket(m_pcap, reinterpret_cast<const u_char *>(f.constData()), f.size())
                == 0;
    }

    const PcapApi *m_api;
    pcap_t *m_pcap;             // 逐帧降级路径用（pcap_sendpacket）；批量路径不碰它
    void *m_adapter;            // Packet.dll 的 LPADAPTER；批量提交走它。空 → 逐帧
    const PacketApi *m_pkt;     // Packet.dll 符号；空 → 逐帧
    QByteArray m_buf;           // 批量缓冲，复用避免每轮重分配
    QMutex m_mutex;
    QWaitCondition m_cv;
    QVector<QByteArray> m_pending;
    bool m_stop = false;
    qint64 m_stFrames = 0, m_stBytes = 0, m_stDropped = 0, m_stSendUs = 0, m_stBatches = 0;
};

// ————————————————————————— 收帧线程 —————————————————————————
// 为什么收帧也必须离开工作线程（和发送是两个独立的病因）：
//   收帧本来是事件驱动的（Npcap 事件句柄 + QWinEventNotifier + mintocopy(1)，来一字节就该唤醒）。
//   但 Qt 的 Windows 事件分发把 winEventNotifier 放在消息派发的**最后一档** —— 只有 PeekMessage
//   彻底取空才轮到它。而工作线程上挂着几十上百条到 mihomo 的 QTcpSocket，每条可读都是一条
//   窗口消息。于是收帧的优先级低于**每一条** socket 事件。
//   真机实证：重载窗口里 pump 与 wakes 逐窗口相等（如 pump=204 wakes=203），一轮事件循环各服务
//   一次，排空周期塌到 49 ms。设备发出的每个包 —— 包括每个 TCP ACK、每个新连接的 SYN —— 平均
//   多躺半个周期。ACK 晚到直接压死对端发送窗口，这是「设备比本机慢」的另一半原因。
//
// 挪到独立线程之后：pcap_next_ex 在自己的线程上阻塞等（to_ms=1 的等待此时不再是成本，正是
// 我们想要的「有帧就醒」），帧一到就深拷贝进批，整批一次投递给工作线程。工作线程侧仍然是
// 一次 posted event 处理一批，**每帧一次信号派发的语义完全不变**。
//
// 反压（这一段不能省）：工作线程要是跟不上，投递出去的批会在事件队列里无限堆积、吃光内存。
// 所以记「已投递未消费」的帧数，超过高水位就让收帧线程**等**（不是丢！）—— 让帧压在 Npcap
// 的 4 MiB 内核缓冲里，那里满了才丢，且丢在内核、有 tp_drops 类计数可查。用户态静默丢帧是
// 这条链路上最难查的故障，绝不引入。
class RxWorker final : public QThread
{
public:
    // 高/低水位（帧）。8192 帧 ≈ 12 MB 满帧，足够扛住工作线程一次几十毫秒的卡顿；
    // 迟滞是为了别在水位线上反复唤醒。
    static constexpr int kHighWater = 8192;
    static constexpr int kLowWater = 4096;

    RxWorker(const PcapApi *api, pcap_t *pcap, HANDLE evt, QObject *target,
             std::function<void(const QVector<QByteArray> &, qint64)> sink)
        : m_api(api), m_pcap(pcap), m_evt(evt), m_target(target), m_sink(std::move(sink))
    {
    }

    // 工作线程消费完一批后调，放行可能正在等水位的收帧线程。
    void consumed(int frames)
    {
        QMutexLocker lk(&m_mutex);
        m_outstanding -= frames;
        if (m_outstanding <= kLowWater)
            m_cv.wakeOne();
    }
    void takeStats(qint64 *wakes, qint64 *us)
    {
        QMutexLocker lk(&m_mutex);
        *wakes = m_stWakes; m_stWakes = 0;
        *us = m_stUs;       m_stUs = 0;
    }
    void stopAndJoin()
    {
        {
            QMutexLocker lk(&m_mutex);
            m_stop = true;
            m_cv.wakeAll(); // 可能正卡在水位上
        }
        wait();
    }

protected:
    void run() override
    {
        // 一次最多攒多少帧再投递。太小 → posted event 太多；太大 → 首帧等太久。
        // 256 帧在实测的 300 帧/秒量级下约等于「有多少收多少」，重载时才会真的攒满。
        constexpr int kBatchMax = 256;
        QVector<QByteArray> batch;
        batch.reserve(kBatchMax);
        qint64 batchBytes = 0;

        for (;;) {
            {
                QMutexLocker lk(&m_mutex);
                if (m_stop)
                    return;
                // 反压：等工作线程把积压消化到低水位。绝不丢帧（见类注释）。
                while (!m_stop && m_outstanding > kHighWater)
                    m_cv.wait(&m_mutex);
                if (m_stop)
                    return;
            }

            const qint64 t0 = nowUs();
            struct pcap_pkthdr *hdr = nullptr;
            const u_char *data = nullptr;
            int r;
            // ★ 别指望 next_ex 的 to_ms 当阻塞原语：它在空缓冲上很快就返回 0（实测这个循环
            //   空转到 1640 次/秒，白烧 ~9% 的一个核，还把 rxWakes/fpw 又一次冲成无意义的数）。
            //   正确做法是**空转了才去等 pcap 的事件句柄**：有帧时一轮都不多等（next_ex 直接
            //   返回数据），没帧时阻塞在 WaitForSingleObject 上，事件一来立刻醒。
            //   100 ms 只是「没帧时多久回来看一眼 m_stop」的上限，不影响收帧延迟。
            while ((r = m_api->next_ex(m_pcap, &hdr, &data)) == 1) {
                if (data && hdr) {
                    // ★ 深拷贝：帧要跨线程，绝不能把指向 Npcap 内核缓冲的视图传出去
                    //   （IL2Endpoint::frameReceived 的三条硬约束之一）。
                    batch.append(QByteArray(reinterpret_cast<const char *>(data), hdr->caplen));
                    batchBytes += hdr->caplen;
                }
                if (batch.size() >= kBatchMax)
                    break;
            }
            const qint64 us = nowUs() - t0;

            if (batch.isEmpty()) {
                // 这一轮什么都没收到 → 阻塞等事件，别空转。也不记 wake：rxWakes 的语义是
                // 「被唤醒并真的取到帧的次数」，fpw(帧/唤醒) 才有意义。
                if (m_evt)
                    WaitForSingleObject(m_evt, 100);
                else
                    msleep(1); // 拿不到事件句柄时的兜底，至少别 100% 占核
                continue;
            }
            {
                QMutexLocker lk(&m_mutex);
                ++m_stWakes;
                m_stUs += us;
                m_outstanding += batch.size();
            }
            // 一次 posted event 投递整批。QVector<QByteArray> 的拷贝只是一串引用计数自增。
            {
                auto sink = m_sink;
                const QVector<QByteArray> payload = batch;
                const qint64 bytes = batchBytes;
                QMetaObject::invokeMethod(
                    m_target, [sink, payload, bytes] { sink(payload, bytes); },
                    Qt::QueuedConnection);
                batch.clear();
                batchBytes = 0;
            }
            if (r < 0) {
                // 句柄出错（网卡拔了）。别空转刷屏，退出；close() 会收尾。
                dbg("rx thread: next_ex 返回 %d，退出", r);
                return;
            }
        }
    }

private:
    const PcapApi *m_api;
    pcap_t *m_pcap;
    HANDLE m_evt;   // pcap_getevent：空转时阻塞在它上面
    QObject *m_target;
    std::function<void(const QVector<QByteArray> &, qint64)> m_sink;
    QMutex m_mutex;
    QWaitCondition m_cv;
    bool m_stop = false;
    int m_outstanding = 0;
    qint64 m_stWakes = 0, m_stUs = 0;
};

// —— Packet.dll（Npcap 底层 API）运行时绑定 ——
// 为什么不用 pcap_sendqueue_transmit：它走 NPF 的 BufferedWrite，实测在 **WiFi** 网卡上会丢帧
// （sq_transmit 返回成功、帧却不上空口，lwIP 只能狂重传）。而 Packet.dll 的 PacketSendPackets
// 是**另一条内核路径**，把一批帧一次性交给驱动逐个发。独立探针实测（同一块 WiFi 网卡）：
//     逐帧 pcap_sendpacket   738 µs/帧    2.0 MB/s
//     PacketSendPackets(256) 12.3 µs/帧  115.9 MB/s   —— 53 倍
// 根因是逐帧每次一个 DeviceIoControl 内核往返，WiFi 上要 ~700µs；批量一次往返摊薄到十几 µs。
// 头文件 Packet32.h 与 pcap.h 的 bpf_program 定义冲突，故这里**手动声明**需要的三个符号、不 include。
// 64 位下只有一种调用约定，函数指针签名不必操心 __cdecl/__stdcall。
const PacketApi *packetApi()
{
    static bool tried = false;
    static PacketApi api;
    static bool ok = false;
    if (tried)
        return ok ? &api : nullptr;
    tried = true;
    // ★ Packet.dll 在 %SystemRoot%\System32\Npcap\，**不在默认 DLL 搜索路径**。裸
    //   LoadLibraryA("Packet.dll") 会失败 —— 这正是之前 fpb=1（批量没生效、退回逐帧）的原因：
    //   pcapApi() 加载 wpcap 时特意 SetDllDirectory 到 Npcap 目录再还原，我这里漏了这一步。
    //   而 wpcap.dll 依赖 Packet.dll，pcapApi() 成功后它一定**已经加载**了，所以先用
    //   GetModuleHandle 按基名直接拿到已加载的那份（确定命中，且不增引用计数）。
    HMODULE h = GetModuleHandleA("Packet.dll");
    if (!h) {
        // 兜底：万一没被依赖带进来，按 Npcap 全路径加载。
        char sysdir[MAX_PATH] = {0};
        if (GetSystemDirectoryA(sysdir, MAX_PATH)) {
            const QByteArray full = QByteArray(sysdir) + "\\Npcap\\Packet.dll";
            h = LoadLibraryA(full.constData());
        }
        if (!h)
            h = LoadLibraryA("Packet.dll");
    }
    if (!h)
        return nullptr;
    api.openAdapter = reinterpret_cast<decltype(api.openAdapter)>(GetProcAddress(h, "PacketOpenAdapter"));
    api.closeAdapter =
        reinterpret_cast<decltype(api.closeAdapter)>(GetProcAddress(h, "PacketCloseAdapter"));
    api.sendPackets =
        reinterpret_cast<decltype(api.sendPackets)>(GetProcAddress(h, "PacketSendPackets"));
    // 可选符号：拿不到不影响可用性（只是少一个优化），故不进 ok 的判据。
    api.setLoopback =
        reinterpret_cast<decltype(api.setLoopback)>(GetProcAddress(h, "PacketSetLoopbackBehavior"));
    ok = api.openAdapter && api.closeAdapter && api.sendPackets;
    return ok ? &api : nullptr;
}

// 加载并解析 wpcap.dll。返回 nullptr = 没装 Npcap（或版本太老、缺符号）。
// 只做一次；DLL 句柄故意不释放（进程生命周期内一直要用）。
const PcapApi *pcapApi()
{
    static bool tried = false;
    static PcapApi api;
    static bool ok = false;
    if (tried)
        return ok ? &api : nullptr;
    tried = true;

    // Npcap 的 DLL 装在 %SystemRoot%\System32\Npcap\，不在默认搜索路径上，先把它加进去。
    // 用 AddDllDirectory 语义的 LOAD_LIBRARY_SEARCH_* 更干净，但那要求 LoadLibraryEx 带 flag
    // 且系统支持；这里沿用 SetDllDirectory + LoadLibrary，行为与旧代码一致。
    char sysdir[MAX_PATH] = {0};
    if (GetSystemDirectoryA(sysdir, MAX_PATH)) {
        const QByteArray npcapDir = QByteArray(sysdir) + "\\Npcap";
        SetDllDirectoryA(npcapDir.constData());
    }
    HMODULE h = LoadLibraryA("wpcap.dll");
    SetDllDirectoryA(nullptr); // 还原进程级 DLL 搜索路径，别留副作用给别的模块
    if (!h)
        return nullptr;

    auto sym = [h](const char *name) { return GetProcAddress(h, name); };
    api.open_live = reinterpret_cast<decltype(&pcap_open_live)>(sym("pcap_open_live"));
    api.close = reinterpret_cast<decltype(&pcap_close)>(sym("pcap_close"));
    api.setmintocopy = reinterpret_cast<decltype(&pcap_setmintocopy)>(sym("pcap_setmintocopy"));
    api.getevent = reinterpret_cast<decltype(&pcap_getevent)>(sym("pcap_getevent"));
    api.sendpacket = reinterpret_cast<decltype(&pcap_sendpacket)>(sym("pcap_sendpacket"));
    api.next_ex = reinterpret_cast<decltype(&pcap_next_ex)>(sym("pcap_next_ex"));
    api.stats = reinterpret_cast<decltype(&pcap_stats)>(sym("pcap_stats"));
    api.compile = reinterpret_cast<decltype(&pcap_compile)>(sym("pcap_compile"));
    api.setfilter = reinterpret_cast<decltype(&pcap_setfilter)>(sym("pcap_setfilter"));
    api.freecode = reinterpret_cast<decltype(&pcap_freecode)>(sym("pcap_freecode"));
    api.setbuff = reinterpret_cast<decltype(&pcap_setbuff)>(sym("pcap_setbuff"));
    api.sq_alloc = reinterpret_cast<decltype(&pcap_sendqueue_alloc)>(sym("pcap_sendqueue_alloc"));
    api.sq_destroy =
        reinterpret_cast<decltype(&pcap_sendqueue_destroy)>(sym("pcap_sendqueue_destroy"));
    api.sq_queue = reinterpret_cast<decltype(&pcap_sendqueue_queue)>(sym("pcap_sendqueue_queue"));
    api.sq_transmit =
        reinterpret_cast<decltype(&pcap_sendqueue_transmit)>(sym("pcap_sendqueue_transmit"));

    // 缺任何一个都当没装：宁可优雅降级，也不要半残地跑到一半崩。
    // setbuff / sq_* **不在**必需清单里——前者是收方优化、后者是发方优化，
    // 老 wpcap 缺了只是少一层加速，逐帧路径照常工作。
    ok = api.open_live && api.close && api.setmintocopy && api.getevent && api.sendpacket
            && api.next_ex && api.compile && api.setfilter && api.freecode;
    return ok ? &api : nullptr;
}

// 由 QNetworkInterface::name() 反查这张网卡的 Npcap 设备名与 MAC。
//
// ★ 这里有个足以让整个网关在 Windows 上永远打不开的坑：**Qt 6 的 QNetworkInterface::name()
//   在 Windows 上返回的不是适配器 GUID**，而是 LUID 名（ConvertInterfaceLuidToNameW 的结果，
//   形如 "ethernet_32775"）；只有取不到 LUID 名时才退回 IP_ADAPTER_ADDRESSES::AdapterName
//   （那个才是 "{XXXXXXXX-....}"）。而 Npcap 的设备名固定是 \Device\NPF_{GUID}。
//   直接把 name() 拼进设备名 ⇒ "\Device\NPF_ethernet_32775" ⇒ 必然打不开。
//   （这个错误此前一直被 Npcap 的 AdminOnly「拒绝访问」挡在前面，看不出来。）
// 所以按三种口径逐个比对，命中后一律用该适配器**真正的 AdapterName(GUID)** 拼设备名。
struct AdapterInfo
{
    QByteArray npfName; // "\Device\NPF_{GUID}"
    QByteArray mac;     // 6 字节；取不到则为空
    bool wifi = false;  // IfType == IF_TYPE_IEEE80211：决定发送走批量还是逐帧，见 open()
    bool ok = false;
};

AdapterInfo adapterFor(const QString &ifname)
{
    AdapterInfo out;
    if (ifname.isEmpty())
        return out;

    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, nullptr, &size)
        != ERROR_BUFFER_OVERFLOW)
        return out;
    std::vector<char> buf(size);
    auto *addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addrs, &size)
        != NO_ERROR)
        return out;

    for (auto *a = addrs; a; a = a->Next) {
        const QString guid = QString::fromLatin1(a->AdapterName);

        wchar_t luidBuf[IF_MAX_STRING_SIZE + 1] = {0};
        QString luidName;
        if (ConvertInterfaceLuidToNameW(&a->Luid, luidBuf, IF_MAX_STRING_SIZE + 1) == NO_ERROR)
            luidName = QString::fromWCharArray(luidBuf);

        const QString friendly =
                a->FriendlyName ? QString::fromWCharArray(a->FriendlyName) : QString();

        const bool hit = ifname.compare(guid, Qt::CaseInsensitive) == 0
                || (!luidName.isEmpty() && ifname.compare(luidName, Qt::CaseInsensitive) == 0)
                || (!friendly.isEmpty() && ifname.compare(friendly, Qt::CaseInsensitive) == 0);
        if (!hit)
            continue;

        out.npfName = QByteArray("\\Device\\NPF_") + guid.toLatin1();
        if (a->PhysicalAddressLength == 6)
            out.mac = QByteArray(reinterpret_cast<const char *>(a->PhysicalAddress), 6);
        out.wifi = (a->IfType == IF_TYPE_IEEE80211);
        out.ok = !guid.isEmpty();
        break;
    }
    return out;
}

class WinL2Endpoint final : public IL2Endpoint
{
public:
    explicit WinL2Endpoint(QObject *parent = nullptr) : IL2Endpoint(parent) {}
    ~WinL2Endpoint() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        // wpcap.dll 运行时绑定（见上面的 pcapApi）：解析不到 = 没装 Npcap，优雅降级
        // （gatewayReady=false），程序其余部分照常。
        m_api = pcapApi();
        if (!m_api) {
            if (err) *err = QStringLiteral("未检测到 Npcap（请安装 Npcap 驱动后重试）");
            return false;
        }

        // ifname 是 QNetworkInterface::name()，在 Win 上可能是 LUID 名而非 GUID —— 必须反查，
        // 详见 adapterFor 的注释。
        const AdapterInfo adapter = adapterFor(ifname);
        if (!adapter.ok) {
            if (err)
                *err = QStringLiteral("系统里找不到网卡 %1（已按 LUID 名/友好名/GUID 三种口径查过）")
                               .arg(ifname);
            return false;
        }

        // ★ 非混杂模式打开（promisc=0）。这是「开代理后本机+设备一起断网」的根因修复：
        //   混杂模式下网卡把**所有**帧上交，Windows 自己的网络栈也会看到我们**单播发给
        //   设备/网关的 ARP 欺骗应答**（sha=本机MAC, spa=网关IP），于是把本机 ARP 缓存也更新成
        //   「网关 → 本机MAC」——本机自投毒，出网流量指回自己 → 本机断网；mihomo 替设备转发
        //   也走同一条已中毒的默认路由 → 被代理设备一起断。两台物理机确定性复现，正是这个。
        //   而这套「先投毒、再抓设备发来的帧」的透明网关**根本不需要混杂**：设备被投毒后，
        //   它发往「网关」的帧目的 MAC 就是本机 MAC，本机网卡与 Npcap 都会正常收到；广播的
        //   ARP 请求也照收。开混杂只是白抓无用帧、还顺手坑了本机。
        //   应急开关：COAST_GATEWAY_PROMISC=1 可强制回到混杂模式（仅调试/排查用）。
        const int promisc = qEnvironmentVariableIsSet("COAST_GATEWAY_PROMISC") ? 1 : 0;
        dbg("open dev=%s promisc=%d", adapter.npfName.constData(), promisc);
        char errbuf[PCAP_ERRBUF_SIZE] = {0};
        m_pcap = m_api->open_live(adapter.npfName.constData(), 65536, promisc, 1 /*ms*/, errbuf);
        if (!m_pcap) {
            // errbuf 里是 Windows 本地化的系统错误文案（zh-CN 下是 GBK），fromLatin1 会拧成乱码。
            const QString raw = QString::fromLocal8Bit(errbuf);
            // 走到这里 wpcap.dll 已经加载成功 = Npcap 装着的，所以旧文案那句「需装 Npcap」必然是
            // 误导。这里按「驱动是否被锁 × 本进程是否提权」分三种情况给出真正能照着做的下一步，
            // 并且**永远**把这两个事实附在末尾——这一带的问题隔着截图很难猜，state 直接写出来
            // 就不用再来回问「你是管理员账户还是右键以管理员身份运行的」。
            const bool locked = npcapstatus::adminOnly();
            const bool elevated = npcapstatus::processElevated();
            const QString state =
                    QStringLiteral("[AdminOnly=%1, 本进程%2提权]")
                            .arg(locked ? QStringLiteral("1") : QStringLiteral("0"),
                                 elevated ? QStringLiteral("已") : QStringLiteral("未"));
            if (err) {
                if (locked && !elevated) {
                    // 绝大多数「总是打开网卡失败」都落在这里：账户是管理员 ≠ 进程被提权，
                    // UAC 开着时双击启动拿到的是受限令牌，照样被 Npcap 挡在门外。
                    *err = QStringLiteral("Npcap 驱动被限制为「仅管理员可访问」（安装时勾选了 "
                                          "Restrict Npcap driver's access to Administrators only）"
                                          "——请在设备页点「修复权限」一键解除。原始错误: ")
                            + raw + QLatin1Char(' ') + state;
                } else if (locked) {
                    *err = QStringLiteral("已提权但仍打不开二层设备——若刚改过 Npcap 权限，"
                                          "需重启 npcap 驱动或重启电脑才生效。原始错误: ")
                            + raw + QLatin1Char(' ') + state;
                } else {
                    *err = QStringLiteral("打不开二层设备: ") + raw + QLatin1Char(' ') + state;
                }
            }
            return false;
        }
        // 先加大驱动内核收缓冲，再武装通知器——必须在开始收包前设好（见 kRxKernelBufBytes）。
        // pcap_setbuff 会清空当前缓冲，此刻还没开始收，无副作用；失败不致命（只是少了这层余量）。
        if (m_api->setbuff && m_api->setbuff(m_pcap, kRxKernelBufBytes) != 0)
            dbg("pcap_setbuff(%d) 失败，沿用驱动默认收缓冲", kRxKernelBufBytes);
        m_api->setmintocopy(m_pcap, 1); // 有 1 字节就触发事件，降低延迟
        m_localMac = adapter.mac;

        // —— 批量发送队列：**只在以太网上用** ——
        // 真机 A/B（同一台手机、同一条 WiFi、同一时刻）：
        //   批量开 → 5.7~176 KB/s、ICMP 丢包 20%、TLS 握手最长 18 秒
        // —— 专用发送句柄 + 发送线程（理由见 TxWorker 的注释）——
        // pcap_t 不是线程安全的，所以**另开一个句柄**给发送线程独占，绝不与收帧共用。
        // to_ms/snaplen 取最小：这个句柄从不读包，snaplen 只影响捕获路径。
        // 任何一步失败都整体降级回「工作线程同步发送」= 本改动之前的行为，不冒险。
        if (txThreadEnabled()) {
            char txerr[PCAP_ERRBUF_SIZE] = {0};
            m_txPcap = m_api->open_live(adapter.npfName.constData(), 128, 0 /*非混杂*/, 1, txerr);
            if (m_txPcap) {
                // 让这个句柄一个包都不捕获：否则内核收缓冲会白白攒满（我们从不读它）。
                struct bpf_program prog;
                if (m_api->compile(m_txPcap, &prog, "greater 65535", 1, PCAP_NETMASK_UNKNOWN) >= 0) {
                    m_api->setfilter(m_txPcap, &prog);
                    m_api->freecode(&prog);
                }
                if (m_api->setbuff)
                    m_api->setbuff(m_txPcap, 64 * 1024); // 收缓冲用不上，压到最小

                // 批量提交走 Packet.dll 的 PacketSendPackets（有线/WiFi 都用它，实测两种介质
                // 都是 10~15 µs/帧且不丢帧；替代了会在 WiFi 上丢帧的 pcap_sendqueue）。
                // COAST_GW_TXBATCH=0 → 不开 adapter，TxWorker 退回逐帧 pcap_sendpacket。
                //
                // ★ 这条路曾被我判成"报成功但一帧不上线"，**是误判，已推翻**（2026-08-03）。
                //   当时的证据是「默认批量 → 靶机侧抓不到投毒帧；COAST_GW_TXBATCH=0 → 抓得到」，
                //   但那次批量侧的取样并不成立（靶机当时没真被接管）。用三条路同时上线的对照实验
                //   重测，同一台机器、同一块网卡、对端抓包计数：
                //       pcap_sendpacket 逐帧            20/20 上线
                //       PacketSendPackets 批量          20/20 上线
                //       pcap_sendqueue_transmit         20/20 上线
                //       PacketSendPackets 批量 200 小帧  200/200
                //       PacketSendPackets 灌满 256 KiB（171 × 1514B）171/171
                //   再用真网关复测（批量为默认）：靶机 45 秒收到 128 个投毒帧，ARP 表被改到本机 MAC，
                //   设备 204 通、出口 IP 是节点的、国内站点 200 —— 批量路径功能完好。
                // ★ 顺带确认缓冲格式无误：Npcap 驱动的 NPF_BufferedWrite 是
                //   `Pos = Pos + sizeof(dump_bpf_hdr) + pHdr->caplen` 逐条前进，**不做任何对齐填充**
                //   （见 npcap packetWin7/npf/npf/Write.c），与这里"连续 (hdr + 帧)、无填充"一致。
                //   MinGW 侧实测 sizeof=16、caplen@8、len@12，与 MSVC 版 Npcap 布局相同。
                // ★ 留下的真教训是测试口径：**批量/逐帧的 A/B 必须先确认两次取样里设备都真被接管**
                //   （日志里要有 `[RESUME] <mac> -> armed`），否则"没抓到帧"量的是别的东西。
                const PacketApi *pkt = packetApi();
                if (pkt && !txBatchForcedOff()) {
                    m_txAdapter = pkt->openAdapter(adapter.npfName.constData());
                    if (!m_txAdapter)
                        dbg("PacketOpenAdapter 失败，退回逐帧发送");
                    // ★ 关掉 loopback —— 实测这是发送路径上最大的一个单项优化（2.6×）。
                    //   默认（WinPcap 兼容）行为会给每个注入帧加 NDIS_SEND_FLAGS_CHECK_FOR_LOOPBACK，
                    //   并且**不豁免本进程自己的抓包实例**：于是每发一帧，Npcap 的 NPF_DoTap() 都要
                    //   为该网卡上每个打开的抓包句柄跑一遍 BPF。而 ARP 网关的收帧句柄是**常开**的，
                    //   所以这条代价在生产里一直在付。
                    //   微基准（bench/npcap_tx3.c，抓包句柄常开、batch=64、1514B、各 3 轮取中位）：
                    //       默认 loopback  39.45 µs CPU/帧   225 Mbps   3.26 核/Gbps
                    //       禁用 loopback  15.23 µs CPU/帧   572 Mbps   1.26 核/Gbps
                    //   功能验证：禁用后靶机 tcpdump 实收 40000/40000，帧照常上线。
                    //   我们本来就不需要捕获自己注入的帧（收帧只关心被劫持设备发出的帧），
                    //   所以这个语义变化对网关无副作用。
                    if (m_txAdapter && pkt->setLoopback)
                        pkt->setLoopback(m_txAdapter, COAST_NPF_DISABLE_LOOPBACK);
                }
                // ★ 把**最终选中的发送路径**写进日志。以前只能从诊断行的 `fpb` 去猜，而
                //   `fpb=1` 有歧义：既可能是"批量没生效、退回逐帧"，也可能是"队列里本来就
                //   只有一帧"。这两者在 WiFi 上差 53 倍（738 µs/帧 vs 12.3 µs/帧），
                //   猜错方向就会去查完全不相干的地方。真机上为这件事绕过一圈。
                GatewayDiag::note("txPath",
                                  QStringLiteral("%1 的发送路径：%2")
                                          .arg(QString::fromLatin1(ifname.toLatin1()),
                                               (pkt && !txBatchForcedOff() && m_txAdapter)
                                                       ? QStringLiteral("PacketSendPackets 批量")
                                                       : QStringLiteral("逐帧 pcap_sendpacket"
                                                                        "（WiFi 上约 738 µs/帧，"
                                                                        "吞吐会被钉在 ~2 MB/s）")),
                                  /*minGapMs=*/60000);

                dbg("发送模式=%s（介质=%s）", m_txAdapter ? "批量(PacketSendPackets)" : "逐帧",
                    adapter.wifi ? "WiFi" : "以太网");
                m_tx = new TxWorker(m_api, m_txPcap, m_txAdapter,
                                    m_txAdapter ? pkt : nullptr);
                m_tx->start();
            } else {
                dbg("发送专用句柄打不开（%s），退回工作线程同步发送", txerr);
            }
        }

        // —— 收帧：独立线程（默认）或 QWinEventNotifier（降级）——
        if (rxThreadEnabled()) {
            m_rx = new RxWorker(m_api, m_pcap, m_api->getevent(m_pcap), this,
                                [this](const QVector<QByteArray> &frames, qint64 bytes) {
                                    onRxBatch(frames, bytes);
                                });
            m_rx->start();
            dbg("rx thread started");
        } else {
            HANDLE h = m_api->getevent(m_pcap);
            if (!h) {
                if (err) *err = QStringLiteral("pcap_getevent 返回空句柄");
                close();
                return false;
            }
            m_notifier = new QWinEventNotifier(h, this);
            // rxWakes 只在**事件驱动**这条路上自增（泵兜底走 rxDrains），fpw 才有意义。
            QObject::connect(m_notifier, &QWinEventNotifier::activated, this, [this](HANDLE) {
                ++GatewayDiag::c.rxWakes;
                drain();
                pollKernelDrops();
            });
            m_notifier->setEnabled(true);
        }
        dbg("open ok, notifier armed, localMac=%s", m_localMac.toHex(':').constData());
        return true;
    }

    // Npcap 内核收缓冲的丢包数。★ 这一栏此前**从未被写入过** —— GatewayDiag.h 的注释只
    //   警告了 macOS「恒 0，别当成没丢包」，漏了 Windows，而 Windows 恰恰是唯一靠抓包
    //   收帧的平台。一个从未被写入的计数器比没有计数器更危险：它会主动提供虚假的排除证据。
    //   今晚排查「8 条并发里固定有一两条 SYN 从未到达用户态栈」时，我至少三次拿
    //   `rxdrop=0` 当作"抓包层没丢帧"的依据而排除了正确方向。
    //
    //   pcap_stats 的 ps_drop 是**自进程启动以来的累计值**，而 GatewayDiag 要的是窗口增量，
    //   所以这里存上次读数、只把差值加进去。
    //
    // ★★ **必须降频**。第一版写成"每次收帧唤醒读一次"，真机当场回归：rx 从 900~1400 帧/10s
    //    掉到 191~534，txKB 从 6972~15501 掉到 6~21，8 条并发下载全部 0 字节。唤醒频率是
    //    每秒几百次，而 pcap_stats 是一次用户态→驱动的 IOCTL —— 它把收帧路径本身拖垮了。
    //    这一栏是**诊断量**，1 秒的粒度绰绰有余（采样窗口本来就是 10 秒）。
    void pollKernelDrops()
    {
        if (!m_api || !m_api->stats || !m_pcap)
            return;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - m_lastStatsMs < 1000)
            return;
        m_lastStatsMs = nowMs;
        pcap_stat st {};
        if (m_api->stats(m_pcap, &st) != 0)
            return;
        const qint64 now = qint64(st.ps_drop);
        if (now > m_lastPsDrop)
            GatewayDiag::c.rxKernelDrops += now - m_lastPsDrop;
        m_lastPsDrop = now; // 驱动重置过就跟着回退，不去猜

        // ★ ps_recv = **通过 BPF 过滤、交给这个句柄**的帧数。它和我们自己数的 rxFrames 一比，
        //   就把「帧压根没到内核抓包」和「到了却在环形缓冲/drain 这一段丢了」切成两半 ——
        //   而这一刀**不依赖 ps_drop**。必须如此：Npcap 的 ps_drop 在不少驱动/模式下根本
        //   不被写入，恒 0；拿它当「没丢帧」的证据，正是本项目栽过的那类假证据
        //   （rxKernelDrops 这一栏在本次会话之前从未被写入过）。两个独立来源互为对照。
        const qint64 rec = qint64(st.ps_recv);
        if (rec > m_lastPsRecv)
            GatewayDiag::c.rxKernelRecv += rec - m_lastPsRecv;
        m_lastPsRecv = rec;
    }

    qint64 m_lastPsDrop = 0;
    qint64 m_lastPsRecv = 0;
    qint64 m_lastStatsMs = 0;
    void close() override
    {
        // 收帧线程持有 m_pcap，必须在 m_api->close(m_pcap) 之前停掉，否则是 use-after-free。
        if (m_rx) {
            m_rx->stopAndJoin();
            delete m_rx;
            m_rx = nullptr;
        }
        if (m_notifier) {
            m_notifier->setEnabled(false);
            delete m_notifier;
            m_notifier = nullptr;
        }
        // ★ 先让发送线程把队列排空再退出，然后才关句柄。停网关时 ArpSpoofer 会发最后一轮
        //   「还原 ARP」，那几帧要是丢了，被代理设备就会一直把网关指着一台已经不转发的机器
        //   ——彻底断网直到邻居缓存老化。stopAndJoin 的契约就是「队列真空了才返回」。
        if (m_tx) {
            m_tx->stopAndJoin();
            qint64 f=0,b=0,d=0,us=0,n=0;
            m_tx->takeStats(&f,&b,&d,&us,&n); // 最后一批的账也要记上
            GatewayDiag::c.txFrames += f;
            GatewayDiag::c.txBytes += b;
            GatewayDiag::c.txSendUs += us;
            GatewayDiag::c.txBatches += n;
            GatewayDiag::c.txDropped += d;
            delete m_tx;
            m_tx = nullptr;
        }
        // 发送线程停干净后才关它用的句柄（PacketOpenAdapter 的 adapter + 逐帧降级的 pcap）。
        if (m_txAdapter) {
            if (const PacketApi *pkt = packetApi())
                pkt->closeAdapter(m_txAdapter);
            m_txAdapter = nullptr;
        }
        if (m_txPcap) {
            m_api->close(m_txPcap);
            m_txPcap = nullptr;
        }
        if (m_pcap) {
            m_api->close(m_pcap); // m_pcap 非空 ⇒ open 成功过 ⇒ m_api 必然有效
            m_pcap = nullptr;
        }
    }
    bool isOpen() const override { return m_pcap != nullptr; }

    // ——— 崩溃兜底通道（IL2Endpoint::panicSender 的契约）———
    // Windows 上没有信号，走的是 SetUnhandledExceptionFilter：崩溃线程同步执行本函数。
    // 严格说 pcap_sendpacket 不是「异常安全」的（它会进驱动、可能取锁），但这是**最后一搏**——
    // 不发这几帧，被代理设备就一直把网关指着这台已经死掉的机器。做完就让异常继续传给系统。
    static void panicSendImpl(void *ctx, const unsigned char *data, int len)
    {
        const PcapApi *api = pcapApi();
        if (!api || !api->sendpacket || !ctx || !data || len < 14)
            return;
        (void)api->sendpacket(static_cast<pcap_t *>(ctx), data, len);
    }
    RawSendFn panicSender() const override { return &panicSendImpl; }
    void *panicContext() override { return m_pcap; }

    // 定时器泵每拍调一次的排空兜底（契约与理由见 IL2Endpoint.h 的 drainNow）。
    // 与通知器回调走的是同一个 drain()：pcap_next_ex 本来就是「有就取、没有返 0」的语义，
    // 多调一次不会重复消费、也不会阻塞。
    // ★ 计数走 rxDrains 而**不是** rxWakes：混在一起会把 fpw 稀释成恒 0，等于把「事件驱动
    //   到底还灵不灵」这个唯一的判据弄瞎（详见 GatewayDiag.h 里 rxDrains 的注释）。
    void drainNow() override
    {
        if (!m_pcap)
            return;
        // ★ 有收帧线程时这里必须什么都不做：pcap_t 非线程安全，两个线程同时 next_ex 同一个
        //   句柄就是竞态。而且也没必要了 —— 收帧线程本来就在阻塞等帧，泵兜底这条路存在的
        //   前提（收帧被事件循环饿死）已经不成立。
        if (m_rx)
            return;
        ++GatewayDiag::c.rxDrains;
        drain();
    }

    // 发一帧。
    //
    // 【异步路径（默认）】只把帧挂进 TxWorker 的队列就返回，由发送线程去碰驱动。返回 true 的
    //   含义因此从「驱动收下了」变成「进队了」—— 与逐帧路径的语义其实一致：两者都只保证
    //   「交出去了」，都不保证空口上真的送达（那由 TCP 重传兜底）。而且栈的出帧回调
    //   （NetStack.cpp）与两个投毒器**本来就不看返回值**，没有调用方被这个变化误导。
    //   代价：帧不再立即出网卡。所有产帧路径都必须有 flushTx() 收口点，契约见 IL2Endpoint.h。
    //
    // 【逐帧路径（降级）】pcap_sendpacket 是**同步**的：把帧写进驱动 TX 缓冲，成功返 0、失败返 -1。
    // 没有 EAGAIN/可写事件那套语义，所以这里不做「满了排队等可写」——做不了（无可写通知），
    // 也不必（驱动 TX 满时 WriteFile 会阻塞而非丢）。失败基本都是持久性错误（网卡拔了/句柄失效），
    // 排队重发无意义，直接计数丢弃、交由 lwIP 重传兜底。计数与 linux/mac 端点对称，接进 GatewayDiag。
    bool send(const QByteArray &frame) override
    {
        if (!m_pcap)
            return false;
        // 有发送线程就只入队，**工作线程绝不碰驱动**（那是 306 µs/帧的阻塞，见 TxWorker）。
        // 入队失败只有一种情况：队列到 kTxQueueMaxFrames 上限，即驱动已经卡了一秒以上 ——
        // 此时丢帧交 TCP 重传，比继续攒着让延迟雪崩要好。
        if (m_tx)
            return m_tx->post(frame);
        // 降级路径（发送句柄开不出来 / COAST_GW_TXBATCH=0）：与本改动之前完全一致的同步发送。
        return sendOne(frame);
    }

    // 把攒着的一批交给驱动。空队列时只做一次判空（契约要求可以随便调）。
    void flushTx() override
    {
        if (!m_tx)
            return; // 降级路径下 send() 本来就是立即发，无事可做
        // 踢一下：发送线程多半已经在跑，这一下只保证「刚入队的帧不会等到下一次有人入队」。
        // 延迟敏感的路径（ARP 抢答补帧、还原帧、DNS 回程）靠它把队头立刻推出去。
        m_tx->kick();
        // 顺带把发送线程攒的统计取回来折进 GatewayDiag —— 在**工作线程**上做，
        // GatewayDiag::c 的「单线程前提」因此依然成立。flushTx 在各条出帧路径上都会被调到，
        // 采样频率远高于 10 s 的诊断窗口，不会漏账。
        qint64 f=0,b=0,d=0,us=0,n=0;
        m_tx->takeStats(&f,&b,&d,&us,&n);
        GatewayDiag::c.txFrames += f;
        GatewayDiag::c.txBytes += b;
        GatewayDiag::c.txSendUs += us;
        GatewayDiag::c.txBatches += n;
        if (d > 0) {
            GatewayDiag::c.txDropped += d;
            reportTxDropThrottled();
        }
    }
    QByteArray localMac() const override { return m_localMac; }
    int ifIndex() const override { return 0; }
    int mtu() const override { return 1500; }

    // 内核态源 MAC 过滤：pcap_compile 一条 "ether src A or ether src B ..." 的 BPF，pcap_setfilter
    // 下发给 Npcap 驱动。过滤只影响捕获（收），pcap_sendpacket 不受影响。契约见 IL2Endpoint.h。
    bool setSourceMacFilter(const QVector<QByteArray> &macs) override
    {
        if (!m_pcap)
            return false;

        QByteArray expr;
        if (macs.isEmpty()) {
            // 没有被劫持设备时**不再全丢**：仍放行 ARP + ICMPv6(NDP)，让被动监视器 ArpWatch 能检测
            //「有人冒充网关/本机在代理我」——这类检测不需要正在劫持任何设备（idle 也要能报警）。数据帧
            // 照旧被挡在内核外（非混杂模式 + 这条过滤只匹配 ARP/ICMPv6 低频帧，近零成本）。与 Linux 端
            // L2Endpoint_linux 的 idle 放行 ARP/NDP 对齐。
            expr = "arp or icmp6";
        } else {
            QByteArray macClause;
            for (const QByteArray &m : macs) {
                if (m.size() != 6)
                    continue;
                if (!macClause.isEmpty())
                    macClause += " or ";
                macClause += "ether src " + macToText(m);
            }
            if (macClause.isEmpty())
                return false; // 一个合法 MAC 都没有：不装，交用户态兜底
            // 末尾「或 arp」:必须捕获 ARP——尤其真网关广播的 who-has(携带「网关在真 MAC」会把设备
            // 解毒),LanGateway 收到后立刻反制重投。ARP 低频,全收几乎零成本。契约见 IL2Endpoint.h。
            //
            // ★「或 icmp6」是同一条理由的 v6 版本，**这里一度漏掉，代价是 v6 的防守整个失效**。
            //   上面 macs 为空那一档放行了 icmp6，有设备在册这一档却没有 —— 于是「一开始代理，
            //   真路由器的 RA/NA 就在内核这层被丢掉」（它的源 MAC 不在册，也不是 ARP）。而网关
            //   对抗「被解毒」全靠看见路由器的帧就立刻反投：LanGateway_linux.cpp 的
            //   learnRouterFromRa（学 v6 路由器）与紧随其后的 reassertNow（v6 反制）。两者的
            //   触发帧都被挡在内核外 ⇒ 代理期间 v6 侧**只有我们 1s 一轮的周期重投，没有任何反制**，
            //   而 v4 侧因为放行了 arp 一直是好的。
            //   真机现场（2026-08-05，小米路由器 cc:d8:43:9b:e3:b6 + 米系手机，v6 前缀
            //   240e:3a1:7ed3:daa0::/64 正常下发）：手机遵循 Happy Eyeballs 主走 v6，路由器每发
            //   一次 RA/NA 就把它的邻居缓存治好一次、流量改走真路由器，我们再抢回来 —— 来回拉锯，
            //   每切换一次在途的 v6 连接断一批，表现就是「有网但是不稳定」。
            //   Linux 端（L2Endpoint_linux）两种状态下**都**放行 ARP+NDP，所以这是 Windows 独有
            //   的偏差，不是取舍。
            //   代价：非在册源的 ICMPv6(NS/NA/RS/RA/MLD) 也会进用户态，量级与 ARP 同级（低频）。
            //   它们在 LanGateway 的收帧链里走到 victim 判定时会被计进 **nonVictim** 后丢弃 ——
            //   所以这项改动会让诊断行的 nonVictim 抬高一截，那是预期内的，别当成回归。
            expr = "(" + macClause + ") or arp or icmp6";
        }

        struct bpf_program prog;
        // optimize=1；netmask 用 PCAP_NETMASK_UNKNOWN（表达式里不含 ip broadcast，用不到掩码）。
        if (m_api->compile(m_pcap, &prog, expr.constData(), 1, PCAP_NETMASK_UNKNOWN) < 0) {
            dbg("filter compile FAILED expr=%s (回退用户态兜底)", expr.constData());
            return false; // 编译失败：不装内核过滤，用户态照旧兜底
        }
        const int rc = m_api->setfilter(m_pcap, &prog);
        m_api->freecode(&prog);
        dbg("filter set rc=%d expr=%s", rc, expr.constData());
        return rc == 0;
    }

private:
    // 逐帧提交（无批量队列时的老路，也是批量灌不进时的兜底）。
    bool sendOne(const QByteArray &frame)
    {
        const qint64 t0 = nowUs();
        const int rc = m_api->sendpacket(m_pcap, reinterpret_cast<const u_char *>(frame.constData()),
                                         frame.size());
        GatewayDiag::c.txSendUs += nowUs() - t0;
        ++GatewayDiag::c.txBatches; // 逐帧 = 每帧一批，fpb 会如实显示成 1
        if (rc == 0) {
            ++GatewayDiag::c.txFrames;
            GatewayDiag::c.txBytes += frame.size();
            return true;
        }
        ++GatewayDiag::c.txDropped;
        reportTxDropThrottled();
        return false;
    }

    // 6 字节 MAC → "aa:bb:cc:dd:ee:ff"（拼进 pcap 过滤表达式）。
    static QByteArray macToText(const QByteArray &m)
    {
        const auto *b = reinterpret_cast<const unsigned char *>(m.constData());
        return QString::asprintf("%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5])
            .toLatin1();
    }

    void reportTxDropThrottled()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastTxDropReportMs < kDropReportMinIntervalMs)
            return;
        m_lastTxDropReportMs = now;
        qWarning().noquote()
            << "L2Endpoint(Npcap): 发帧失败累计" << GatewayDiag::c.txDropped
            << "帧（pcap_sendpacket 返回错误——网卡异常/句柄失效）。本机→设备方向丢帧会让被代理"
               "设备「时好时坏 / 极慢」。";
    }

    // 收帧线程投递来的一批帧 —— 跑在**工作线程**上（QueuedConnection）。
    // 对下游而言语义与原来完全一致：仍然是逐帧 emit frameReceived，仍然是直连，仍然在工作线程。
    // 变的只是「谁把帧从驱动里捞出来」。
    void onRxBatch(const QVector<QByteArray> &frames, qint64 bytes)
    {
        GatewayDiag::c.rxFrames += frames.size();
        GatewayDiag::c.rxBytes += bytes;
        for (const QByteArray &f : frames) {
            if (dbgOn() && f.size() >= 14) {
                const uchar *p = reinterpret_cast<const uchar *>(f.constData());
                if (m_rxLogged < 20) {
                    ++m_rxLogged;
                    dbg("rx#%lld len=%d dst=%02x:%02x:%02x:%02x:%02x:%02x "
                        "src=%02x:%02x:%02x:%02x:%02x:%02x eth=%02x%02x",
                        m_rxCount + 1, int(f.size()), p[0], p[1], p[2], p[3], p[4], p[5],
                        p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13]);
                } else if ((m_rxCount % 200) == 0) {
                    dbg("rx total=%lld", m_rxCount + 1);
                }
            }
            ++m_rxCount;
            emit frameReceived(f);
        }
        // 这一批喂完，lwIP 顺带打出来的 ACK/数据段/ARP 抢答都还在发送队列里 —— 一次收口。
        // 「收 N 帧 → 发 M 帧」这条批量链路就是在这里成立的。
        flushTx();
        // 放行可能正卡在高水位上的收帧线程，并把它的统计折进 GatewayDiag（工作线程上做，
        // 单线程约定不破）。
        if (m_rx) {
            m_rx->consumed(frames.size());
            qint64 wakes = 0, us = 0;
            m_rx->takeStats(&wakes, &us);
            GatewayDiag::c.rxWakes += wakes;
            GatewayDiag::c.rxDrainUs += us;
        }
    }

    void drain()
    {
        const qint64 t0 = nowUs();
        struct pcap_pkthdr *hdr = nullptr;
        const u_char *data = nullptr;
        int r;
        // 1=拿到一帧；0=本轮无更多(超时)；<0=出错。循环取尽本次事件的所有帧。
        while (m_pcap && (r = m_api->next_ex(m_pcap, &hdr, &data)) == 1) {
            if (data && hdr) {
                ++GatewayDiag::c.rxFrames;
                GatewayDiag::c.rxBytes += hdr->caplen;
                // 诊断：过滤器已把「非被劫持设备」的帧挡在内核里，所以这里出现的每一帧都
                // 应当来自某台被代理设备。前 20 帧逐帧打印源/目的 MAC + ethertype，之后每
                // 200 帧报一次计数——用来判定「设备的帧到底有没有进到这一层」。
                if (dbgOn() && hdr->caplen >= 14) {
                    const u_char *f = data;
                    if (m_rxLogged < 20) {
                        ++m_rxLogged;
                        dbg("rx#%lld len=%u dst=%02x:%02x:%02x:%02x:%02x:%02x "
                            "src=%02x:%02x:%02x:%02x:%02x:%02x eth=%02x%02x",
                            m_rxCount + 1, hdr->caplen, f[0], f[1], f[2], f[3], f[4], f[5],
                            f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]);
                    } else if ((m_rxCount % 200) == 0) {
                        dbg("rx total=%lld", m_rxCount + 1);
                    }
                }
                ++m_rxCount;
                emit frameReceived(QByteArray(reinterpret_cast<const char *>(data), hdr->caplen));
            }
        }
        // ★ 一次排空里 lwIP 顺带打出来的所有帧（ACK、被 ACK 打开窗口后续发的段、ARP 抢答…）
        //   都还攒在发送队列里 —— 在这里一次提交。这是 flushTx 三个调用点里的第一个，
        //   也是「一次唤醒收 N 帧 → 一次提交 M 帧」这条批量链路成立的关键。
        flushTx();
        GatewayDiag::c.rxDrainUs += nowUs() - t0;
    }

    const PcapApi *m_api = nullptr; // open() 里解析；进程级单例，不属本对象所有
    pcap_t *m_pcap = nullptr;
    pcap_t *m_txPcap = nullptr;       // 逐帧降级句柄（TxWorker 独占；pcap_t 非线程安全）
    void *m_txAdapter = nullptr;      // Packet.dll LPADAPTER：批量提交走它；close 时释放
    TxWorker *m_tx = nullptr;         // 空 = 降级到工作线程同步发送
    QWinEventNotifier *m_notifier = nullptr; // 仅降级路径使用
    RxWorker *m_rx = nullptr;                // 空 = 降级到通知器 + 工作线程 drain()
    QByteArray m_localMac;
    long long m_rxCount = 0; // 收到的帧总数（诊断用）
    int m_rxLogged = 0;      // 已逐帧打印的条数（封顶 20）
    qint64 m_lastTxDropReportMs = -kDropReportMinIntervalMs; // 发帧失败告警节流
};

} // namespace

IL2Endpoint *createL2Endpoint(QObject *parent)
{
    return new WinL2Endpoint(parent);
}

#endif // _WIN32
