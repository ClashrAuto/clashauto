#ifndef COAST_LWIP_DIAG_H
#define COAST_LWIP_DIAG_H

/* Coast 诊断补丁：lwIP 那几条**静默丢 SYN / 静默杀连接**的路径的计数器。
 *
 * 为什么必须打进 lwIP 而不是在 NetStack 里数：这几件事全都发生在「上层根本不会被回调」的位置——
 *   · tcp_listen_input 的 backlog 判据：accepts_pending 顶到 backlog 就 `return`，
 *     既不分配 pcb、也不发 SYN-ACK、更不通知上层。表现 = 设备侧全员 SYN-SENT，网关侧毫无痕迹。
 *   · tcp_timewait_input：SYN 命中一条还在 TIME_WAIT 的四元组，且序号不在窗内 → 直接 return。
 *     同样是零痕迹的静默丢弃，而客户端会一路 RTO 退避重传。
 *   · tcp_alloc 的降级链：kill TIME_WAIT / LAST_ACK / CLOSING / **一条活连接**。前三者的
 *     memp err 会被 MEMP_STATS_DEC 减回去（见 tcp.c 里那几处「adjust err stats」），
 *     所以 lwip_stats 里的 err 恒为 0，光看 err 会以为「池子没撞过上限」——实际一直在撞。
 *
 * 全是 u32 明码累加，无原子、无判开关（lwIP 单线程跑在网关工作线程上，与 GatewayDiag 同前提）。
 * 读取方：NetStack.cpp 的 lwipStatsLine()，按窗口算增量写进 gateway-diag.log。
 * 关掉这套计数只需把下面的宏定义成空——但它们的成本是每事件一条 add，没有关掉的理由。
 */

#ifdef __cplusplus
extern "C" {
#endif

struct coast_lwip_diag {
    unsigned int syn_drop_backlog;  /* tcp_listen_input：accepts_pending >= backlog → 静默丢 SYN */
    unsigned int syn_hit_timewait;  /* tcp_timewait_input：SYN 命中 TIME_WAIT 的四元组（复用端口） */
    unsigned int tw_rst;            /* 其中序号落在窗内 → 回 RST（另一半原本是静默丢） */
    unsigned int tw_recycled;       /* 其中序号在窗外 → Coast 补丁按 RFC 1122 回收该 TIME_WAIT，当新连接接下 */
    unsigned int tw_killed;         /* tcp_alloc 池满 → 杀掉最老的一条 TIME_WAIT */
    unsigned int lastack_killed;    /* 同上，TIME_WAIT 也没得杀 → 杀 LAST_ACK */
    unsigned int prio_killed;       /* 再没得杀 → **杀一条活连接**（设备侧看到连接凭空断掉） */
    unsigned int alloc_fail;        /* 降级链走完仍拿不到 pcb → SYN 被丢 */
    unsigned int tw_scan;           /* tcp_kill_timewait 累计走过的链表节点数（除以 tw_killed = 每次扫多长） */
};

extern struct coast_lwip_diag coast_lwip_diag;

#ifdef __cplusplus
}
#endif

#endif /* COAST_LWIP_DIAG_H */
