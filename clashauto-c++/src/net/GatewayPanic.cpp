#include "GatewayPanic.h"

#include <atomic>
#include <cstring>
#include <exception>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

constexpr int kMaxSlots = 16;    // 同时被代理的设备数上限（产品量级：十几台）
constexpr int kMaxFrames = 4;    // ArpSpoofer::healFrames 恒为 4 帧
constexpr int kFrameCap = 64;    // ARP 帧 42 字节，留点余量
constexpr int kRepeat = 2;       // 崩溃时每帧发两遍（丢一帧的代价是设备继续断网）

// 槽位状态：0=空、1=正在写（处理器必须跳过）、2=可用。用 atomic 是为了让崩溃处理器读到的
// 要么是完整的一条、要么什么都没有 —— 处理器可能在任意指令边界插进来。
struct Slot {
    std::atomic<int> state{0};
    quint64 key = 0;
    IL2Endpoint::RawSendFn fn = nullptr;
    void *ctx = nullptr;
    int count = 0;
    int len[kMaxFrames] = {0};
    unsigned char buf[kMaxFrames][kFrameCap] = {};
};

Slot g_slots[kMaxSlots];
std::atomic<bool> g_installed{false};

// 崩溃处理器的**全部**工作：遍历可用槽位，把预存的字节发出去。没有分配、没有锁、没有 Qt。
void flushAll()
{
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot &s = g_slots[i];
        if (s.state.load(std::memory_order_acquire) != 2 || !s.fn)
            continue;
        for (int r = 0; r < kRepeat; ++r)
            for (int f = 0; f < s.count; ++f)
                s.fn(s.ctx, s.buf[f], s.len[f]);
    }
}

#if defined(Q_OS_WIN)
LONG WINAPI panicExceptionFilter(EXCEPTION_POINTERS *info)
{
    Q_UNUSED(info);
    flushAll();
    return EXCEPTION_CONTINUE_SEARCH; // 发完就交回系统，正常产生崩溃报告
}
#else
void panicSignalHandler(int sig)
{
    flushAll();
    // SA_RESETHAND 已经把默认行为放回去了，这里重新抛给自己 → 产生 core / 正常的崩溃退出码。
    ::raise(sig);
}
#endif

void panicTerminateHandler()
{
    flushAll();
    std::abort(); // abort 会走 SIGABRT；那条路的处理器已被 SA_RESETHAND 复位，不会递归
}

} // namespace

namespace GatewayPanic {

void installHandlers()
{
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true))
        return; // 已装过

#if defined(Q_OS_WIN)
    SetUnhandledExceptionFilter(&panicExceptionFilter);
#else
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &panicSignalHandler;
    sigemptyset(&sa.sa_mask);
    // SA_RESETHAND：处理器只跑一次，之后恢复默认动作 —— 崩溃里再崩溃时不会无限递归。
    sa.sa_flags = SA_RESETHAND;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        ::sigaction(sig, &sa, nullptr);
#endif
    std::set_terminate(&panicTerminateHandler);
}

void arm(quint64 key, IL2Endpoint::RawSendFn fn, void *ctx, const QVector<QByteArray> &frames)
{
    if (!fn || frames.isEmpty())
        return;
    // 先找同 key（覆盖），没有再找空位。
    int slot = -1;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (g_slots[i].state.load(std::memory_order_acquire) == 2 && g_slots[i].key == key) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < kMaxSlots; ++i) {
            if (g_slots[i].state.load(std::memory_order_acquire) == 0) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0)
        return; // 槽位满：放弃兜底（正常路径的还原不受影响）

    Slot &s = g_slots[slot];
    s.state.store(1, std::memory_order_release); // 写入中 → 处理器跳过这一条
    s.key = key;
    s.fn = fn;
    s.ctx = ctx;
    s.count = 0;
    for (const QByteArray &f : frames) {
        if (s.count >= kMaxFrames || f.size() > kFrameCap || f.size() < 14)
            continue;
        std::memcpy(s.buf[s.count], f.constData(), size_t(f.size()));
        s.len[s.count] = f.size();
        ++s.count;
    }
    s.state.store(s.count > 0 ? 2 : 0, std::memory_order_release);
}

void disarm(quint64 key)
{
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot &s = g_slots[i];
        if (s.state.load(std::memory_order_acquire) == 2 && s.key == key) {
            s.state.store(1, std::memory_order_release);
            s.fn = nullptr;
            s.ctx = nullptr;
            s.count = 0;
            s.state.store(0, std::memory_order_release);
        }
    }
}

void disarmAll()
{
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot &s = g_slots[i];
        s.state.store(1, std::memory_order_release);
        s.fn = nullptr;
        s.ctx = nullptr;
        s.count = 0;
        s.state.store(0, std::memory_order_release);
    }
}

} // namespace GatewayPanic
