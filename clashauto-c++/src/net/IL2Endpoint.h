#pragma once

// 二层（以太帧）收发抽象 —— 透明网关的最底层。
// 平台后端：Linux = AF_PACKET(SOCK_RAW)；macOS = BPF(/dev/bpf，经 root helper)；Windows = Npcap。
// M1 只实现 Linux；其余平台 createL2Endpoint() 返回 nullptr（LanGateway 据此 isAvailable=false）。
//
// 契约：
//  - open() 绑定到指定网卡，成功后 frameReceived() 会在 Qt 事件循环线程发出**每一个**收到的完整
//    以太帧（含 14 字节以太头；不含 FCS）。用 QSocketNotifier 驱动读取，切勿轮询/阻塞。
//  - send() 发送一个完整以太帧（调用方负责填好 dst/src MAC 与 ethertype）。
//    · **frame 必须是自有内存的 QByteArray**，不能是 fromRawData 的视图：内核发送缓冲满时
//      实现会把它排进积压队列（等 fd 可写再发），队列里存的是隐式共享副本 = 底层同一块内存，
//      要活到下一次事件循环。需要回注收到的帧时请先深拷贝。
//    · 返回 false = 这一帧最终没发出去（积压也满了 / 网卡出错）。调用方**可以**忽略它——
//      本机→设备方向的丢帧由 TCP 重传兜底，实现侧自带计数与节流告警。
//  - localMac() 返回本网卡 6 字节 MAC；ifIndex() 返回接口索引（AF_PACKET sll_ifindex 用）。
//  - 需要 CAP_NET_RAW / root。open() 失败（权限/网卡不存在）时置 *err 并返回 false。
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

class IL2Endpoint : public QObject
{
    Q_OBJECT
public:
    explicit IL2Endpoint(QObject *parent = nullptr) : QObject(parent) {}
    ~IL2Endpoint() override = default;

    virtual bool open(const QString &ifname, QString *err) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual bool send(const QByteArray &frame) = 0; // 完整以太帧（>=14 字节）
    virtual QByteArray localMac() const = 0;         // 6 字节
    virtual int ifIndex() const = 0;
    virtual int mtu() const = 0;                      // 接口 MTU（不含以太头），失败回 1500

    // 设置「感兴趣的源 MAC 集合」——内核态源 MAC 过滤，纯属**收方**优化，不影响 send()。
    //
    // 为什么需要：抓包是混杂模式，默认整个网段的每一帧都被复制进用户态、分配 QByteArray、发一次
    //   frameReceived 信号，再在 LanGateway 的槽里按 src MAC 丢弃——别人在局域网里传个文件，本进程
    //   就在 GUI 线程上空转烧 CPU。把「只要这几台被劫持设备发出的帧」这条判断下沉到内核，绝大多数
    //   无关帧根本不会进用户态。
    //
    // 到底该收哪些帧（关键，别搞反）：透明网关是**完整终结式代理**——被劫持设备的连接在 lwIP 里
    //   终结、再经 SOCKS 由本机正常协议栈重新拨出去；ARP 投毒只发不收（网关 MAC 由设备发现流程另行
    //   解析）。所以这条抓包链路真正需要的**只有「源 MAC = 被劫持设备」的帧**，网关/其它主机发来的
    //   帧一律用不到。
    //   尤其注意：过滤只作用在这条**独立的抓包 socket**（AF_PACKET / pcap / BPF）上，和内核给本机
    //   正常 TCP/IP 栈的投递是两条路——所以「网关发给本机的回程帧」照样由正常协议栈收到，不会被这里
    //   挡掉；我们也压根不需要在抓包链路上收它。
    //
    // 语义：sourceMacs 每项为 6 字节 MAC，过滤条件是它们的「或」，且可随时重设（设备开/关代理时集合
    //   会变）。传空 = 没有任何被劫持设备 → 装一个「全丢」的内核过滤，最大化省 CPU。
    //   返回 false = 未能装上内核过滤（平台不支持 / 编译或装载失败），此时**完全依赖用户态过滤兜底**，
    //   功能不受影响——内核过滤是优化不是替代，调用方无需因 false 而改变行为。
    virtual bool setSourceMacFilter(const QVector<QByteArray> &sourceMacs)
    {
        Q_UNUSED(sourceMacs);
        return false; // 默认：不支持内核过滤，交用户态兜底
    }

    // ———————————— 崩溃兜底用的「裸发」通道（见 GatewayPanic）————————————
    // send() 走不通的场合只有一个：**进程已经在崩了**（SIGSEGV/abort 的处理器里）。那里不能碰
    // QByteArray、不能分配、不能加锁，而我们又必须把「还原 ARP」这几帧发出去——否则被代理设备
    // 会一直把网关指着一个已经死掉的进程所在的 MAC。
    // 于是端点额外暴露一条最小通道：一个**只做一次系统调用**的函数指针 + 一个地址稳定的上下文。
    // 契约：panicSender() 返回的函数必须是 async-signal-safe（只允许 sendto/write 这类），
    // panicContext() 返回的指针必须在端点存活期内地址不变。不支持的平台返回 nullptr。
    using RawSendFn = void (*)(void *ctx, const unsigned char *data, int len);
    virtual RawSendFn panicSender() const { return nullptr; }
    virtual void *panicContext() { return nullptr; }

signals:
    // 收到一个完整以太帧（含以太头）。接收方零拷贝语义：帧内容仅在槽内有效，需要保留请拷贝。
    //
    // 「零拷贝」在 Linux 后端是字面意思：TPACKET_v3 收环里的帧由 QByteArray::fromRawData 直接指过去，
    // 底层就是那块和内核共享的 mmap 内存，槽一返回、整块还给内核，内核立刻会往里写新帧。由此有三条
    // **硬约束**，改动这个信号的连接方式前务必先看：
    //  1. **必须是直连（Qt::DirectConnection / 同线程的 AutoConnection）**。绝不能用 Queued /
    //     BlockingQueued，也不能把 sender 和 receiver 放在不同线程上——队列连接会把 QByteArray
    //     拷进事件队列，而「拷贝一个 fromRawData 的 QByteArray」拷的只是那个指向环内存的视图，
    //     等事件真正被派发时那块内存早已被内核覆写 ⇒ 悬垂读。
    //     （现状：LanGateway 的帧过滤 lambda 以 worker 自身为 context，端点也 parent 在 worker 上，
    //      二者同线程 ⇒ 直连；GatewaySelfTest 同理在主线程内直连。）
    //  2. 想把帧留到槽之外，必须**深拷贝**：QByteArray(f.constData(), f.size())，或取严格子串
    //     f.mid(pos, len)（pos>0 时 Qt6 走 sliced() = 深拷贝）。注意 f.mid(0, f.size()) 在 Qt6 里
    //     命中「Full」分支返回的是浅拷贝，那样存下来照样悬垂。
    //  3. 数据**不保证以 '\0' 结尾**，别把 constData() 当 C 字符串用。
    void frameReceived(const QByteArray &frame);
};

// 工厂：返回当前平台的二层端点实现；不支持的平台返回 nullptr。所有权归 parent。
IL2Endpoint *createL2Endpoint(QObject *parent = nullptr);
