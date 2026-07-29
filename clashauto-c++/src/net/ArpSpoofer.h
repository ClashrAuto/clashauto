#pragma once

// ARP 双向投毒（中间人）—— 透明网关把被劫持设备的流量引到本机的手段。
//
// 原理：对每台 victim 周期性（~1s）发**伪造的 ARP**：
//   (a) 发给 victim：谎称「网关 IP 在本机 MAC」→ victim 把出网流量发给我们；
//   (a2) 再补一条**伪造的 ARP request**（op=1，tpa=victimIP，spa=网关IP、sha=本机MAC）：设备收到
//        「目标是自己」的请求时按 RFC 826 必须处理 sender 并回应——**即使它的邻居表项已老化删除**
//        也会把「网关在本机」重新装上。这是治「设备空闲后首次访问先失败/先走直连」的关键：现代系统
//        (Windows/macOS/iOS/Android) 的邻居缓存 STALE/老化后**不吃非请求 reply**，只有 request 能钉回。
//   (b) 发给网关：  谎称「victim IP 在本机 MAC」→ 回程流量也发给我们。
// 于是本机成为该设备与网关之间的中间人（配合 IL2Endpoint 抓帧 + 用户态栈终结）。
//
// 安全红线：**必须可靠还原**。停止劫持 / 析构 / 退出时给 victim 与网关重发「正确映射」的 ARP
// （heal），否则被劫持设备的 ARP 缓存仍指向本机 → 设备直接断网。healOne() 连发数遍以抵消缓存。
//
// 本类**跨平台可编译**：只拼 QByteArray 以太帧并交给 IL2Endpoint::send()，不含任何系统调用。
// 端点由外部持有（本类不拥有）。未配置（缺网关 MAC 等）时所有操作 no-op + qWarning。
#include <QByteArray>
#include <QElapsedTimer>
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

    // 抢答：若 frame 是被劫持设备发出的「谁是网关?」ARP 请求，立刻回一帧「网关 IP 在本机 MAC」。
    // 周期重发（~1s）太慢——设备会被真网关的正确应答反复夺回，导致时通时不通；每次它一问就
    // 同步抢答，才能把投毒压住。返回 true = 确实是问网关且已抢答。契约：调用方在收到该设备的
    // ARP 帧时调用（源 MAC 已由二层过滤限定为被劫持设备）。
    bool answerGatewayArp(const QByteArray &frame);

    // 反应式反制:一看到真网关自己发的 ARP(它的广播 who-has 会携带「网关 IP 在真 MAC」把设备
    // 解毒)就立刻把所有 victim 重投一轮,盖回「网关在本机」。这是「时通时不通」的根治——周期
    // 重发跟 UniFi 的 ARP 刷新是 1:1 拉锯,必须一看到解毒就抢回。自带 ~50ms 节流防 ARP 风暴放大。
    void reassertNow();

    // 唤醒沿高频重投：某 victim 空闲一段时间后又开始发帧 = 它多半刚把网关重新解析过（或即将解析）。
    // 立刻推一轮欺骗（含 request-form，能装进已老化的空表项 → 首包不再走真路由/被丢），随后
    // 50ms×N 的高频窗口压过唤醒期真网关的应答，直到 1s 周期的稳态接管。调用方在检测到「空闲→活跃」
    // 沿时调一次（幂等：重复调用只是刷新窗口）。治「空闲后第一次访问先失败、再直连、再代理」的过渡。
    void startBoost();

    QStringList victims() const; // 当前被劫持的 victim MAC 列表

    // 本机/网关信息是否齐备且合法。**未配置时 startSpoof/heal 全是静默 no-op** —— 调用方（
    // LanGateway 的 enableDevice）必须先问这一句再决定「劫持是否真的上了」，否则会把「一帧毒都
    // 没发」当成劫持成功，设备就卡在「UI 显示代理中、实际走直连、而且再也不重试」的状态。
    bool configured() const;

private:
    struct Target {
        QByteArray mac; // 6 字节
        QByteArray ip;  // 4 字节
    };

    void tick();                 // 定时重发所有 victim 的欺骗 ARP
    void boostTick();            // 唤醒沿高频窗口内的重投（50ms 一发，跑满 N 拍自停）
    void sendSpoof(const Target &t); // 给一个 victim 发欺骗 ARP（(a)reply+(a2)request 给 victim +(b)给网关）
    // 给一个 victim 发数遍正确 ARP：gatewayIp is-at gatewayMac（给 victim）、victimIp is-at victimMac（给网关）。
    void healOne(const QByteArray &victimMac, const QByteArray &victimIp);
    bool hasVictimMac(const QByteArray &mac6) const; // 6 字节 MAC 是否仍在被劫持集合里（延迟连发前复核）

    static QByteArray macToBytes(const QString &); // "aa:bb:.." → 6 字节（非法返回空）
    static QByteArray ipToBytes(const QString &);  // "1.2.3.4"  → 4 字节（用 QHostAddress；非法返回空）
    // 拼一个完整以太 ARP 帧（42 字节，op=1 request / 2 reply）。所有 QByteArray 需已是正确长度。
    static QByteArray buildArp(quint8 op, const QByteArray &ethDst, const QByteArray &ethSrc,
                               const QByteArray &senderMac, const QByteArray &senderIp,
                               const QByteArray &targetMac, const QByteArray &targetIp);
    static QByteArray buildArpReply(const QByteArray &ethDst, const QByteArray &ethSrc,
                                    const QByteArray &senderMac, const QByteArray &senderIp,
                                    const QByteArray &targetMac, const QByteArray &targetIp);
    static QByteArray buildArpRequest(const QByteArray &ethDst, const QByteArray &ethSrc,
                                      const QByteArray &senderMac, const QByteArray &senderIp,
                                      const QByteArray &targetMac, const QByteArray &targetIp);

    IL2Endpoint *m_endpoint = nullptr; // 不拥有
    QTimer *m_timer = nullptr;
    QTimer *m_boostTimer = nullptr; // 唤醒沿高频窗口（50ms），跑满 kBoostTicks 拍后自停
    int m_boostRemaining = 0;       // 高频窗口剩余拍数
    QElapsedTimer m_lastReassert; // reassertNow 节流(防网关 ARP 风暴时无限放大)
    QByteArray m_localMac;   // 6 字节（本机 = 冒充的“网关”）
    QByteArray m_gatewayIp;  // 4 字节
    QByteArray m_gatewayMac; // 6 字节（真实网关，heal 时用）
    QHash<QString, Target> m_victims; // key = 小写 victimMac
};
