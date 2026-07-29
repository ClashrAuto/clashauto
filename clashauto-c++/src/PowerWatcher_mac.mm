// PowerWatcher 的 macOS 后端：NSWorkspace 的睡眠/唤醒通知。
//
// NSWorkspaceWillSleepNotification 是**同步**投递的 —— 系统会等所有观察者的回调返回后才真正
// 挂起（有几秒预算），所以在回调里直接把「撤劫持 + 发还原 ARP」做完是安全的，这正是我们要的。
// 唤醒用 NSWorkspaceDidWakeNotification。两者都只在真正的系统睡眠/唤醒时发（不含屏幕休眠）。
#import <AppKit/AppKit.h>

#include "PowerWatcher.h"

namespace {
id g_sleepObserver = nil;
id g_wakeObserver = nil;
} // namespace

void coastPowerWatchStart(PowerWatcher *w)
{
    if (!w || g_sleepObserver)
        return;
    NSNotificationCenter *nc = [[NSWorkspace sharedWorkspace] notificationCenter];
    g_sleepObserver = [nc addObserverForName:NSWorkspaceWillSleepNotification
                                      object:nil
                                       queue:nil
                                  usingBlock:^(NSNotification *) {
                                    emit w->aboutToSleep();
                                  }];
    g_wakeObserver = [nc addObserverForName:NSWorkspaceDidWakeNotification
                                     object:nil
                                      queue:nil
                                 usingBlock:^(NSNotification *) {
                                   emit w->wokeUp();
                                 }];
}

void coastPowerWatchStop()
{
    NSNotificationCenter *nc = [[NSWorkspace sharedWorkspace] notificationCenter];
    if (g_sleepObserver) {
        [nc removeObserver:g_sleepObserver];
        g_sleepObserver = nil;
    }
    if (g_wakeObserver) {
        [nc removeObserver:g_wakeObserver];
        g_wakeObserver = nil;
    }
}
