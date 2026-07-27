#pragma once

// lwIP 移植配置（Coast 透明网关的用户态协议栈）。
// 运行模式：NO_SYS=1（无操作系统/单线程），仅 raw API，由 Qt 事件循环驱动：
//   · IL2Endpoint 收到帧 → 交 NetStack 包成 pbuf → netif->input(ethernet_input)。
//   · lwIP 需要出网时 → netif->linkoutput → 序列化成以太帧 → IL2Endpoint::send。
//   · 定时器：QTimer 周期调用 sys_check_timeouts()（TCP 重传/超时/ND6 老化等）。
// 做 IPv4 **和 IPv6** 双栈 + TCP + UDP；不启用 DHCP/DNS/socket/netconn。
// IPv6 与 IPv4 完全对称：ip6.c 有和 ip4.c 同样的 accept-all 补丁，nd6.c 有和 etharp 静态项
// 对应的「静态邻居」补丁（见本文件末尾 IPv6 段与 third_party/lwip 里的 Coast 补丁注释）。

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
// 结构大小（mingw x86_64，本移植的开关组合；MSVC x64 同布局——cc.h 的 pack 只作用于协议头结构）：
//   tcp_pcb 272 | tcp_pcb_listen 56 | tcp_seg 32 | udp_pcb 48
//   pbuf 24 | ip_reassdata 40 | etharp_q_entry 16 | sys_timeo 32
//   PBUF_POOL 单元 = align(24) + align(1600) = 1624
// tcp_pcb 曾实测 216（未开窗口缩放/SACK 时）；现按字段布局推算为 272：
//   +24 = LWIP_WND_SCALE：tcpwnd_size_t 由 u16_t 变 u32_t（tcp.h 里有 8 个这种字段：
//         rcv_wnd / rcv_ann_wnd / cwnd / ssthresh / snd_wnd / snd_wnd_max / snd_buf /
//         bytes_acked）= +16 B，末尾新增 snd_scale + rcv_scale = +2 B，重排后的尾部填充 +6 B
//   +32 = LWIP_TCP_SACK_OUT：rcv_sacks[LWIP_TCP_MAX_SACK_NUM=4]，每项 {u32 left, u32 right}
// **本机无法编译，272 是推算值**，改动此文件时若能在 CI/真机上 size -A 复核一次更稳妥
// （偏差只影响下面小结表的账，不影响正确性）。
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   4

// mem.c 堆：**只服务「本机→设备」方向的发送数据**。NetStack::pumpToLwip 用
// tcp_write(..., TCP_WRITE_FLAG_COPY) 把 socks 收来的字节拷进 PBUF_RAM，这些 pbuf 就在这个堆里；
// 另有 tcp_output/ICMP 的小头部 pbuf（几十字节，瞬时）。
// 单条连接最多占 tcp_sndbuf() ≤ TCP_SND_BUF = 128 KiB，所以堆容量 ≈ 能同时「满窗下载」的连接数：
//     16 MiB / 128 KiB = 128 条并发大流量连接
// 十几台设备里同时真正在拉满带宽的连接远不到这个数（其余是空闲/小请求，几乎不占堆）。
// ★ 这个分母是**跟着 TCP_SND_BUF 走**的，别只改一头：TCP_SND_BUF 从 60 KiB 提到 128 KiB
//   （理由见下面「TCP 调参」里开窗口缩放的那段），若堆仍是 8 MiB 就只剩 8 MiB/128 KiB = 64 条，
//   相对上一版的 139 条是**并发能力腰斩** —— 而容量目标是「十几台设备、每台 50~150 条连接」。
//   所以堆同步翻倍到 16 MiB，把「能同时满窗下载的连接数」按在 128 条（139 → 128，基本持平）。
//   代价是 BSS 多 8 MiB，惰性提交，见本节开头；实际 RSS 仍只随真实在途字节增长。
// 堆耗尽不会静默丢数据：tcp_write 返回 ERR_MEM → pumpToLwip 保留 c->toLwip 里的剩余字节，
// 等 tcp_sent 回调再续（降速而已）。2 MiB 时只够 ~16 条，十几台设备一起下载就会明显限速。
#define MEM_SIZE                        (16 * 1024 * 1024)  // 16,777,216 B

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
//     2048 × 272 = 557,056 B（≈544 KiB）
// 不取更大的理由不是内存，是 tcp_active_pcbs 是**单链表**：tcp_slowtmr(500ms) 每次全表走一遍
// （2048 项 ≈ 0.1 ms，可忽略），tcp_input 查表虽是 O(n) 但命中后会把该 pcb 移到表头(MRU)，
// 热流基本 O(1)。再往上（万级）收益递减，且本机侧 ephemeral 端口（Windows 默认 16384 个）
// 和 mihomo 的 socket 数会先成为瓶颈。
#define MEMP_NUM_TCP_PCB                2048
#define MEMP_NUM_TCP_PCB_LISTEN         8       // 8 × 56 = 448 B（只挂 accept-all 监听，不随设备数变）

// tcp_seg：出站发送队列（未发/未确认）+ 乱序重组队列(TCP_QUEUE_OOSEQ)。
// 单条连接的发送队列硬上限 = TCP_SND_QUEUELEN = 4*128KiB/1460 = 359 段。
// 「所有连接同时满队列」= 2048 × 359 = 735k 段 = 23 MB —— 这是病态上限，不值得预留。
// 按现实取值：真正把队列填满的是「正在满窗下载」的连接，其在途深度 ≈ TCP_SND_BUF/TCP_MSS = 89 段
// （设备在局域网侧 RTT ~1ms，队列排空很快；WiFi 抖到 20~30ms 时才会真的堆到这个深度）。
//     16384 / 359 = 45 条连接可同时顶到 TCP_SND_QUEUELEN 硬上限
//     16384 /  89 = 184 条连接可同时满窗下载
// 池子跟着 TCP_SND_BUF 一起翻倍（8192 → 16384）：不翻的话上面两个数会掉到 22 / 92，
// 相对上一版的 48 / 195 是明显回退。代价 16384 × 32 = 524,288 B（512 KiB），便宜。
// 另：段池耗尽是**优雅降级**（tcp_write 返 ERR_MEM 由 pumpToLwip 重试；OOSEQ 拷贝失败就丢，
// 对端重传补上），不像 TCP_PCB 耗尽那样会杀活连接。
// 硬约束：MEMP_NUM_TCP_SEG(16384) >= TCP_SND_QUEUELEN(359)，否则 init.c #error。
#define MEMP_NUM_TCP_SEG                16384

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
// 一条丢包重传中的连接最多扣住 min(TCP_WND/TCP_MSS, TCP_OOSEQ_MAX_PBUFS) 个 pbuf。
// ★ TCP_WND 提到 128 KiB 后，前一项是 90，光靠它就只剩 2048/90 ≈ 22 条连接可同时丢包恢复 ——
//   比上一版（60 KiB 窗口 → 42 个 → 48 条）差一倍多。而 PBUF_POOL 是本文件**单价最贵**的池
//   （1624 B/条），靠把 2048 加到 4096 去补要多花 3.2 MiB，太亏。改用 TCP_OOSEQ_MAX_PBUFS
//   给**每条连接**的乱序队列直接封顶 48（见下面「TCP 调参」），于是：
//     2048 / 48 ≈ 42 条连接可同时处于丢包恢复而不耗尽池子（与上一版的 48 条基本持平）
// 代价 2048 × 1624 = 3,325,952 B（≈3.17 MiB），是本文件第二大的一块（仅次于 mem.c 堆）。
// 池子见底时 lwIP 还有全局安全阀（NO_SYS + PBUF_POOL_FREE_OOSEQ=1 默认开）：pbuf_pool_is_empty()
// 会调 tcp_free_ooseq() 把乱序队列整体丢掉换回 pbuf，代价只是几次重传。
// 硬约束：TCP_WND(131072) <= PBUF_POOL_SIZE × (PBUF_POOL_BUFSIZE - 54) = 2048 × 1546 = 3,166,208 ✓
#define PBUF_POOL_SIZE                  2048
#define PBUF_POOL_BUFSIZE               1600  // > MTU+以太头

// —— 静态内存小结（BSS，惰性提交）——
//   mem.c 堆               16,777,216
//   PBUF_POOL  2048×1624    3,325,952
//   TCP_PCB    2048× 272      557,056
//   TCP_SEG   16384×  32      524,288
//   PBUF        256×  24        6,144
//   ARP_QUEUE    64×  16        1,024
//   UDP_PCB      16×  48          768
//   REASSDATA    16×  40          640
//   SYS_TIMEOUT  16×  32          512
//   PCB_LISTEN    8×  56          448
//   ──────────────────────────────────
//   合计        4,416,832 + 16,777,216 = 21,194,048 B ≈ **20.21 MiB**
// 上一版（未开窗口缩放/SACK）同口径实测（mingw x86_64，size -A 各 .obj 的 .bss）是
//   12,428,480 B ≈ 11.85 MiB（memp.c 4,039,712 + mem.c 8,388,672 + stats.c 96；与表格差
//   288 B 是池描述符表/对齐）。
// 本版**没有实测**（本机无工具链）：mem.c 那半是精确的（+8 MiB），memp.c 那半里 TCP_SEG 是
//   精确的（+262,144），只有 TCP_PCB 依赖上面 272 的推算（若实际是 272±8，总账偏差 ≤ 16 KiB）。
// → 相对上一版 **净增 ≈ 8.4 MiB 的 BSS**：其中 ≈8.3 MiB 是「窗口从 60 KiB 提到 128 KiB 之后，
//   为了不让并发连接容量回退而同步扩的池子」，64 KiB 是 SACK 的 rcv_sacks 数组。
//   BSS 惰性提交，实际 RSS 仍按真实用量增长。
//
// —— IPv6 追加的静态内存（本次双栈改造新增，未实测，按结构大小估）——
//   ND6 三张表都是 nd6.c 里的全局静态数组：
//     neighbor_cache[64]      ≈ 64 × ~56  ≈ 3.6 KiB（ip6_addr + netif* + lladdr + 计数联合）
//     destination_cache[64]   ≈ 64 × ~44  ≈ 2.8 KiB
//     prefix_list[8] / router_list[8]      < 1 KiB
//     MEMP_NUM_ND6_QUEUE 16 × sizeof(nd6_q_entry)          ≈ 0.3 KiB
//   合计 ND6 三张表 v6 追加 **< 10 KiB**。
//   另有一处**跟着开 v6 变大的**：ip_addr_t 从「4 字节 v4」变成「v4/v6 联合体 + 类型标签」≈20 字节，
//   每个 tcp_pcb 有 local_ip/remote_ip 两个 → 单个 tcp_pcb 约 +32 字节，2048 条 ≈ +64 KiB；
//   TCP_SEG/PBUF/PBUF_POOL 不含 ip_addr_t，不变。v4/v6 连接共用同一批 tcp_pcb/tcp_seg/pbuf 池
//   （一条 v6 连接和一条 v4 的各占一条，不是两套池）。
//   → 双栈相对纯 v4 的 BSS 净增 ≈ **75 KiB**（ND6 表 <10K + tcp_pcb 增量 ~64K），相对 20 MiB 是噪声。
//   ★ 这些数字**没有实测**（本机无工具链）：改动此文件的 v6 项后若能在真机 size -A 复核更稳妥。

// —— 协议开关 ——
#define LWIP_IPV4                       1
// ★ IPv6 双栈：双栈设备的 v6 流量以前直接走真 v6 路由、完全绕过代理（既不进 mihomo、也不在设备页
//   出现），网络 v6 上游一坏就「时通时不通」。打开后 v6 与 v4 走**完全对称**的路径：NdpSpoofer 投毒
//   设备的邻居缓存（把默认路由指到本机 MAC）→ ip6.c 的 accept-all 补丁让 lwIP 终结发往任意公网 v6 的
//   连接 → Socks5 用 ATYP=0x04 拨 mihomo。详细开关见本节下方「IPv6 细调」。
#define LWIP_IPV6                       1
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

// Coast 透明网关补丁开关：让 ip4_input / ip6_input 接收「目的 IP 不属于本机」的单播包
//（见 ip4.c、ipv6/ip6.c 补丁），用户态栈据此为被劫持设备终结发往任意公网 IP 的连接。
// 同一个开关同时管 v4/v6 两个补丁：关掉它这两处代码就完全不改变原生 lwIP 行为。
#define LWIP_ACCEPT_ALL_UNICAST         1

// —— IPv6 细调（与上面 IPv4 段对称；只列与 opt.h 默认值不同或必须钉死的项）——
// 邻居发现（ND6）必开——v6 没有 ARP，「设备 MAC ↔ v6 地址」的解析全靠它，也是投毒/被投毒的载体。
#define LWIP_IPV6_ND                    1
#define LWIP_ICMP6                      1
// 关掉地址作用域（zone）：本移植只有一张逻辑链路视角，被劫持设备的目的绝大多数是全局单播（无 zone），
// 静态邻居项的存/取用的是同一个地址。关掉 zone 后 ip6_addr_eq 退化成纯地址比较、ZONECHECK 变 no-op，
// 省掉「link-local 地址必须带正确 netif zone 才相等」这一整类易错逻辑（NetStack 因此无需给地址 assign
// zone）。代价：不支持「同一 link-local 前缀跨多张卡」的严格区分——本网关用不到。
#define LWIP_IPV6_SCOPES                0
// MLD（组播侦听）：本移植用不到——被劫持流量是单播，accept-all 补丁直接把无主单播收到 inp 上；
// 组播（含 solicited-node）由 ip6_input 的非-MLD 回退分支按 netif 自身地址处理即可。关掉省内存。
#define LWIP_IPV6_MLD                   0
// 不做 v6 分片/重组：TCP 段被两端 MSS 协商钳住（设备侧按自己 v6 MTU 算出 ≤1440 的发送 MSS，不会
// 造出需要分片的段）；UDP 由 NetStack 手工 NAT，根本不进 lwIP。与 IPv4 段「UDP 分片被上面截走」同理，
// 省下 ip6_frag 的重组缓冲。极少数带扩展头/超 MTU 的 v6 分片包会被丢（首个版本可接受）。
#define LWIP_IPV6_FRAG                  0
#define LWIP_IPV6_REASS                 0
#define LWIP_IPV6_FORWARD               0
// 无状态地址自动配置 / DHCPv6 / 路由器请求：一律不要。netif 只需要一个 **link-local** 当自己的
// v6 身份（作为我们发 NS/NA 的源、以及 nd6 内部一致性）；全局地址不配——被劫持设备的 v6 目的是
// 「任意公网地址」，由 accept-all 补丁 + nd6 静态邻居补丁接管，不依赖本机有没有全局 v6。
#define LWIP_IPV6_AUTOCONFIG            0
#define LWIP_IPV6_DHCP6                 0
#define LWIP_IPV6_SEND_ROUTER_SOLICIT   0
// 跳过重复地址检测（DAD）：我们给 netif 的 link-local 是本机 MAC 派生、且这条抓包链路上不会有别人
// 抢它；关掉 DAD 免得启动时先发一轮 NS 等待，netif 的 v6 地址即刻可用（PREFERRED）。
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS   0

// ND6 表容量：与 ARP_TABLE_SIZE(64) 对齐——十几台设备各占一条静态邻居项（nd6.c 的 Coast 静态
// 邻居补丁装的），加上路由器/目的缓存，默认的 10 太小会互相挤掉（表现为某台设备 v6 莫名不通）。
#define LWIP_ND6_NUM_NEIGHBORS          64
#define LWIP_ND6_NUM_DESTINATIONS       64
#define LWIP_ND6_NUM_PREFIXES           8
#define LWIP_ND6_NUM_ROUTERS            8
// ND6 出站待解析队列：被劫持设备都由 addDevice6 预置**静态邻居**，出站几乎从不排队等解析
//（和 IPv4 的静态 ARP 同理）。保持一个小值即可。
#define LWIP_ND6_QUEUEING               1
#define MEMP_NUM_ND6_QUEUE              16

// —— TCP 调参 ——（桌面高吞吐）
#define LWIP_TCP                        1
#define TCP_MSS                         1460

// ★ TCP 定时器周期。默认 250ms → tcp_fasttmr(延迟 ACK/快重传的发送时机) 250ms 一拍、
//   tcp_slowtmr(**超时重传**、persist、keepalive) 500ms 一拍。这个粒度是给嵌入式设备定的，
//   放在本网关上偏粗得离谱：lwIP 这条 TCP 的对端是**同一个局域网里的设备**，RTT 是 0.3~30ms，
//   而丢一帧要等半秒才重传 —— 等待时间比 RTT 高两个数量级，恢复期间该连接完全停摆。
//   叠加 NetStack 那边的泵周期（改前 200ms，与 250ms 不整除）后实测节奏更差：fasttmr ≈400ms、
//   slowtmr ≈600ms。用户侧的现象就是「被代理设备访问什么都慢、偶尔打不开」，**与目标是国内
//   还是国外无关**——丢的是本机↔设备这条所有流量共用的腿。
//   降到 100ms：fasttmr 100ms / slowtmr 200ms。RTO 由 lwIP 按毫秒换算成 slowtmr 拍数
//   （tcp.c 的 LWIP_TCP_RTO_TIME/TCP_SLOW_INTERVAL），所以**初始** RTO 仍是 3s，不受影响；
//   真正变的是**收敛后的下限**：VJ 估计器在局域网上（实测 RTT 恒 < 1 拍 → m=0）会让 sa→0、
//   sv 收敛到 3（sv<4 时 sv>>2 == 0，不再下降），于是 rto_min = (sa>>3)+sv = 3 拍 ——
//     · 旧值 250ms：slowtmr 500ms × 3 = **1.5 秒**才重传第一次；
//     · 新值 100ms：slowtmr 200ms × 3 = **0.6 秒**。
//   即：本机→设备方向丢一帧的代价从 1.5s 降到 0.6s。（还想更低就得改 lwIP 的估计器本身，
//   那是另一码事；本网关真正的大头是「别丢帧」，见 L2Endpoint_linux.cpp 发方那一节。）
//   代价：tcp_slowtmr 走一遍 tcp_active_pcbs 单链表的频率从 2 次/秒变 5 次/秒。按本文件
//   MEMP_NUM_TCP_PCB 那段的实测量级（2048 项 ≈ 0.1ms/遍），合计 0.5ms/秒，可忽略。
//   ★ 改这个值必须同步看 NetStack::init 里的泵周期 kLwipPumpIntervalMs——它要保持在本值的
//     1/4 左右，否则定时器要等下一次泵才跑得到，等于白改。
#define TCP_TMR_INTERVAL                100

// ★ 窗口缩放（RFC 7323）。**先把收益口径说清楚，别当成「翻数倍吞吐」的银弹**：
// 本网关是**终结式（split-TCP）代理** —— 被劫持设备的 TCP 连接就在 lwIP 里终结，去上游的
// 那条连接由 mihomo 另开。所以 lwIP 这条 TCP 的 RTT 是「设备 ↔ 本机」的**局域网 RTT**，
// 不是跨境节点那 50ms+（那部分 RTT 落在 mihomo 的 socket 上，与本文件无关）。
// 单流上限 ≈ 窗口 / RTT：
//   · 有线 0.3~1ms： 60 KiB → 约 480 Mbps ~ 1.6 Gbps。窗口**根本不是瓶颈**，本改动零收益。
//   · WiFi 5~30ms（同频干扰、省电休眠、A-MPDU 聚合都会把 RTT 抖上去，且是**抖动**不是常数）：
//        60 KiB → 20ms 时约 24 Mbps、30ms 时约 16 Mbps ——**这里窗口就是瓶颈**。
//       128 KiB → 20ms 时约 51 Mbps、30ms 时约 35 Mbps。
// 结论：收益集中在「WiFi / 高延迟或高抖动的局域网链路」，外加突发流量时少一些「窗口满了只能
// 干等 ACK」的停顿。有线千兆桌面基本看不出差别 —— 不要对外宣称成倍提升。
// 已知代价（老实写在这）：单条连接可以在本机排到 128 KiB 的下行数据，对一条只有 5 Mbps 的
// 弱 WiFi 客户端就是 ~200ms 的自制缓冲膨胀。只影响该条大流量连接自身的时延（拥塞窗口才是
// 真正的发送闸门，snd_buf 只是上限），且 NetStack 的下行水位另有 64 KiB 的二级闸。
#define LWIP_WND_SCALE                  1
// 缩放因子：TCP 头里通告的是 TCP_WND >> TCP_RCV_SCALE。init.c 的三条硬约束：
//   TCP_RCV_SCALE <= 14 ✓ | TCP_WND <= (0xFFFF << TCP_RCV_SCALE) | (TCP_WND >> TCP_RCV_SCALE) != 0
//   取 1 会 #error：131072 > 0xFFFF<<1 = 131070。取 2：131072 <= 262140 ✓，>>2 = 32768 ✓。
// 只取「刚好够用」的 2：因子越大通告粒度越粗（2 → 4 字节一档，无所谓），但也越容易在将来
// 调大 TCP_WND 时忘掉这条约束 —— 保持紧配合，改 TCP_WND 时编译器会当场提醒。
// 对端不支持窗口缩放时 lwIP 自动降级：TCP_WND_MAX(pcb) 在 TF_WND_SCALE 未置位时返回
// TCPWND16(TCP_WND) = 65535，rcv_wnd 停在 tcp_alloc 时的 TCPWND_MIN16(TCP_WND)，行为同旧版。
#define TCP_RCV_SCALE                   2

// 两个方向压的是**完全不同的池**（改任一个都要回上面把对应的账算一遍）：
//   TCP_WND     = 设备→本机（设备**上传**）的接收窗口 → 压 PBUF_POOL（乱序队列扣住的 pbuf）
//   TCP_SND_BUF = 本机→设备（设备**下载**）的发送缓冲 → 压 mem.c 堆（tcp_write COPY 进 PBUF_RAM）
// 这里仍取同一个值（128 KiB）只是因为两边各自配了对应的补偿（堆翻倍 / OOSEQ 封顶），不是因为
// 它们有什么内在关系。为什么不拉到 256 KiB：那要么把「能同时满窗下载的连接数」从 128 条
// 再砍半到 64 条，要么把堆再翻到 32 MiB；而 20ms RTT 的 WiFi 本来也交付不了 100 Mbps 的单流，
// 属于拿实打实的并发容量去换用不上的峰值。
#define TCP_WND                         (128 * 1024)  // 131,072
#define TCP_SND_BUF                     (128 * 1024)  // 131,072
// 沿用「4 段/MSS」的老公式：4 × 131072 / 1460 = 359 段。
// init.c 硬约束：<= 0xFFFF ✓ | >= 2 ✓ | >= 2*(TCP_SND_BUF/TCP_MSS) = 178 ✓
//               | MEMP_NUM_TCP_SEG(16384) >= 359 ✓
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
// ★ 必须**显式**给，不能吃 opt.h 的默认值。默认是
//     min(max(TCP_SND_BUF/2, 2*MSS+1), TCP_SND_BUF-1) = 65536，
// 而 init.c 有一条 `TCP_SNDLOWAT >= (0xFFFF - 4*TCP_MSS)`（= 59695）就 #error 的检查
// （它是替 api_msg.c 里的 u16 运算兜底的）—— 60 KiB 时默认值 30720 恰好过关，128 KiB 时
// 直接撞墙。取 32 KiB：远低于 59695，也远低于 TCP_SND_BUF。
// 用不上但必须合法：TCP_SNDLOWAT 只被 netconn/socket 的「可写」判据引用，而本移植
// LWIP_NETCONN = LWIP_SOCKET = 0，raw API 一处都不碰它。写死常量而不是 TCP_SND_BUF/4，
// 是为了将来有人再调大 TCP_SND_BUF 时不会又悄悄漂过 59695。
#define TCP_SNDLOWAT                    (32 * 1024)
#define TCP_QUEUE_OOSEQ                 1
// ★ 给**每条连接**的乱序队列封顶 48 个 pbuf（≈68 KiB），配合上面 TCP_WND 的提升。
// 不封顶的话一条正在丢包恢复的连接最多扣住 TCP_WND/TCP_MSS = 90 个 PBUF_POOL 条目，
// 2048/90 ≈ 22 条这样的连接就能把池子抽干（上一版：60 KiB → 42 个 → 48 条）。48 把它拉回
// 2048/48 ≈ 42 条，代价为零 —— 比把单价 1624 B 的 PBUF_POOL 加大划算得多。
// 触发时的行为（tcp_in.c，TCP_OOSEQ_PBUFS_LIMIT 分支）：把越界的那个 seg **及其之后**整段
// tcp_segs_free 掉，靠对端重传补回来；开了 SACK 时还会同步 tcp_remove_sacks_gt 收回对应的
// SACK 通告，不会出现「SACK 说收到了、实际已经丢了」的谎报。
// 这跟池子见底时 pbuf_pool_is_empty() → tcp_free_ooseq() 是同一类降级，只是改成**每连接**
// 触发：一条坏链路的连接不再有能力把全局池子拖垮。
// 48 × 1460 ≈ 68 KiB 的乱序缓冲，覆盖「第 1 个包丢了、后面 47 个都到了」的典型场景绰绰有余。
#define TCP_OOSEQ_MAX_PBUFS             48

// ★ 选择性确认（RFC 2018）。**注意方向：这个开关只管「出」，名字里的 _OUT 是字面意思。**
// 打开后 lwIP 作为**接收方**会在 ACK 里带上 SACK 块，告诉对端「这几段我已经收到了」；
// lwIP 作为**发送方**并不解析收到的 SACK（tcp_in.c 的 tcp_parseopt 只认 MSS/TS/WS/SACK_PERM，
// 没有 SACK 块的解析），所以它自己的重传仍是「从丢失点整段重来」。于是：
//   · 受益方向 = 设备 → 本机（设备**上传**）：设备内核拿到我们的 SACK 后只补真正缺的那几段，
//     不用把丢包点之后已经送达的数据全部重发。
//   · 本机 → 设备（设备**下载**）**没有**收益 —— 那是 lwIP 当发送方，它看不懂 SACK。
// 再强调一遍口径（和窗口缩放同理，这是终结式代理）：这里能救的是**局域网丢包**——WiFi 的
// 重传耗尽、AP/交换机缓冲溢出、信道拥塞。跨境节点那段丢包发生在 mihomo 的 OS socket 上，
// 跟 lwIP 这条 TCP 没有任何关系，本开关**管不着**。别把它当"跨境链路优化"。
// 家用 WiFi 上行虽然量不大（多是 ACK、表单、上传照片、视频会议推流），但恰恰是最容易丢包的
// 那条腿：一次丢包在无 SACK 时可能白白重传半个窗口，视频会议/网盘上传会直接卡一下。
//
// 代价，逐项：
//   · 内存：每个 tcp_pcb 多 LWIP_TCP_MAX_SACK_NUM × 8 = 32 B（rcv_sacks 数组），
//     2048 条 → +65,536 B（64 KiB）。默认的 4 条 SACK 范围够用，不下调（调小只省几十 KiB，
//     却会在多点丢包时少通告一段，反而多一次重传）。
//   · 选项字节：SYN-ACK 多 4 B 的 SACK_PERM（含 NOP 对齐），与 MSS(4) + WS(4) 合计 12 B，
//     远在 40 B 的 TCP 选项上限内 ✓。带 SACK 块的 ACK：第一块 12 B（2 NOP + 2 B 头 + 8 B），
//     之后每块 +8 B，最多 4 块 = 36 B ≤ TCP_MAX_OPTION_BYTES(40)，lwIP 的 tcp_get_num_sacks
//     自己按 optlen 收着发，不会溢出。
//   · 多几个纯 ACK：lwIP 不支持把 SACK 塞进带数据的包，有待发 SACK 时 tcp_in.c 会强制
//     tcp_ack_now() 单发一个空 ACK。只在乱序期间发生，是丢包恢复的代价，划算。
//   · CPU：tcp_add_sack / tcp_remove_sacks_lt|gt 都是 O(LWIP_TCP_MAX_SACK_NUM)=O(4) 的小循环，
//     且只在 ooseq 非空（即真的乱序了）时才跑，正常收包路径零开销。
// 硬约束（init.c）：LWIP_TCP_SACK_OUT 需要 TCP_QUEUE_OOSEQ=1 ✓（SACK 块就是从乱序队列推出来的）；
// LWIP_TCP_MAX_SACK_NUM >= 1 ✓（用 opt.h 默认的 4）。
// 与上面 TCP_OOSEQ_MAX_PBUFS 的配合已由 lwIP 保证：乱序队列被截断时会
// tcp_remove_sacks_gt() 同步收回对应的 SACK 通告，不会谎报「已收到」。
#define LWIP_TCP_SACK_OUT               1
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
// ICMPv6（含 ND6 的 NS/NA/RS/RA）也只生成不校验，理由同 ICMP。
#define CHECKSUM_GEN_ICMP6              1
#define CHECKSUM_CHECK_ICMP6            0

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
// 读取方：NetStack.cpp 的 pollLwipPoolStats()，搭已有的 200ms lwIP 定时器泵（不另开定时器）。
// 常态只做十来次 u16 比较；某个池的 err 相对上次上报变了才 qWarning 一条，且两次上报至少隔
// 30s（池子见底时每 200ms 都有新失败，不节流会刷屏）；被节流吃掉的那次不推进基线，下个窗口补报。
#define LWIP_STATS                      1
#define MEM_STATS                       1
#define MEMP_STATS                      1
#define LINK_STATS                      0
#define ETHARP_STATS                    0
#define IP_STATS                        0
#define IPFRAG_STATS                    0
#define ICMP_STATS                      0
#define UDP_STATS                       0
// ★ TCP_STATS 打开：给诊断日志（NetStack.cpp 的 lwipStatsLine → gateway-diag.log）提供
// xmit / recv / drop / memerr 四个量，用来判断「TCP 这一层在不在异常」。
// **说明白它给不了什么**：lwIP 的 struct stats_proto 里**没有重传计数器**，tcp_out.c 的三个
// 重传入口一个计数都不加 —— 所以拿不到「重传了几次」这个最想要的数。目前只能用 xmit 相对
// 设备侧应有段数偏高来间接判断；要精确值得往 vendored lwIP 打一个 Coast 补丁（同 accept-all /
// nd6 静态邻居的路数），留待以后。
// 代价：几处 u16 自增 + 一个 struct stats_proto（约 24 B），可忽略。
// 注意 LWIP_STATS_LARGE=0 → 计数器是 u16_t 会回绕；采样器用 u16 运算算增量，回绕一次仍正确。
#define TCP_STATS                       1
#define LWIP_STATS_DISPLAY              0   // 需要 printf，保持关
#define LWIP_DEBUG                      0
// 联调时可临时改回 1 并打开 IP_DEBUG/TCP_INPUT_DEBUG/ETHARP_DEBUG=LWIP_DBG_ON，让 lwIP 打印丢包原因。

// —— 杂项 ——
#define LWIP_NETIF_API                  0
#define LWIP_SO_RCVTIMEO                0
#define LWIP_TCPIP_CORE_LOCKING         0
#define LWIP_CHECKSUM_ON_COPY           1
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1
