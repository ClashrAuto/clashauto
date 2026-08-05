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
    uint32_t synAckSeq = 0;
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
        c->synAckSeq = (uint32_t(f[38]) << 24) | (uint32_t(f[39]) << 16)
            | (uint32_t(f[40]) << 8) | uint32_t(f[41]);
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

/// 设备侧通告窗口。默认满开；下行拥塞用例调小它，逼出 toStack 的部分写路径。
uint16_t g_devWnd = 0xFFFF;

// 返回 54 字节的 eth+ip+tcp 帧
void buildTcp(uint8_t *f, uint16_t sport, uint16_t dport, uint8_t flags, uint32_t seq,
              uint32_t ack)
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
    t[4] = uint8_t(seq >> 24); t[5] = uint8_t(seq >> 16);
    t[6] = uint8_t(seq >> 8);  t[7] = uint8_t(seq);
    t[8] = uint8_t(ack >> 24); t[9] = uint8_t(ack >> 16);
    t[10] = uint8_t(ack >> 8); t[11] = uint8_t(ack);
    t[12] = 0x50; t[13] = flags;                     // offset 5
    // window：默认满开；下行拥塞用例把它调小来模拟"设备侧慢"（见 g_devWnd）
    t[14] = static_cast<uint8_t>(g_devWnd >> 8);
    t[15] = static_cast<uint8_t>(g_devWnd & 0xff);
    uint32_t ph = 0;
    ph = onesSum(kDevIp, 4, ph);
    ph = onesSum(kDstIp, 4, ph);
    ph += 6; ph += 20;
    const uint16_t tc = fold16(onesSum(t, 20, ph));
    t[16] = static_cast<uint8_t>(tc >> 8); t[17] = static_cast<uint8_t>(tc & 0xff);
}

inline void buildSyn(uint8_t *f, uint16_t sport, uint16_t dport)
{
    buildTcp(f, sport, dport, 0x02 /*SYN*/, 1000, 0);
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
    uint8_t pkt[54];
    buildTcp(pkt, 51000, 443, 0x02 /*SYN*/, 1000, 0);
    CHECK(coast_stack_input(s, 7, pkt, sizeof(pkt)) == COAST_OK, "喂 SYN 应成功");
    coast_stack_poll(s, 10);

    // ★ SYN 之后**还不该**有 conn_new —— 引擎刻意等三次握手完成才 promote，
    //   与 lwIP 的 accept 时机一致；在 SynReceived 就拨上游 = SYN 洪水每个 SYN 一条上游连接。
    //   （这个时机差异是 A/B 网关自测在 lwip 侧跑不过才发现的。）
    CHECK(ctx.connNew == 0, "SynReceived 阶段不该触发 conn_new");
    CHECK(ctx.synAckCount >= 1, "此时应已吐出 SYN-ACK");

    // 补第三次握手
    buildTcp(pkt, 51000, 443, 0x10 /*ACK*/, 1001, ctx.synAckSeq + 1);
    CHECK(coast_stack_input(s, 7, pkt, sizeof(pkt)) == COAST_OK, "喂 ACK 应成功");
    coast_stack_poll(s, 20);

    CHECK(ctx.connNew == 1, "握手完成后应恰好触发一次 conn_new");
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

// ═══════ NetStack 级自证：整条 smoltcp 路径（COAST_SMOLGW_SELFTEST=1）═══════
//
// 上面那个只证明「Rust 引擎自己能用」。这个证明**接进 NetStack 之后整条路仍然通**：
// 合成 SYN → NetStack::inputFrame → 桥接 → Socks5Tcp → 真的拨出 SOCKS CONNECT，
// 且带着该设备的身份。
//
// ★ 判据必须校验 cmd==CONNECT(0x01)。GatewaySelfTest.cpp 里记着一个真实教训：
//   UDP ASSOCIATE(0x03) 与 CONNECT(0x01) 在字节上完全同形，而 associate 的用户名取自
//   devices 表、**不走** TCP 那条 userForIp 管线 —— 于是「每设备身份」这条断言曾在
//   TCP 路径彻底断掉的情况下照样 PASS。这里只喂 TCP 帧、且显式判 cmd，双保险。
#include "IL2Endpoint.h"
#include "GatewayDiag.h"
#include "NetStack.h"
#include "Socks5Client.h"
#include "core/ProxyConfig.h"
#include "core/CoreDialerFactory.h"

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {

/// 记录出帧的假二层端点（不碰真网卡）
class FakeEp final : public IL2Endpoint
{
public:
    explicit FakeEp(const QByteArray &mac) : m_mac(mac) {}
    bool open(const QString &, QString *) override { return true; }
    void close() override {}
    bool isOpen() const override { return true; }
    bool send(const QByteArray &f) override
    {
        noteAck(f);
        noteData(f);
        ++sent;
        if (f.size() >= 54 && quint8(f[12]) == 0x08 && quint8(f[13]) == 0x00
            && quint8(f[23]) == 6 && (quint8(f[47]) & 0x12) == 0x12) {
            ++synAck;
            // ★ 记下第一条 SYN-ACK 的到达时刻。这是「poll 提频到底生效没有」的唯一判据：
            //   poll 只挂 25ms 定时器时它必须等下一拍（0~25ms，均值约 12ms）；
            //   接上 schedulePoll 之后应在**本轮事件循环末尾**就发出，亚毫秒级。
            if (clock && synAckUs < 0)
                synAckUs = clock->nsecsElapsed() / 1000;
            synAckSport = quint16((quint8(f[34]) << 8) | quint8(f[35]));
            synAckSeq = (quint32(quint8(f[38])) << 24) | (quint32(quint8(f[39])) << 16)
                | (quint32(quint8(f[40])) << 8) | quint32(quint8(f[41]));
        }
        return true;
    }
    // 任何带 ACK 位的出帧都更新确认号/窗口（吞吐基准据此决定还能灌多少）
    void noteAck(const QByteArray &f)
    {
        if (f.size() < 54 || quint8(f[12]) != 0x08 || quint8(f[13]) != 0x00
            || quint8(f[23]) != 6)
            return;
        if ((quint8(f[47]) & 0x10) == 0)
            return;
        lastAck = (quint32(quint8(f[42])) << 24) | (quint32(quint8(f[43])) << 16)
                | (quint32(quint8(f[44])) << 8) | quint32(quint8(f[45]));
        lastWnd = quint32((quint8(f[48]) << 8) | quint8(f[49]));
        // 多连接聚合基准：按**设备源端口**（= 出方向帧的目的端口）分开记
        const quint16 devPort = quint16((quint8(f[36]) << 8) | quint8(f[37]));
        ackByPort[devPort] = qMakePair(lastAck, lastWnd);
    }
    // 出方向数据帧：累计载荷长度（下行基准据此回 ACK 并计吞吐）
    void noteData(const QByteArray &f)
    {
        if (f.size() <= 54 || quint8(f[12]) != 0x08 || quint8(f[13]) != 0x00
            || quint8(f[23]) != 6)
            return;
        const int ihl = (quint8(f[14]) & 0x0F) * 4;
        const int thl = ((quint8(f[34 + 12]) >> 4) & 0x0F) * 4;
        const int total = (quint8(f[16]) << 8) | quint8(f[17]);
        const int plen = total - ihl - thl;
        if (plen <= 0)
            return;
        if (!sawData) {
            sawData = true;
            firstDataSeq = (quint32(quint8(f[38])) << 24) | (quint32(quint8(f[39])) << 16)
                         | (quint32(quint8(f[40])) << 8) | quint32(quint8(f[41]));
        }
        dataBytes += plen;
    }
    QByteArray localMac() const override { return m_mac; }
    int ifIndex() const override { return 1; }
    int mtu() const override { return 1500; }

    int sent = 0;
    int synAck = 0;
    QElapsedTimer *clock = nullptr; // 由自测在喂 SYN 前挂上
    qint64 synAckUs = -1;           // 喂 SYN → 吐 SYN-ACK 的微秒数
    // 吞吐基准用：出方向 ACK 里的确认号与通告窗口。
    // 设备必须遵守窗口，否则灌进去的是出窗数据、被静默丢弃 —— 量出来的就是假数
    //（Rust 侧那三次返工全栽在这上面，别在 C++ 侧再栽一次）。
    quint32 lastAck = 0;
    quint32 lastWnd = 65535;
    QHash<quint16, QPair<quint32, quint32>> ackByPort; // 设备源端口 → (ack, 窗口)
    // 下行基准用：出方向 TCP 帧里的载荷字节总数，以及第一段数据的起始序号。
    qint64 dataBytes = 0;
    quint32 firstDataSeq = 0;
    bool sawData = false;
    quint16 synAckSport = 0;
    quint32 synAckSeq = 0;

private:
    QByteArray m_mac;
};

/// 最小 SOCKS5 服务端：校验 用户名 + cmd==CONNECT + 目的端口
class MiniSocks final : public QObject
{
public:
    explicit MiniSocks(quint16 port)
    {
        QObject::connect(&m_srv, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_srv.nextPendingConnection()) {
                auto *st = new St;
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, st] { onData(s, st); });
                QObject::connect(s, &QTcpSocket::disconnected, s, [s, st] {
                    delete st;
                    s->deleteLater();
                });
            }
        });
        ok = m_srv.listen(QHostAddress::LocalHost, port);
    }
    bool ok = false;
    bool gotConnect = false;
    QString user;
    quint16 dport = 0;
    qint64 rxBytes = 0;             // 隧道建立后收到的字节（上行基准的判据）
    QTcpSocket *tunnel = nullptr;   // 隧道 socket（下行基准从这里灌数据）

private:
    struct St { int phase = 0; QByteArray buf; QString user; };
    void onData(QTcpSocket *s, St *c)
    {
        // ★ 隧道建立后直接丢弃、**不拷贝**：夹具自己的 readAll()+append 是每字节两次拷贝，
        //   会被算进"0→1 段"的归因里，把 SOCKS 回环那一段的成本虚高。
        //   skip() 在 QIODevice 层丢弃，不构造 QByteArray。
        if (c->phase == 3) {
            rxBytes += s->skip(s->bytesAvailable());
            return;
        }
        c->buf += s->readAll();
        if (c->phase == 0) {
            if (c->buf.size() < 2) return;
            const int n = quint8(c->buf[1]);
            if (c->buf.size() < 2 + n) return;
            const bool up = c->buf.mid(2, n).contains(char(0x02));
            c->buf.remove(0, 2 + n);
            const char rep[2] = {0x05, char(up ? 0x02 : 0x00)};
            s->write(rep, 2);
            c->phase = up ? 1 : 2;
        }
        if (c->phase == 1) {
            if (c->buf.size() < 2) return;
            const int ul = quint8(c->buf[1]);
            if (c->buf.size() < 2 + ul + 1) return;
            const int pl = quint8(c->buf[2 + ul]);
            if (c->buf.size() < 3 + ul + pl) return;
            c->user = QString::fromLatin1(c->buf.mid(2, ul));
            c->buf.remove(0, 3 + ul + pl);
            const char rep[2] = {0x01, 0x00};
            s->write(rep, 2);
            c->phase = 2;
        }
        if (c->phase == 2) {
            if (c->buf.size() < 4) return;
            const int cmd = quint8(c->buf[1]);
            const int atyp = quint8(c->buf[3]);
            int need = 4 + 2;
            if (atyp == 0x01) need += 4;
            else if (atyp == 0x04) need += 16;
            else if (atyp == 0x03) {
                if (c->buf.size() < 5) return;
                need += 1 + quint8(c->buf[4]);
            }
            if (c->buf.size() < need) return;
            // ★ 只认 CONNECT —— UDP ASSOCIATE 同形但走另一条身份管线（见上面的警告）
            if (cmd == 0x01) {
                gotConnect = true;
                user = c->user;
                dport = quint16((quint8(c->buf[need - 2]) << 8) | quint8(c->buf[need - 1]));
                // ★ 必须回成功应答：Socks5Tcp 在 established 之前会把写入深拷贝进 pending，
                //   不回复的话数据永远流不到 socket 上，吞吐基准会量到 0。
                //   （对既有断言无影响 —— 它们只看 gotConnect/user/dport。）
                const char rep[10] = {0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
                s->write(rep, 10);
                c->phase = 3; // 之后一律计入 rxBytes
                tunnel = s;   // 下行基准要用它往回灌
            }
            c->buf.clear();
            return;
        }
        // 切到 phase 3 那一刻缓冲里可能已经跟着数据，别丢
        if (c->phase == 3) {
            rxBytes += c->buf.size();
            c->buf.clear();
        }
    }
    QTcpServer m_srv;
};

} // namespace

// ———————————————— 每卡入站接线自测（COAST_NICWIRING_SELFTEST）————————————————
//
// ★ **这个自测是为两个真机 bug 补的，它们都在「已有自测全绿」的情况下发生。**
//
//   ① `outFactoryFor()` 用**对象身份**判断「全局工厂是不是 CoastCore 的」：
//        if (d->outFactory && d->outFactory != d->ownedDefault) return d->outFactory;
//      而关掉 CoastCore 的分支会 `setOutboundFactory(new Socks5OutboundFactory(...))` ——
//      新对象同样 `!= ownedDefault`，于是**每卡工厂被无条件吞掉**，所有设备一律拨基准口。
//      副卡的设备因此一条连接都建不起来（配置里那个口没人监听 → Connection refused）。
//      `runSmolGatewaySelfTest` 测不到：它 `addNic(..., 0, ...)` 且从不装全局工厂，
//      **出问题的那条分支一次都没走过**。
//
//   ② 网卡序号变了（拔掉/禁用另一张卡）时，栈里缓存的 `socksPort` 不跟着变 —— 配置侧已经
//      重生成成新端口，而网关还在拨旧的，每条新连接被拒。真机上禁用有线卡当场全断。
//
//   两条都不是"算错了一个值"，而是**接线**错了。所以这里不造合成断言，而是照生产的样子接：
//   装全局工厂 + 给网卡一个不同的专属口，然后看 SOCKS CONNECT 落在哪个口上。
int runNicWiringSelfTest()
{
    constexpr quint16 kGlobalPort = 47899; // 「基准口」——绝不该被拨到
    constexpr quint16 kNicPort = 47900;    // 这张卡的专属口——正确答案
    constexpr quint16 kNicPort2 = 47901;   // 序号变化后的新口
    const QString kUser = QStringLiteral("dev-abc123");

    int fails = 0;
    auto check = [&fails](bool cond, const char *what) {
        std::fputs(cond ? "  ok   " : "  FAIL ", stdout);
        std::fputs(what, stdout);
        std::fputs("\n", stdout);
        if (!cond)
            ++fails;
    };

    MiniSocks global(kGlobalPort), perNic(kNicPort), renumbered(kNicPort2);
    if (!global.ok || !perNic.ok || !renumbered.ok) {
        std::fprintf(stderr, "[nicwiring] 夹具端口被占（%u/%u/%u）——这不是被测逻辑的问题\n",
                     kGlobalPort, kNicPort, kNicPort2);
        return 3;
    }

    QString err;
    NetStack net(kGlobalPort);
    if (!net.init(&err)) {
        std::fprintf(stderr, "[nicwiring] 本平台没有用户态栈（%s）——跳过\n",
                     err.toLatin1().constData());
        return 0; // Linux/mac 走 TPROXY/pf，没有用户态栈可测，不算失败
    }

    // ★ 复现生产接线：关掉 CoastCore 的那条分支曾在这里装一个**新的** Socks5 工厂。
    //   它 != ownedDefault，正是 ① 的触发条件。
    auto *globalFactory = new Socks5OutboundFactory(kGlobalPort);
    net.setOutboundFactory(globalFactory);

    FakeEp ep(QByteArray(reinterpret_cast<const char *>(kOurMac), 6));
    if (!net.addNic(&ep, ep.localMac(), QStringLiteral("10.99.0.2"),
                    QStringLiteral("255.255.255.0"), kNicPort, &err)) {
        std::fprintf(stderr, "[nicwiring] addNic 失败: %s\n", err.toLatin1().constData());
        return 3;
    }
    net.addDevice(QStringLiteral("10.99.0.1"),
                  QByteArray(reinterpret_cast<const char *>(kDevMac), 6), kUser);

    // 跑一次完整握手 + SOCKS，返回「哪个夹具收到了 CONNECT」。
    auto dialOnce = [&](quint16 sport) -> MiniSocks * {
        uint8_t f[54];
        QElapsedTimer clk;
        clk.start();
        ep.clock = &clk;
        ep.synAck = 0;
        buildTcp(f, sport, 443, 0x02 /*SYN*/, 1000, 0);
        net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), sizeof(f)));
        {
            QEventLoop w;
            QTimer t, poll;
            t.setSingleShot(true);
            QObject::connect(&t, &QTimer::timeout, &w, &QEventLoop::quit);
            QObject::connect(&poll, &QTimer::timeout, &w, [&] {
                if (ep.synAck > 0)
                    w.quit();
            });
            poll.start(5);
            t.start(2000);
            w.exec();
        }
        if (ep.synAck < 1)
            return nullptr;
        buildTcp(f, sport, 443, 0x10 /*ACK*/, 1001, ep.synAckSeq + 1);
        net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), sizeof(f)));
        QEventLoop loop;
        QTimer deadline, tick;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
            if (global.gotConnect || perNic.gotConnect || renumbered.gotConnect)
                loop.quit();
        });
        tick.start(5);
        deadline.start(3000);
        loop.exec();
        if (perNic.gotConnect)
            return &perNic;
        if (renumbered.gotConnect)
            return &renumbered;
        if (global.gotConnect)
            return &global;
        return nullptr;
    };

    // —— ① 每卡工厂不能被全局工厂吞掉 ——
    MiniSocks *hit = dialOnce(51000);
    check(hit != nullptr, "握手走完并拨出了上游");
    check(hit == &perNic,
          "★ 拨的是**这张卡的专属口**，不是基准口（全局工厂不该吞掉每卡工厂）");
    check(hit && hit->user == kUser, "SOCKS 用户名是这台设备的（按设备分流的依据）");
    check(!global.gotConnect, "基准口一次都没被拨到");

    // —— ② 序号变了要能就地换口 ——
    check(net.setNicSocksPort(&ep, kNicPort2), "setNicSocksPort 认得这张卡");
    perNic.gotConnect = false;
    MiniSocks *hit2 = dialOnce(51001);
    check(hit2 == &renumbered,
          "★ 换口后新连接拨的是**新口**（网卡序号变了就是这条路）");
    check(!perNic.gotConnect, "旧口不再被拨");

    // 换成一个不存在的网卡端点要老实返回 false，别假装成功。
    FakeEp other(QByteArray(reinterpret_cast<const char *>(kDevMac), 6));
    check(!net.setNicSocksPort(&other, kNicPort), "没登记过的网卡换口返回 false");

    std::printf(fails ? "每卡入站接线自测：%d 条失败\n" : "每卡入站接线自测：全部通过\n", fails);
    std::fflush(stdout);
    return fails ? 1 : 0;
}

int runSmolGatewaySelfTest()
{
    const quint16 kSocksPort = 47899;
    const QString kUser = QStringLiteral("dev-abc123");

    MiniSocks socks(kSocksPort);
    if (!socks.ok) {
        std::fprintf(stderr, "[smolgw] FAIL: 假 SOCKS 监听 %u 失败\n", kSocksPort);
        return 3;
    }

    // ★ 这里原本是个 A/B 夹具：同一套断言分别跑 smoltcp 与 lwIP，两条都 PASS 才算"换栈后
    //   行为一致"。**那个 A/B 抓到过一个真 bug**（smoltcp 在 SynReceived 就 promote、lwIP 只在
    //   Established 才 accept —— 只测 smoltcp 永远发现不了 SYN 洪水放大），所以它值得记一笔。
    //   lwIP 移除后没有第二条栈可比了，夹具退化成"确认跑在 smoltcp 上"。
    NetStack net(kSocksPort);
    QString err;
    if (!net.init(&err)) {
        std::fprintf(stderr, "[smolgw] FAIL: NetStack::init: %s\n", err.toLatin1().constData());
        return 1;
    }

    // ★ 先证明测的确实是那条数据面。断言本身对任何 catch-all 实现都会绿，
    //   不确认路径就等于没测（COAST_RUST=OFF 时 init 会失败，这里则会报 none）。
    if (QByteArray(net.activeTcpStack()) != QByteArray("smoltcp")) {
        std::fprintf(stderr, "[smolgw] FAIL: 没跑在 smoltcp 上（实际=%s）\n",
                     net.activeTcpStack());
        return 3;
    }

    // ★ **第二个实例必须也能建起来。** lwIP 时代这里会被判重拒掉（lwip_init 全局、
    //   整个进程只有一份栈），真实后果是用户在网关开着时点「增强」永远打不开，报
    //   「已有一个网关协议栈实例在运行」。smoltcp 每实例独立，那条约束已随 lwIP 删除 ——
    //   这条断言就是它的护栏：谁要是把单例判重加回来，这里立刻红。
    {
        NetStack second(kSocksPort + 1);
        QString err2;
        if (!second.init(&err2)) {
            std::fprintf(stderr,
                         "[smolgw] FAIL: 第二个 NetStack 建不起来（%s）—— 单例约束回来了，"
                         "网关开着时「增强」会打不开\n",
                         err2.toLatin1().constData());
            return 4;
        }
    }

    FakeEp ep(QByteArray(reinterpret_cast<const char *>(kOurMac), 6));
    if (!net.addNic(&ep, ep.localMac(), QStringLiteral("10.99.0.2"),
                    QStringLiteral("255.255.255.0"), 0 /*单网卡自测：沿用构造时的口*/, &err)) {
        std::fprintf(stderr, "[smolgw] FAIL: addNic: %s\n", err.toLatin1().constData());
        return 1;
    }
    net.addDevice(QStringLiteral("10.99.0.1"),
                  QByteArray(reinterpret_cast<const char *>(kDevMac), 6), kUser);

    // ★ 必须走完**三次握手**才会拨上游 —— 两条栈都是在连接建立后才 accept 的。
    //   （smoltcp 侧一度在 SynReceived 就 promote，是这个 A/B 自测在 lwIP 上跑不过才发现的：
    //     那等于 SYN 洪水每个 SYN 都拨一条上游。已修，见 engine.rs 的 pending 一段。）
    uint8_t f[54];
    buildTcp(f, 51000, 443, 0x02 /*SYN*/, 1000, 0);
    QElapsedTimer synClock;
    synClock.start();
    ep.clock = &synClock;
    net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), sizeof(f)));

    // 等 SYN-ACK（lwIP 侧要等泵跑一拍）
    {
        QEventLoop w;
        QTimer t;
        t.setSingleShot(true);
        QObject::connect(&t, &QTimer::timeout, &w, &QEventLoop::quit);
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &w, [&] {
            if (ep.synAck > 0)
                w.quit();
        });
        poll.start(5);
        t.start(2000);
        w.exec();
    }
    if (ep.synAck < 1) {
        std::fprintf(stderr, "[smolgw] FAIL: 没收到 SYN-ACK（出帧=%d）\n", ep.sent);
        return 1;
    }
    // ★ 时延断言 —— **可证伪**：把 schedulePoll 全去掉（只留 25ms 定时器兜底）这里必然超阈值。
    //   没有它，「poll 提频了」和「代码编过了」在绿灯里分不清。
    //   阈值 5000us：定时器那条路最坏 25000us、均值约 12000us，留够 CI 抖动也不会误判。
    constexpr qint64 kSynAckBudgetUs = 5000;
    if (ep.synAckUs < 0 || ep.synAckUs > kSynAckBudgetUs) {
        std::fprintf(stderr,
                     "[smolgw] FAIL: SYN-ACK 用了 %lld us（预算 %lld us）—— "
                     "说明它仍在等 25ms 定时器那一拍，schedulePoll 没生效\n",
                     static_cast<long long>(ep.synAckUs),
                     static_cast<long long>(kSynAckBudgetUs));
        return 3;
    }

    buildTcp(f, 51000, 443, 0x10 /*ACK*/, 1001, ep.synAckSeq + 1);
    net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), sizeof(f)));

    // 等 SOCKS 握手跑完（泵 25ms 一拍，SOCKS 在回环上）
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
        if (socks.gotConnect)
            loop.quit();
    });
    tick.start(20);
    deadline.start(5000);
    loop.exec();

    if (!socks.gotConnect) {
        std::fprintf(stderr,
                     "[smolgw] FAIL: 5s 内没等到 SOCKS CONNECT（出帧=%d SYN-ACK=%d）"
                     "—— TCP 路径没打通\n",
                     ep.sent, ep.synAck);
        return 1;
    }
    if (socks.user != kUser) {
        std::fprintf(stderr, "[smolgw] FAIL: 每设备身份错：期望 %s 实得 %s\n",
                     kUser.toLatin1().constData(), socks.user.toLatin1().constData());
        return 2;
    }
    if (socks.dport != 443) {
        std::fprintf(stderr, "[smolgw] FAIL: 目的端口错：期望 443 实得 %u\n", socks.dport);
        return 2;
    }
    if (ep.synAck < 1 || ep.synAckSport != 443) {
        std::fprintf(stderr, "[smolgw] FAIL: SYN-ACK 缺失或源端口错（%d 个，sport=%u）\n",
                     ep.synAck, ep.synAckSport);
        return 2;
    }

    std::fprintf(stderr,
                 "[smolgw] PASS(%s) —— 整条路径通：SYN → SYN-ACK(sport=%u) → "
                 "SOCKS CONNECT(user=%s, dport=%u)\n",
                 net.activeTcpStack(), ep.synAckSport, socks.user.toLatin1().constData(),
                 socks.dport);
    std::fprintf(stderr, "[smolgw]   SYN-ACK 时延 %lld us（预算 %lld us）\n",
                 static_cast<long long>(ep.synAckUs),
                 static_cast<long long>(kSynAckBudgetUs));
    return 0;
}

// ═══ 真网卡自证（COAST_SMOLGW_REALNIC_SELFTEST=1）═══════════════════════════
//
// 上面那个用假二层端点，证明的是协议逻辑；这个用**真 Npcap 端点 + 真机发来的真帧**，
// 证明的是「在真实收发路径上也能用」—— 合成帧永远替代不了的一环。
//
// ★ 关键：**不做 ARP 投毒**。导流靠靶机自己的静态路由：
//     ip route add 203.0.113.0/24 via <win-ip>
//     ip neigh replace <win-ip> lladdr <win-mac> dev <if>
//   所以本测试**不可能让任何设备断网**（只影响 203.0.113.0/24 这个不存在的网段），
//   两条命令即可完全还原。ArpSpoofer 归 LanGateway 管，这里根本不建 LanGateway。
//
// 需要管理员权限（Npcap 抓包）。环境变量：
//   COAST_GW_VICTIM_IP   靶机 IP（必填）
//   COAST_GW_VICTIM_MAC  靶机 MAC，如 bc:24:11:ad:4a:06（必填）
//   COAST_GW_WAIT_MS     等待靶机发起连接的时间，默认 20000
#include <QNetworkInterface>

namespace {

QByteArray parseMacStr(const QString &s)
{
    QByteArray out;
    const QString norm = QString(s).replace(QLatin1Char('-'), QLatin1Char(':'));
    for (const QString &p : norm.split(QLatin1Char(':'), Qt::SkipEmptyParts))
        out.append(char(p.toUInt(nullptr, 16)));
    return out;
}

} // namespace

int runSmolGatewayRealNicSelfTest()
{
    const QString victimIp = qEnvironmentVariable("COAST_GW_VICTIM_IP");
    const QByteArray victimMac = parseMacStr(qEnvironmentVariable("COAST_GW_VICTIM_MAC"));
    if (victimIp.isEmpty() || victimMac.size() != 6) {
        std::fprintf(stderr, "[realnic] FAIL: 需要 COAST_GW_VICTIM_IP 与 COAST_GW_VICTIM_MAC\n");
        return 3;
    }
    const int waitMs = qEnvironmentVariableIsSet("COAST_GW_WAIT_MS")
        ? qEnvironmentVariable("COAST_GW_WAIT_MS").toInt()
        : 20000;

    // 找出与靶机同网段的本机网卡
    QString ifname, localIp, netmask;
    QByteArray localMac;
    const QHostAddress vaddr(victimIp);
    for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces()) {
        if (!(nif.flags() & QNetworkInterface::IsUp)
            || (nif.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &e : nif.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (vaddr.isInSubnet(e.ip(), e.prefixLength())) {
                ifname = nif.name();
                localIp = e.ip().toString();
                netmask = e.netmask().toString();
                localMac = parseMacStr(nif.hardwareAddress().toLower());
                break;
            }
        }
        if (!ifname.isEmpty())
            break;
    }
    if (ifname.isEmpty() || localMac.size() != 6) {
        std::fprintf(stderr, "[realnic] FAIL: 找不到与 %s 同网段的本机网卡\n",
                     victimIp.toLatin1().constData());
        return 3;
    }
    std::fprintf(stderr, "[realnic] 网卡=%s 本机=%s/%s mac=%s 靶机=%s\n",
                 ifname.toLatin1().constData(), localIp.toLatin1().constData(),
                 netmask.toLatin1().constData(), localMac.toHex(':').constData(),
                 victimIp.toLatin1().constData());

    const quint16 kSocksPort = 47898;
    const QString kUser = QStringLiteral("realnic-user");
    MiniSocks socks(kSocksPort);
    if (!socks.ok) {
        std::fprintf(stderr, "[realnic] FAIL: 假 SOCKS 监听失败\n");
        return 3;
    }

    NetStack net(kSocksPort);
    QString err;
    if (!net.init(&err)) {
        std::fprintf(stderr, "[realnic] FAIL: NetStack::init: %s\n", err.toLatin1().constData());
        return 1;
    }
    // 同上：先确认路径再断言（lwIP 移除后只剩一条，但"跑的是哪条"仍要写在日志里）。
    if (QByteArray(net.activeTcpStack()) != QByteArray("smoltcp")) {
        std::fprintf(stderr, "[realnic] FAIL: 期望跑在 smoltcp 上，实际=%s\n",
                     net.activeTcpStack());
        return 3;
    }

    // 真二层端点（Npcap）。**只收发，不投毒**。
    IL2Endpoint *ep = createL2Endpoint(nullptr);
    if (!ep || !ep->open(ifname, &err)) {
        std::fprintf(stderr, "[realnic] FAIL: 打开网卡失败（需管理员 + 已装 Npcap）：%s\n",
                     err.toLatin1().constData());
        return 3;
    }
    if (!net.addNic(ep, localMac, localIp, netmask, 0 /*单网卡自测：沿用构造时的口*/, &err)) {
        std::fprintf(stderr, "[realnic] FAIL: addNic: %s\n", err.toLatin1().constData());
        return 1;
    }
    // ★ COAST_GW_COASTCORE=1：装进程内出站（DIRECT），把这条真机自测变成
    //   **coastcore 的真机验证** —— 连接不再拨回环 SOCKS，而是由 DirectOutbound
    //   从本机直接出去。判据在结尾：ccInProcess 必须涨、SOCKS 必须零连接。
    std::shared_ptr<ProxyConfigStore> ccStore;
    if (qEnvironmentVariableIsSet("COAST_GW_COASTCORE")) {
        ccStore = std::make_shared<ProxyConfigStore>();
        QVector<ProxyNode> nodes;
        nodes.append(ProxyNode::direct());
        ccStore->reload(std::make_shared<const ProxyConfig>(
            nodes, QStringLiteral("DIRECT"), ProxyConfig::Mode::Global));
        auto *ccF = new CoreDialerFactory(ccStore.get(), new Socks5OutboundFactory(kSocksPort));
        ccF->setRouter([](const QString &, const QString &) { return QStringLiteral("DIRECT"); });
        ccF->setInboundTag(QStringLiteral("Coast-Gateway"));
        net.setOutboundFactory(ccF);
        std::fprintf(stderr, "[realnic] CoastCore 进程内出站：已启用（DIRECT）\n");
    }
    // ★ 必须把靶机 MAC 登记进端点的抓包过滤器。端点装的 BPF 是
    //   `(ether src <在册 MAC>) or arp` —— **不登记就在内核那层被丢掉**，
    //   现象是"网关 0 帧，而靶机 curl 照常通"（那是 Windows 三层转发干的，与数据面无关）。
    //   生产路径由 LanGateway 的 pushMacFilter 负责，本自测绕开了 LanGateway，所以要自己调。
    if (!ep->setSourceMacFilter({victimMac}))
        std::fprintf(stderr, "[realnic] 警告：MAC 过滤登记失败，可能收不到帧\n");

    net.addDevice(victimIp, victimMac, kUser);

    // 只把**来自靶机**的帧喂进栈（等价于 LanGateway 的 victim 过滤，其余逻辑不需要）
    int fedCount = 0;
    QObject::connect(ep, &IL2Endpoint::frameReceived, &net, [&](const QByteArray &f) {
        if (f.size() < 14 + 20)
            return;
        if (memcmp(f.constData() + 6, victimMac.constData(), 6) != 0)
            return; // 源 MAC 不是靶机
        const uchar *p = reinterpret_cast<const uchar *>(f.constData());
        if (p[12] != 0x08 || p[13] != 0x00)
            return; // 只测 IPv4
        // ★★ 只喂**发往测试网段 203.0.113.0/24** 的帧。
        //   只判源 MAC 是不够的：靶机发给本机的正常流量（SSH！）源 MAC 也是它，
        //   喂进栈后 smoltcp 会当成"要代理的连接"去终结、回 RST —— 第一次跑就是这样
        //   把我自己的 SSH 会话打断的。生产的 LanGateway 正是靠 bypLan/bypBcast 这组
        //   旁路判据避免这件事（LanGateway_linux.cpp 的过滤链），这里做等价的最小版本。
        if (!(p[30] == 203 && p[31] == 0 && p[32] == 113))
            return;
        ++fedCount;
        // 深拷贝：收环里的帧是 fromRawData 视图，槽返回即失效（IL2Endpoint.h 的硬约束）
        net.inputFrame(ep, QByteArray(f.constData(), f.size()));
    });

    std::fprintf(stderr,
                 "[realnic] 已就绪（栈=%s）。靶机上执行：\n"
                 "    ip route add 203.0.113.0/24 via %s\n"
                 "    ip neigh replace %s lladdr %s dev <if>\n"
                 "    curl -m 5 http://203.0.113.5/\n"
                 "  等待 %d ms ...\n",
                 net.activeTcpStack(), localIp.toLatin1().constData(),
                 localIp.toLatin1().constData(), localMac.toHex(':').constData(), waitMs);

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
        if (socks.gotConnect)
            loop.quit();
    });
    tick.start(50);
    deadline.start(waitMs);
    loop.exec();

    if (!socks.gotConnect) {
        std::fprintf(stderr,
                     "[realnic] FAIL: 没等到 SOCKS CONNECT（从靶机喂入 %d 帧）\n"
                     "  fed=0 → 抓包没收到靶机的帧（网卡选错 / 靶机没发 / 过滤挡住）\n"
                     "  fed>0 → 帧进来了但栈没终结连接（这才是栈本身的问题）\n",
                     fedCount);
        return 1;
    }
    if (socks.user != kUser) {
        std::fprintf(stderr, "[realnic] FAIL: 每设备身份错：期望 %s 实得 %s\n",
                     kUser.toLatin1().constData(), socks.user.toLatin1().constData());
        return 2;
    }
    std::fprintf(stderr,
                 "[realnic] PASS(%s) —— 真 Npcap + 真机帧：喂入 %d 帧 → "
                 "SOCKS CONNECT(user=%s, dport=%u)\n",
                 net.activeTcpStack(), fedCount, socks.user.toLatin1().constData(), socks.dport);
    return 0;
}

// ————————————————————— 软件路径吞吐基准（COAST_GW_THROUGHPUT=1）—————————————————————
//
// ★ 存在的理由：**把网卡整个拿掉**。
//   本仓库此前所有网关吞吐数字都在一台 QEMU 虚机上量，而 2026-08-04 的判别实验证明
//   那台机器上「每帧 12~17 µs」里绝大部分是 e1000 模拟的开销，不是我们的代码
//   （同一个 Npcap 换到 Hyper-V 虚拟网卡后掉到 2.9~3.8 µs，见
//    docs/gateway-bottleneck-audit.md 第八节）。于是"网关有多快"这个问题在这台机器上
//   一直没法回答 —— 测出来的都是台子。
//
//   `FakeEp` 不碰任何驱动：出帧直接落进内存。所以这条基准量的是**纯软件成本**：
//     inputFrame → coast_stack_input → schedulePoll → poll → conn_data
//     → Socks5Tcp::write → 回环 socket → 假 SOCKS
//   这个数**与网卡无关，因而可跨机器比较**，也是 poll 提频那次改动在集成层面的回归护栏
//   （Rust 侧那条 throughput_scales_with_poll_rate 只覆盖引擎，不含 C++ 桥接与 SOCKS）。
//
// ⚠️ 它**不是**端到端吞吐：不含 Npcap 收发、不含真实链路、不含真实 mihomo 的加解密。
//    别拿它去承诺用户能跑多快。它回答的是"我们自己这段代码值多少 CPU"。
namespace {

/// 带载荷的数据帧。buildTcp 只造 54 字节的无载荷帧，基准要灌数据。
/// 返回总帧长。
int buildTcpData(uint8_t *f, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                 const uint8_t *payload, int plen)
{
    const int ipLen = 20 + 20 + plen;
    std::memset(f, 0, 14 + ipLen);
    std::memcpy(f, kOurMac, 6);
    std::memcpy(f + 6, kDevMac, 6);
    f[12] = 0x08; f[13] = 0x00;
    f[14] = 0x45;
    f[16] = static_cast<uint8_t>(ipLen >> 8); f[17] = static_cast<uint8_t>(ipLen & 0xff);
    f[22] = 64; f[23] = 6;
    std::memcpy(f + 26, kDevIp, 4);
    std::memcpy(f + 30, kDstIp, 4);
    const uint16_t ipc = fold16(onesSum(f + 14, 20, 0));
    f[24] = static_cast<uint8_t>(ipc >> 8); f[25] = static_cast<uint8_t>(ipc & 0xff);

    uint8_t *t = f + 34;
    t[0] = static_cast<uint8_t>(sport >> 8); t[1] = static_cast<uint8_t>(sport & 0xff);
    t[2] = static_cast<uint8_t>(dport >> 8); t[3] = static_cast<uint8_t>(dport & 0xff);
    t[4] = uint8_t(seq >> 24); t[5] = uint8_t(seq >> 16);
    t[6] = uint8_t(seq >> 8);  t[7] = uint8_t(seq);
    t[8] = uint8_t(ack >> 24); t[9] = uint8_t(ack >> 16);
    t[10] = uint8_t(ack >> 8); t[11] = uint8_t(ack);
    t[12] = 0x50; t[13] = 0x18; // PSH|ACK
    t[14] = 0xFF; t[15] = 0xFF;
    if (plen > 0)
        std::memcpy(t + 20, payload, static_cast<size_t>(plen));
    uint32_t ph = 0;
    ph = onesSum(kDevIp, 4, ph);
    ph = onesSum(kDstIp, 4, ph);
    ph += 6;
    ph += static_cast<uint32_t>(20 + plen);
    const uint16_t tc = fold16(onesSum(t, 20 + plen, ph));
    t[16] = static_cast<uint8_t>(tc >> 8); t[17] = static_cast<uint8_t>(tc & 0xff);
    return 14 + ipLen;
}

double cpuSeconds()
{
#ifdef Q_OS_WIN
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        const quint64 kt = (quint64(k.dwHighDateTime) << 32) | k.dwLowDateTime;
        const quint64 ut = (quint64(u.dwHighDateTime) << 32) | u.dwLowDateTime;
        return double(kt + ut) / 1e7;
    }
#endif
    return 0.0;
}

} // namespace

int runGatewayThroughputBench()
{
    const quint16 kSocksPort = 47901;
    const QString kUser = QStringLiteral("dev-abc123");

    MiniSocks socks(kSocksPort);
    if (!socks.ok) {
        std::fprintf(stderr, "[gwbench] FAIL: 假 SOCKS 监听 %u 失败\n", kSocksPort);
        return 3;
    }

    NetStack net(kSocksPort);
    QString err;
    if (!net.init(&err)) {
        std::fprintf(stderr, "[gwbench] FAIL: NetStack::init: %s\n", err.toLatin1().constData());
        return 3;
    }
    FakeEp ep(QByteArray(reinterpret_cast<const char *>(kOurMac), 6));
    if (!net.addNic(&ep, ep.localMac(), QStringLiteral("10.99.0.2"),
                    QStringLiteral("255.255.255.0"), 0 /*单网卡自测：沿用构造时的口*/, &err)) {
        std::fprintf(stderr, "[gwbench] FAIL: addNic: %s\n", err.toLatin1().constData());
        return 3;
    }
    net.addDevice(QStringLiteral("10.99.0.1"),
                  QByteArray(reinterpret_cast<const char *>(kDevMac), 6), kUser, false);

    // ——— 三次握手 ———
    uint8_t f[2048];
    buildSyn(f, 51000, 443);
    net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), 54));
    for (int i = 0; i < 200 && ep.synAck < 1; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (ep.synAck < 1) {
        std::fprintf(stderr, "[gwbench] FAIL: 没等到 SYN-ACK\n");
        return 1;
    }
    const quint32 ackToPeer = ep.synAckSeq + 1;
    buildTcp(f, 51000, 443, 0x10, 1001, ackToPeer);
    net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), 54));
    for (int i = 0; i < 400 && !socks.gotConnect; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (!socks.gotConnect) {
        std::fprintf(stderr, "[gwbench] FAIL: 没等到 SOCKS CONNECT\n");
        return 1;
    }
    // 等 SOCKS 回复被 Socks5Tcp 消化（established 之后写才会真正流向 socket）
    for (int i = 0; i < 100; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);

    // ═══════════════ 下行基准（COAST_GW_BENCH_DIR=down）═══════════════
    //
    // ★ 存在的理由：文档里一直写着"下行没做归因，但结构对称，预计结论类似" ——
    //   那是**假设**。而下行有一处上行没有的嫌疑：`SmolConn::toStack` 是 QByteArray，
    //   每次部分写之后 `remove(0, wrote)` 会 memmove 剩余部分，队列深时是 O(n²)。
    //   假设必须去量。
    //
    // 路径：假 SOCKS 写 → 回环 → Socks5Tcp::dataReceived → toStack → coast_conn_send
    //       → poll → out_frame → FakeEp（丢弃并计数）
    // 设备侧由本函数按 FakeEp 统计到的载荷量回 ACK，保证窗口不关死。
    if (qEnvironmentVariableIsSet("COAST_GW_BENCH_DIR")
        && qgetenv("COAST_GW_BENCH_DIR") == QByteArray("down")) {
        if (!socks.tunnel) {
            std::fprintf(stderr, "[gwbench] FAIL: 下行基准拿不到隧道 socket\n");
            return 3;
        }
        // ★ 拥塞用例：把设备通告窗口调小，逼出 smolPumpToStack 的**部分写**路径 ——
        //   `toStack.remove(0, wrote)` 每次要 memmove 剩余部分，队列深时是 O(n²)。
        //   判据不能看吞吐（窗口小了吞吐本来就降），要看 **CPU/字节**：
        //   若它随窗口变小而显著上升，就是那个 memmove 在放大。
        if (qEnvironmentVariableIsSet("COAST_GW_BENCH_DEVWND")) {
            const int w = qEnvironmentVariableIntValue("COAST_GW_BENCH_DEVWND");
            if (w >= 512 && w <= 65535)
                g_devWnd = uint16_t(w);
        }
        QByteArray blob(256 * 1024, 'D');
        const qint64 dtarget = (g_devWnd < 16384) ? 4 * 1024 * 1024 : 32 * 1024 * 1024;
        QElapsedTimer dwall;
        dwall.start();
        const double dcpu0 = cpuSeconds();
        int idle = 0;
        quint32 devAck = 0;
        qint64 dspins = 0, dspinsProd = 0; // 同上行：区分"真干活"与"空转"

        while (ep.dataBytes < dtarget && dwall.elapsed() < 20000) {
            // 假 SOCKS 侧尽量灌（写缓冲有上限时 Qt 自己会攒着）
            while (socks.tunnel->bytesToWrite() < 4 * 1024 * 1024)
                socks.tunnel->write(blob);

            ++dspins;
            const qint64 before = ep.dataBytes;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 3);
            if (ep.dataBytes != before)
                ++dspinsProd;

            // 设备回 ACK：确认 FakeEp 已经收到的全部载荷，否则窗口很快关死
            if (ep.sawData) {
                const quint32 want = ep.firstDataSeq + quint32(ep.dataBytes);
                if (want != devAck) {
                    devAck = want;
                    // 设备在下行模式里不发数据，自身序号恒为 1001（握手后的下一个）
                    buildTcp(f, 51000, 443, 0x10, 1001, devAck);
                    net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), 54));
                }
            }
            if (ep.dataBytes == before) {
                if (++idle > 600)
                    break;
            } else {
                idle = 0;
            }
        }

        const double dsecs = double(dwall.elapsed()) / 1000.0;
        const double dcpu = cpuSeconds() - dcpu0;
        const double dmbps = double(ep.dataBytes) * 8.0 / dsecs / 1e6;
        const double dcore = (dmbps > 1.0) ? (dcpu / dsecs) / (dmbps / 1000.0) : 0.0;
        // 下行的"帧"按 MSS 计（栈自己分段），用 1460 折算
        const double dframes = double(ep.dataBytes) / 1460.0;
        std::fprintf(stderr,
                     "[gwbench][下行] 设备窗口=%u 上线 %lld B / %.2fs → %.0f Mb/s；CPU %.2fs "
                     "(%.3f 核/Gbps, %.2f us/帧, %.2f ns/字节)\n",
                     unsigned(g_devWnd), static_cast<long long>(ep.dataBytes), dsecs, dmbps,
                     dcpu, dcore,
                     (dframes > 0 ? dcpu * 1e6 / dframes : 0.0),
                     (ep.dataBytes > 0 ? dcpu * 1e9 / double(ep.dataBytes) : 0.0));
        std::fprintf(stderr,
                     "[gwbench][下行]   主循环 %lld 轮，其中有进展的 %lld 轮（%.1f%%）\n",
                     static_cast<long long>(dspins), static_cast<long long>(dspinsProd),
                     (dspins > 0 ? 100.0 * double(dspinsProd) / double(dspins) : 0.0));
        if (ep.dataBytes < dtarget / 16) {
            std::fprintf(stderr, "[gwbench][下行] FAIL: 只搬了 %lld B —— 路径有阻塞\n",
                         static_cast<long long>(ep.dataBytes));
            return 2;
        }
        return 0;
    }

    // ★ 短路必须在握手**之后**才打开：STAGE>=2 关掉 poll，而握手本身要 poll。
    if (qEnvironmentVariableIsSet("COAST_GW_BENCH_STAGE"))
        coastSetBenchStage(qEnvironmentVariableIntValue("COAST_GW_BENCH_STAGE"));

    // ——— 灌数据 ———
    // 设备严格遵守我方通告窗口（FakeEp 从出方向 ACK 里解析 ack/window）。
    // ★ 载荷尺寸可调（COAST_GW_BENCH_PAYLOAD）。这不是调参，是**判别器**：
    //   · 若 us/帧 随载荷基本不变 → 成本是**每帧固定的**（事件派发、分配、系统调用次数）
    //   · 若 us/帧 随载荷线性增长 → 成本是**每字节的**（拷贝、校验和）
    //   两者的修法完全不同，不先分清就是瞎优化。
    int kPayload = 1400;
    if (qEnvironmentVariableIsSet("COAST_GW_BENCH_PAYLOAD")) {
        const int v = qEnvironmentVariableIntValue("COAST_GW_BENCH_PAYLOAD");
        if (v >= 64 && v <= 1400)
            kPayload = v;
    }
    static uint8_t payload[1400];
    std::memset(payload, 0x5A, sizeof payload);

    // ═══════════ 多连接聚合基准（COAST_GW_BENCH_CONNS=N）═══════════
    //
    // ★ 回答一个此前一直没问的问题：**420 Mb/s 是"一条连接"的数，还是"整台机器"的数？**
    //   两者含义天差地别：
    //     · 若是每连接的 → 多设备并发时总量还能往上叠，420 只是单流上限
    //     · 若是聚合的   → 那就是**整个网关的总容量**，加设备只会互相分摊
    //   数据面是**单线程**的（NetStack.h 的线程模型、coaststack.h 的单线程契约），
    //   所以后者的可能性很大 —— 但"很可能"不是测量。
    //
    // 判据：N 条连接同时灌，看总吞吐是否随 N 上升。
    //   总吞吐基本不变 ⇒ 单线程已饱和，420 Mb/s 就是整机上限。
    if (qEnvironmentVariableIsSet("COAST_GW_BENCH_CONNS")) {
        const int nConn = qBound(1, qEnvironmentVariableIntValue("COAST_GW_BENCH_CONNS"), 64);

        struct Conn {
            quint16 sport;
            quint32 seq;
            quint32 ackToPeer;
        };
        QVector<Conn> conns;
        uint8_t mf[2048];

        // 逐条握手（第一条已经在上面建好了，这里从第二条开始）
        conns.append(Conn{51000, 1001, ackToPeer});
        for (int i = 1; i < nConn; ++i) {
            const quint16 sp = quint16(51000 + i);
            ep.synAck = 0;
            buildSyn(mf, sp, 443);
            net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(mf), 54));
            for (int k = 0; k < 200 && ep.synAck < 1; ++k)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            if (ep.synAck < 1) {
                std::fprintf(stderr, "[gwbench] FAIL: 第 %d 条连接没等到 SYN-ACK\n", i + 1);
                return 1;
            }
            const quint32 a = ep.synAckSeq + 1;
            buildTcp(mf, sp, 443, 0x10, 1001, a);
            net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(mf), 54));
            for (int k = 0; k < 100; ++k)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            conns.append(Conn{sp, 1001, a});
        }
        // 等所有 SOCKS 隧道就绪
        for (int k = 0; k < 300; ++k)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);

        const qint64 mtarget = 32 * 1024 * 1024;
        const qint64 rx0 = socks.rxBytes;
        QElapsedTimer mwall;
        mwall.start();
        const double mcpu0 = cpuSeconds();
        int midle = 0;

        while (socks.rxBytes - rx0 < mtarget && mwall.elapsed() < 20000) {
            bool fed = false;
            for (Conn &c : conns) {
                const auto st = ep.ackByPort.value(c.sport, qMakePair(c.seq, quint32(65535)));
                for (;;) {
                    const quint32 inflight = c.seq - st.first;
                    if (inflight + quint32(kPayload) > st.second)
                        break;
                    const int len =
                        buildTcpData(mf, c.sport, 443, c.seq, c.ackToPeer, payload, kPayload);
                    net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(mf), len));
                    c.seq += quint32(kPayload);
                    fed = true;
                }
            }
            const qint64 before = socks.rxBytes;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 3);
            if (!fed && socks.rxBytes == before) {
                if (++midle > 400)
                    break;
            } else {
                midle = 0;
            }
        }

        const qint64 moved = socks.rxBytes - rx0;
        const double msecs = double(mwall.elapsed()) / 1000.0;
        const double mcpu = cpuSeconds() - mcpu0;
        const double mmbps = double(moved) * 8.0 / msecs / 1e6;
        std::fprintf(stderr,
                     "[gwbench][聚合] 连接数=%d 送达 %lld B / %.2fs → **总 %.0f Mb/s**"
                     "（每连接 %.0f Mb/s）；CPU %.2fs = %.0f%% 单核\n",
                     nConn, static_cast<long long>(moved), msecs, mmbps, mmbps / nConn, mcpu,
                     mcpu / msecs * 100.0);
        if (moved < mtarget / 8) {
            std::fprintf(stderr, "[gwbench][聚合] FAIL: 只搬了 %lld B\n",
                         static_cast<long long>(moved));
            return 2;
        }
        return 0;
    }

    // ★ 先标定「造帧」本身的开销，之后从总量里扣掉。
    //   基准每帧都要算 1420 字节的 TCP 校验和 —— 那是**夹具**的成本，生产路径不付。
    //   不扣掉就会把它算进"我们的代码有多贵"，得出偏高的结论。
    // ★ GetProcessTimes 的粒度是 15.6 ms。标定只跑 2 万帧时总耗时约 1 个 tick，
    //   误差 ±0.78 us/帧 —— 足以把 STAGE=2/3 的净值算成负数（第一版就是这么废的）。
    //   20 万帧 ≈ 10+ 个 tick，误差降到 10% 以内。
    constexpr int kCalib = 200000;
    const double calib0 = cpuSeconds();
    for (int i = 0; i < kCalib; ++i)
        buildTcpData(f, 51000, 443, 1001 + quint32(i) * 1400, ackToPeer, payload, kPayload);
    const double calibUsPerFrame = (cpuSeconds() - calib0) * 1e6 / kCalib;

    const qint64 target = 16 * 1024 * 1024; // 16 MiB（小载荷时帧数已经很多）
    quint32 seq = 1001;
    QElapsedTimer wall;
    wall.start();
    const double cpu0 = cpuSeconds();
    int stalls = 0;

    // ★ STAGE>=3 时没有任何东西在消费，窗口反馈不存在 —— 改成灌固定帧数，
    //   判据从"送达多少"换成"喂了多少"。两种模式量的都是 CPU，可直接相减做归因。
    const int stage = qEnvironmentVariableIsSet("COAST_GW_BENCH_STAGE")
                          ? qEnvironmentVariableIntValue("COAST_GW_BENCH_STAGE")
                          : 0;
    qint64 fedBytes = 0;
    // ★ 主循环自旋次数。processEvents(AllEvents, 3) 在事件队列空时**立刻返回**，
    //   所以这个 while 是忙等 —— 它自己烧的 CPU 会被算进下面报的每一个数字里。
    //   报出来才知道污染有多大（空转占比高 = 污染严重）。
    qint64 spins = 0;
    qint64 spinsFed = 0;
    if (stage >= 3) {
        // 同理：STAGE>=3 每帧只有 1~2 us，按 target 只够跑 0.02 s = 一两个 tick。
        // 放大 20 倍，让总 CPU 落在几百毫秒量级才量得准。
        const qint64 nFrames = target * 20 / kPayload;
        for (qint64 i = 0; i < nFrames; ++i) {
            const int len = buildTcpData(f, 51000, 443, seq, ackToPeer, payload, kPayload);
            net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), len));
            seq += quint32(kPayload);
            fedBytes += kPayload;
            if ((i & 0x3FF) == 0)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
        }
    } else {
        // ★ STAGE=1 时 SOCKS 侧收不到任何东西，socks.rxBytes 恒 0 —— 用它当条件会空跑到超时。
        while ((stage >= 1 ? fedBytes : socks.rxBytes) < target && wall.elapsed() < 20000) {
            ++spins;
            bool fed = false;
            for (;;) {
                const quint32 inflight = seq - ep.lastAck;
                if (inflight + quint32(kPayload) > ep.lastWnd)
                    break;
                const int len = buildTcpData(f, 51000, 443, seq, ackToPeer, payload, kPayload);
                net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), len));
                seq += quint32(kPayload);
                fedBytes += kPayload;
                fed = true;
            }
            const qint64 before = socks.rxBytes;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 3);
            if (fed)
                ++spinsFed;
            if (!fed && socks.rxBytes == before) {
                if (++stalls > 400)
                    break;
            } else {
                stalls = 0;
            }
        }
    }
    // 归因时以"喂进去的字节"为准（STAGE>=1 时 SOCKS 侧本来就收不到）
    const qint64 accounted = (stage >= 1) ? fedBytes : socks.rxBytes;

    const double secs = double(wall.elapsed()) / 1000.0;
    const double cpu = cpuSeconds() - cpu0;
    const double mbps = double(accounted) * 8.0 / secs / 1e6;
    const double corePerGbps = (mbps > 1.0) ? (cpu / secs) / (mbps / 1000.0) : 0.0;
    const double frames = double(accounted) / kPayload;
    const double usPerFrame = (accounted > 0) ? cpu * 1e6 / frames : 0.0;
    // 扣掉夹具造帧的那部分，才是「我们的代码」的净成本
    const double netUsPerFrame = usPerFrame - calibUsPerFrame;
    const double netCorePerGbps =
        (mbps > 1.0) ? (netUsPerFrame * frames / 1e6 / secs) / (mbps / 1000.0) : 0.0;

    std::fprintf(stderr,
                 "[gwbench] 载荷=%d 送达 %lld B / %.2fs → %.0f Mb/s；CPU %.2fs "
                 "(%.3f 核/Gbps, %.2f us/帧)\n", kPayload,
                 static_cast<long long>(accounted), secs, mbps, cpu, corePerGbps, usPerFrame);
    std::fprintf(stderr,
                 "[gwbench]   主循环自旋 %lld 次，其中真正喂了帧的 %lld 次（%.1f%%）"
                 " —— 空转占比越高，下面的 CPU 数被夹具污染得越厉害\n",
                 static_cast<long long>(spins), static_cast<long long>(spinsFed),
                 (spins > 0 ? 100.0 * double(spinsFed) / double(spins) : 0.0));
    std::fprintf(stderr,
                 "[gwbench]   背压：上行节流 %lld 次，下行暂停 %lld 次"
                 "（各级都应接近 0，否则流态不可比、归因不成立）\n",
                 static_cast<long long>(GatewayDiag::c.upThrottleHits),
                 static_cast<long long>(GatewayDiag::c.downPauseHits));
    std::fprintf(stderr,
                 "[gwbench]   STAGE=%d 扣掉夹具造帧 %.2f us/帧 → **净 %.2f us/帧 "
                 "(%.2f ns/字节), %.3f 核/Gbps**\n",
                 stage, calibUsPerFrame, netUsPerFrame, netUsPerFrame * 1000.0 / kPayload,
                 netCorePerGbps);
    std::fprintf(stderr,
                 "[gwbench]   ★ 这是**去掉网卡之后**的纯软件成本（FakeEp 不碰驱动）；"
                 "不含 Npcap 收发/真实链路/真实核心，别当端到端吞吐用。\n");

    if (stage == 0 && socks.rxBytes < target / 4) {
        std::fprintf(stderr, "[gwbench] FAIL: 20s 内只搬了 %lld B，远低于目标 —— 路径有阻塞\n",
                     static_cast<long long>(socks.rxBytes));
        return 2;
    }
    return 0;
}

// ═══════ CoastCore 进程内出站端到端自测（COAST_GW_COASTCORE_SELFTEST=1）═══════
//
// ★ 补的是一个**真空白**：阶段 1~5 全部落地之后，`coastcore: true` 这条路
//   一次都没被真正跑过 —— 所有既有自测都是在它**关着**的情况下绿的。
//   也就是说那五个阶段的代码从没执行过一行；"编过了 + 别的测试没红"证明不了它能用。
//
// 判据两条，缺一不可：
//   ① GatewayDiag::ccInProcess > 0 —— 拨号真的走了**进程内**出站
//   ② 假 SOCKS 一次都没被连上   —— 没有偷偷回退核心
// 只有 ① 成立说明进程内路径被执行了；只有 ② 成立说明它不是"看着开了、实则全回退"。
// （目的地是否可达**不在判据里**：这里测的是选路与拨号，不是公网连通性。）
int runCoastCoreOutboundSelfTest()
{
    const quint16 kSocksPort = 47903;
    const QString kUser = QStringLiteral("dev-abc123");

    // 假 SOCKS：这条自测里它**应当一次都不被用到**。留着就是为了证明这一点。
    MiniSocks socks(kSocksPort);
    if (!socks.ok) {
        std::fprintf(stderr, "[cc] FAIL: 假 SOCKS 监听失败\n");
        return 3;
    }

    NetStack net(kSocksPort);
    QString err;
    if (!net.init(&err)) {
        std::fprintf(stderr, "[cc] FAIL: NetStack::init: %s\n", err.toLatin1().constData());
        return 3;
    }

    // 装进程内出站：只含内建 DIRECT 的快照 + 恒选 DIRECT 的 router。
    // 回退工厂仍是假 SOCKS —— 一旦 router 判不了而回退，判据②立刻红，
    // 正好把"其实走了回退"这种假通过挡在外面。
    auto store = std::make_shared<ProxyConfigStore>();
    {
        QVector<ProxyNode> nodes;
        nodes.append(ProxyNode::direct());
        store->reload(std::make_shared<const ProxyConfig>(
            nodes, QStringLiteral("DIRECT"), ProxyConfig::Mode::Global));
    }
    auto *factory = new CoreDialerFactory(store.get(), new Socks5OutboundFactory(kSocksPort));
    factory->setRouter([](const QString &, const QString &) { return QStringLiteral("DIRECT"); });
    factory->setInboundTag(QStringLiteral("Coast-Gateway"));
    net.setOutboundFactory(factory);

    FakeEp ep(QByteArray(reinterpret_cast<const char *>(kOurMac), 6));
    if (!net.addNic(&ep, ep.localMac(), QStringLiteral("10.99.0.2"),
                    QStringLiteral("255.255.255.0"), 0 /*单网卡自测：沿用构造时的口*/, &err)) {
        std::fprintf(stderr, "[cc] FAIL: addNic: %s\n", err.toLatin1().constData());
        return 3;
    }
    net.addDevice(QStringLiteral("10.99.0.1"),
                  QByteArray(reinterpret_cast<const char *>(kDevMac), 6), kUser, false);

    const qint64 ccBefore = GatewayDiag::c.ccInProcess;

    uint8_t f[64];
    buildSyn(f, 51100, 443);
    net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), 54));
    for (int i = 0; i < 200 && ep.synAck < 1; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (ep.synAck < 1) {
        std::fprintf(stderr, "[cc] FAIL: 没等到 SYN-ACK\n");
        return 1;
    }
    buildTcp(f, 51100, 443, 0x10, 1001, ep.synAckSeq + 1);
    net.inputFrame(&ep, QByteArray(reinterpret_cast<const char *>(f), 54));
    for (int i = 0; i < 300; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);

    const qint64 ccDelta = GatewayDiag::c.ccInProcess - ccBefore;
    if (ccDelta <= 0) {
        std::fprintf(stderr,
                     "[cc] FAIL: ccInProcess 没涨（Δ=%lld）—— 拨号没走进程内出站。"
                     "五个阶段的代码等于没被执行\n",
                     static_cast<long long>(ccDelta));
        return 2;
    }
    if (socks.gotConnect) {
        std::fprintf(stderr,
                     "[cc] FAIL: 假 SOCKS 被连上了 —— 偷偷回退了核心，"
                     "「绕过核心」这件事没真发生\n");
        return 2;
    }

    std::fprintf(stderr,
                 "[cc] PASS —— 设备 SYN → smoltcp 终结 → **进程内出站**（ccInProcess Δ=%lld，"
                 "SOCKS 回退 0 次）\n",
                 static_cast<long long>(ccDelta));
    return 0;
}
