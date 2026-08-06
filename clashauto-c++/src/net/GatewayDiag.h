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
    // —— 按网卡分桶的帧分流计数 ——
    //
    // 为什么要分：多网卡下「设备没流量」这类问题，全局计数**回答不了是哪张卡**。真机上一台机器
    // 有线接 A 路由、WiFi 接 B 路由时，B 那张卡整段不工作而 A 正常，合计数字看起来完全健康。
    //
    // **只分这四项**，不是整个 Counters：它们是在 LanGateway 的帧分流里加的，那里手上正好握着
    // 网卡；rx/tx 那些加在二层端点内部，端点并不知道自己是第几张卡 —— 要分得先给端点带一个槽位，
    // 为此改三个平台的端点实现不值当。这四项已经足够回答「是哪张卡在丢 / 在旁路 / 根本没帧」。
    struct NicCounters {
        qint64 fedLwip;
        qint64 bypassLan;
        qint64 bypassBcast;
        qint64 dropNonVictim;
    };
    // 槽位数。**最后一个是溢出桶**（网卡多于槽位时都记到它），这样 nicSlot 永远返回合法下标，
    // 热路径上就不需要判空 —— 与 Counters 那条"直接 ++、不判开关"的取舍同源。
    static constexpr int kMaxNicSlots = 5;
    static inline NicCounters nics[kMaxNicSlots] {};
    static inline QString nicNames[kMaxNicSlots] {};
    /// 给一张网卡要一个计数槽（按 ifname 幂等）。只在 configureLocal 里调（工作线程，低频）。
    static int nicSlot(const QString &ifname);

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
        // 拨号发出 / 隧道建成。★ 差值 = **卡在 SOCKS 握手里、既不成功也不报错**的连接。
        //   这类连接在核心的 /connections 里根本不出现（mihomo 要解析完 CONNECT 才登记），
        //   socksFail 也不涨（failed 信号没触发），于是它是一条彻底的盲路。真机实测：
        //   8 条并发下载，栈接受了 9 条、核心里只有 3 条，而 socksFail=0。
        qint64 tcpDialed;
        qint64 tcpEstablished;
        // 我们**发出去**的 SYN-ACK 帧数。★ 补的是最后一条盲路：tcpAccepted 只在三次握手
        //   **完成后**才涨，所以「回了 SYN-ACK、但对端没收到」这种状态此前没有任何计数器覆盖。
        //   真机症状：8 条并发里固定有一两条 curl 报 Connection timed out、0 字节、
        //   local_port=-1（SYN 发了、55 秒等不到 SYN-ACK），而 rxdrop/nonVictim/socksFail
        //   全是 0、栈的 SYN 突发自测又证明一拍 200 个 SYN 全接得住。
        //   与设备侧 tcpdump 抓到的 SYN-ACK 数对照：差值 = 注入丢帧；相等 = 栈压根没回。
        qint64 synAckTx;
        // SYN 进到 C++ 收帧入口 → 我们把 SYN-ACK 发出去，之间隔了多久（微秒）。
        // ★ 补的是「回得慢」这条盲路：真机抓包里网关要 3.08 秒、设备重传 4 个 SYN 之后才
        //   收到 SYN-ACK，而 Rust 侧三条用例（一拍 200 个 SYN 全接住 / 积压 2000 帧仍 4 拍内
        //   回 / RST 立刻中止）全绿 —— 说明延迟发生在栈**之前**。这两个数把「进得晚」和
        //   「进来之后回得慢」分开：synLatMax 只量后半段，前半段由 rx/wakes/fpw 反映。
        qint64 synLatMaxUs;  // 本窗口最大值（瞬时量，每窗口清零）
        qint64 synLatSlow;   // 超过 200ms 的次数——设备初始 RTO 是 1 秒，200ms 已是危险信号
        // 定期清扫收掉的「上游已关」连接。★ 这个数**长期为 0 才是正常**：它统计的是
        // 「本该在 smolPumpToStack 里关掉、却因为再没人调用它而漏下来」的连接。不为 0
        // 说明那条主路又漏了事件，而不是清扫器在干活 —— 清扫器只是兜底，不是主路。
        qint64 tcpReaped;
        // —— 背压（触发次数；频繁触发 = 两头速率长期不匹配）——
        qint64 upThrottleHits;  // 上行水位到顶 → 扣住 lwIP 接收窗口
        qint64 downPauseHits;   // 下行水位到顶 → 停止从 socks 读
        // —— UDP ——
        qint64 udpFlowsCreated;
        qint64 udpFlowsEvicted; // 撞上限被淘汰（只淘汰**确已空闲**的，见 NetStack 的准入策略）
        qint64 udpFlowsRefused; // 撞上限且一条空闲的都没有 → 拒收这条新流的包（不顶活流）
        qint64 dnsHijacked;     // 转投 mihomo DNS 的查询数
        qint64 dnsNoReply;      // 5s 兜底回收时仍没等到应答（= 转投出去了，核心那侧没回）
        // 事务 ID 分配失败（在途查询把 65536 的 txid 空间占满）→ 这条查询**根本没发出去**。
        // 必须与 dnsNoReply 分开：两者的处置**相反** —— dnsNoReply 高是「核心/上游解析不动了」，
        // 该去查核心；dnsNoId 高是「我们自己的在途表满了」，该去调容量/超时。混在一栏里
        // （最初就是这么写的）等于把两个方向的结论搅在一起，真出问题时读不出该往哪查。
        qint64 dnsNoId;
        // —— 进程内出站 vs 回退核心（CoastCore，阶段 1 移植）——
        // **「离完全替换 mihomo 还差多少」的唯一凭据**：ccInProcess 之外那几栏全为 0，
        // 才说明这条数据面已经不需要核心了。分原因记账是因为「回退」这一个数说明不了任何问题：
        // 缺协议、规则判不了、fake-ip 没反查到，对策完全不同。
        qint64 ccInProcess;      // 走进程内出站的连接数（TCP+UDP）
        qint64 fbNoRoute;        // router 没给节点：fake-ip 没反查到 / 无配置快照 / 规则需先解析 IP
        qint64 fbNodeMissing;    // 给了节点名，但当前快照里查不到它
        qint64 fbProtoMissing;   // 协议没注册（缺 OpenSSL/msquic，或该 type 尚未实现）
        qint64 fbUdpUnsupported; // 节点协议没有 UDP 出站实现
        qint64 ccStrictRefused;  // 严格模式下**拒绝回退**而直接失败的连接数
        // 由旁听到的 DNS 映射把 fake-ip 目的地成功改写成域名的连接数。
        // >0 才说明「域名类流量真的能进进程内出站」；恒 0 = 改写没生效，域名类还在回退核心。
        qint64 dnsFakeIpResolved;
        qint64 dnsLocalFake;     // 本地当场合成 fake-ip 应答的查询数（含给 AAAA 回 NODATA）
        qint64 dnsLocalForward;  // 本地看不懂/不该 fake → 转发给上游的查询数
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

    /// 往同一个 gateway-diag.log 里补一条**事件行**（不是十秒采样行）。
    ///
    /// ★ 为什么不能用 qWarning/qDebug：本二进制是 **GUI 子系统**，Qt 的日志在 Windows 上走
    ///   OutputDebugString —— stdout/stderr/文件里**一个字都看不到**。真机排查时那等于没写。
    ///   （CI 里那三个自测也栽在同一件事上，见 release.yml 里的说明。）
    ///
    /// `key` 用来限频：同一个 key 在 `minGapMs` 内只写一条。坏起来往往是每条连接一次，
    /// 不限频会把十秒采样行全冲掉 —— 而那些行才是判断趋势的依据。
    static void note(const char *key, const QString &text, int minGapMs = 3000);
};
