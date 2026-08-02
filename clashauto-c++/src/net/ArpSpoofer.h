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
#include <QSet>
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

    // —— 局域网隔离（policy=reject 的设备）——
    //
    // 目标：**被禁设备不能主动访问局域网，但局域网仍能访问它**。
    //
    // 只骗被禁设备 A 一台：告诉它「你要找的那个同网段 IP 也在本机」。于是
    //   · A → B 的帧全部到我们手上，由调用方按方向决定放不放（见 LanGateway 的隔离过滤）；
    //   · B → A 完全不受影响 —— 我们从不碰 B 的缓存，B 解析出的永远是 A 的真实 MAC。
    // 这个不对称是「只骗一边」的天然结果，也是它比传统全网段 MITM 安全得多的原因。
    //
    // 抢答而不是主动向全网段投毒：A 要找谁一定先广播 who-has，我们本来就收得到，
    // 只在那一刻回一帧即可 —— 无需为整个 /24 维护 254 条投毒条目、也没有额外发包量。
    void setIsolatedMacs(const QVector<QByteArray> &macs6); // 被隔离设备的 MAC（6 字节）
    // 对被隔离设备发出的「who-has <同网段 IP>」抢答本机 MAC。返回是否抢答了。
    // subnetBase/subnetMask 用于判「是不是同网段」；本机 IP 与网关 IP 不在此列
    //（网关走 answerGatewayArp，本机本来就该回自己）。
    bool answerIsolationArp(const QByteArray &frame, quint32 subnetBase, quint32 subnetMask,
                            quint32 localIp4, quint32 gatewayIp4);
    // 学到的局域网 IP→MAC（从任何观察到的 ARP 帧的 sender 字段）。转发与还原都要用真实 MAC。
    void learnLanMac(quint32 ip, const QByteArray &mac6);
    QByteArray lanMac(quint32 ip) const;
    // 撤销隔离：把我们替某台设备答过的那些条目还原成真实 MAC（只还原学到过真实 MAC 的）。
    void healIsolation(const QByteArray &victimMac6);
    // 反制真实主机对被隔离目标的抢先应答：收到「B 回给被隔离设备 A 的 reply」就立刻把
    // 「B 在本机」重投盖回去。这是隔离对真实对端能不能稳的关键（真主机硬件应答比我们快 ~77µs）。
    // 返回是否反制了。
    bool counterIsolationReply(const QByteArray &frame);
    // 主动解析一个局域网对端：以本机身份广播 who-has。对端的应答是**单播给我们**的，
    // 交换机会送到本端口 → learnLanMac 收得到。用于隔离转发查不到对端 MAC 时补一次解析。
    // ownIp4 = 本机在该网段的 IPv4（主机序）。内部按目标节流，避免每个被丢的帧都触发一次。
    void resolveLanPeer(quint32 targetIp4, quint32 ownIp4);

private:
    void reassertIsolation(); // 跟着 tick 周期重投隔离条目（一次性抢答压不住真主机，见实现）
public:

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

    // 某台 victim 的「还原」帧（healOne 实际发出去的那四帧，各一份，不含重发）。给崩溃兜底
    // GatewayPanic 预先缓存用：信号处理器里不能拼帧、不能分配，只能把现成字节裸发出去。
    // 未配置或地址非法返回空。
    QVector<QByteArray> healFrames(const QString &victimMac, const QString &victimIp) const;

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
    // 拼那四帧（唯一一份实现，healOne 与 healFrames 共用）。
    QVector<QByteArray> buildHealFrames(const QByteArray &victimMac,
                                        const QByteArray &victimIp) const;
    bool hasVictimMac(const QByteArray &mac6) const; // 6 字节 MAC 是否仍在被劫持集合里（延迟连发前复核）
    // 按 6 字节 MAC 取 victim 的 4 字节 IP。m_victims 的键是**小写 MAC 字符串**，直接拿
    // QByteArray 去 contains/value 会走 QByteArray→QString 隐式转换：编译得过、永远查不中
    // （reassertIsolation 原先就这么写，victimIp 恒为 0.0.0.0）。查不到返回空。
    QByteArray victimIpByMac(const QByteArray &mac6) const;
    void seedIsolationTargets(const QByteArray &victimMac6, qint64 now);

    QVector<QByteArray> m_isolated;             // 被隔离设备 MAC（6 字节）
    QHash<quint32, QByteArray> m_lanMac;        // 观察学到的 局域网 IP → 真实 MAC
    // 我们替哪台设备、对哪些目标 IP 抢答过 —— 撤销隔离时按这份还原。
    // 值是「目标 IP → 最后一次为它抢答的时刻(uptime ms)」。
    // ★ 必须带时间戳、且必须有上限 —— 真机实测过放大：被禁设备 ping 一遍**离线** IP 段，
    //   每个不存在的地址也会建一条（who-has 收得到就答），然后 tick() 每秒全量重投：
    //   目标 20 个 → 23.5 帧/秒；扩到 80 个 → 89.4 帧/秒，每条约 1.08 帧/秒**完全线性**。
    //   一台被禁设备扫一遍 /24 就能让我们持续发 250+ 帧/秒，多台叠加更糟 ——
    //   等于把自己变成 ARP 放大源，还是被攻击方一句 nmap 就能触发的。
    QHash<QByteArray, QHash<quint32, qint64>> m_isoAnswered;
    // 主动毒化的**持续播种窗口**：MAC → 截止时刻。窗口内每个 tick 都从 m_lanMac 补齐目标。
    // 不能只在「进入隔离」那一刻播一次 —— 那一刻通常是进程刚起来、m_lanMac 还基本是空的
    // （局域网 IP→MAC 全靠被动观察 ARP 学），播了等于没播（真机上第一轮偶然成功、第二轮就漏）。
    QHash<QByteArray, qint64> m_isoSeedUntil;
    // ★ 「我到底毒过谁」的账本 —— 与 m_isoAnswered **分开**记，且**不参与空闲老化**。
    //   m_isoAnswered 回答的是「还要不要继续每秒重投」，30s 不访问就该清掉(防放大)；
    //   但设备自己的 ARP 缓存活得久得多(macOS 约 20 分钟)。两者共用一份表的后果真机踩到了：
    //   隔离持续 90s 后目标已老化出表，此时解除隔离/退出，healIsolation 无从还原，那台**已经
    //   不再被隔离**的设备局域网继续黑洞十几分钟(外网却正常，症状极难定位)。
    //   上限比 kIsoMaxTargetsPerDevice 宽：它是「同一时刻重投多少」，这里是「累计毒过多少」。
    QHash<QByteArray, QSet<quint32>> m_isoPoisoned;
    // resolveLanPeer 的每目标节流：目标 IP → 上次发 who-has 的时刻(uptime ms)。
    QHash<quint32, qint64> m_lanResolveAt;

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
    // configure 被每轮扫描重复调用（真机 ~5s 一次/网卡），所以配置状态只在**翻转**时说一次。
    // 与 NdpSpoofer 的 m_lastConfigOk/m_configLogged 同构 —— 那边早有此处理，这边漏了。
    bool m_lastConfigOk = false;
    bool m_configLogged = false;
    QHash<QString, Target> m_victims; // key = 小写 victimMac
};
