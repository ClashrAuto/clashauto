#pragma once

// ARP 双向投毒（中间人）—— 透明网关把被劫持设备的流量引到本机的手段。
//
// 原理：对每台 victim 周期性（~1.5s）发两条**伪造的 ARP reply**：
//   (a) 发给 victim：谎称「网关 IP 在本机 MAC」→ victim 把出网流量发给我们；
//   (b) 发给网关：  谎称「victim IP 在本机 MAC」→ 回程流量也发给我们。
// 于是本机成为该设备与网关之间的中间人（配合 IL2Endpoint 抓帧 + 用户态栈终结）。
//
// 安全红线：**必须可靠还原**。停止劫持 / 析构 / 退出时给 victim 与网关重发「正确映射」的 ARP
// （heal），否则被劫持设备的 ARP 缓存仍指向本机 → 设备直接断网。healOne() 连发数遍以抵消缓存。
//
// 本类**跨平台可编译**：只拼 QByteArray 以太帧并交给 IL2Endpoint::send()，不含任何系统调用。
// 端点由外部持有（本类不拥有）。未配置（缺网关 MAC 等）时所有操作 no-op + qWarning。
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class IL2Endpoint;
class QTimer;

class ArpSpoofer : public QObject
{
    Q_OBJECT
public:
    explicit ArpSpoofer(IL2Endpoint *endpoint, QObject *parent = nullptr);
    ~ArpSpoofer() override; // 析构必须 healAll()，兜底还原所有被劫持设备

    // 配置本机拓扑。三个参数皆为字符串（localMac/gatewayMac = "aa:bb:cc:dd:ee:ff"，gatewayIp = 点分十进制）。
    // 解析失败则视为未配置（后续操作 no-op）。网段/网关变化时可重配。
    void configure(const QString &localMac, const QString &gatewayIp, const QString &gatewayMac);

    // 开始劫持某设备：记录并立即发一次欺骗 ARP，确保定时器在跑。
    void startSpoof(const QString &victimMac, const QString &victimIp);
    // 停止劫持某设备：立刻还原（heal）该设备与网关的 ARP，并从集合移除。
    void stopSpoof(const QString &victimMac);
    // 给所有被劫持设备发正确 ARP（heal）后清空集合、停表。退出/急停调用。
    void healAll();

    QStringList victims() const; // 当前被劫持的 victim MAC 列表

private:
    struct Target {
        QByteArray mac; // 6 字节
        QByteArray ip;  // 4 字节
    };

    void tick();                 // 定时重发所有 victim 的欺骗 ARP
    void sendSpoof(const Target &t); // 给一个 victim 发两条欺骗 ARP（(a)给victim +(b)给网关）
    // 给一个 victim 发数遍正确 ARP：gatewayIp is-at gatewayMac（给 victim）、victimIp is-at victimMac（给网关）。
    void healOne(const QByteArray &victimMac, const QByteArray &victimIp);
    bool configured() const;     // 本机/网关信息是否齐备且合法

    static QByteArray macToBytes(const QString &); // "aa:bb:.." → 6 字节（非法返回空）
    static QByteArray ipToBytes(const QString &);  // "1.2.3.4"  → 4 字节（用 QHostAddress；非法返回空）
    // 拼一个完整以太 ARP-reply 帧（42 字节）。所有 QByteArray 需已是正确长度。
    static QByteArray buildArpReply(const QByteArray &ethDst, const QByteArray &ethSrc,
                                    const QByteArray &senderMac, const QByteArray &senderIp,
                                    const QByteArray &targetMac, const QByteArray &targetIp);

    IL2Endpoint *m_endpoint = nullptr; // 不拥有
    QTimer *m_timer = nullptr;
    QByteArray m_localMac;   // 6 字节（本机 = 冒充的“网关”）
    QByteArray m_gatewayIp;  // 4 字节
    QByteArray m_gatewayMac; // 6 字节（真实网关，heal 时用）
    QHash<QString, Target> m_victims; // key = 小写 victimMac
};
