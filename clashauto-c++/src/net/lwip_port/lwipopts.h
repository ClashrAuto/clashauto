#pragma once

// lwIP 移植配置（Coast 透明网关的用户态协议栈）。
// 运行模式：NO_SYS=1（无操作系统/单线程），仅 raw API，由 Qt 事件循环驱动：
//   · IL2Endpoint 收到帧 → 交 NetStack 包成 pbuf → netif->input(ethernet_input)。
//   · lwIP 需要出网时 → netif->linkoutput → 序列化成以太帧 → IL2Endpoint::send。
//   · 定时器：QTimer 周期调用 sys_check_timeouts()（TCP 重传/超时等）。
// 只做 IPv4 + TCP + UDP；不启用 DHCP/DNS/IPv6/socket/netconn。

// —— 系统模式 ——
#define NO_SYS                          1
#define LWIP_TIMERS                     1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
// **必须为 0**：网关支持多网卡（有线接 A 路由 + WiFi 接 B 路由，两个网段的设备都能代理），
// 一个 lwIP 实例要挂多个 netif。置 1 会把 netif 链表遍历整个编译掉——ip4_route() 直接返回
// netif_default、NETIF_FOREACH 只访问 netif_default，于是 B 网段设备的回包会从 A 网卡发出去，
// 而且**不报任何错**，只是死活不通。
#define LWIP_SINGLE_NETIF               0

// —————————————————————————— 内存 ——————————————————————————
// 容量目标：**十几台活跃设备**（每台手机/电脑开着浏览器+若干 App，轻松挂 50~150 条 TCP）。
//
// 两类内存都是 **static 数组**（mem.c 的 ram_heap、memp.c 的 memp_memory_*_base），落在 BSS ——
// 进程启动时只是保留地址空间，操作系统按页惰性提交，RSS 只随**实际用量**增长。所以这里给足
// 余量的代价是「虚拟地址 + 一点 BSS」，不是「开机就吃掉十几 MB 物理内存」。
//
// 每个 memp 池的字节数 = 条目数 × LWIP_MEM_ALIGN_SIZE(sizeof(结构))。
// 结构大小实测（mingw x86_64，本移植的开关组合；MSVC x64 同布局——cc.h 的 pack 只作用于协议头结构）：
//   tcp_pcb 216 | tcp_pcb_listen 56 | tcp_seg 32 | udp_pcb 48
//   pbuf 24 | ip_reassdata 40 | etharp_q_entry 16 | sys_timeo 32
//   PBUF_POOL 单元 = align(24) + align(1600) = 1624
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   4

// mem.c 堆：**只服务「本机→设备」方向的发送数据**。NetStack::pumpToLwip 用
// tcp_write(..., TCP_WRITE_FLAG_COPY) 把 socks 收来的字节拷进 PBUF_RAM，这些 pbuf 就在这个堆里；
// 另有 tcp_output/ICMP 的小头部 pbuf（几十字节，瞬时）。
// 单条连接最多占 tcp_sndbuf() ≤ TCP_SND_BUF = 60 KB，所以堆容量 ≈ 能同时「满窗下载」的连接数：
//     8 MiB / 60 KB ≈ 139 条并发大流量连接
// 十几台设备里同时真正在拉满带宽的连接远不到这个数（其余是空闲/小请求，几乎不占堆）。
// 堆耗尽不会静默丢数据：tcp_write 返回 ERR_MEM → pumpToLwip 保留 c->toLwip 里的剩余字节，
// 等 tcp_sent 回调再续（降速而已）。2 MiB 时只够 ~34 条，十几台设备一起下载就会明显限速。
#define MEM_SIZE                        (8 * 1024 * 1024)   // 8,388,608 B

// MEMP_PBUF：只给 PBUF_ROM/PBUF_REF 用的「无 payload 的 pbuf 壳」。
// 本移植 **不随连接数增长**：tcp_write 一律 COPY(走 mem.c 堆，不用 PBUF_ROM)，
// 唯一的用户是 ip4_frag.c 出站分片(PBUF_REF)——而 TCP 已被 MSS 钳住不会分片，实际近乎不用。
// 保持 256 即可。256 × 24 = 6,144 B
#define MEMP_NUM_PBUF                   256

// ★ 并发被劫持连接总数（**所有设备共享**）。
// 256 太小：一台手机 50~150 条，3~5 台活跃设备就打满。打满后 tcp_alloc() 的降级链是
//   杀最老 TIME-WAIT → 杀 LAST-ACK → 杀 CLOSING → **杀一条优先级更低的活连接** → 才丢 SYN，
// 全程无日志：设备侧只看到「网页转圈」或「连接莫名断掉」，极难定位（故下面打开 MEMP_STATS）。
// 取 2048：十几台设备 × ~130 条 ≈ 2000，家用场景有充分余量。
//     2048 × 216 = 442,368 B（≈432 KiB）
// 不取更大的理由不是内存，是 tcp_active_pcbs 是**单链表**：tcp_slowtmr(500ms) 每次全表走一遍
// （2048 项 ≈ 0.1 ms，可忽略），tcp_input 查表虽是 O(n) 但命中后会把该 pcb 移到表头(MRU)，
// 热流基本 O(1)。再往上（万级）收益递减，且本机侧 ephemeral 端口（Windows 默认 16384 个）
// 和 mihomo 的 socket 数会先成为瓶颈。
#define MEMP_NUM_TCP_PCB                2048
#define MEMP_NUM_TCP_PCB_LISTEN         8       // 8 × 56 = 448 B（只挂 accept-all 监听，不随设备数变）

// tcp_seg：出站发送队列（未发/未确认）+ 乱序重组队列(TCP_QUEUE_OOSEQ)。
// 单条连接的发送队列硬上限 = TCP_SND_QUEUELEN = 4*60KB/1460 = 168 段。
// 「所有连接同时满队列」= 2048 × 168 = 344k 段 = 11 MB —— 这是病态上限，不值得预留。
// 按现实取值：真正把队列填满的是「正在满窗下载」的连接，其在途深度 ≈ TCP_WND/TCP_MSS = 42 段
// （设备在局域网侧 RTT ~1ms，队列排空很快）。
//     8192 / 168 = 48 条连接可同时顶到 TCP_SND_QUEUELEN 硬上限
//     8192 /  42 = 195 条连接可同时满窗下载
// 覆盖十几台设备绰绰有余，代价仅 8192 × 32 = 262,144 B（256 KiB）。
// 另：段池耗尽是**优雅降级**（tcp_write 返 ERR_MEM 由 pumpToLwip 重试；OOSEQ 拷贝失败就丢，
// 对端重传补上），不像 TCP_PCB 耗尽那样会杀活连接。
// 硬约束：MEMP_NUM_TCP_SEG >= TCP_SND_QUEUELEN(168)，否则 init.c #error。
#define MEMP_NUM_TCP_SEG                8192

// ★ lwIP 的 UDP 层在本移植里**完全没用到**：NetStack::inputFrame 见 proto==17 就转
// handleUdpFrame 自己做四元组 NAT（上限在 NetStack 的 kMaxUdpFlowsTotal/PerDevice），
// 根本不会走到 udp_input；全工程零处 udp_new()。原来的 256 条纯属白占 12 KB。
// 留 16 条当兜底（LWIP_UDP=1 时 lwIP 要求 MEMP_NUM_UDP_PCB>=1）。16 × 48 = 768 B
#define MEMP_NUM_UDP_PCB                16

// IP 分片重组：同理，只有 TCP/ICMP 的分片才会进 lwIP（UDP 分片被上面截走了），极罕见。
// 硬约束 MEMP_NUM_REASSDATA <= IP_REASS_MAX_PBUFS（每个 reassdata 至少要装 2 个 pbuf）。
// 把 IP_REASS_MAX_PBUFS 从默认 10 提到 64（借 PBUF_POOL 的 pbuf，不额外占内存），
// reassdata 提到 16 条并发。opt.h 另建议 PBUF_POOL_SIZE > 2*IP_REASS_MAX_PBUFS → 2048 > 128 ✓
//     16 × 40 = 640 B
#define MEMP_NUM_REASSDATA              16
#define IP_REASS_MAX_PBUFS              64

// etharp 出站待解析队列。被劫持设备都由 NetStack::addDevice 预置**静态** ARP 项，
// 出站几乎从不需要排队等 ARP，所以**不随设备数增长**；保持 64。64 × 16 = 1,024 B
#define MEMP_NUM_ARP_QUEUE              64
// lwIP 内部周期定时器数 = LWIP_TCP+IP_REASSEMBLY+LWIP_ARP+LWIP_ACD = 1+1+1+0 = 3，16 足够有余。
//     16 × 32 = 512 B
#define MEMP_NUM_SYS_TIMEOUT            16

// PBUF_POOL：**入站**方向的帧缓冲（inputFrame 用 pbuf_alloc(PBUF_RAW,…,PBUF_POOL)），
// 以及被 TCP 乱序队列(OOSEQ)扣住不放的那些段。一帧 ≤1514 < 1600，恒为单个 pbuf。
// 在序数据是「收到→lwipTcpRecv 拷走→立刻 free」，占用极短；真正压住池子的是 OOSEQ：
// 一条丢包重传中的连接最多扣住 TCP_WND/TCP_MSS ≈ 42 个 pbuf。
//     2048 / 42 ≈ 48 条连接可同时处于丢包恢复而不耗尽池子
// 代价 2048 × 1624 = 3,325,952 B（≈3.17 MiB），是本文件最大的一块。
// 池子见底时 lwIP 有安全阀（NO_SYS + PBUF_POOL_FREE_OOSEQ=1 默认开）：pbuf_pool_is_empty()
// 会调 tcp_free_ooseq() 把乱序队列整体丢掉换回 pbuf，代价只是几次重传。
// 硬约束：TCP_WND(61440) <= PBUF_POOL_SIZE × (PBUF_POOL_BUFSIZE - 54) = 2048 × 1546 = 3,166,208 ✓
#define PBUF_POOL_SIZE                  2048
#define PBUF_POOL_BUFSIZE               1600  // > MTU+以太头

// —— 静态内存小结（BSS，惰性提交）——
//   mem.c 堆                8,388,608
//   PBUF_POOL 2048×1624     3,325,952
//   TCP_PCB   2048× 216       442,368
//   TCP_SEG   8192×  32       262,144
//   PBUF       256×  24         6,144
//   ARP_QUEUE   64×  16         1,024
//   UDP_PCB     16×  48           768
//   REASSDATA   16×  40           640
//   SYS_TIMEOUT 16×  32           512
//   PCB_LISTEN   8×  56           448
//   ─────────────────────────────────
//   合计         4,040,000 + 8,388,608
// 实测（mingw x86_64，size -A 各 .obj 的 .bss）：memp.c 4,039,712 + mem.c 8,388,672
//   + stats.c 96 = **12,428,480 B ≈ 11.85 MiB**（与上表差 288 B 是池描述符表/对齐）。
// 调优前同口径 ≈ 3,021,000 B ≈ 2.88 MiB → **净增 ≈ 9.0 MiB 的 BSS**（惰性提交，实际 RSS 按用量）。

// —— 协议开关 ——
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
// 为每台被劫持设备预置静态 ARP(ip↔mac)，lwIP 回包直接用其 MAC，不发 ARP 请求（默认关，必须打开）。
#define ETHARP_SUPPORT_STATIC_ENTRIES   1
// ARP 表容量：每台被劫持设备占一条静态项，默认 10 条太小（多网卡下两个网段的设备加起来更容易顶到）。
// 顶满时 etharp_add_static_entry 会挤掉别人的条目 —— 表现为某台设备莫名不通，不会报错。
#define ARP_TABLE_SIZE                  64
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_DNS                        0
#define IP_FORWARD                      0
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1

// Coast 透明网关补丁开关：让 ip4_input 接收「目的 IP 不属于本机」的单播包（见 ip4.c 补丁），
// 用户态栈据此为被劫持设备终结发往任意公网 IP 的连接。
#define LWIP_ACCEPT_ALL_UNICAST         1

// —— TCP 调参 ——（桌面高吞吐）
#define LWIP_TCP                        1
#define TCP_MSS                         1460
// TCP_WND/TCP_SND_BUF 必须 <= u16_t(65535)（未开窗口缩放）；取 60KB 兼顾吞吐与该约束。
#define TCP_WND                         (60 * 1024)
#define TCP_SND_BUF                     (60 * 1024)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_QUEUE_OOSEQ                 1
#define LWIP_TCP_SACK_OUT               0
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      0xff

// —— UDP ——
#define LWIP_UDP                        1

// —— netif 回调 ——（NetStack 需要监听 link/status 可按需开；此处最简）
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_HOSTNAME             0
#define LWIP_NETIF_TX_SINGLE_PBUF       1

// —— 校验和：真实 NIC，需要 lwIP 自己算/验 ——
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_ICMP               1
// 只「生成」不「校验」入站校验和：TAP/虚拟链路常因 checksum offload 使入站帧校验和不完整（会被
// 误丢弃）；真实 NIC 上入站帧的校验和由对端硬件已算好，此处信任之（TCP 层另有端到端校验）。
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_TCP              0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_CHECK_ICMP             0

// —— 统计/调试 ——
// 只开 **memp/mem 池统计**，协议计数器全关。
// 为什么开：上面几个池（尤其 MEMP_TCP_PCB / PBUF_POOL / MEMP_TCP_SEG）耗尽是本模块最阴的故障——
// lwIP 不打日志、不返回错误给上层，SYN 就没了，甚至会**悄悄杀掉一条活着的连接**（见 tcp_alloc
// 的降级链）。开了之后 memp_malloc 失败会累加 lwip_stats.memp[MEMP_TCP_PCB]->err，
// 同时 ->max 是高水位线，能直接回答「到底是不是撞上限了 / 离上限还有多远」。
// 代价：静态内存 ≈ 350 B（每池一个 struct stats_mem + 指针表）；运行时每次 memp_malloc/free
// 多一次 u16 自增和一次 max 比较——相对每包的校验和/拷贝完全是噪声。
// 注意 LWIP_STATS_LARGE=0 → 计数器是 u16_t：err 只当「有没有发生过」的信号用；
// max 的量程 65535 也覆盖得住当前所有池的条目数（最大 2048）。
// TODO(未做，需改 NetStack.cpp)：目前只有「数据存在」，还没人读它。要真正可诊断，
//       需要在 NetStack 里定期把 memp[*]->err/->max 打到 LogModel 或状态栏。
#define LWIP_STATS                      1
#define MEM_STATS                       1
#define MEMP_STATS                      1
#define LINK_STATS                      0
#define ETHARP_STATS                    0
#define IP_STATS                        0
#define IPFRAG_STATS                    0
#define ICMP_STATS                      0
#define UDP_STATS                       0
#define TCP_STATS                       0
#define LWIP_STATS_DISPLAY              0   // 需要 printf，保持关
#define LWIP_DEBUG                      0
// 联调时可临时改回 1 并打开 IP_DEBUG/TCP_INPUT_DEBUG/ETHARP_DEBUG=LWIP_DBG_ON，让 lwIP 打印丢包原因。

// —— 杂项 ——
#define LWIP_NETIF_API                  0
#define LWIP_SO_RCVTIMEO                0
#define LWIP_TCPIP_CORE_LOCKING         0
#define LWIP_CHECKSUM_ON_COPY           1
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1
