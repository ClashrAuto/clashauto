#pragma once

// lwIP 编译器/平台抽象层（arch/cc.h）——最小实现，面向 GCC/Clang（Linux）。
// lwIP 2.x 自带标准整型(<stdint.h>)，这里只需字节序、结构体打包、诊断/断言宏。
#include <stdio.h>
#include <stdlib.h>

// —— 字节序 ——（由编译器内建宏推导，x86_64/arm64 均小端）
// 先保证 LITTLE_ENDIAN/BIG_ENDIAN 常量存在（mac/Windows 处理 cc.h 时可能尚未定义），再定 BYTE_ORDER。
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef BYTE_ORDER
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define BYTE_ORDER BIG_ENDIAN
#else
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#endif

// —— 结构体打包 ——
#ifdef _MSC_VER
// MSVC：用 #pragma pack 包住结构体（PACK_STRUCT_BEGIN/END 分别 push/pop），STRUCT 后缀留空。
#define PACK_STRUCT_BEGIN __pragma(pack(push, 1))
#define PACK_STRUCT_END __pragma(pack(pop))
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_FIELD(x) x
#else
// GCC/Clang：attribute((packed))。
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(x) x
#endif

// —— 诊断 / 断言 ——
// ★ 必须走 **stderr 且显式 fflush**，不能用 printf：stdout 在非 tty（systemd/重定向）下是**全缓冲**的，
//   而断言紧接着 abort()，缓冲区根本来不及刷 —— 消息就此人间蒸发。真机压测时就吃过这个亏：
//   进程 SIGABRT 挂掉，journal 里**一个字都没有**，只能靠 gdb 抓栈才知道是 lwIP 的 tcp_receive 断言。
//   stderr 无缓冲，再补一次 fflush 保底，崩溃现场才留得下来。
// 注意 DIAG 的参数 x 是**带括号的实参表**（`("fmt", a, b)`），只能原样跟在函数名后面 ——
// 想换成 fprintf 就得塞进 stream 参数，宏做不到，所以保留 printf、补一次 fflush 即可。
#define LWIP_PLATFORM_DIAG(x)   do { printf x; fflush(stdout); } while (0)
#define LWIP_PLATFORM_ASSERT(x)                                                                    \
    do {                                                                                           \
        fprintf(stderr, "lwip assert: %s @ %s:%d\n", (x), __FILE__, __LINE__);                     \
        fflush(stderr);                                                                            \
        abort();                                                                                   \
    } while (0)

// 让 lwIP 使用标准 <string.h>（memcpy 等）与随机数：
#define LWIP_RAND() ((u32_t)rand())
