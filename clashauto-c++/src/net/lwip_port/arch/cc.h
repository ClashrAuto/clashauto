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
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) \
    do { printf("lwip assert: %s @ %s:%d\n", (x), __FILE__, __LINE__); abort(); } while (0)

// 让 lwIP 使用标准 <string.h>（memcpy 等）与随机数：
#define LWIP_RAND() ((u32_t)rand())
