#pragma once

// Coast 用户态 TCP 数据面的 C ABI —— C++(NetStack) ↔ Rust(smoltcp) 的唯一边界。
//
// 这个头是**手写并提交进仓库**的，不在 CI 跑 cbindgen（多一个工具 = 多一个装不上的地方）。
// 与 vendored lwIP 的做法同构。改这个头就必须同步改 rust/coaststack/src/ffi.rs，两边都在仓库里。
//
// ── 为什么存在 ────────────────────────────────────────────────────────────────
// Windows 是唯一没有内核重定向可用的平台（Linux 走 TPROXY、macOS 走 pf rdr，
// `WfpRedirect.h:91` 记载 gatewayTproxy/gatewayPf 在 Windows 上恒为 false），
// 只能靠用户态栈。现状是 vendored lwIP；本模块是它的替代品。
// 选型依据见 docs/lwip-alternatives.md（18 轮实测：单核吞吐 4.2×、每 Gbps CPU 便宜 4.2×）。
// ★ 但 R19–R21 同样实测：Windows 上真正的绑定约束是 Npcap 发送路径，不是 TCP 栈。
//   换栈的收益是"总 CPU 降约 1/3"，**不是 4.2×，且吞吐天花板不动**。别拿 4.2× 去承诺。
//
// ── 职责边界（谁做什么）──────────────────────────────────────────────────────
// 本模块**只做 TCP**。明确不做：
//   · UDP / DNS —— 生产早就不走 lwIP，是 NetStack.cpp 里一套独立的手工 NAT（约 700 行，
//     每源端口一条 Socks5Udp + 双档 LRU 老化 + DNS 事务 ID 多路复用）。换栈时那 700 行零改动。
//   · ARP / NDP —— 投毒由 ArpSpoofer/NdpSpoofer 独立负责；且 ARP/NDP 帧在 NetStack::inputFrame
//     里就被丢弃、根本不进栈。所以**本模块不需要任何 ARP/NDP 协议实现**，回程帧的目的 MAC
//     由 C++ 侧在 add_nic/设备表里注入，或直接从收到帧的源 MAC 学。
//   · SOCKS 拨号与每设备身份 —— 全在 C++ 侧（Socks5Tcp + userForIp）。
//
// ── 线程模型 ────────────────────────────────────────────────────────────────
// **单线程**，与 NetStack 现有前提一致（NetStack.h:19-24）：所有 coast_* 调用必须来自同一个
// 线程；所有回调也在该线程、在某个 coast_* 调用**内部**同步发出。回调里可以再调 coast_*
// （典型：conn_new 里立刻 conn_close），实现必须容忍这种重入。
//
// ── 内存所有权 ──────────────────────────────────────────────────────────────
// 所有 (ptr,len) 缓冲的所有权都**不转移**：回调里的指针只在回调返回前有效，需要留就自己拷贝；
// 传进来的指针只在该次调用内被读取。
//
// ── panic 纪律 ──────────────────────────────────────────────────────────────
// Rust 侧是 panic="abort"，panic 会**直接杀掉整个 GUI 进程**。因此 FFI 实现里零 unwrap、
// 所有可失败路径显式返回错误码。（R15 在 bench 里栽过：NAT 表 bind().unwrap() 遇 EMFILE 直接 core dump。）

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ———————————————————————— 基本类型 ————————————————————————

// v4/v6 通吃的地址。v4 时 is_v6=false、只用 bytes[0..4]（网络序）。
typedef struct {
    bool is_v6;
    uint8_t bytes[16]; // 网络序；v4 用前 4 字节
} CoastAddr;

typedef struct CoastStack CoastStack;

typedef uint64_t CoastConnId; // 0 恒为无效值
typedef uint32_t CoastNicId;

// 错误码：0 = 成功，负值 = 失败。所有可失败入口都返回它，绝不 panic。
enum {
    COAST_OK = 0,
    COAST_ERR_BADARG = -1,
    COAST_ERR_NOCONN = -2,  // conn_id 已失效（可能刚被回调关掉）
    COAST_ERR_NONIC = -3,
    COAST_ERR_NOMEM = -4,
    COAST_ERR_STATE = -5,   // 连接状态不允许该操作（如已关闭还要写）
};

// ———————————————————————— 回调（Rust → C++）————————————————————————
//
// 全部在调用线程内同步发出。user 是 coast_stack_new 时传入的不透明指针。
typedef struct {
    // 栈要发一帧出去。C++ 侧据 nic_id 找到对应 IL2Endpoint 并 send()。
    // ★ 注意 flushTx 契约：本回调只负责把帧交出去，攒批与 flush 时机由 C++ 侧掌握
    //   （现有 6 个收口点见 NetStack.cpp，漏掉任何一个 = 设备侧最多多等 25ms）。
    void (*out_frame)(void *user, CoastNicId nic, const uint8_t *frame, size_t len);

    // 新连接被 catch-all 接住。dst 是**设备原本想访问的目的地**（不是本机地址），
    // src 是设备自己 —— C++ 侧据 src 查 socksUser、据 dst 拨 SOCKS。
    // 返回 true = 接受（C++ 已准备好桥接）；false = 立刻拒绝（栈会 abort 该连接）。
    bool (*conn_new)(void *user, CoastConnId id, CoastNicId nic,
                     const CoastAddr *src, uint16_t src_port,
                     const CoastAddr *dst, uint16_t dst_port);

    // 上行：设备发来的数据。
    // ★ 收到不等于归还窗口 —— 背压的整套机制建立在"先不归还"之上（对应 lwIP 的 tcp_recved）。
    //   C++ 侧把数据落地给 SOCKS 之后，再显式调 coast_conn_recved() 把窗口还回来。
    //   不还就等于对设备说"别发了"，这正是我们要的闸门。
    void (*conn_data)(void *user, CoastConnId id, const uint8_t *data, size_t len);

    // 下行发送缓冲腾出了 n 字节（对应 lwIP 的 tcp_sent）。C++ 侧据此继续 pump。
    void (*conn_sent)(void *user, CoastConnId id, uint32_t n);

    // 连接结束。is_abort=true 表示是 RST/异常，false 表示优雅关闭。
    // 本回调返回后 id 立即失效，C++ 侧必须停止对它的一切调用。
    void (*conn_closed)(void *user, CoastConnId id, bool is_abort);
} CoastCallbacks;

// ———————————————————————— 生命周期 ————————————————————————

// 建栈。cb 会被拷贝，user 由调用方保证在 free 之前一直有效。失败返回 NULL。
CoastStack *coast_stack_new(const CoastCallbacks *cb, void *user);
void coast_stack_free(CoastStack *s);

// ———————————————————————— 网卡 ————————————————————————
//
// 每张网卡一个独立的协议栈实例（smoltcp 的 Interface 不是全局的 —— 这点比 lwIP 的
// NO_SYS 全局状态干净：lwIP 得靠 ip4_route 按子网从一条 netif 链里挑，配错掩码就会
// 把 B 网段的回包从 A 网卡发出去，见 NetStack.cpp:1383-1386）。
// mac6 = 本机在该卡上的 MAC；ip/mask 用于本机地址（回程源地址仍是"原始目的 IP"，靠 any_ip）。
int32_t coast_stack_add_nic(CoastStack *s, CoastNicId nic, const uint8_t mac6[6],
                            const CoastAddr *ip, uint8_t prefix_len);
int32_t coast_stack_remove_nic(CoastStack *s, CoastNicId nic);

// **仅供归因测量**：关掉收包方向的 TCP/IPv4 校验和验证（发送仍然生成）。
// ⚠️ 生产绝不能开：我们是中间盒，终结设备的 TCP 后再以新连接转发，转发时会算一个
//    **有效**的校验和。收包不验 = 把损坏数据洗成"看起来正确"的新连接，端到端校验失效。
//    以太网 FCS 逐跳重算，救不了这个。它只用来回答"校验和占协议栈那段的多少"。
int32_t coast_stack_debug_skip_rx_checksum(CoastStack *s, bool on);

// ———————————————————————— 数据面 ————————————————————————

// 喂入一个完整以太帧（含 14 字节以太头）。只应传 IPv4/IPv6 的 TCP 帧 ——
// UDP 由 C++ 侧的手工 NAT 处理、ARP/NDP 由投毒器处理，都不该到这里。
int32_t coast_stack_input(CoastStack *s, CoastNicId nic, const uint8_t *frame, size_t len);

// 驱动定时器（重传/延迟 ACK/TIME_WAIT 回收）。now_ms 为单调毫秒。
// 返回**建议的下次调用延迟**（毫秒）；返回 UINT64_MAX 表示当前无待办定时器。
// 调用方仍可按自己的节奏调（现有泵是 25ms 固定周期 + Qt::PreciseTimer，见 NetStack.cpp:1301)。
uint64_t coast_stack_poll(CoastStack *s, uint64_t now_ms);

// ———————————————————————— 连接操作 ————————————————————————

// 下行：把 SOCKS 来的数据写给设备。返回**实际接受的字节数**（可能小于 len，对应
// lwIP 的 tcp_sndbuf 限流 + 部分写）；返回负值为错误码。
// 没写完的部分由调用方留着，等 conn_sent 回调再续。
int32_t coast_conn_send(CoastStack *s, CoastConnId id, const uint8_t *data, size_t len);

// 查询下行发送缓冲还能收多少（对应 tcp_sndbuf）。失败返回负值。
int32_t coast_conn_sndbuf(CoastStack *s, CoastConnId id);

// 归还接收窗口（对应 tcp_recved）。上行背压的命脉：conn_data 收到的字节要在**落地之后**
// 才调这个，窗口才会重新打开。
// ★ 与 lwIP 的一个差异：这里 n 是 uint32_t，不必像 tcp_recved 那样按 0xFFFF 分多次
//   （NetStack.cpp:595-602 那个循环在新栈里不需要）。
int32_t coast_conn_recved(CoastStack *s, CoastConnId id, uint32_t n);

// 优雅关闭（发 FIN）。
// ★ 调用前**必须把窗口还满**，否则栈会改发 RST 而不是 FIN，设备侧表现为"下载到一半被断"。
//   这正是 lwIP 那个坑（NetStack.cpp:709-713 的 tcp_close 看到 rcv_wnd != TCP_WND_MAX 就发 RST）。
//   实现会在窗口未还满时返回 COAST_ERR_STATE，把这个约束变成显式失败而不是静默劣化。
int32_t coast_conn_close(CoastStack *s, CoastConnId id);

// 强制中止（发 RST）。任何状态下都可调。
int32_t coast_conn_abort(CoastStack *s, CoastConnId id);

// 关掉某个设备地址上的**所有**连接（换址/移除设备/DHCP 续约时用）。
// 对应 lwIP 那边遍历 tcp_active_pcbs（NetStack.cpp:1502，靠 priv 头才拿得到）。
// 不能省：lwIP 的 established 超时是 24 小时，不主动关会造成慢性连接表泄漏
//（实测 40 条连接换址后 PCB 卡在 43 纹丝不动，见 NetStack.cpp:1476-1485）。
// 返回关掉的条数，或负错误码。每关一条都会发 conn_closed 回调。
int32_t coast_stack_close_device_conns(CoastStack *s, const CoastAddr *device);

// ———————————————————————— 诊断 ————————————————————————
//
// 对应 GatewayDiag 里 lwIP 那一栏（NetStack.cpp 的 lwipStatsLine）。换栈后日志不能出空洞。
typedef struct {
    uint64_t conns_accepted;
    uint64_t conns_closed;
    uint64_t conns_aborted;
    uint64_t conns_refused;   // catch-all 接住但被拒（socket 表满 / conn_new 返回 false）
    uint64_t rx_overflow;     // ★ 收队列满而丢的帧数。非 0 = **工作线程跟不上**，
                              //   与 rxdrop（链路/内核缓冲丢）分属不同环节，别混。
    uint64_t rx_frames;
    uint64_t tx_frames;
    uint64_t rx_dropped;      // 解析失败/无对应 nic
    uint64_t retransmits;     // ★ lwIP 根本没有这个计数器（NetStack.cpp:1007-1016 记着这事），
                              //   换栈是补上真正 rexmit 计数的机会
    uint32_t conns_active;
    uint32_t poll_max_gap_ms; // ★ 瞬时量，读完清零。对应 lwIP 的 fastGapMax ——
                              //   平均值会把毛刺抹平，而延迟 ACK 等的就是下一拍，最坏间隔才决定性
} CoastStats;

void coast_stack_stats(CoastStack *s, CoastStats *out);

// 版本自证：让运行期能确认链进来的库和这个头是同一版（对应 lwipopts 那种"产物大小一样、
// 光看文件名分不出跑的是哪个配置"的教训，见 bench 里 lwip2socks 的 banner）。
const char *coast_stack_version(void);

#ifdef __cplusplus
} // extern "C"
#endif
