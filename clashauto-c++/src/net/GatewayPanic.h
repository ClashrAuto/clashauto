#pragma once

// 崩溃兜底：进程非正常死亡时，把「还原被代理设备 ARP」的帧发出去。
//
// 为什么需要：被劫持设备的默认网关就是本机，它的**每一个**出网包都要经本机的用户态栈转发。
// 正常退出（退出/关机/睡眠）都有 LanGateway::disableAll() 还原 ARP，设备亚秒内回到真路由器；
// 但进程被 SIGSEGV/abort/未处理异常打死时，那条路一步都走不到 —— 设备会一直把网关指着一个
// 已经不存在的转发者，直到自己的邻居缓存老化（几十秒，取决于客户端和真网关的 ARP 活跃度）。
//
// 做法：上劫持时就把该设备的还原帧**预先拼好**存进这里（连同一个只做一次系统调用的裸发函数），
// 崩溃处理器里只做「遍历 + 发字节」，不分配、不加锁、不碰 Qt —— 这是能在信号处理器里安全做的
// 极少数事情之一。拼帧、注册、注销都在正常路径上完成。
//
// 覆盖范围（诚实说明）：
//   覆盖 —— 段错误 / abort（含 assert、std::terminate、glibc 的 double free 检测）/ Windows 未处理异常。
//   不覆盖 —— 断电、SIGKILL、TerminateProcess、内核 OOM 杀进程（这些不给进程任何执行机会），
//             以及网线已经拔掉的场合（帧物理上发不出去）。那些只能靠设备侧邻居缓存老化。
//   只发 v4 ARP —— v6 的 NDP 还原依赖运行期学到的设备地址，做不到「一次注册长期有效」，
//                  仍由正常路径（disableAll）负责。设备的默认路由是 v4 网关，先保住它。
#include <QByteArray>
#include <QVector>

#include "IL2Endpoint.h"

namespace GatewayPanic {

// 装崩溃处理器。进程启动时调一次即可（幂等）。POSIX 装 SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT
// （SA_RESETHAND，发完帧就把默认行为放回去，让崩溃照常产生 core/报错）；Windows 装
// SetUnhandledExceptionFilter；两边都额外接 std::set_terminate。
void installHandlers();

// 登记一台设备的「遗嘱」。key 为设备 MAC 的打包值（调用方自己的 macKey）；frames 为
// ArpSpoofer::healFrames() 的结果。同 key 重复登记 = 覆盖。槽位有限（kMaxSlots），满了就忽略
// （代理十几台设备是产品上限，不做动态分配 —— 崩溃处理器里不能碰堆）。
void arm(quint64 key, IL2Endpoint::RawSendFn fn, void *ctx, const QVector<QByteArray> &frames);

// 撤销登记（正常 disable / disableAll / 端点销毁前必须调，否则崩溃时会对着已释放的 fd 发帧）。
void disarm(quint64 key);
void disarmAll();

} // namespace GatewayPanic
