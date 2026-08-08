// GatewayDiag 的落盘实现。设计与取舍见 GatewayDiag.h。
//
// 这个 TU **所有平台都编**（main_qml.cpp 要调 setLogDir，而它是跨平台的），但只有 POSIX 上
// 真的有网关在往计数器里写。没有网关的平台上它就是一个从不被 sample() 的空壳。
#include "GatewayDiag.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <QFileInfo>
#include <QTextStream>

namespace {

// 单文件上限 + 只留一份备份 → 最坏 2×。4 MiB 按下面的行长（约 400 B）能存约一万条采样，
// 10s 一条 = 28 小时；轮转一份就是两天多的历史，够回溯「昨晚开始变慢」这类问题。
constexpr qint64 kMaxBytes = 4 * 1024 * 1024;
constexpr int kDefaultSampleMs = 10000;
constexpr int kMinSampleMs = 1000;

QString g_path;                 // 空 = 未启用落盘
// 上一次采样时的快照，用来算增量。**显式值初始化**：Counters 故意不带默认成员初始化器
// （原因见 GatewayDiag.h 里那段 ★★），所以这里的 {} 不是冗余，去掉就是未初始化读。
GatewayDiag::Counters g_prev{};
// 按网卡那四项的上一次快照（同样显式值初始化）。
GatewayDiag::NicCounters g_nicPrev[GatewayDiag::kMaxNicSlots] {};

// 采样窗口的起点，用来把速率折算成「每秒」。
qint64 g_lastSampleMs = 0;

bool envDisabled()
{
    static const bool off = qEnvironmentVariable("COAST_GW_DIAG") == QLatin1String("0");
    return off;
}

// 到点就把 .log 挪成 .log.1（覆盖旧备份）。轮转失败不致命——最多是文件继续长，
// 下一次采样再试；绝不因为轮转失败就丢采样。
void rotateIfNeeded()
{
    if (QFileInfo(g_path).size() < kMaxBytes)
        return;
    const QString bak = g_path + QLatin1String(".1");
    QFile::remove(bak);
    QFile::rename(g_path, bak);
}

// 差值。计数器都是单调自增的 qint64，不会回绕（int64 在这个量级上等于无限）。
qint64 d(qint64 now, qint64 prev)
{
    return now - prev;
}

} // namespace

int GatewayDiag::nicSlot(const QString &ifname)
{
    if (ifname.isEmpty())
        return kMaxNicSlots - 1; // 溢出桶
    for (int i = 0; i < kMaxNicSlots - 1; ++i) {
        if (nicNames[i] == ifname)
            return i; // 幂等：每轮 configureLocal 都会来问一次
    }
    for (int i = 0; i < kMaxNicSlots - 1; ++i) {
        if (nicNames[i].isEmpty()) {
            nicNames[i] = ifname;
            nics[i] = NicCounters{};
            return i;
        }
    }
    // 槽位用完 → 最后那个溢出桶。名字标出来，免得把它的数当成某一张卡的。
    nicNames[kMaxNicSlots - 1] = QStringLiteral("(其它)");
    return kMaxNicSlots - 1;
}

void GatewayDiag::setLogDir(const QString &userDir)
{
    if (userDir.isEmpty() || envDisabled()) {
        g_path.clear();
        return;
    }
    const QString dir = userDir + QLatin1String("/logs");
    QDir().mkpath(dir); // 和核心的 logs/ 同一个目录；核心自己也会建，这里幂等
    g_path = dir + QLatin1String("/gateway-diag.log");
    g_lastSampleMs = QDateTime::currentMSecsSinceEpoch();
}

bool GatewayDiag::enabled()
{
    return !g_path.isEmpty();
}

int GatewayDiag::sampleIntervalMs()
{
    static const int ms = [] {
        bool ok = false;
        const int v = qEnvironmentVariableIntValue("COAST_GW_DIAG_MS", &ok);
        return (ok && v >= kMinSampleMs) ? v : kDefaultSampleMs;
    }();
    return ms;
}

// 过滤前的 SYN 分桶（源 IPv4 → 本窗口计数）。与 GatewayDiag 其余部分同一个单线程前提。
static QHash<quint32, quint32> g_rawSynBySrc;

// 被各分支吃掉的 SYN（原因字符 → 源 IPv4 → 计数）。
static QHash<char, QHash<quint32, quint32>> g_synDrop;

// 是纯 IPv4 TCP SYN 就返回源 IP（网络序转主机序），否则返回 0。
static quint32 pureSynSrcIp(const unsigned char *f, int len)
{
    if (!f || len < 54)
        return 0;
    // 只认 IPv4/TCP 的**纯** SYN（SYN 置位、ACK 清零）。IPv6 不分桶：真机复现用的是 v4，
    // 而这一栏存在的唯一目的就是和设备侧 tcpdump 的 v4 计数相减。
    if (f[12] != 0x08 || f[13] != 0x00 || (f[14] >> 4) != 4 || f[23] != 6)
        return 0;
    const int off = 14 + (f[14] & 0x0F) * 4;
    if (off + 14 > len || (f[off + 13] & 0x12) != 0x02)
        return 0;
    return (quint32(f[26]) << 24) | (quint32(f[27]) << 16) | (quint32(f[28]) << 8) | quint32(f[29]);
}

void GatewayDiag::noteRawSyn(const unsigned char *f, int len)
{
    const quint32 src = pureSynSrcIp(f, len);
    if (!src)
        return;
    if (g_rawSynBySrc.size() > 256) // 有界：诊断量，宁可丢样本
        g_rawSynBySrc.clear();
    ++g_rawSynBySrc[src];
}

void GatewayDiag::noteSynDrop(const unsigned char *f, int len, char why)
{
    const quint32 src = pureSynSrcIp(f, len);
    if (!src)
        return;
    auto &bucket = g_synDrop[why];
    if (bucket.size() > 64)
        bucket.clear();
    ++bucket[src];
}

QString GatewayDiag::rawSynLine()
{
    if (g_rawSynBySrc.isEmpty())
        return QString();
    QStringList parts;
    for (auto it = g_rawSynBySrc.constBegin(); it != g_rawSynBySrc.constEnd(); ++it)
        parts << QStringLiteral("%1.%2/%3")
                     .arg((it.key() >> 8) & 0xFF)
                     .arg(it.key() & 0xFF)
                     .arg(it.value());
    g_rawSynBySrc.clear();
    parts.sort();
    QString out = QStringLiteral(" synRaw=") + parts.join(QLatin1Char(','));

    if (!g_synDrop.isEmpty()) {
        QStringList drops;
        for (auto b = g_synDrop.constBegin(); b != g_synDrop.constEnd(); ++b)
            for (auto it = b.value().constBegin(); it != b.value().constEnd(); ++it)
                drops << QStringLiteral("%1:%2.%3/%4")
                             .arg(QChar(b.key()))
                             .arg((it.key() >> 8) & 0xFF)
                             .arg(it.key() & 0xFF)
                             .arg(it.value());
        g_synDrop.clear();
        drops.sort();
        out += QStringLiteral(" synDrop=") + drops.join(QLatin1Char(','));
    }
    return out;
}

void GatewayDiag::sample(const QString &extra)
{
    if (g_path.isEmpty())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 winMs = qMax<qint64>(1, now - g_lastSampleMs);

    // 空窗口跳过：设备没在用网关时不写任何东西（否则一晚上全是零行，把真正有信息的行冲掉）。
    // 判据只看「有没有帧进出」——泵一直在跳，不能拿它当活动依据。
    const qint64 rxF = d(c.rxFrames, g_prev.rxFrames);
    const qint64 txF = d(c.txFrames, g_prev.txFrames);
    if (rxF == 0 && txF == 0) {
        // 峰值是瞬时量，即使不写也要清，否则会把上一个忙窗口的高水位一直带到下一个忙窗口。
        c.txBacklogPeak = 0;
        c.pumpMaxLagMs = 0;
        c.synLatMaxUs = 0;
        g_prev = c;
        g_lastSampleMs = now;
        return;
    }

    const double secs = double(winMs) / 1000.0;
    const auto rate = [secs](qint64 n) { return qint64(double(n) / secs + 0.5); };

    // 单行 key=value，方便 grep/awk，也方便以后直接喂给脚本画图。
    // 顺序按「链路从外到内」：收 → 分流 → 发 → TCP → 背压 → UDP → 泵 → lwIP(extra)。
    QString line;
    line.reserve(512);
    line += QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    line += QStringLiteral(" win=%1ms").arg(winMs);
    const qint64 wakes = d(c.rxWakes, g_prev.rxWakes);
    const qint64 drains = d(c.rxDrains, g_prev.rxDrains);
    line += QStringLiteral(" rx=%1(%2/s) rxKB=%3 wakes=%4 fpw=%5 rxdrop=%6")
                .arg(rxF)
                .arg(rate(rxF))
                .arg(d(c.rxBytes, g_prev.rxBytes) / 1024)
                .arg(wakes)
                // 每次唤醒平均取回多少帧：收环批处理的实际效果。掉到 1 附近 = 退化成逐帧唤醒。
                // ★ 分母**只算事件驱动的唤醒**，泵主动排空(drains)另计——否则这一栏恒为 0。
                .arg(wakes > 0 ? rxF / wakes : 0)
                .arg(d(c.rxKernelDrops, g_prev.rxKernelDrops));
    line += QStringLiteral(" krecv=%1").arg(d(c.rxKernelRecv, g_prev.rxKernelRecv));
    // 排空成本：drains = 泵兜底排空次数；drainUs = 每次排空平均微秒（含缓冲空时的阻塞等待）。
    line += QStringLiteral(" drains=%1 drainUs=%2")
                .arg(drains)
                .arg((wakes + drains) > 0 ? d(c.rxDrainUs, g_prev.rxDrainUs) / (wakes + drains) : 0);
    line += QStringLiteral(" fed=%1 bypLan=%2 bypBcast=%3 nonVictim=%4")
                .arg(d(c.fedLwip, g_prev.fedLwip))
                .arg(d(c.bypassLan, g_prev.bypassLan))
                .arg(d(c.bypassBcast, g_prev.bypassBcast))
                .arg(d(c.dropNonVictim, g_prev.dropNonVictim));
    // 按网卡拆开的那四项 —— **只在真有两张以上网卡登记时才打**（单网卡下它与上面那行逐字相同，
    // 白占宽度）。多网卡时上面那行是合计数，一张卡整段不工作也看不出来，这一段就是为它加的。
    {
        int registered = 0;
        for (const QString &n : GatewayDiag::nicNames) {
            if (!n.isEmpty())
                ++registered;
        }
        if (registered >= 2) {
            for (int i = 0; i < GatewayDiag::kMaxNicSlots; ++i) {
                if (GatewayDiag::nicNames[i].isEmpty())
                    continue;
                const GatewayDiag::NicCounters &n = GatewayDiag::nics[i];
                GatewayDiag::NicCounters &p = g_nicPrev[i];
                line += QStringLiteral(" [%1 fed=%2 bypLan=%3 bypBcast=%4 nonVictim=%5]")
                                .arg(GatewayDiag::nicNames[i])
                                .arg(d(n.fedLwip, p.fedLwip))
                                .arg(d(n.bypassLan, p.bypassLan))
                                .arg(d(n.bypassBcast, p.bypassBcast))
                                .arg(d(n.dropNonVictim, p.dropNonVictim));
            }
        }
        for (int i = 0; i < GatewayDiag::kMaxNicSlots; ++i)
            g_nicPrev[i] = GatewayDiag::nics[i];
    }
    line += QStringLiteral(" tx=%1(%2/s) txKB=%3 defer=%4 txdrop=%5 backlogPeakKB=%6")
                .arg(txF)
                .arg(rate(txF))
                .arg(d(c.txBytes, g_prev.txBytes) / 1024)
                .arg(d(c.txDeferred, g_prev.txDeferred))
                .arg(d(c.txDropped, g_prev.txDropped))
                .arg(c.txBacklogPeak / 1024);
    // ★ 本条是回答「设备路径为什么慢」的核心一栏（理由见 GatewayDiag.h 的 txSendUs）：
    //   usPerTx = 每帧平均花在驱动里的微秒数。它若在 200~400 µs 量级，就坐实了
    //   「每帧一次同步 pcap_sendpacket、驱动等 NDIS 发送完成」= WiFi 无法做 A-MPDU 聚合，
    //   单帧空口时间被完整串行化；批量发之后这个数应当掉一个数量级、fpb 明显 > 1。
    line += QStringLiteral(" txBatch=%1 fpb=%2 usPerTx=%3")
                .arg(d(c.txBatches, g_prev.txBatches))
                .arg(d(c.txBatches, g_prev.txBatches) > 0
                         ? txF / d(c.txBatches, g_prev.txBatches)
                         : 0)
                .arg(txF > 0 ? d(c.txSendUs, g_prev.txSendUs) / txF : 0);
    line += QStringLiteral(" tcpAcc=%1 tcpClose=%2 tcpAbort=%3 socksFail=%4 tcpReap=%5")
                .arg(d(c.tcpAccepted, g_prev.tcpAccepted))
                .arg(d(c.tcpClosed, g_prev.tcpClosed))
                .arg(d(c.tcpAborted, g_prev.tcpAborted))
                .arg(d(c.socksFailed, g_prev.socksFailed))
                .arg(d(c.tcpReaped, g_prev.tcpReaped));
    line += QStringLiteral(" synLatMaxMs=%1 synSlow=%2")
                .arg(c.synLatMaxUs / 1000)
                .arg(d(c.synLatSlow, g_prev.synLatSlow));
    line += QStringLiteral(" synRxIn=%1")
                .arg(d(c.synRxIn, g_prev.synRxIn));
    line += rawSynLine();
    line += QStringLiteral(" synackTx=%1")
                .arg(d(c.synAckTx, g_prev.synAckTx));
    line += QStringLiteral(" dial=%1 estab=%2")
                .arg(d(c.tcpDialed, g_prev.tcpDialed))
                .arg(d(c.tcpEstablished, g_prev.tcpEstablished));
    line += QStringLiteral(" upThrottle=%1 downPause=%2")
                .arg(d(c.upThrottleHits, g_prev.upThrottleHits))
                .arg(d(c.downPauseHits, g_prev.downPauseHits));
    line += QStringLiteral(" udpNew=%1 udpEvict=%2 udpRefuse=%3 dns=%4 dnsNoReply=%5 dnsNoId=%6")
                .arg(d(c.udpFlowsCreated, g_prev.udpFlowsCreated))
                .arg(d(c.udpFlowsEvicted, g_prev.udpFlowsEvicted))
                .arg(d(c.udpFlowsRefused, g_prev.udpFlowsRefused))
                .arg(d(c.dnsHijacked, g_prev.dnsHijacked))
                .arg(d(c.dnsNoReply, g_prev.dnsNoReply))
                .arg(d(c.dnsNoId, g_prev.dnsNoId));
    // ★ 进程内出站的记账（CoastCore）。**只在有活动时才打**，免得把行撑爆：
    //   阶段 1 只是把引擎编进来、没有构造点，这些恒为 0，整段自动省略。
    //   cc=<进程内>/<无路由>/<节点缺>/<协议缺>/<UDP不支持>/<严格拒绝>
    {
        const qint64 cc = d(c.ccInProcess, g_prev.ccInProcess);
        const qint64 f1 = d(c.fbNoRoute, g_prev.fbNoRoute);
        const qint64 f2 = d(c.fbNodeMissing, g_prev.fbNodeMissing);
        const qint64 f3 = d(c.fbProtoMissing, g_prev.fbProtoMissing);
        const qint64 f4 = d(c.fbUdpUnsupported, g_prev.fbUdpUnsupported);
        const qint64 f5 = d(c.ccStrictRefused, g_prev.ccStrictRefused);
        const qint64 fr = d(c.dnsFakeIpResolved, g_prev.dnsFakeIpResolved);
        const qint64 lf = d(c.dnsLocalFake, g_prev.dnsLocalFake);
        const qint64 lw = d(c.dnsLocalForward, g_prev.dnsLocalForward);
        const qint64 ln = d(c.dnsLearned, g_prev.dnsLearned);
        if (cc || f1 || f2 || f3 || f4 || f5 || fr || lf || lw || ln) {
            line += QStringLiteral(" cc=%1/%2/%3/%4/%5/%6 fakeipLearned=%7 fakeipResolved=%8"
                                   " dnsLocal=%9/%10")
                        .arg(cc).arg(f1).arg(f2).arg(f3).arg(f4).arg(f5)
                        .arg(ln).arg(fr).arg(lf).arg(lw);
        }
    }
    line += QStringLiteral(" pump=%1 late=%2 maxLagMs=%3")
                .arg(d(c.pumpTicks, g_prev.pumpTicks))
                .arg(d(c.pumpLateTicks, g_prev.pumpLateTicks))
                .arg(c.pumpMaxLagMs);
    if (!extra.isEmpty())
        line += QLatin1Char(' ') + extra;

    rotateIfNeeded();
    QFile f(g_path);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << line << '\n';
    }

    c.txBacklogPeak = 0; // 瞬时量：下个窗口重新统计高水位
    c.pumpMaxLagMs = 0;
    g_prev = c;
    g_lastSampleMs = now;
}

void GatewayDiag::note(const char *key, const QString &text, int minGapMs)
{
    if (g_path.isEmpty() || !key)
        return;
    static QHash<QByteArray, qint64> lastAt;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 &t = lastAt[QByteArray(key)];
    if (t != 0 && now - t < minGapMs)
        return;
    t = now;

    rotateIfNeeded();
    QFile f(g_path);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
           << " !! " << text << '\n';
    }
}

void GatewayDiag::flush(const QString &extra, const char *reason)
{
    if (g_path.isEmpty())
        return;
    // 先把最后一个窗口写掉（sample 自己会处理空窗口），再补一条停机标记，
    // 这样日志里「网关是什么时候停的」一目了然，不用靠时间戳断层去猜。
    sample(extra);
    rotateIfNeeded();
    QFile f(g_path);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
           << " --- gateway " << (reason ? reason : "stop") << " ---\n";
    }
}
