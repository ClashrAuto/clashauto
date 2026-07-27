// GatewayDiag 的落盘实现。设计与取舍见 GatewayDiag.h。
//
// 这个 TU **所有平台都编**（main_qml.cpp 要调 setLogDir，而它是跨平台的），但只有 POSIX 上
// 真的有网关在往计数器里写。没有网关的平台上它就是一个从不被 sample() 的空壳。
#include "GatewayDiag.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
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
    line += QStringLiteral(" rx=%1(%2/s) rxKB=%3 wakes=%4 fpw=%5 rxdrop=%6")
                .arg(rxF)
                .arg(rate(rxF))
                .arg(d(c.rxBytes, g_prev.rxBytes) / 1024)
                .arg(d(c.rxWakes, g_prev.rxWakes))
                // 每次唤醒平均取回多少帧：收环批处理的实际效果。掉到 1 附近 = 退化成逐帧唤醒。
                .arg(d(c.rxWakes, g_prev.rxWakes) > 0
                         ? rxF / d(c.rxWakes, g_prev.rxWakes)
                         : 0)
                .arg(d(c.rxKernelDrops, g_prev.rxKernelDrops));
    line += QStringLiteral(" fed=%1 bypLan=%2 bypBcast=%3 nonVictim=%4")
                .arg(d(c.fedLwip, g_prev.fedLwip))
                .arg(d(c.bypassLan, g_prev.bypassLan))
                .arg(d(c.bypassBcast, g_prev.bypassBcast))
                .arg(d(c.dropNonVictim, g_prev.dropNonVictim));
    line += QStringLiteral(" tx=%1(%2/s) txKB=%3 defer=%4 txdrop=%5 backlogPeakKB=%6")
                .arg(txF)
                .arg(rate(txF))
                .arg(d(c.txBytes, g_prev.txBytes) / 1024)
                .arg(d(c.txDeferred, g_prev.txDeferred))
                .arg(d(c.txDropped, g_prev.txDropped))
                .arg(c.txBacklogPeak / 1024);
    line += QStringLiteral(" tcpAcc=%1 tcpClose=%2 tcpAbort=%3 socksFail=%4")
                .arg(d(c.tcpAccepted, g_prev.tcpAccepted))
                .arg(d(c.tcpClosed, g_prev.tcpClosed))
                .arg(d(c.tcpAborted, g_prev.tcpAborted))
                .arg(d(c.socksFailed, g_prev.socksFailed));
    line += QStringLiteral(" upThrottle=%1 downPause=%2")
                .arg(d(c.upThrottleHits, g_prev.upThrottleHits))
                .arg(d(c.downPauseHits, g_prev.downPauseHits));
    line += QStringLiteral(" udpNew=%1 udpEvict=%2 dns=%3 dnsNoReply=%4")
                .arg(d(c.udpFlowsCreated, g_prev.udpFlowsCreated))
                .arg(d(c.udpFlowsEvicted, g_prev.udpFlowsEvicted))
                .arg(d(c.dnsHijacked, g_prev.dnsHijacked))
                .arg(d(c.dnsNoReply, g_prev.dnsNoReply));
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
