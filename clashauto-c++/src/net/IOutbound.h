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
};
