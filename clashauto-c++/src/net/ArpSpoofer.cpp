#include "ArpSpoofer.h"

#include <cstdio>

#include "IL2Endpoint.h"

#include <QDebug>
#include <QDateTime>
#include <QHostAddress>
#include <QTimer>

#include <cstring>

// 重发周期：主力是 answerGatewayArp 的「一问就抢答」，周期重发只作兜底（设备缓存未过期、
// 不主动 ARP 的间隙里，抵消真网关可能发来的 gratuitous ARP）。1s 比原来的 1.5s 更压得住，
// 又不至于像几百毫秒那样像 ARP 风暴、招 UniFi 这类设备的防护。
static constexpr int kSpoofIntervalMs = 1000;
// heal 连发遍数：多发几遍抵消可能滞留的欺骗缓存，确保设备恢复联网。
static constexpr int kHealRepeat = 3;
// 唤醒沿高频窗口：50ms 一发、跑 8 拍（≈400ms）。空闲后设备重新解析网关那一瞬，1s 的周期重发太慢
// ——真网关的解析应答会先落地把设备夺回；这 400ms 高频重投把这段过渡期压过去，直到稳态接管。
static constexpr int kBoostIntervalMs = 50;
static constexpr int kBoostTicks = 8;
// ARP 抢答的「短促连发」延迟：真网关也会应答同一个请求，一发抢答不保证是「最后写入者」。除了当场
// 连发两帧，再在 5ms / 18ms 各补一帧，把真网关那一发（软件转发路径通常更慢）盖回去。
static constexpr int kAnswerBurstDelay1Ms = 5;
static constexpr int kAnswerBurstDelay2Ms = 18;
// 隔离目标条目的空闲老化与上限。真机实测过放大：被禁设备扫一遍离线 IP 段，每个不存在的地址
// 也会建条目，tick() 每秒全量重投 —— 20 个目标 23.5 帧/秒、80 个 89.4 帧/秒，完全线性。
// 老化让「不再访问的目标」自然退出；上限是兜底，防 nmap 这类一瞬间铺满整个 /24。
// 30s：远大于正常设备连续访问同一对端的间隔，又远小于「扫一次就钉一整天」。
static constexpr qint64 kIsoTargetIdleMs = 30000;
// 64：一台设备真正会持续互访的局域网对端不会有这么多（NAS/打印机/网关/几台电脑）；
// 顶到上限说明它在扫段，那正是我们要拒绝放大的场景 —— 丢掉最老的一条腾位。
static constexpr int kIsoMaxTargetsPerDevice = 64;
// 主动毒化的播种窗口：进入隔离后这段时间内，每个 tick 都拿最新的 m_lanMac 补齐目标。
// 60s：足够覆盖「进程刚起来、局域网 MAC 还没学全」那段空窗（实测 50s 时仍可能漏），
// 又不至于让设备真正不访问的对端被无限期钉住（窗口结束后照常走 kIsoTargetIdleMs 老化）。
static constexpr qint64 kIsoSeedWindowMs = 60000;
// 「毒过谁」账本的每设备上限。只存 quint32，256 条不到 2KB；够覆盖一个 /24 里真实存在的主机数，
// 又不至于让一台狂扫段的设备把内存吃穿。顶到上限就不再记 —— 那些条目由设备自己老化。
static constexpr int kIsoPoisonLedgerMax = 256;

// 4 字节网络序 IP → quint32（与 m_lanMac / m_isoAnswered 的键同一编码：大端拼装）。
static quint32 ipFromBytes(const QByteArray &b)
{
    if (b.size() != 4)
        return 0;
    const uchar *p = reinterpret_cast<const uchar *>(b.constData());
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

ArpSpoofer::ArpSpoofer(IL2Endpoint *endpoint, QObject *parent)
    : QObject(parent), m_endpoint(endpoint), m_timer(new QTimer(this)),
      m_boostTimer(new QTimer(this))
{
    m_timer->setInterval(kSpoofIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &ArpSpoofer::tick);
    m_boostTimer->setInterval(kBoostIntervalMs);
    // 高频窗口整个存在的意义就是「抢在真网关的应答之前」，差几十毫秒就输。默认 CoarseTimer 在
    // Windows 上受 15.6ms 系统节拍量化，50ms 实际是 62.5ms，8 拍的窗口从 400ms 变成 500ms 且每拍
    // 都晚 25% —— 正好晚在最要紧的那一瞬。周期重发那个 1s 的定时器不需要（5% 的粗调无所谓）。
    // 同 NetStack 的 lwIP 泵，理由详见那里。
    m_boostTimer->setTimerType(Qt::PreciseTimer);
    connect(m_boostTimer, &QTimer::timeout, this, &ArpSpoofer::boostTick);
}

ArpSpoofer::~ArpSpoofer()
{
    // 兜底：无论如何退出，都把被劫持设备的 ARP 还原，避免留人断网。
    healAll();
}

bool ArpSpoofer::configured() const
{
    return m_localMac.size() == 6 && m_gatewayIp.size() == 4 && m_gatewayMac.size() == 6;
}

void ArpSpoofer::configure(const QString &localMac, const QString &gatewayIp, const QString &gatewayMac)
{
    m_localMac = macToBytes(localMac);
    m_gatewayIp = ipToBytes(gatewayIp);
    m_gatewayMac = macToBytes(gatewayMac);
    // ★ 只在**状态翻转**时说一次。configure 每轮扫描都会被调用（真机实测 ~5s 一次、每网卡一次），
    //   原来无条件 qWarning：一张没有网关的网卡就能让日志永远以 5s 一条的速度长下去 ——
    //   实测 512s 刷了 102 行同一句话（装了 Docker 的机器人手一堆 veth，全都命中），
    //   真正有信息的行被淹掉。NdpSpoofer::configure 早就是边沿触发的，这边一直漏着。
    //   措辞也一并改：这多半不是「配置错了」，而是**这张卡所在链路上就没有网关**
    //   （veth/netns/docker0 之类），属正常情况，不该报得像故障。
    const bool ok = configured();
    if (ok != m_lastConfigOk || !m_configLogged) {
        m_lastConfigOk = ok;
        m_configLogged = true;
        if (!ok)
            qWarning() << "ArpSpoofer: 该卡无可用网关，v4 劫持在此卡停用（正常：veth/netns 等无网关链路）"
                       << "local=" << localMac << "gatewayIp=" << gatewayIp
                       << "gatewayMac=" << gatewayMac;
    }
}

void ArpSpoofer::startSpoof(const QString &victimMac, const QString &victimIp)
{
    if (!configured()) {
        qWarning() << "ArpSpoofer: 未配置，忽略 startSpoof" << victimMac;
        return;
    }
    const QByteArray vmac = macToBytes(victimMac);
    const QByteArray vip = ipToBytes(victimIp);
    if (vmac.size() != 6 || vip.size() != 4) {
        qWarning() << "ArpSpoofer: victim 地址非法，忽略" << victimMac << victimIp;
        return;
    }

    const Target t{vmac, vip};
    m_victims.insert(victimMac.toLower(), t);
    sendSpoof(t); // 立刻抢占一次，别等下个 tick
    if (!m_timer->isActive())
        m_timer->start();
}

void ArpSpoofer::stopSpoof(const QString &victimMac)
{
    const auto it = m_victims.find(victimMac.toLower());
    if (it == m_victims.end())
        return;
    if (configured())
        healOne(it->mac, it->ip); // 先还原再移除
    m_victims.erase(it);
    if (m_victims.isEmpty()) {
        if (m_timer->isActive())
            m_timer->stop();
        if (m_boostTimer && m_boostTimer->isActive())
            m_boostTimer->stop();
        m_boostRemaining = 0;
    }
}

bool ArpSpoofer::answerGatewayArp(const QByteArray &frame)
{
    if (!m_endpoint || !configured())
        return false;
    // 完整以太 + ARP 至少 42 字节：14 以太头 + 28 ARP 载荷。
    if (frame.size() < 42)
        return false;
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    // ethertype 必须是 ARP(0x0806)、op 必须是 request(1)。htype/ptype/hlen/plen 按以太+IPv4 校验，
    // 免得把别的 ARP 变种误当请求。
    if (f[12] != 0x08 || f[13] != 0x06)
        return false;
    if (f[14] != 0x00 || f[15] != 0x01 || f[16] != 0x08 || f[17] != 0x00 || f[18] != 6 || f[19] != 4)
        return false;
    if (f[20] != 0x00 || f[21] != 0x01) // op=1 request
        return false;

    // ARP 载荷：sha=f[22..27], spa=f[28..31], tpa=f[38..41]。
    // 只对「问的正是我们冒充的那个网关 IP」抢答。
    if (std::memcmp(f + 38, m_gatewayIp.constData(), 4) != 0)
        return false;

    const QByteArray senderMac(reinterpret_cast<const char *>(f + 22), 6); // 设备 MAC
    const QByteArray senderIp(reinterpret_cast<const char *>(f + 28), 4);  // 设备 IP
    // 回一帧欺骗 reply 给设备：sha=本机MAC, spa=网关IP → 「网关在本机」。目的就是发起请求的设备。
    const QByteArray reply =
            buildArpReply(senderMac, m_localMac, m_localMac, m_gatewayIp, senderMac, senderIp);
    // 短促连发（item 1）：当场两帧 + 5ms/18ms 各补一帧。真网关也会应答这次请求，只发一帧不保证
    // 我们是设备最终采信的「最后写入者」；连发 + 延迟重盖把真网关那一发压回去。延迟帧发出前复核该
    // victim 是否还在集合里（其间可能已 stopSpoof），避免对已还原的设备再投毒。
    m_endpoint->send(reply);
    m_endpoint->send(reply);
    QTimer::singleShot(kAnswerBurstDelay1Ms, this, [this, reply, senderMac] {
        if (m_endpoint && hasVictimMac(senderMac)) {
            m_endpoint->send(reply);
            // ★ 立刻收口：帧进了批量发送队列要等下一个 flushTx 点才真出网卡，而这条抢答路径的
            //   全部意义就是「抢在真网关的应答之前当最后写入者」，那是毫秒级的竞争。延迟帧由
            //   singleShot 从定时器回调进来，不在收帧排空里，漏掉这句就要等泵那一拍（最多 25ms），
            //   5ms/18ms 这两发精心挑的补帧就白设计了。
            m_endpoint->flushTx();
        }
    });
    QTimer::singleShot(kAnswerBurstDelay2Ms, this, [this, reply, senderMac] {
        if (m_endpoint && hasVictimMac(senderMac)) {
            m_endpoint->send(reply);
            // ★ 立刻收口：帧进了批量发送队列要等下一个 flushTx 点才真出网卡，而这条抢答路径的
            //   全部意义就是「抢在真网关的应答之前当最后写入者」，那是毫秒级的竞争。延迟帧由
            //   singleShot 从定时器回调进来，不在收帧排空里，漏掉这句就要等泵那一拍（最多 25ms），
            //   5ms/18ms 这两发精心挑的补帧就白设计了。
            m_endpoint->flushTx();
        }
    });
    return true;
}

void ArpSpoofer::reassertNow()
{
    if (!m_endpoint || !configured() || m_victims.isEmpty())
        return;
    // 节流:网关若短时间连发多条 ARP,别每条都重投一整轮(会把我们变成 ARP 风暴源)。50ms 一次
    // 足以在设备解毒后立刻盖回,又不放大。
    if (m_lastReassert.isValid() && m_lastReassert.elapsed() < 50)
        return;
    m_lastReassert.restart();
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        sendSpoof(it.value());
}

void ArpSpoofer::startBoost()
{
    if (!configured() || m_victims.isEmpty())
        return;
    // 立刻推一轮（item 3 的「首包兜底」）：sendSpoof 里带 request-form，能把「网关在本机」装进设备
    // **已老化删除**的空表项——赶在它排队的首包解析完成前钉好，首包因此不再走真路由/被丢。
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        sendSpoof(it.value());
    // 随后进入 50ms×N 的高频窗口（item 2），压过唤醒期真网关的解析应答，直到稳态接管。
    m_boostRemaining = kBoostTicks;
    if (m_boostTimer && !m_boostTimer->isActive())
        m_boostTimer->start();
}

void ArpSpoofer::boostTick()
{
    if (!configured() || m_victims.isEmpty()) {
        if (m_boostTimer)
            m_boostTimer->stop();
        m_boostRemaining = 0;
        return;
    }
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        sendSpoof(it.value());
    if (--m_boostRemaining <= 0 && m_boostTimer)
        m_boostTimer->stop();
}

void ArpSpoofer::healAll()
{
    // ★ 先还原隔离毒化，再还原网关劫持 —— 顺序要紧：healIsolation 要用 m_victims 查 victimIP
    //   （伪造 request 的 tpa），下面那句 m_victims.clear() 一执行就查不到了。
    //
    //   healAll 原先**只**还原网关那条，隔离毒化的条目全靠设备自己老化。主动毒化上线后这
    //   变成了实打实的故障：退出时最多 64 个对端仍指向本机，而进程已经不在了，那台设备的
    //   局域网直接黑洞（外网却正常，症状极具迷惑性）。真机踩过一次。
    for (const QByteArray &mac : m_isoAnswered.keys())
        healIsolation(mac);
    if (configured()) {
        for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
            healOne(it->mac, it->ip);
    }
    m_victims.clear();
    if (m_timer && m_timer->isActive())
        m_timer->stop();
    if (m_boostTimer && m_boostTimer->isActive())
        m_boostTimer->stop();
    m_boostRemaining = 0;
}

QStringList ArpSpoofer::victims() const
{
    return m_victims.keys();
}

void ArpSpoofer::tick()
{
    if (!configured())
        return;
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        sendSpoof(it.value());
    reassertIsolation();
}

// ★ 隔离条目必须**跟着周期重投**，只在 who-has 那一刻抢答是不够的 —— 真主机也会应答同一次
//   请求，一次性抢答赢不稳。真机实测：判据全过、抢答帧确实发了，A 的缓存里仍是对端的真实
//   MAC，ping 照通。网关那条之所以稳，正是因为 tick() 每秒重投把真网关的应答压回去；
//   这里照做。
//
//   注意这**不等于**「向全网段投毒」：只对 `m_isoAnswered` 里记着的目标重投，而那份记录是
//   「A 真的问过谁」按需长出来的 —— A 没访问过的主机一条都不发。发包量与 A 的实际行为成正比，
//   而不是与子网大小成正比。
void ArpSpoofer::reassertIsolation()
{
    if (!m_endpoint || m_isoAnswered.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 播种窗口内的设备：先拿**当前**的 m_lanMac 补齐目标，再往下重投。
    for (const QByteArray &m : m_isolated)
        if (now < m_isoSeedUntil.value(m, 0))
            seedIsolationTargets(m, now);
    for (auto it = m_isoAnswered.begin(); it != m_isoAnswered.end(); ++it) {
        const QByteArray &victimMac = it.key();
        if (!m_isolated.contains(victimMac))
            continue; // 已经解除隔离，等 healIsolation 收尾
        // 必须走 victimIpByMac：m_victims 的键是小写 MAC **字符串**，拿 6 字节 QByteArray 去
        // contains/value 会走隐式转换，编译得过但永远查不中 —— 原先这里 victimIp 恒为 0.0.0.0，
        // 下面那条伪造 request 的 tpa 也就恒为 0，设备按 RFC 826 根本不会当成「问我」来处理。
        QByteArray victimIp = victimIpByMac(victimMac);
        if (victimIp.size() != 4)
            victimIp = QByteArray(4, char(0));
        // ★ 空闲老化：设备已经不再访问的目标停止重投。没有它，一次全网段扫描留下的条目会
        //   **永久**每秒重投（实测每条约 1.08 帧/秒、完全线性）。设备自己的 ARP 缓存本来就
        //   几分钟就老化，我们钉得比它久没有意义 —— 它下次要用会重新 who-has，那时再抢答即可。
        for (auto t = it->begin(); t != it->end();) {
            if (now - t.value() > kIsoTargetIdleMs)
                t = it->erase(t);
            else
                ++t;
        }
        for (auto t = it->constBegin(); t != it->constEnd(); ++t) {
            const quint32 ip = t.key();
            QByteArray ipb(4, char(0));
            ipb[0] = char((ip >> 24) & 0xFF);
            ipb[1] = char((ip >> 16) & 0xFF);
            ipb[2] = char((ip >> 8) & 0xFF);
            ipb[3] = char(ip & 0xFF);
            // 记进「毒过谁」账本（不老化），退出/解除隔离时据此还原。
            QSet<quint32> &ledger = m_isoPoisoned[victimMac];
            if (ledger.size() < kIsoPoisonLedgerMax)
                ledger.insert(ip);
            const QByteArray reply =
                    buildArpReply(victimMac, m_localMac, m_localMac, ipb, victimMac, victimIp);
            m_endpoint->send(reply);
            // 再补一条伪造 request（op=1，tpa=victimIP/tha=victimMAC，sender 仍是 对端IP/本机MAC），
            // 与 sendSpoof 的 (a2) 同理同据：现代系统对**非请求** reply 视而不见，缓存里已有对端
            // 真实 MAC 时上面那条 reply 是打不动它的；只有「目标是自己」的 request 才按 RFC 826
            // 强制吸收 sender。主动毒化能不能真盖掉热缓存，全靠这一条。
            if (victimIp != QByteArray(4, char(0)))
                m_endpoint->send(
                        buildArpRequest(victimMac, m_localMac, m_localMac, ipb, victimMac, victimIp));
        }
    }
    m_endpoint->flushTx();
}

void ArpSpoofer::sendSpoof(const Target &t)
{
    if (!m_endpoint || !configured())
        return;

    // (a) 发给 victim 的欺骗 reply：sha=本机MAC，spa=网关IP → “网关 IP 在本机”，victim 出网走我们。
    const QByteArray replyToVictim =
        buildArpReply(t.mac, m_localMac, m_localMac, m_gatewayIp, t.mac, t.ip);
    // (a2) 再补一条**伪造 ARP request**（op=1，tpa=victimIP、tha=victimMAC；sender 仍是 网关IP/本机MAC）。
    //   设备收到「目标是自己」的请求时按 RFC 826 **必须**处理 sender、把「网关 IP 在本机 MAC」装进缓存
    //   并回应——**即使它的邻居表项此前已 STALE/老化删除**（现代系统对非请求 reply 视而不见，只认这条）。
    //   这是「设备空闲后首次访问先失败/先走真路由」的根治：空闲期让缓存不掉、唤醒期让首包直接命中我们。
    const QByteArray requestToVictim =
        buildArpRequest(t.mac, m_localMac, m_localMac, m_gatewayIp, t.mac, t.ip);
    // (b) 发给网关的欺骗 reply：sha=本机MAC，spa=victimIP → “victim IP 在本机”，回程走我们。
    const QByteArray replyToGateway =
        buildArpReply(m_gatewayMac, m_localMac, m_localMac, t.ip, m_gatewayMac, m_gatewayIp);

    m_endpoint->send(replyToVictim);
    m_endpoint->send(requestToVictim);
    m_endpoint->send(replyToGateway);
    // 收口：本函数由 1s 周期 tick 和 50ms boost 两个定时器回调驱动，都不在收帧排空路径上。
    // boost 存在的意义是「设备邻居缓存刚老化、正在重新解析时抢在真网关前面」——再压 25ms
    // 就正好错过那个窗口。
    m_endpoint->flushTx();
}

QVector<QByteArray> ArpSpoofer::healFrames(const QString &victimMac, const QString &victimIp) const
{
    if (!configured())
        return {};
    const QByteArray vmac = macToBytes(victimMac);
    const QByteArray vip = ipToBytes(victimIp);
    if (vmac.size() != 6 || vip.size() != 4)
        return {};
    return buildHealFrames(vmac, vip);
}

// 一台 victim 的四帧「还原」报文。healOne 发它们，GatewayPanic 也预先缓存它们——所以拼帧的逻辑
// 只能有这一份（两边发的必须字节级一致，否则崩溃兜底发出去的是另一套语义）。
QVector<QByteArray> ArpSpoofer::buildHealFrames(const QByteArray &victimMac,
                                                const QByteArray &victimIp) const
{
    // ★ 还原必须能盖过设备「刚被投毒刷新、正处于 REACHABLE」的网关表项——否则停代理后设备一直把网关
    //   指向已停工的本机 MAC，直到表项自己老化/重解析才恢复（就是之前那个「尽力而为、有恢复窗口」）。
    //   关键机制：普通(非无偿) ARP 改一个「1 秒内刚更新过」的表项会被内核 LOCKTIME 挡掉（Linux 的
    //   arp_process：override = 表项超过 LOCKTIME 未更新 || is_garp）。而**无偿 ARP（sender IP == target
    //   IP）**命中 is_garp → **无视 LOCKTIME 直接 override**，立刻改。所以 heal 发的是「网关 IP is-at
    //   真实网关 MAC」的**无偿** ARP（sender IP=target IP=网关 IP）。reply 和 request 各发一份，兼容
    //   「只认无偿 request」的 BSD/macOS 与「reply 也认」的 Linux。eth 源 MAC 用**本机 MAC**（WiFi 站点
    //   模式下发不出伪造源 MAC；ARP 处理只看载荷里的 sender，与 eth 源无关）。
    //   同理给真网关补一份「victim IP is-at victim MAC」的无偿 ARP，还原它那一侧被投毒的映射。
    static const QByteArray kZeroMac(6, char(0));
    const QByteArray garpToVictim =
        buildArpReply(victimMac, m_localMac, m_gatewayMac, m_gatewayIp, m_gatewayMac, m_gatewayIp);
    const QByteArray garpReqToVictim =
        buildArpRequest(victimMac, m_localMac, m_gatewayMac, m_gatewayIp, kZeroMac, m_gatewayIp);
    const QByteArray garpToGateway =
        buildArpReply(m_gatewayMac, m_localMac, victimMac, victimIp, victimMac, victimIp);
    const QByteArray garpReqToGateway =
        buildArpRequest(m_gatewayMac, m_localMac, victimMac, victimIp, kZeroMac, victimIp);

    return {garpToVictim, garpReqToVictim, garpToGateway, garpReqToGateway};
}

void ArpSpoofer::healOne(const QByteArray &victimMac, const QByteArray &victimIp)
{
    if (!m_endpoint)
        return;
    const QVector<QByteArray> frames = buildHealFrames(victimMac, victimIp);
    for (int i = 0; i < kHealRepeat; ++i)
        for (const QByteArray &f : frames)
            m_endpoint->send(f);
    // ★ 还原帧**必须**立刻出网卡：这条路径跑在「撤劫持 / 退出 / 睡眠」上，攒在队列里等下一个
    //   flushTx 点是赌运气 —— 端点随后就可能被 close()/析构。漏发一次的代价是被代理设备的 ARP
    //   一直指着本机，彻底断网直到缓存老化。
    m_endpoint->flushTx();
}

QByteArray ArpSpoofer::macToBytes(const QString &mac)
{
    const QStringList parts = mac.split(QLatin1Char(':'));
    if (parts.size() != 6)
        return {};
    QByteArray out(6, char(0));
    for (int i = 0; i < 6; ++i) {
        bool ok = false;
        const uint v = parts.at(i).toUInt(&ok, 16);
        if (!ok || v > 0xFFu)
            return {};
        out[i] = static_cast<char>(v);
    }
    return out;
}

QByteArray ArpSpoofer::ipToBytes(const QString &ip)
{
    const QHostAddress addr(ip);
    bool ok = false;
    const quint32 v = addr.toIPv4Address(&ok); // 网络序无关：下面手动按大端拆字节
    if (!ok)
        return {};
    QByteArray out(4, char(0));
    out[0] = static_cast<char>((v >> 24) & 0xFFu);
    out[1] = static_cast<char>((v >> 16) & 0xFFu);
    out[2] = static_cast<char>((v >> 8) & 0xFFu);
    out[3] = static_cast<char>(v & 0xFFu);
    return out;
}

bool ArpSpoofer::hasVictimMac(const QByteArray &mac6) const
{
    for (const Target &t : m_victims)
        if (t.mac == mac6)
            return true;
    return false;
}

QByteArray ArpSpoofer::buildArp(quint8 op, const QByteArray &ethDst, const QByteArray &ethSrc,
                                const QByteArray &senderMac, const QByteArray &senderIp,
                                const QByteArray &targetMac, const QByteArray &targetIp)
{
    // 完整以太帧：14 字节以太头 + 28 字节 ARP 载荷 = 42 字节。op：1=request，2=reply。
    QByteArray f;
    f.reserve(42);
    f.append(ethDst);            // 目的 MAC (6)
    f.append(ethSrc);            // 源 MAC   (6)
    f.append(char(0x08));        // ethertype = 0x0806 (ARP)
    f.append(char(0x06));
    f.append(char(0x00));        // htype = 0x0001 (Ethernet)
    f.append(char(0x01));
    f.append(char(0x08));        // ptype = 0x0800 (IPv4)
    f.append(char(0x00));
    f.append(char(0x06));        // hlen = 6
    f.append(char(0x04));        // plen = 4
    f.append(char(0x00));        // op（高字节恒 0）
    f.append(char(op));
    f.append(senderMac);         // sha (6)
    f.append(senderIp);          // spa (4)
    f.append(targetMac);         // tha (6)
    f.append(targetIp);          // tpa (4)
    return f;
}

QByteArray ArpSpoofer::buildArpReply(const QByteArray &ethDst, const QByteArray &ethSrc,
                                     const QByteArray &senderMac, const QByteArray &senderIp,
                                     const QByteArray &targetMac, const QByteArray &targetIp)
{
    return buildArp(0x02, ethDst, ethSrc, senderMac, senderIp, targetMac, targetIp);
}

QByteArray ArpSpoofer::buildArpRequest(const QByteArray &ethDst, const QByteArray &ethSrc,
                                       const QByteArray &senderMac, const QByteArray &senderIp,
                                       const QByteArray &targetMac, const QByteArray &targetIp)
{
    return buildArp(0x01, ethDst, ethSrc, senderMac, senderIp, targetMac, targetIp);
}

void ArpSpoofer::resolveLanPeer(quint32 targetIp4, quint32 ownIp4)
{
    if (!m_endpoint || targetIp4 == 0 || ownIp4 == 0)
        return;
    // 每目标 1s 一次：转发路径查不到 MAC 时会连丢好几帧，别每帧都广播一次 who-has。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_lanResolveAt.value(targetIp4, 0);
    if (last != 0 && now - last < 1000)
        return;
    m_lanResolveAt.insert(targetIp4, now);

    auto u32ToBytes = [](quint32 v) {
        QByteArray b(4, char(0));
        b[0] = char((v >> 24) & 0xFF);
        b[1] = char((v >> 16) & 0xFF);
        b[2] = char((v >> 8) & 0xFF);
        b[3] = char(v & 0xFF);
        return b;
    };
    // 以**本机身份**广播 who-has <target>：sender=本机MAC/本机IP，target=对端。对端回的是单播
    // reply（目的=本机 MAC），交换机会送到本端口，被 learnLanMac 学到 —— 这正是隔离转发缺的那块。
    static const QByteArray bcast(6, char(0xFF));
    static const QByteArray zeroMac(6, char(0));
    const QByteArray req = buildArpRequest(bcast, m_localMac, m_localMac, u32ToBytes(ownIp4),
                                           zeroMac, u32ToBytes(targetIp4));
    m_endpoint->send(req);
    m_endpoint->flushTx();
}

// ———————————————————————— 局域网隔离 ————————————————————————

QByteArray ArpSpoofer::victimIpByMac(const QByteArray &mac6) const
{
    for (auto it = m_victims.constBegin(); it != m_victims.constEnd(); ++it)
        if (it->mac == mac6)
            return it->ip;
    return {};
}

void ArpSpoofer::setIsolatedMacs(const QVector<QByteArray> &macs6)
{
    const QVector<QByteArray> before = m_isolated;
    m_isolated.clear();
    for (const QByteArray &m : macs6) {
        if (m.size() == 6)
            m_isolated.append(m);
    }
    // ★ 主动毒化：设备**刚进入**隔离时，把当前已知的所有局域网对端一次性塞进重投集合。
    //
    //   没有这段的话隔离是纯被动的——只有设备自己 who-has 某个对端时 answerIsolationArp 才
    //   有机会抢答，而设备的 ARP 缓存里若还留着对端的**真实** MAC（macOS 约 20 分钟、Linux
    //   base_reachable_time 30s 起随机），它压根不会再问，流量直接 L2 直达，隔离形同虚设。
    //   真机实测过：34 刚跟 239 通过信、缓存是热的，设成隔离后 SSH banner 照样读得到，nft 的
    //   两条隔离规则命中都是 0（包根本没到网关）；手工 `arp -d` 清掉缓存后立刻就拦住了。
    //
    //   这里只打时间戳，实际发帧交给 reassertIsolation（跟着 1s tick 走）。kIsoTargetIdleMs
    //   的空闲老化会在 30s 后把设备实际没在访问的目标清掉，所以这是个**有界**的毒化窗口，
    //   不会退化成「一次全网段扫描留下的条目永久每秒重投」那个老问题。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QByteArray &m : m_isolated) {
        if (before.contains(m))
            continue; // 本来就在隔离中，别重开窗口
        m_isoSeedUntil[m] = now + kIsoSeedWindowMs;
        seedIsolationTargets(m, now); // 立刻播一次，剩下的交给 tick 补齐
    }
    // 解除隔离的：关掉窗口（还原由 LanGateway 侧调 healIsolation 负责）
    for (auto it = m_isoSeedUntil.begin(); it != m_isoSeedUntil.end();) {
        if (m_isolated.contains(it.key()))
            ++it;
        else
            it = m_isoSeedUntil.erase(it);
    }
}

// 把当前已知的局域网对端塞进某台被隔离设备的重投集合。窗口内每 tick 调一次。
void ArpSpoofer::seedIsolationTargets(const QByteArray &victimMac6, qint64 now)
{
    const QByteArray ownIp = victimIpByMac(victimMac6);
    const quint32 own = ownIp.size() == 4 ? ipFromBytes(ownIp) : 0;
    const quint32 gw = m_gatewayIp.size() == 4 ? ipFromBytes(m_gatewayIp) : 0;
    QHash<quint32, qint64> &tgts = m_isoAnswered[victimMac6];
    for (auto it = m_lanMac.constBegin(); it != m_lanMac.constEnd(); ++it) {
        // 自己不用毒；网关那条由 sendSpoof 的 (a)/(a2) 专门负责，别在这儿发重复帧。
        if (it.key() == own || it.key() == gw)
            continue;
        if (tgts.size() >= kIsoMaxTargetsPerDevice && !tgts.contains(it.key()))
            break; // 与抢答路径同一个上限，别让毒化成为放大器
        tgts.insert(it.key(), now);
    }
}

void ArpSpoofer::learnLanMac(quint32 ip, const QByteArray &mac6)
{
    // 只学「像样」的：非零 IP、非零/非广播 MAC。学错一条就会把还原帧发到错地方。
    if (ip == 0 || ip == 0xFFFFFFFFu || mac6.size() != 6)
        return;
    if (mac6 == QByteArray(6, '\0') || mac6 == QByteArray(6, char(0xFF)))
        return;
    m_lanMac.insert(ip, mac6);
}

QByteArray ArpSpoofer::lanMac(quint32 ip) const
{
    return m_lanMac.value(ip);
}

bool ArpSpoofer::answerIsolationArp(const QByteArray &frame, quint32 subnetBase,
                                    quint32 subnetMask, quint32 localIp4, quint32 gatewayIp4)
{
    if (!m_endpoint || m_isolated.isEmpty() || !subnetMask)
        return false;
    if (frame.size() < 42)
        return false;
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    if (f[12] != 0x08 || f[13] != 0x06)
        return false;
    if (f[14] != 0x00 || f[15] != 0x01 || f[16] != 0x08 || f[17] != 0x00 || f[18] != 6 || f[19] != 4)
        return false;
    if (f[20] != 0x00 || f[21] != 0x01) // 只抢答 request
        return false;

    const QByteArray senderMac(reinterpret_cast<const char *>(f + 22), 6);
    if (!m_isolated.contains(senderMac))
        return false; // 不是被隔离设备问的 → 不插手

    const quint32 tpa = (quint32(f[38]) << 24) | (quint32(f[39]) << 16)
                        | (quint32(f[40]) << 8) | quint32(f[41]);
    // 只管同网段、且不是本机/网关的目标：网关由 answerGatewayArp 负责，本机本来就该回自己。
    if ((tpa & subnetMask) != (subnetBase & subnetMask))
        return false;
    if (tpa == localIp4 || tpa == gatewayIp4 || tpa == 0)
        return false;

    const QByteArray senderIp(reinterpret_cast<const char *>(f + 28), 4);
    QByteArray tpaBytes(4, '\0');
    tpaBytes[0] = char((tpa >> 24) & 0xFF);
    tpaBytes[1] = char((tpa >> 16) & 0xFF);
    tpaBytes[2] = char((tpa >> 8) & 0xFF);
    tpaBytes[3] = char(tpa & 0xFF);

    // 回「这个 IP 在本机」。连发 + 两拍补帧，理由与 answerGatewayArp 完全相同：真主机也会应答，
    // 我们必须是设备采信的最后写入者。
    const QByteArray reply =
            buildArpReply(senderMac, m_localMac, m_localMac, tpaBytes, senderMac, senderIp);
    m_endpoint->send(reply);
    m_endpoint->send(reply);
    // 与 answerGatewayArp 对齐：5ms + 18ms 两拍补帧。之前只补了 5ms 那一发，对真实主机偏弱
    // ——真机抓包实测真主机硬件应答比我们早约 77µs，一发抢答赢不稳。两拍延迟重盖把它压回去。
    QTimer::singleShot(kAnswerBurstDelay1Ms, this, [this, reply, senderMac] {
        if (m_endpoint && m_isolated.contains(senderMac)) {
            m_endpoint->send(reply);
            m_endpoint->flushTx();
        }
    });
    QTimer::singleShot(kAnswerBurstDelay2Ms, this, [this, reply, senderMac] {
        if (m_endpoint && m_isolated.contains(senderMac)) {
            m_endpoint->send(reply);
            m_endpoint->flushTx();
        }
    });
    QHash<quint32, qint64> &targets = m_isoAnswered[senderMac];
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!targets.contains(tpa) && targets.size() >= kIsoMaxTargetsPerDevice) {
        // 顶到上限：淘汰最久没被访问的那条，给新目标腾位（而不是无限增长）。
        quint32 oldestIp = 0;
        qint64 oldestMs = nowMs;
        for (auto t = targets.constBegin(); t != targets.constEnd(); ++t) {
            if (t.value() <= oldestMs) {
                oldestMs = t.value();
                oldestIp = t.key();
            }
        }
        if (oldestIp)
            targets.remove(oldestIp);
    }
    targets.insert(tpa, nowMs);
    return true;
}

void ArpSpoofer::healIsolation(const QByteArray &victimMac6)
{
    if (!m_endpoint || victimMac6.size() != 6)
        return;
    // 按**账本**还原，不是按重投表 —— 重投表被空闲老化清过，据它还原会漏掉大半。
    QSet<quint32> all = m_isoPoisoned.value(victimMac6);
    const QList<quint32> answered = m_isoAnswered.value(victimMac6).keys();
    for (quint32 ip : answered)
        all.insert(ip);
    const QList<quint32> targets = all.values();
    m_isoAnswered.remove(victimMac6);
    m_isoPoisoned.remove(victimMac6);
    QByteArray victimIp = victimIpByMac(victimMac6);
    if (victimIp.size() != 4)
        victimIp = QByteArray(4, char(0));
    for (quint32 ip : targets) {
        const QByteArray real = m_lanMac.value(ip);
        // ★ 不知道真实 MAC 就**不发**：胡乱还原等于再投一次毒。这种条目让设备自己老化掉
        //   （Linux/Android 几分钟，Windows 更短），代价是这段时间它仍以为对端在本机 —— 而那时
        //   我们已经不再拦截，帧到本机后无人处理，等价于「访问不通」，不会造成错误连接。
        if (real.size() != 6)
            continue;
        QByteArray ipb(4, '\0');
        ipb[0] = char((ip >> 24) & 0xFF);
        ipb[1] = char((ip >> 16) & 0xFF);
        ipb[2] = char((ip >> 8) & 0xFF);
        ipb[3] = char(ip & 0xFF);
        // spa=目标IP, sha=目标真实MAC → 把设备缓存改回真相。目的就是那台设备。
        const QByteArray heal = buildArpReply(victimMac6, real, real, ipb, victimMac6, victimIp);
        m_endpoint->send(heal);
        m_endpoint->send(heal);
        // ★ 还要发一条伪造 request（tpa=victimIP/tha=victimMAC，sender=对端IP/对端真实MAC）。
        //   与毒化侧完全对称：只发 reply 是**还原不掉**的 —— 现代系统对非请求 reply 视而不见，
        //   设备缓存里那条指向本机的条目会一直留到自己老化。真机实测过：解除隔离后 macOS 仍
        //   把对端解析到网关 MAC，而此时我们已不再转发，等于把一台**没被隔离**的设备的局域网
        //   打成黑洞（外网正常，只有 LAN 不通，很难查）。原先 tpa 还写死 0.0.0.0，
        //   设备按 RFC 826 更不会当成「问我」来处理。
        if (victimIp != QByteArray(4, char(0)))
            m_endpoint->send(buildArpRequest(victimMac6, real, real, ipb, victimMac6, victimIp));
    }
    if (!targets.isEmpty())
        m_endpoint->flushTx();
}

// ★ 反制真实主机的抢先应答 —— 这才是隔离对真实对端能不能稳的关键。
//
//   抓包实证：被隔离设备 A 广播 who-has <真实主机 B> 时，B 硬件直接回 reply、比我们早约 77µs
//   （我们要经 收帧→用户态判定→回帧 一整圈）。光靠 answerIsolationArp 的「一问抢答」压不住，
//   A 的缓存于是时有时无。但 **B 那条抢先的 reply 我们在混杂模式下也收得到** —— 收到就立刻
//   把「B 在本机」重投一遍盖回去，正如 answerGatewayArp/reassertNow 反制真网关的解毒 ARP。
//
//   返回是否反制了（调用方据此决定要不要打诊断）。只处理「op=reply、spa=某个被隔离目标、
//   目的 MAC = 某台被隔离设备」的帧；别的 reply 一律不碰。
bool ArpSpoofer::counterIsolationReply(const QByteArray &frame)
{
    if (!m_endpoint || m_isoAnswered.isEmpty() || frame.size() < 42)
        return false;
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    if (f[12] != 0x08 || f[13] != 0x06)            // ARP
        return false;
    if (f[20] != 0x00 || f[21] != 0x02)            // 只看 reply
        return false;
    const QByteArray sha(reinterpret_cast<const char *>(f + 22), 6); // 应答者 MAC
    if (sha == m_localMac)
        return false;                              // 我们自己发的抢答，别自反制
    const QByteArray ethDst(reinterpret_cast<const char *>(f + 0), 6); // 这条 reply 发给谁
    // 目的必须是某台被隔离设备（真主机的 solicited reply 单播给发问的 A）。
    if (!m_isolated.contains(ethDst))
        return false;
    const quint32 spa = (quint32(f[28]) << 24) | (quint32(f[29]) << 16)
                        | (quint32(f[30]) << 8) | quint32(f[31]); // 应答者 IP = 被争夺的目标
    // 这个目标确实是我们正为这台设备接管的？（避免对无关 reply 乱投）
    // ★ 用 constFind 而不是 `const QHash& = m_isoAnswered.value(...)`：QHash::value() **按值返回**，
    //   把它绑到 const 引用上绑的是临时对象 —— 生命周期延长规则让它不至于立刻出错，但每次调用都
    //   悄悄做了一次完整的 QHash 深拷贝，而本函数在**收帧热路径**上（每个 ARP reply 都进来一次），
    //   一台设备最多 64 个隔离目标，等于每帧白拷 64 项。而且那种写法会让读者误以为拿到的是容器内
    //   部引用。constFind 零拷贝、语义明确。
    const auto tit = m_isoAnswered.constFind(ethDst);
    if (tit == m_isoAnswered.constEnd() || !tit->contains(spa))
        return false;
    // 顺手把真相记下来：这条 reply 里就有目标的真实 MAC，转发路径要用（省一次 resolveLanPeer）。
    learnLanMac(spa, sha);

    // 立刻重投「目标在本机」盖回 A 的缓存。
    //   sender = 本机MAC / 目标IP(spa)     → 「目标在我这」
    //   target = A 的 MAC / A 的 IP        → 这条 reply 明确回给 A
    // A 的 IP 就在 B 那条 reply 的 target 字段（f[38..41]），A 的 MAC = ethDst。
    QByteArray spaBytes(4, char(0));
    spaBytes[0] = char((spa >> 24) & 0xFF);
    spaBytes[1] = char((spa >> 16) & 0xFF);
    spaBytes[2] = char((spa >> 8) & 0xFF);
    spaBytes[3] = char(spa & 0xFF);
    const QByteArray aIp(reinterpret_cast<const char *>(f + 38), 4);
    const QByteArray reassert =
            buildArpReply(ethDst, m_localMac, m_localMac, spaBytes, ethDst, aIp);
    m_endpoint->send(reassert);
    m_endpoint->send(reassert);
    m_endpoint->flushTx();
    return true;
}
