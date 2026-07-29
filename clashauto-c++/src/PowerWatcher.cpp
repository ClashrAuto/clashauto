#include "PowerWatcher.h"

#include <QCoreApplication>

#include <cstdio>
#include <cstring>

#if defined(Q_OS_WIN)
#include <QAbstractNativeEventFilter>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(COAST_HAVE_DBUS)
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <unistd.h>
#endif

#if defined(Q_OS_UNIX)
#include <QSocketNotifier>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#endif

// macOS 的实现（NSWorkspace 通知）在 PowerWatcher_mac.mm 里，导出下面这两个 C 函数。
#if defined(Q_OS_MACOS)
void coastPowerWatchStart(PowerWatcher *w);
void coastPowerWatchStop();
#endif

namespace {
void logPower(const char *what)
{
    // 一行日志（不吃开关）：现场排查「合盖之后设备为什么断网」时，这就是唯一凭据。ASCII，
    // 因为 Windows 控制台/重定向文件是 GBK，中文会变成乱码。
    std::fprintf(stderr, "[POWER] %s\n", what);
    std::fflush(stderr);
}
} // namespace

#if defined(Q_OS_WIN)
namespace {
// 电源事件在 Windows 上是发给顶层窗口的 WM_POWERBROADCAST。用原生事件过滤器接，和会话结束
// （WM_QUERYENDSESSION，见 main_qml.cpp）同一个套路。
class WinPowerFilter final : public QAbstractNativeEventFilter
{
public:
    explicit WinPowerFilter(PowerWatcher *w) : m_w(w) {}
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override
    {
        const MSG *msg = static_cast<const MSG *>(message);
        if (!msg || msg->message != WM_POWERBROADCAST)
            return false;
        switch (msg->wParam) {
        case PBT_APMSUSPEND: // 即将挂起（系统给了执行窗口，这里同步做完还原）
            logPower("PBT_APMSUSPEND -> sleep");
            emit m_w->aboutToSleep();
            break;
        case PBT_APMRESUMESUSPEND:  // 用户唤醒
        case PBT_APMRESUMEAUTOMATIC: // 系统自动唤醒（定时任务等）
            logPower("PBT_APMRESUME -> wake");
            emit m_w->wokeUp();
            break;
        default:
            break;
        }
        return false; // 不拦截，交回 Qt 默认处理
    }

private:
    PowerWatcher *m_w;
};
} // namespace
#endif

class PowerWatcher::Impl
{
public:
#if defined(Q_OS_WIN)
    WinPowerFilter *filter = nullptr;
#endif
#if defined(COAST_HAVE_DBUS)
    QDBusInterface *login1 = nullptr;
    QDBusUnixFileDescriptor inhibitFd; // delay 抑制锁：拿着它，systemd 才会等我们处理完再挂起
#endif
};

#if defined(Q_OS_UNIX)
// 测试钩子的自管道（信号处理器里只能 write 一个字节）。sleep=1、wake=2。
static int s_powerSigFd[2] = {-1, -1};
static void powerTestSignalHandler(int sig)
{
    const char c = (sig == SIGUSR1) ? 1 : 2;
    ssize_t r = ::write(s_powerSigFd[0], &c, 1);
    (void)r;
}
#endif

PowerWatcher::PowerWatcher(QObject *parent) : QObject(parent), d(new Impl)
{
#if defined(Q_OS_WIN)
    d->filter = new WinPowerFilter(this);
    if (QCoreApplication::instance())
        QCoreApplication::instance()->installNativeEventFilter(d->filter);
#endif

#if defined(COAST_HAVE_DBUS)
    // systemd-logind：PrepareForSleep(true) 在挂起前发、(false) 在恢复后发。
    // 只订阅信号是不够的 —— 没有抑制锁的话，systemd 不保证我们能在挂起前被调度到。
    // 所以再取一把 "delay" 锁（默认最多 5s，我们只需要发几帧 ARP），处理完就释放让系统继续睡。
    QDBusConnection sys = QDBusConnection::systemBus();
    if (sys.isConnected()) {
        sys.connect(QStringLiteral("org.freedesktop.login1"),
                    QStringLiteral("/org/freedesktop/login1"),
                    QStringLiteral("org.freedesktop.login1.Manager"),
                    QStringLiteral("PrepareForSleep"), this, SLOT(onPrepareForSleep(bool)));
        d->login1 = new QDBusInterface(QStringLiteral("org.freedesktop.login1"),
                                       QStringLiteral("/org/freedesktop/login1"),
                                       QStringLiteral("org.freedesktop.login1.Manager"), sys, this);
        takeInhibitLock();
    }
#endif

#if defined(Q_OS_MACOS)
    coastPowerWatchStart(this);
#endif

#if defined(Q_OS_UNIX)
    if (qEnvironmentVariableIsSet("COAST_POWER_TESTHOOK")
        && ::socketpair(AF_UNIX, SOCK_STREAM, 0, s_powerSigFd) == 0) {
        auto *sn = new QSocketNotifier(s_powerSigFd[1], QSocketNotifier::Read, this);
        connect(sn, &QSocketNotifier::activated, this, [this] {
            char c = 0;
            if (::read(s_powerSigFd[1], &c, 1) != 1)
                return;
            if (c == 1) {
                logPower("SIGUSR1 (testhook) -> sleep");
                emit aboutToSleep();
            } else {
                logPower("SIGUSR2 (testhook) -> wake");
                emit wokeUp();
            }
        });
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = &powerTestSignalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        ::sigaction(SIGUSR1, &sa, nullptr);
        ::sigaction(SIGUSR2, &sa, nullptr);
        logPower("test hook armed (SIGUSR1=sleep, SIGUSR2=wake)");
    }
#endif
}

PowerWatcher::~PowerWatcher()
{
#if defined(Q_OS_WIN)
    if (d->filter && QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(d->filter);
    delete d->filter;
#endif
#if defined(Q_OS_MACOS)
    coastPowerWatchStop();
#endif
    delete d;
}

void PowerWatcher::takeInhibitLock()
{
#if defined(COAST_HAVE_DBUS)
    if (!d->login1 || !d->login1->isValid())
        return;
    QDBusReply<QDBusUnixFileDescriptor> reply = d->login1->call(
        QStringLiteral("Inhibit"), QStringLiteral("sleep"), QStringLiteral("Coast"),
        QStringLiteral("restore proxied devices' ARP before sleep"), QStringLiteral("delay"));
    if (reply.isValid())
        d->inhibitFd = reply.value();
#endif
}

void PowerWatcher::releaseInhibitLock()
{
#if defined(COAST_HAVE_DBUS)
    d->inhibitFd = QDBusUnixFileDescriptor(); // 关掉 fd = 释放锁，systemd 随即继续挂起
#endif
}

void PowerWatcher::onPrepareForSleep(bool going)
{
#if !defined(COAST_HAVE_DBUS)
    Q_UNUSED(going);
#else
    if (going) {
        logPower("logind PrepareForSleep(true) -> sleep");
        emit aboutToSleep(); // 同步：抑制锁还拿着，做完再放
        releaseInhibitLock();
    } else {
        logPower("logind PrepareForSleep(false) -> wake");
        takeInhibitLock(); // 为下一次睡眠重新占位
        emit wokeUp();
    }
#endif
}
