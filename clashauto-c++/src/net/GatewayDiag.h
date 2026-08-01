#pragma once

// 透明网关**数据面**的诊断日志 —— 把「以前只能靠猜」的那几件事落成一个本地文件。
//
// 为什么需要它（这不是「多打点日志」那种可有可无的东西）：
//   这条链路的故障模式几乎全是**静默降级**——丢一帧、挤掉一个池子条目、事件循环被堵住半秒，
//   对外全都只表现为「被代理设备有点慢」。既有的 COAST_GATEWAY_DEBUG 只往 stderr 打，进程一关
//   就没了，而且是逐帧刷屏的量级，既不能常开、事后也查不到。真机上排查一次「慢」要么靠反复
//   复现，要么靠猜——发方静默丢帧那个 bug 就是这么藏了很久（见 L2Endpoint_linux.cpp 文件头）。
//   所以这里改成：常开、低频、定量、落盘、自动轮转。
//
// 设计取舍：
//   · **热路径零开销**：计数器是一个 C++17 inline 静态结构体里的普通 qint64，直接 `++`。
//     不是原子、没有函数调用、没有 thread-safe-static 的 guard 变量、不判开关
//     （判开关比自增本身还贵）。开销 = 一条 inc 指令。
//   · **单线程前提**：所有自增都发生在网关工作线程上（LanGateway_linux.cpp 的线程模型：
//     二层端点 / NetStack / 所有 Socks5* 都 moveToThread 到那一个线程），headless 自测则全在
//     主线程。两者从不并存 —— 与 NetStack 的 g_impl 单例是同一个前提。**别在别的线程上碰它。**
//   · **采样而非事件流**：每 kSampleIntervalMs 由 NetStack 的定时器泵分频调用一次 sample()，
//     写「本窗口的增量 + 若干瞬时量」。空窗口（完全没流量）直接跳过，不写空行。
//   · **有界**：单文件到 kMaxBytes 就轮转成 .1（只留一份备份），最坏占 2×kMaxBytes。
//
// 关掉/调频：COAST_GW_DIAG=0 关闭；COAST_GW_DIAG_MS=<毫秒> 改采样间隔（下限 1000）。
//
// 日志位置：<userDir>/logs/gateway-diag.log（userDir 由 main_qml.cpp 在建 LanGateway 前用
// setLogDir() 传进来）。没设过路径 = 未启用，sample() 直接返回，计数器照常累加但不落盘。
#include <QString>
#include <QtGlobal>

class GatewayDiag
{
public:
    // ★ 全部是「窗口内增量」语义的累计量，除了名字里带 Peak 的那几个。
    //   加字段时记得同步 sample() 里的输出，否则加了等于没加。
    //
    // ★★ 字段一律**不写 `= 0`**（不给默认成员初始化器），归零靠下面 `c{}` 的值初始化。
    //    别"顺手补上"—— Counters 是 GatewayDiag 的嵌套类，而 C++ 规定嵌套类的默认成员初始化器
    //    要到**外层类**闭合大括号之后才可用；`static inline Counters c{}` 恰好写在外层类内部，
    //    于是 gcc 直接报 "default member initializer required before the end of its enclosing
    //    class"。没有 NSDMI 时 Counters 是纯聚合体，`c{}` 值初始化把每个成员置 0，效果相同。
    //    唯一的代价：谁要是在栈上 `Counters x;` 就是未初始化的——本工程只有静态存储的实例
    //    （c 和 .cpp 里的 g_prev{}），都显式值初始化了。
    struct Counters {
        // —— 二层：收 ——
        qint64 rxFrames;      // 从二层端点交上来的帧（已过内核 BPF 源 MAC 过滤）
        qint64 rxBytes;
        qint64 rxWakes;       // **事件驱动**的可读唤醒次数。rxFrames/rxWakes = 批处理效率（收环的核心收益）
        // 泵主动排空（IL2Endpoint::drainNow）的次数。**必须与 rxWakes 分开记**：
        // 它是「每拍每网卡固定加一」的量，混进 rxWakes 会把 fpw(帧/唤醒) 稀释成恒 0，
        // 于是「事件驱动到底还灵不灵」这个唯一能回答收帧饥饿的指标就瞎了（f524cb5 实际发生过：
        // 07-29 之后所有窗口 fpw 恒 0、wakes ≈ 2×pump）。
        qint64 rxDrains;
        // 内核 PACKET_STATISTICS 的 tp_drops：环满、用户态没跟上。
        // ★ 仅 Linux 有值；mac 的 bpf 要走 BIOCGSTATS(bs_drop)，还没接 —— 在 mac 上这一栏恒 0，
        //   别把它当「mac 上没丢包」的证据。
        qint64 rxKernelDrops;
        // —— 二层：发 ——
        qint64 txFrames;
        qint64 txBytes;
        qint64 txDeferred;    // 内核缓冲满 → 排进积压队列（不是丢，但已经是拥塞信号）
        qint64 txDropped;     // 积压也满了 → 真丢帧，只能等 TCP 重传
        qint64 txBacklogPeak; // 本窗口积压字节数的高水位（瞬时量，每窗口清零）
        // ★ 发方**时延**计数（Windows）。加它的直接理由：本文件原有的量全是「次数」，
        //   而 6000 个采样窗口对 (win − pump×标称周期) 做二元最小二乘的结果是
        //     每发一帧 ≈ 306 µs 的工作线程墙钟、每收一帧 ≈ 0（收方成本被发方完全盖住）。
        //   306 µs 比这条路上所有用户态动作（QByteArray 分配 + 1500B memcpy ≈ 0.3 µs）大三个数量级，
        //   只可能是 pcap_sendpacket 里那次同步等待 NDIS 发送完成。但「只可能是」不是「测到了」——
        //   这两个计数把它变成可直接读出的事实：usPerTx = txSendUs/txFrames。
        //   开销：每次发送两次 QueryPerformanceCounter（约 40 ns），相对被测量的 300 µs 是 0.01%。
        qint64 txSendUs;      // 累计花在「把帧交给驱动」上的微秒数（含批量提交）
        qint64 txBatches;     // 提交批次数。txFrames/txBatches = 平均每批多少帧（批量发的实际效果）
        // 收方排空耗时：一次 drain() 的全程，含 ① pcap_next_ex 在缓冲空时那次阻塞等待
        //（Npcap 的 PacketReceivePacket 会 WaitForSingleObject(ReadEvent, to_ms)，to_ms=1ms）、
        // ② 每帧喂进 lwIP 的处理、③ 末尾那次 flushTx —— 所以它与 txSendUs 有意重叠。
        // rxDrainUs/(rxWakes+rxDrains) = 平均一次排空多贵，正是「泵里排空（f524cb5）到底是
        // 省了延迟还是把泵自己拖慢了」的直接判据。
        qint64 rxDrainUs;
        // —— 帧分流（LanGateway 的过滤链，回答「流量断在哪一环」）——
        qint64 fedLwip;       // 真出网、喂进用户态栈的帧
        qint64 bypassLan;     // 同网段直连旁路
        qint64 bypassBcast;   // 广播/组播旁路
        qint64 dropNonVictim; // 源 MAC 不在劫持名单（内核过滤漏网的 / 台账 MAC 对不上）
        // —— lwIP TCP 连接生命周期 ——
        qint64 tcpAccepted;   // catch-all 监听接下的新 SYN
        qint64 tcpClosed;     // 优雅关闭
        qint64 tcpAborted;    // RST 收场（多半是上游 SOCKS 失败/出错）
        qint64 socksFailed;   // 拨 mihomo 失败（握手/认证/连不上）
        // —— 背压（触发次数；频繁触发 = 两头速率长期不匹配）——
        qint64 upThrottleHits;  // 上行水位到顶 → 扣住 lwIP 接收窗口
        qint64 downPauseHits;   // 下行水位到顶 → 停止从 socks 读
        // —— UDP ——
        qint64 udpFlowsCreated;
        qint64 udpFlowsEvicted; // 撞上限被淘汰（只淘汰**确已空闲**的，见 NetStack 的准入策略）
        qint64 udpFlowsRefused; // 撞上限且一条空闲的都没有 → 拒收这条新流的包（不顶活流）
        qint64 dnsHijacked;     // 转投 mihomo DNS 的查询数
        qint64 dnsNoReply;      // 5s 兜底回收时仍没等到应答
        // —— 定时器泵健康度（**单线程饱和度**的直接指标）——
        // 泵是 25ms 一拍的固定周期。它迟到就说明工作线程上一拍还没跑完——数据面被自己堵住了。
        // 这是回答「到底是链路丢包还是本机算不过来」的关键一栏，其它计数都替代不了。
        qint64 pumpTicks;
        qint64 pumpLateTicks; // 实际间隔 > 2 倍期望的拍数
        qint64 pumpMaxLagMs;  // 本窗口最大迟到量（瞬时量，每窗口清零）
    };

    // 直接拿引用自增：`GatewayDiag::c.rxFrames++`。C++17 inline 静态成员 —— 有且仅有一个实例，
    // 且不像函数内 static 那样每次访问都要过一遍初始化 guard。
    static inline Counters c{};

    // 由 main_qml.cpp 传 AppConfig::userDir（日志落在其 logs/ 子目录）。空 = 停用落盘。
    static void setLogDir(const QString &userDir);
    static bool enabled();
    static int sampleIntervalMs();

    // 写一行采样。extra 由调用方（NetStack）拼好——lwIP 的池/重传统计要 lwIP 头才拿得到，
    // 不适合让这个跨平台 TU 去碰。窗口内毫无活动时不写。
    static void sample(const QString &extra);

    // 进程退出/网关停用时调一次：把最后一个窗口写掉，并留一条 "stop" 标记。
    static void flush(const QString &extra, const char *reason);
};
