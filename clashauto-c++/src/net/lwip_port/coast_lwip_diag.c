/* `coast_lwip_diag` 的唯一定义。
 *
 * 计数器由 lwIP 的 C 文件（tcp_in.c 等）累加、由 NetStack 读出来写进网关诊断日志，
 * 两边都只 `#include "coast_lwip_diag.h"` 拿声明，实例得有个地方落。放在移植层
 * （lwip_port/）而不是某个 .cpp 里：它属于这份 lwIP 移植，且必须是 C 链接 ——
 * 定义在 C++ 文件里会被名字修饰掉，lwIP 那边链接不到。
 *
 * 零初始化即可（静态存储期），不需要 init 调用。
 */
#include "coast_lwip_diag.h"

struct coast_lwip_diag coast_lwip_diag;
