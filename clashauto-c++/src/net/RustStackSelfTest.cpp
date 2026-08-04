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
    CoastConnId lastId = 0;
    uint16_t lastDport = 0;
    // 出方向看到的 TCP 帧：(flags, sport, dport)
    int synAckCount = 0;
    uint16_t synAckSport = 0;
};

void cbOutFrame(void *u, CoastNicId, const uint8_t *f, size_t len)
{
    if (!u) return;
    Ctx *c = static_cast<Ctx *>(u);
    c->outFrames++;
    // 认出 SYN-ACK：IPv4(0x0800) + proto TCP(6) + flags 含 SYN|ACK
    if (len >= 54 && f[12] == 0x08 && f[13] == 0x00 && f[23] == 6 && (f[47] & 0x12) == 0x12) {
        c->synAckCount++;
        c->synAckSport = static_cast<uint16_t>((f[34] << 8) | f[35]);
    }
}
bool cbConnNew(void *u, CoastConnId id, CoastNicId, const CoastAddr *, uint16_t,
               const CoastAddr *, uint16_t dport)
{
    if (!u) return true;
    Ctx *c = static_cast<Ctx *>(u);
    c->connNew++;
    c->lastId = id;
    c->lastDport = dport;
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


namespace {

// —— 造一个合法的 IPv4/TCP 帧（校验和必须对，smoltcp 默认校验，错了会被静默丢弃）——
uint32_t onesSum(const uint8_t *d, size_t n, uint32_t sum)
{
    size_t i = 0;
    for (; i + 1 < n; i += 2) sum += static_cast<uint32_t>((d[i] << 8) | d[i + 1]);
    if (i < n) sum += static_cast<uint32_t>(d[i] << 8);
    return sum;
}
uint16_t fold16(uint32_t s)
{
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return static_cast<uint16_t>(~s);
}

const uint8_t kDevMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xAA};
const uint8_t kOurMac[6] = {0x02, 0x00, 0x5b, 0x00, 0x00, 0x01};
const uint8_t kDevIp[4]  = {10, 99, 0, 1};
const uint8_t kOurIp[4]  = {10, 99, 0, 2};
const uint8_t kDstIp[4]  = {93, 184, 216, 34}; // 设备想访问的公网地址（非本机）

// 返回 54 字节的 eth+ip+tcp 帧
void buildSyn(uint8_t *f, uint16_t sport, uint16_t dport)
{
    std::memset(f, 0, 54);
    std::memcpy(f, kOurMac, 6);
    std::memcpy(f + 6, kDevMac, 6);
    f[12] = 0x08; f[13] = 0x00;
    f[14] = 0x45;
    f[16] = 0; f[17] = 40;          // total len = 20+20
    f[22] = 64; f[23] = 6;          // ttl, TCP
    std::memcpy(f + 26, kDevIp, 4);
    std::memcpy(f + 30, kDstIp, 4);
    const uint16_t ipc = fold16(onesSum(f + 14, 20, 0));
    f[24] = static_cast<uint8_t>(ipc >> 8); f[25] = static_cast<uint8_t>(ipc & 0xff);
    uint8_t *t = f + 34;
    t[0] = static_cast<uint8_t>(sport >> 8); t[1] = static_cast<uint8_t>(sport & 0xff);
    t[2] = static_cast<uint8_t>(dport >> 8); t[3] = static_cast<uint8_t>(dport & 0xff);
    t[4] = 0; t[5] = 0; t[6] = 0x03; t[7] = 0xE8;   // seq = 1000
    t[12] = 0x50; t[13] = 0x02;                      // offset 5, SYN
    t[14] = 0xFF; t[15] = 0xFF;                      // window
    uint32_t ph = 0;
    ph = onesSum(kDevIp, 4, ph);
    ph = onesSum(kDstIp, 4, ph);
    ph += 6; ph += 20;
    const uint16_t tc = fold16(onesSum(t, 20, ph));
    t[16] = static_cast<uint8_t>(tc >> 8); t[17] = static_cast<uint8_t>(tc & 0xff);
}

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
    CoastAddr ip {};
    ip.is_v6 = false;
    std::memcpy(ip.bytes, kOurIp, 4);
    const uint8_t *mac = kOurMac;

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

    // ⑥ ★ 真实数据面往返 —— 这一段才是「引擎能用」的判据，前面几项只证明 ABI 对得上。
    //    喂一个合法 SYN（目的 93.184.216.34:443，既非本机地址也非本机端口），必须：
    //      · 吐出 SYN-ACK，且源端口**还原成 443**（否则设备直接 RST = catch-all 改写没闭环）
    //      · conn_new 报出原始目的端口 443（C++ 侧据此拨 SOCKS）
    CHECK(coast_stack_add_nic(s, 7, mac, &ip, 24) == COAST_OK, "为数据面测试加网卡");
    uint8_t syn[54];
    buildSyn(syn, 51000, 443);
    CHECK(coast_stack_input(s, 7, syn, sizeof(syn)) == COAST_OK, "喂 SYN 应成功");
    coast_stack_poll(s, 10);

    CHECK(ctx.connNew == 1, "应恰好触发一次 conn_new");
    CHECK(ctx.lastDport == 443, "conn_new 应报原始目的端口 443（不是内部 FIXED_PORT）");
    CHECK(ctx.lastId != 0, "conn id 不能为 0");
    CHECK(ctx.synAckCount >= 1, "应吐出 SYN-ACK（否则协议栈没在终结连接）");
    CHECK(ctx.synAckSport == 443, "SYN-ACK 源端口必须还原成 443");
    CHECK(coast_conn_abort(s, ctx.lastId) == COAST_OK, "abort 应成功");

    CoastStats st2 {};
    coast_stack_stats(s, &st2);
    CHECK(st2.conns_accepted == 1, "conns_accepted 应为 1");
    CHECK(st2.tx_frames >= 1, "tx_frames 应 >= 1");

    coast_stack_free(s);
    coast_stack_free(nullptr); // 空指针 free 必须安全

    std::fprintf(stderr,
                 "[ruststack] PASS —— ABI 一致 + 数据面往返正常"
                 "（SYN→SYN-ACK，源端口还原为 %u，conn_new dport=%u）\n",
                 static_cast<unsigned>(ctx.synAckSport), static_cast<unsigned>(ctx.lastDport));
    return 0;
}
