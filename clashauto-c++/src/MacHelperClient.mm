#include "MacHelperClient.h"

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>
#import "../helper/HelperProtocol.h"

// daemon 的 launchd plist 文件名（位于 .app/Contents/Library/LaunchDaemons/ 下）
static NSString *const kPlistName = @"com.yuehongsun.auto.helper.plist";

namespace MacHelper {

static RegStatus mapStatus(SMAppServiceStatus s)
{
    switch (s) {
        case SMAppServiceStatusEnabled:          return RegStatus::Enabled;
        case SMAppServiceStatusRequiresApproval: return RegStatus::RequiresApproval;
        case SMAppServiceStatusNotRegistered:    return RegStatus::NotRegistered;
        case SMAppServiceStatusNotFound:         return RegStatus::NotFound;
        default:                                 return RegStatus::Unknown;
    }
}

RegStatus status()
{
    if (@available(macOS 13.0, *)) {
        SMAppService *svc = [SMAppService daemonServiceWithPlistName:kPlistName];
        return mapStatus(svc.status);
    }
    return RegStatus::Unknown;
}

RegStatus registerDaemon(QString *err)
{
    if (@available(macOS 13.0, *)) {
        SMAppService *svc = [SMAppService daemonServiceWithPlistName:kPlistName];
        NSError *e = nil;
        const BOOL ok = [svc registerAndReturnError:&e];
        if (!ok && err) {
            *err = e ? QString::fromNSString(e.localizedDescription)
                     : QStringLiteral("registerAndReturnError 失败（未知）");
        }
        return mapStatus(svc.status);
    }
    if (err) *err = QStringLiteral("需要 macOS 13+");
    return RegStatus::Unknown;
}

bool unregisterDaemon(QString *err)
{
    if (@available(macOS 13.0, *)) {
        SMAppService *svc = [SMAppService daemonServiceWithPlistName:kPlistName];
        NSError *e = nil;
        const BOOL ok = [svc unregisterAndReturnError:&e];
        if (!ok && err) {
            *err = e ? QString::fromNSString(e.localizedDescription)
                     : QStringLiteral("unregisterAndReturnError 失败（未知）");
        }
        return ok;
    }
    if (err) *err = QStringLiteral("需要 macOS 13+");
    return false;
}

void openLoginItemsSettings()
{
    if (@available(macOS 13.0, *)) {
        [SMAppService openSystemSettingsLoginItems];
    }
}

QString pingVersion(QString *err)
{
    NSXPCConnection *conn =
        [[NSXPCConnection alloc] initWithMachServiceName:@CA_HELPER_MACH_SERVICE
                                                 options:NSXPCConnectionPrivileged];
    conn.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(CAHelperProtocol)];
    [conn resume];

    __block NSString *result = nil;
    __block NSString *failure = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    id proxy = [conn remoteObjectProxyWithErrorHandler:^(NSError *e) {
        failure = e.localizedDescription;
        dispatch_semaphore_signal(sem);
    }];
    [proxy getVersionWithReply:^(NSString *v) {
        result = v;
        dispatch_semaphore_signal(sem);
    }];

    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)));
    [conn invalidate];

    if (result) return QString::fromNSString(result);
    if (err) *err = failure ? QString::fromNSString(failure) : QStringLiteral("超时/无应答");
    return QString();
}

bool isReady()
{
    if (status() != RegStatus::Enabled) return false;
    // 刚注册/批准的 daemon 由 launchd 按需拉起：首个 XPC 连接会触发 spawn，可能比单次 ping 的
    // 5s 超时更慢。只 ping 一次就判 false，会让「已安装、Enabled」的 helper 被误判不可用，于是
    // 代理/核心静默退回非 root。这里重试几次给冷启动留出时间，任一次拿到版本即视为就绪。
    for (int i = 0; i < 3; ++i) {
        QString err;
        if (!pingVersion(&err).isEmpty()) return true;
    }
    return false;
}

// 通用：建连接、在其上调用一个「reply(BOOL ok, NSString *err)」的远程方法，同步等结果。
static bool callBoolReply(QString *err,
                          void (^invoke)(id<CAHelperProtocol> proxy, void (^reply)(BOOL, NSString *)))
{
    NSXPCConnection *conn =
        [[NSXPCConnection alloc] initWithMachServiceName:@CA_HELPER_MACH_SERVICE
                                                 options:NSXPCConnectionPrivileged];
    conn.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(CAHelperProtocol)];
    [conn resume];

    __block bool ok = false;
    __block NSString *failure = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    id proxy = [conn remoteObjectProxyWithErrorHandler:^(NSError *e) {
        failure = e.localizedDescription;
        dispatch_semaphore_signal(sem);
    }];
    invoke((id<CAHelperProtocol>)proxy, ^(BOOL success, NSString *emsg) {
        ok = success;
        if (!success) failure = emsg.length ? emsg : @"helper 返回失败";
        dispatch_semaphore_signal(sem);
    });
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(15 * NSEC_PER_SEC)));
    [conn invalidate];

    if (!ok && err) *err = failure ? QString::fromNSString(failure) : QStringLiteral("超时/无应答");
    return ok;
}

bool setSystemProxy(bool enable, const QString &host, int port, const QStringList &bypass, QString *err)
{
    NSString *h = host.toNSString();
    NSString *b = bypass.join(QStringLiteral(",")).toNSString();
    return callBoolReply(err, ^(id<CAHelperProtocol> proxy, void (^reply)(BOOL, NSString *)) {
        [proxy setSystemProxyEnabled:enable host:h port:port bypassCommaSep:b withReply:reply];
    });
}

bool startCore(const QString &execPath, const QString &configPath, const QString &userDir, QString *err)
{
    NSString *e = execPath.toNSString();
    NSString *c = configPath.toNSString();
    NSString *u = userDir.toNSString();
    return callBoolReply(err, ^(id<CAHelperProtocol> proxy, void (^reply)(BOOL, NSString *)) {
        [proxy startCoreExec:e config:c userDir:u withReply:reply];
    });
}

bool stopCore(QString *err)
{
    return callBoolReply(err, ^(id<CAHelperProtocol> proxy, void (^reply)(BOOL, NSString *)) {
        [proxy stopCoreWithReply:reply];
    });
}

} // namespace MacHelper
