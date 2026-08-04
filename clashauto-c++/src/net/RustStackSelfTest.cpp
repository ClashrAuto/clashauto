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
    t[14] = 0xFF; t[15] = 0xFF;                      // window
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
#include "NetStack.h"

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
        ++sent;
        if (f.size() >= 54 && quint8(f[12]) == 0x08 && quint8(f[13]) == 0x00
            && quint8(f[23]) == 6 && (quint8(f[47]) & 0x12) == 0x12) {
            ++synAck;
            synAckSport = quint16((quint8(f[34]) << 8) | quint8(f[35]));
            synAckSeq = (quint32(quint8(f[38])) << 24) | (quint32(quint8(f[39])) << 16)
                | (quint32(quint8(f[40])) << 8) | quint32(quint8(f[41]));
        }
        return true;
    }
    QByteArray localMac() const override { return m_mac; }
    int ifIndex() const override { return 1; }
    int mtu() const override { return 1500; }

    int sent = 0;
    int synAck = 0;
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

private:
    struct St { int phase = 0; QByteArray buf; QString user; };
    void onData(QTcpSocket *s, St *c)
    {
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
            }
            c->buf.clear();
        }
    }
    QTcpServer m_srv;
};

} // namespace

int runSmolGatewaySelfTest()
{
    const quint16 kSocksPort = 47899;
    const QString kUser = QStringLiteral("dev-abc123");

    MiniSocks socks(kSocksPort);
    if (!socks.ok) {
        std::fprintf(stderr, "[smolgw] FAIL: 假 SOCKS 监听 %u 失败\n", kSocksPort);
        return 3;
    }

    // A/B 夹具：默认测 smoltcp；显式 COAST_STACK=lwip 则用**同一套断言**测 lwIP。
    // 两条都 PASS 才说明"换栈后行为一致"，只测一条只能说明"这条能跑"。
    QByteArray want = qgetenv("COAST_STACK").trimmed().toLower();
    if (want.isEmpty()) {
        want = "smoltcp";
        qputenv("COAST_STACK", want);
    }
    NetStack net(kSocksPort);
    QString err;
    if (!net.init(&err)) {
        std::fprintf(stderr, "[smolgw] FAIL: NetStack::init: %s\n", err.toLatin1().constData());
        return 1;
    }

    // ★ 先证明测的确实是 smoltcp 那条路。lwIP 的 accept-all 对外行为与 catch-all 一致，
    //   同一个断言在两条路上都会 PASS —— 没有这一条，这个自测证明不了任何东西。
    if (want != QByteArray(net.activeTcpStack())) {
        std::fprintf(stderr,
                     "[smolgw] FAIL: 没跑在 smoltcp 上（实际=%s）——这个自测对 lwIP 也会绿，\n"
                     "         所以必须先确认路径，否则等于没测\n",
                     net.activeTcpStack());
        return 3;
    }

    FakeEp ep(QByteArray(reinterpret_cast<const char *>(kOurMac), 6));
    if (!net.addNic(&ep, ep.localMac(), QStringLiteral("10.99.0.2"),
                    QStringLiteral("255.255.255.0"), &err)) {
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
    return 0;
}
