#pragma once

// 系统电源事件（睡眠 / 唤醒）监视器。
//
// 为什么需要：透明网关把被代理设备的默认网关指到了本机，设备的每个包都要本机转发。本机一睡，
// 转发就停了，但设备的 ARP 还钉在本机 MAC 上 —— 于是**合盖睡眠 = 那台设备直接断网**，直到它
// 自己的邻居缓存老化。关机路径早有还原（aboutToQuit / Windows 的 WM_QUERYENDSESSION），睡眠这条
// 一直是空的，而笔记本上它比关机高频得多。
//
// 语义：
//   aboutToSleep() —— 系统即将睡眠，且**还给了我们执行窗口**（Windows 的 PBT_APMSUSPEND、
//     macOS 的 NSWorkspaceWillSleepNotification、Linux logind 的 PrepareForSleep(true)；
//     Linux 上我们还持有一把 delay 抑制锁，确保这个槽真的能在挂起前跑完）。
//     接收方应当同步做完还原动作（撤劫持 + 发还原 ARP），别用队列连接。
//   wokeUp() —— 系统已恢复。接收方重新扫描拓扑并把劫持补回去（IP 可能已经变了）。
//
// 测试钩子：COAST_POWER_TESTHOOK=1 时（仅 POSIX）额外接 SIGUSR1/SIGUSR2 → aboutToSleep/wokeUp，
// 用来在不真的挂起机器的前提下验证这条链路（真机测试台上不能拿 systemctl suspend 冒险）。
#include <QObject>

class PowerWatcher : public QObject
{
    Q_OBJECT
public:
    explicit PowerWatcher(QObject *parent = nullptr);
    ~PowerWatcher() override;

signals:
    void aboutToSleep();
    void wokeUp();

private slots:
    // Linux/logind 的 PrepareForSleep(bool)。非 DBus 平台上是个空实现（moc 仍会引用它）。
    void onPrepareForSleep(bool going);

private:
    void takeInhibitLock();    // logind 的 "delay" 抑制锁（其它平台空实现）
    void releaseInhibitLock();

    class Impl;
    Impl *d = nullptr;
};
