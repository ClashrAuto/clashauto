// 特权 helper 守护进程（以 root 运行，由 SMAppService/launchd 按需拉起）。
//   - getVersion：版本自检
//   - setSystemProxy：以 root 用 SCPreferences 设/清系统代理（root 提交网络配置无需授权）
//   - startCore/stopCore：以 root 起停 mihomo（TUN 靠 root mihomo 自建 utun）
#import <Foundation/Foundation.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import "HelperProtocol.h"
#include "Version.h" // 由 CMake configure_file 生成：#define APP_VERSION "x.y.z"（与应用同版本号）

// ---- 以 root 用 SCPreferences 在所有已启用网络服务上设/清 HTTP/HTTPS/SOCKS 代理 ----
// 与应用侧 Option B 的 macApplyProxies 同逻辑，但去掉 AuthorizationRef：root 直接提交即可。
static void cfDictSetInt(CFMutableDictionaryRef d, CFStringRef key, int v)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}

static CFMutableDictionaryRef copyProxiesDict(SCPreferencesRef prefs, CFStringRef serviceID)
{
    CFStringRef path = CFStringCreateWithFormat(kCFAllocatorDefault, nullptr,
                                                CFSTR("/NetworkServices/%@/Proxies"), serviceID);
    CFDictionaryRef existing = (CFDictionaryRef)SCPreferencesPathGetValue(prefs, path); // Get 规则，不释放
    CFRelease(path);
    if (existing && CFGetTypeID(existing) == CFDictionaryGetTypeID()) {
        return CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, existing);
    }
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                     &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static BOOL applyProxies(BOOL enable, NSString *host, int port, NSArray<NSString *> *bypass, NSString **errOut)
{
    SCPreferencesRef prefs = SCPreferencesCreate(kCFAllocatorDefault, CFSTR("ClashAutoHelper"), nullptr);
    if (!prefs) { if (errOut) *errOut = @"SCPreferencesCreate 返回空"; return NO; }
    if (!SCPreferencesLock(prefs, true)) {
        if (errOut) *errOut = @"SCPreferencesLock 失败";
        CFRelease(prefs);
        return NO;
    }

    BOOL committed = NO;
    CFArrayRef services = SCNetworkServiceCopyAll(prefs);
    if (services) {
        CFMutableArrayRef exceptions = nullptr;
        if (enable) {
            exceptions = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
            for (NSString *b in bypass) CFArrayAppendValue(exceptions, (__bridge CFStringRef)b);
        }
        CFStringRef cfHost = (__bridge CFStringRef)host;
        const CFIndex count = CFArrayGetCount(services);
        for (CFIndex i = 0; i < count; ++i) {
            SCNetworkServiceRef svc = (SCNetworkServiceRef)CFArrayGetValueAtIndex(services, i);
            if (!SCNetworkServiceGetEnabled(svc)) continue;
            CFStringRef sid = SCNetworkServiceGetServiceID(svc);
            if (!sid) continue;

            CFMutableDictionaryRef proxies = copyProxiesDict(prefs, sid);
            if (enable) {
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPEnable, 1);
                CFDictionarySetValue(proxies, kSCPropNetProxiesHTTPProxy, cfHost);
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPPort, port);
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPSEnable, 1);
                CFDictionarySetValue(proxies, kSCPropNetProxiesHTTPSProxy, cfHost);
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPSPort, port);
                cfDictSetInt(proxies, kSCPropNetProxiesSOCKSEnable, 1);
                CFDictionarySetValue(proxies, kSCPropNetProxiesSOCKSProxy, cfHost);
                cfDictSetInt(proxies, kSCPropNetProxiesSOCKSPort, port);
                if (exceptions) CFDictionarySetValue(proxies, kSCPropNetProxiesExceptionsList, exceptions);
            } else {
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPEnable, 0);
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPSEnable, 0);
                cfDictSetInt(proxies, kSCPropNetProxiesSOCKSEnable, 0);
            }
            CFStringRef path = CFStringCreateWithFormat(kCFAllocatorDefault, nullptr,
                                                        CFSTR("/NetworkServices/%@/Proxies"), sid);
            SCPreferencesPathSetValue(prefs, path, proxies);
            CFRelease(path);
            CFRelease(proxies);
        }
        if (exceptions) CFRelease(exceptions);
        CFRelease(services);

        committed = SCPreferencesCommitChanges(prefs) && SCPreferencesApplyChanges(prefs);
        if (!committed && errOut) *errOut = @"SCPreferencesCommit/Apply 失败";
    } else if (errOut) {
        *errOut = @"SCNetworkServiceCopyAll 返回空";
    }

    SCPreferencesUnlock(prefs);
    CFRelease(prefs);
    return committed;
}

@interface CAHelperService : NSObject <NSXPCListenerDelegate, CAHelperProtocol>
@end

@implementation CAHelperService {
    NSTask *_coreTask; // 当前由 helper 启动的 mihomo（root）
}

// launchd 把每个入站连接交到这里。校验来连者签名后才接受，杜绝任意进程驱动 root。
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)newConnection
{
    if (@available(macOS 13.0, *)) {
        [newConnection setCodeSigningRequirement:@CA_CLIENT_CODE_REQUIREMENT];
    } else {
        return NO; // 本 helper 只面向 macOS 13+（SMAppService daemon）
    }
    newConnection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(CAHelperProtocol)];
    newConnection.exportedObject = self;
    [newConnection resume];
    return YES;
}

#pragma mark - CAHelperProtocol

- (void)getVersionWithReply:(void (^)(NSString *))reply
{
    reply(@APP_VERSION);
}

- (void)setSystemProxyEnabled:(BOOL)enable
                          host:(NSString *)host
                          port:(int)port
                bypassCommaSep:(NSString *)bypassCommaSep
                     withReply:(void (^)(BOOL, NSString *))reply
{
    NSArray<NSString *> *bypass = bypassCommaSep.length
        ? [bypassCommaSep componentsSeparatedByString:@","]
        : @[];
    NSString *err = nil;
    const BOOL ok = applyProxies(enable, host, port, bypass, &err);
    reply(ok, err ?: @"");
}

- (void)startCoreExec:(NSString *)execPath
               config:(NSString *)configPath
              userDir:(NSString *)userDir
            withReply:(void (^)(BOOL, NSString *))reply
{
    // 先停旧核心
    if (_coreTask && _coreTask.isRunning) {
        [_coreTask terminate];
        _coreTask = nil;
    }

    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *logDir = [userDir stringByAppendingPathComponent:@"logs"];
    [fm createDirectoryAtPath:logDir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *logPath = [logDir stringByAppendingPathComponent:@"core.log"];
    // 每次启动重建 core.log（应用侧从头 tail）；创建为 0644，普通用户可读
    [fm createFileAtPath:logPath contents:[NSData data]
              attributes:@{NSFilePosixPermissions: @0644}];
    NSFileHandle *logFh = [NSFileHandle fileHandleForWritingAtPath:logPath];
    if (!logFh) { reply(NO, @"无法打开 core.log 写句柄"); return; }

    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:execPath];
    task.arguments = @[@"-d", userDir, @"-f", configPath];
    task.currentDirectoryURL = [NSURL fileURLWithPath:[execPath stringByDeletingLastPathComponent]];
    task.standardOutput = logFh;
    task.standardError = logFh;

    NSError *e = nil;
    if (![task launchAndReturnError:&e]) {
        reply(NO, e.localizedDescription ?: @"launchAndReturnError 失败");
        return;
    }
    _coreTask = task;
    reply(YES, @"");
}

- (void)stopCoreWithReply:(void (^)(BOOL, NSString *))reply
{
    if (_coreTask && _coreTask.isRunning) {
        [_coreTask terminate];
    }
    _coreTask = nil;
    reply(YES, @"");
}

@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        CAHelperService *service = [[CAHelperService alloc] init];
        NSXPCListener *listener = [[NSXPCListener alloc] initWithMachServiceName:@CA_HELPER_MACH_SERVICE];
        listener.delegate = service;
        [listener resume];
        // launchd daemon：跑主 runloop 等待连接（不返回）
        [[NSRunLoop currentRunLoop] run];
    }
    return 0;
}
