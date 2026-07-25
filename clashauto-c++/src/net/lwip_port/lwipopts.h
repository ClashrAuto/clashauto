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
#define LWIP_SINGLE_NETIF               1   // 只有一个 netif（本网关），省掉遍历

// —— 内存 ——（吞吐优先，桌面内存充裕）
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (2 * 1024 * 1024)

#define MEMP_NUM_PBUF                   256
#define MEMP_NUM_TCP_PCB                256   // 并发被劫持连接上限
#define MEMP_NUM_TCP_PCB_LISTEN         8
#define MEMP_NUM_TCP_SEG                512
#define MEMP_NUM_UDP_PCB                256
#define MEMP_NUM_REASSDATA              16
#define MEMP_NUM_ARP_QUEUE              64
#define MEMP_NUM_SYS_TIMEOUT            16

#define PBUF_POOL_SIZE                  512
#define PBUF_POOL_BUFSIZE               1600  // > MTU+以太头

// —— 协议开关 ——
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
// 为每台被劫持设备预置静态 ARP(ip↔mac)，lwIP 回包直接用其 MAC，不发 ARP 请求（默认关，必须打开）。
#define ETHARP_SUPPORT_STATIC_ENTRIES   1
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
#define TCP_WND                         (64 * 1024)
#define TCP_SND_BUF                     (64 * 1024)
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
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_ICMP             1

// —— 统计/调试 ——（关，省体积）
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
#define LWIP_DEBUG                      0

// —— 杂项 ——
#define LWIP_NETIF_API                  0
#define LWIP_SO_RCVTIMEO                0
#define LWIP_TCPIP_CORE_LOCKING         0
#define LWIP_CHECKSUM_ON_COPY           1
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1
