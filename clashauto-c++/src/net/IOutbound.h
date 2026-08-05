#pragma once

// 出站抽象 —— 把「一条被用户态栈终结的连接接到某个出站」与「出站究竟是谁」解耦。
//
// 现有唯一实现 = Socks5Outbound（拨 mihomo 混合端口，见 Socks5Client.h）。后续 CoastCore 的
// **进程内**协议实现（Shadowsocks / Trojan / VMess…）同样实现这两个接口，NetStack 只依赖接口
// 加一个 OutboundFactory 即可：按节点协议选实现，对尚未实现/出错的协议回退到 Socks5Outbound。
//
// 契约完全承袭 Socks5Tcp / Socks5Udp（背压、零拷贝上行、信号语义都见 Socks5Client.h），
// 唯一区别是**去掉了 socksPort 参数**——出站目标（端口 / 节点 / 协议）在工厂造对象时就绑定好了，
// 调用方不再关心「拨去哪」。
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

// 一条被终结的 TCP 连接的出站隧道。握手成功后即一根双向字节管道：
// established() → 之后 write()/dataReceived()，closed() 表示对端关闭。
class IOutboundTcp : public QObject
{
    Q_OBJECT
public:
    explicit IOutboundTcp(QObject *parent = nullptr) : QObject(parent) {}
    ~IOutboundTcp() override = default;

    // 发起到 dstHost:dstPort 的连接。dstHost 可为 IPv4/IPv6 字面量或域名。
    // user = 该设备的 mihomo 身份（dev-<mac 短哈希>）；进程内实现可据此做每设备策略/归属。
    virtual void connectTo(const QString &dstHost, quint16 dstPort, const QString &user) = 0;
    // ★★ 契约：write() 允许在 connectTo() **之前**被调用，实现必须把这些字节缓冲下来，
    //    并在 connectTo() 之后、隧道就绪时原样补发。**connectTo() 里绝不能清空缓冲区。**
    //
    //    为什么会有「先 write 后 connectTo」这种顺序：NetStack 的 lwipTcpAccept 必须把拨号
    //    推迟一拍（Qt::QueuedConnection）才能让 lwIP 的 tcp_process 安全走完（见那边的长注释）。
    //    而一次 AF_PACKET 批量收包里，设备的三次握手 ACK 和它紧随其后的首个数据段（HTTP 请求 /
    //    TLS ClientHello）常常在**同一批**被处理完 —— 于是 accept→recv 都跑完了，排队的
    //    connectTo 才轮到。此时若 connectTo 清空 pending，设备的首个数据段就被静默吞掉：
    //    隧道建起来了却一个字节都不发，服务器等请求、客户端等响应，直到超时（真机 100% 复现，
    //    表现为「设备 curl 报 Connected 后挂住」、mihomo /connections 里该连接 upload=0）。
    virtual void write(const QByteArray &data) = 0;
    // 裸缓冲零拷贝上行：data 只需在**本次调用期间**有效，实现必须返回前把字节完整复制走
    //（生命周期契约见 Socks5Client.h，NetStack 的零拷贝路径依赖它）。
    virtual void write(const char *data, qsizetype size) = 0;
    virtual void closeTunnel() = 0;
    virtual bool isEstablished() const = 0;

    // —— 背压（流控）：语义见 Socks5Client.h ——
    virtual qint64 bytesToWrite() const = 0;
    virtual void setReadPaused(bool paused) = 0;
    virtual bool isReadPaused() const = 0;

signals:
    void established();                        // 隧道就绪，双向管道可用
    void dataReceived(const QByteArray &data); // 下行字节
    void failed(const QString &reason);        // 握手/连接失败
    void closed();                             // 隧道关闭（对端 FIN / 出错）
    // 上行字节真正交给内核（NetStack 据此按量归还 lwIP 接收窗口）。
    void upstreamBytesWritten(qint64 n);
};

// 一次 UDP 关联会话，承载某设备一个源端口的 UDP（含 DNS）。ready() 后可 sendTo()。
class IOutboundUdp : public QObject
{
    Q_OBJECT
public:
    explicit IOutboundUdp(QObject *parent = nullptr) : QObject(parent) {}
    ~IOutboundUdp() override = default;

    virtual void associate(const QString &user) = 0;
    virtual void sendTo(const QHostAddress &dstIp, quint16 dstPort, const QByteArray &payload) = 0;
    virtual void closeSession() = 0;
    virtual bool isReady() const = 0;

signals:
    void ready();
    void datagramReceived(const QHostAddress &srcIp, quint16 srcPort, const QByteArray &payload);
    void failed(const QString &reason);
    void closed();
};

// 出站工厂 —— 造对象时就把「出站是谁」（端口/节点/协议）绑定进去。NetStack 持有一个工厂，
// 每条被终结的连接向它要一个新的出站对象。切换出站实现（Socks5→进程内、或按节点选）= 换工厂。
class OutboundFactory
{
public:
    virtual ~OutboundFactory() = default;
    virtual IOutboundTcp *createTcp(QObject *parent) = 0;
    virtual IOutboundUdp *createUdp(QObject *parent) = 0;

    /// 这个工厂**自己决定从哪张网卡出去**吗？
    ///
    /// 多网卡时每张卡各有一个核心入站（端口不同、各带 `interface-name`），网关按设备所在
    /// 的卡拨对应那个口。只有进程内出站（CoastCore）是例外 —— 它不经核心，绑卡由它自己做，
    /// 所以它要能盖过每卡工厂。
    ///
    /// ★ 这条以前是用**对象身份**判的（`outFactory != ownedDefault` 就当成 CoastCore）。
    ///   那是错的：关闭 CoastCore 的那条分支会 `setOutboundFactory(new Socks5OutboundFactory(…))`
    ///   —— 一个全新对象，同样 `!= ownedDefault`，于是每卡工厂被永久吞掉，所有设备一律拨
    ///   基准端口。真机后果：副卡上的设备每条连接都拨一个没人监听的口，`socksFail` 100%，
    ///   界面上连一条连接都看不到（流量根本没到核心）。用意图判，不要用身份判。
    virtual bool bindsInterfaceItself() const { return false; }
};
