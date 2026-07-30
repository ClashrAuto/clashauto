#pragma once

// 本机混合入站（HTTP 代理 + SOCKS5）—— CoastCore 的**第二个入口**。
//
// ★ 为什么需要它：在此之前 `CoreDialerFactory`（进程内出站）**只被 LanGateway 装配**，
//   也就是说 CoastCore 只服务「局域网里被代理的设备」，而本机自己的流量（系统代理 :7890、TUN）
//   100% 还是 mihomo 在跑。出站侧其实早就齐了（8 个协议 + QUIC/TLS/uTLS/WS + 规则 + DNS），
//   缺的从来不是「能不能拨出去」，而是「字节从哪进来」。这个单元补的就是本机这个入口。
//   见 docs/mihomo-replacement-gap.md。
//
// 它复用**同一个** OutboundFactory（现实里就是 CoreDialerFactory），所以本机流量与网关流量
// 走完全相同的分流/协议/DNS 路径 —— 不存在「两套逻辑要同步」的问题。
//
// 支持的入站形态（与 mihomo 的 mixed-port 对齐）：
//   · SOCKS5 CONNECT（首字节 0x05）—— 无认证；
//   · HTTP CONNECT（`CONNECT host:port HTTP/1.1`）—— 覆盖全部 HTTPS 流量；
//   · HTTP 绝对形式请求（`GET http://host/path HTTP/1.1`）—— 改写成源形式后转发。
//
// **明确不做**（有意为之，不是遗漏）：
//   · SOCKS4/4a：早已绝迹，加了只是扩大攻击面；首字节 0x04 直接断开。
//   · SOCKS5 UDP ASSOCIATE：本机 UDP 代理留给 TUN 那条路，见 gap 文档的优先级 ④。
//   · 入站认证：只监听回环地址时没有意义；将来要对局域网开放再加。
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QSet>

class QTcpServer;
class QTcpSocket;
class OutboundFactory;
class IOutboundTcp;

class MixedInbound : public QObject
{
    Q_OBJECT
public:
    // factory **不持有**（由上层持有，典型即 CoreDialerFactory）。
    explicit MixedInbound(OutboundFactory *factory, QObject *parent = nullptr);
    ~MixedInbound() override;

    // 默认只监听回环：本机代理没有理由对外暴露（对外暴露 = 开放中继，见上文「不做入站认证」）。
    bool listen(quint16 port, const QHostAddress &addr = QHostAddress::LocalHost);
    void stop();
    quint16 boundPort() const;
    bool isListening() const;

    // 出站的「设备身份」。本机流量统一用一个固定 user，便于在分流/记账里与被代理设备区分开。
    void setUser(const QString &user) { m_user = user; }

    // 背压触发次数（只数「进入节流态」的沿，不数每次评估）。存在的理由是**让测试能证伪**：
    // 大流量用例若没把水位顶上去，节流分支根本没跑，用例 PASS 也是空的。测试里断言它 > 0。
    quint64 upThrottleHits() const { return m_upThrottleHits; }   // 客户端→出站：卡住客户端读
    quint64 downPauseHits() const { return m_downPauseHits; }     // 出站→客户端：暂停从上游读

    // 自检：起一个本地靶服务器 + 一个直连出站桩，用真实的 SOCKS5 / HTTP CONNECT / HTTP 绝对形式
    // 三种客户端各打一遍，校验协议解析与双向转发。返回 true = 全通过（结果打到 stderr）。
    // 由 COAST_INBOUND_SELFTEST=1 触发（见 main_qml.cpp），无需任何节点或网络。
    static bool selfTest();

signals:
    void connectionOpened(const QString &target); // 供上层记账/展示

private:
    struct Session;
    void onNewConnection();
    void onClientReadable(Session *s);
    void handleHandshake(Session *s);  // 嗅探 + 解析，够了就拨号
    void startDial(Session *s, const QString &host, quint16 port, const QByteArray &initialUpstream);
    void pumpClientToOut(Session *s);
    void closeSession(Session *s, const char *why);

    QTcpServer *m_server = nullptr;
    OutboundFactory *m_factory = nullptr; // 不持有
    QString m_user = QStringLiteral("local");
    QSet<Session *> m_sessions;
    quint64 m_upThrottleHits = 0;
    quint64 m_downPauseHits = 0;
};
