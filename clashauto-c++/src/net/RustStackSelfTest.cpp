// Rust(smoltcp) 数据面的**链接 + ABI 自证**（COAST_RUSTSTACK_SELFTEST=1）。
//
// 为什么需要这个文件：Phase 1 的 Rust 库运行时还没接进 NetStack，若只看"CI 绿灯"，
// 「库编出来了」和「库真能被 C++ 调用、且 ABI 对得上」是分不清的。本仓库已经栽过同类的亏
// （见 L2Endpoint_win.cpp:740「批量/逐帧的 A/B 必须先确认设备真被接管」）。
// 所以这里跨 FFI 走一遍完整往返，把「能链接」变成一个可执行的断言。
//
// 断言的是 ABI 契约本身：结构体布局、回调签名、错误码语义、瞬时量读后清零。
// 布局对不上通常不会崩，而是读到垃圾值——所以必须比对**具体数值**，不能只看"没崩"。
#include "coaststack.h"

#include <cstdio>
#include <cstring>

namespace {

struct Ctx {
    int outFrames = 0;
    int connNew = 0;
};

void cbOutFrame(void *u, CoastNicId, const uint8_t *, size_t)
{
    if (u) static_cast<Ctx *>(u)->outFrames++;
}
bool cbConnNew(void *u, CoastConnId, CoastNicId, const CoastAddr *, uint16_t,
               const CoastAddr *, uint16_t)
{
    if (u) static_cast<Ctx *>(u)->connNew++;
    return true;
}
void cbConnData(void *, CoastConnId, const uint8_t *, size_t) {}
void cbConnSent(void *, CoastConnId, uint32_t) {}
void cbConnClosed(void *, CoastConnId, bool) {}

#define CHECK(cond, msg)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "[ruststack] FAIL: %s (%s:%d)\n", msg, __FILE__,        \
                         __LINE__);                                                      \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

} // namespace

int runRustStackSelfTest()
{
    std::fprintf(stderr, "[ruststack] version=%s\n", coast_stack_version());

    // ① 缺回调必须建栈失败（早失败好过运行期空指针）
    CoastCallbacks bad {};
    bad.out_frame = cbOutFrame;
    CHECK(coast_stack_new(&bad, nullptr) == nullptr, "回调不全时应建栈失败");
    CHECK(coast_stack_new(nullptr, nullptr) == nullptr, "空 cb 应建栈失败");

    Ctx ctx;
    CoastCallbacks cb {};
    cb.out_frame = cbOutFrame;
    cb.conn_new = cbConnNew;
    cb.conn_data = cbConnData;
    cb.conn_sent = cbConnSent;
    cb.conn_closed = cbConnClosed;

    CoastStack *s = coast_stack_new(&cb, &ctx);
    CHECK(s != nullptr, "建栈失败");

    // ② 网卡生命周期 + 错误码语义
    const uint8_t mac[6] = {0x02, 0x00, 0x5b, 0x00, 0x00, 0x01};
    CoastAddr ip {};
    ip.is_v6 = false;
    ip.bytes[0] = 192; ip.bytes[1] = 168; ip.bytes[2] = 20; ip.bytes[3] = 51;

    CHECK(coast_stack_add_nic(s, 1, mac, &ip, 24) == COAST_OK, "add_nic 应成功");
    CHECK(coast_stack_add_nic(s, 1, mac, &ip, 24) == COAST_ERR_BADARG, "重复注册应报错");
    CHECK(coast_stack_add_nic(s, 2, mac, &ip, 33) == COAST_ERR_BADARG, "前缀越界应报错");

    // ③ 喂帧 + 计数器（布局对不上会在这里读到垃圾值）
    uint8_t frame[64];
    std::memset(frame, 0, sizeof(frame));
    CHECK(coast_stack_input(s, 1, frame, sizeof(frame)) == COAST_OK, "喂帧应成功");
    CHECK(coast_stack_input(s, 99, frame, sizeof(frame)) == COAST_ERR_NONIC, "未知网卡应报错");
    CHECK(coast_stack_input(s, 1, nullptr, 0) == COAST_ERR_BADARG, "空帧应报错");

    CoastStats st {};
    coast_stack_stats(s, &st);
    CHECK(st.rx_frames == 1, "rx_frames 应为 1（对不上=结构体布局错位）");
    CHECK(st.rx_dropped == 2, "rx_dropped 应为 2");

    // ④ poll 的最坏间隔 + 瞬时量读后清零
    coast_stack_poll(s, 1000);
    coast_stack_poll(s, 1025);
    coast_stack_poll(s, 1225); // gap 200 ← 最坏
    coast_stack_stats(s, &st);
    CHECK(st.poll_max_gap_ms == 200, "poll_max_gap_ms 应为 200");
    coast_stack_stats(s, &st);
    CHECK(st.poll_max_gap_ms == 0, "瞬时量读完必须清零");

    // ⑤ 无效连接 id 一律返回错误码，绝不 panic（panic=abort 会杀掉整个进程）
    CHECK(coast_conn_send(s, 0, frame, 1) == COAST_ERR_NOCONN, "conn id 0 应无效");
    CHECK(coast_conn_recved(s, 12345, 100) == COAST_ERR_NOCONN, "未知 conn 应报错");
    CHECK(coast_conn_close(s, 12345) == COAST_ERR_NOCONN, "未知 conn 应报错");
    CHECK(coast_conn_abort(s, 12345) == COAST_ERR_NOCONN, "未知 conn 应报错");

    CHECK(coast_stack_remove_nic(s, 1) == COAST_OK, "remove_nic 应成功");
    CHECK(coast_stack_remove_nic(s, 1) == COAST_ERR_NONIC, "重复移除应报错");

    coast_stack_free(s);
    coast_stack_free(nullptr); // 空指针 free 必须安全

    std::fprintf(stderr, "[ruststack] PASS —— C ABI 往返正常，结构体布局与错误码语义一致\n");
    return 0;
}
