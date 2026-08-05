// 特权 helper 守护进程（以 root 运行，由 SMAppService/launchd 按需拉起）。
//   - getVersion：版本自检
//   - setSystemProxy：以 root 用 SCPreferences 设/清系统代理（root 提交网络配置无需授权）
//   - startCore/stopCore：以 root 起停 mihomo（TUN 靠 root mihomo 自建 utun）
#import <Foundation/Foundation.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import "HelperProtocol.h"
#include "Version.h" // 由 CMake configure_file 生成：#define APP_VERSION "x.y.z"（与应用同版本号）

#include <errno.h>
#include <fcntl.h>
#include <net/bpf.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// ---- 以 root 用 SCPreferences 在所有已启用网络服务上设/清 HTTP/HTTPS/SOCKS 代理 ----
// 与应用侧 Option B 的 macApplyProxies 同逻辑，但去掉 AuthorizationRef：root 直接提交即可。
// ── pf 数据面用到的常量与小工具 ──────────────────────────────────────────────
// anchor **必须挂在 com.apple/ 下**：macOS 默认 /etc/pf.conf 只引用了 `com.apple/*` 这一组
// 挂载点，顶层 anchor 装了也永远不会被求值（pfctl 全程不报错）。详见 src/net/PfRules.cpp。
#define kPfAnchor "com.apple/coast"
// forwarding 原值存档。放 /var/run：root 可写、重启即清（重启后本来也不需要还原）。
static NSString *const kPfSysctlArchive = @"/var/run/coast-pf-fwd";

// 跑一个外部命令，合并 stdout/stderr 到 *outStr（可传 NULL），返回退出码；失败返回 -1。
static int runTool(NSString *path, NSArray<NSString *> *args, NSString **outStr)
{
    NSTask *t = [[NSTask alloc] init];
    t.executableURL = [NSURL fileURLWithPath:path];
    t.arguments = args;
    NSPipe *pipe = [NSPipe pipe];
    t.standardOutput = pipe;
    t.standardError = pipe;
    NSError *e = nil;
    if (![t launchAndReturnError:&e]) {
        if (outStr) *outStr = e.localizedDescription ?: @"启动失败";
        return -1;
    }
    // 先读干净再 wait：管道缓冲满时子进程会阻塞在写上，先 wait 会死锁。
    NSData *d = [pipe.fileHandleForReading readDataToEndOfFile];
    [t waitUntilExit];
    if (outStr)
        *outStr = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
    return t.terminationStatus;
}

static NSString *sysctlReadStr(NSString *key)
{
    NSString *out = nil;
    if (runTool(@"/usr/sbin/sysctl", @[@"-n", key], &out) != 0) return nil;
    return [out stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static void sysctlWriteStr(NSString *key, NSString *value)
{
    runTool(@"/usr/sbin/sysctl", @[@"-w", [NSString stringWithFormat:@"%@=%@", key, value]], NULL);
}

// 网卡名白名单：字母数字 + . : -。拼进 pf 规则文件前必须过一遍（helper 是 root）。
static BOOL pfNameIsSafe(NSString *s)
{
    static NSCharacterSet *bad = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        bad = [[NSCharacterSet characterSetWithCharactersInString:
                    @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-"] invertedSet];
    });
    return [s rangeOfCharacterFromSet:bad].location == NSNotFound;
}

// 严格的点分四段 IPv4 校验（不接受前导零以外的任何花样，也不接受主机名）。
static BOOL ipv4IsSafe(NSString *s)
{
    NSArray<NSString *> *parts = [s componentsSeparatedByString:@"."];
    if (parts.count != 4) return NO;
    for (NSString *p in parts) {
        if (!p.length || p.length > 3) return NO;
        for (NSUInteger i = 0; i < p.length; ++i) {
            const unichar c = [p characterAtIndex:i];
            if (c < '0' || c > '9') return NO;
        }
        if (p.intValue > 255) return NO;
    }
    return YES;
}

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
    SCPreferencesRef prefs = SCPreferencesCreate(kCFAllocatorDefault, CFSTR("CoastHelper"), nullptr);
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
    NSXPCInterface *iface = [NSXPCInterface interfaceWithProtocol:@protocol(CAHelperProtocol)];
    // openBpf 的 reply 参数含 NSFileHandle（fd 传递），需在接口上显式允许该类。
    [iface setClasses:[NSSet setWithObject:[NSFileHandle class]]
         forSelector:@selector(openBpfForInterface:withReply:)
         argumentIndex:0
         ofReply:YES];
    newConnection.exportedInterface = iface;
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

- (void)openBpfForInterface:(NSString *)ifname
                  withReply:(void (^)(NSFileHandle *, NSString *))reply
{
    // 逐个试 /dev/bpf0..255（root 权限）。
    int fd = -1;
    for (int i = 0; i < 256 && fd < 0; ++i) {
        char dev[16];
        snprintf(dev, sizeof(dev), "/dev/bpf%d", i);
        fd = open(dev, O_RDWR);
        if (fd < 0 && errno != EBUSY)
            break;
    }
    if (fd < 0) {
        reply(nil, [NSString stringWithFormat:@"open /dev/bpf 失败: %s", strerror(errno)]);
        return;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname.UTF8String ?: "", IFNAMSIZ - 1);
    if (ioctl(fd, BIOCSETIF, &ifr) < 0) {
        NSString *e = [NSString stringWithFormat:@"BIOCSETIF(%@) 失败: %s", ifname, strerror(errno)];
        close(fd);
        reply(nil, e);
        return;
    }
    u_int on = 1, off = 0;
    ioctl(fd, BIOCIMMEDIATE, &on);  // 立即返回
    ioctl(fd, BIOCPROMISC, &on);    // 混杂：收目的非本机 MAC 的帧
    ioctl(fd, BIOCSHDRCMPLT, &on);  // 写时提供完整链路头
    ioctl(fd, BIOCSSEESENT, &off);  // 不回显自己发出的帧
    // 传 fd 给应用；closeOnDealloc:YES —— reply 序列化(dup)后本 fh 释放即关闭 helper 侧 fd。
    NSFileHandle *fh = [[NSFileHandle alloc] initWithFileDescriptor:fd closeOnDealloc:YES];
    reply(fh, nil);
}

#pragma mark - pf 数据面（透明网关的 rdr 重定向）

- (void)pfInstallRedirPort:(int)redirPort
                   dnsPort:(int)dnsPort
           ifnamesCommaSep:(NSString *)ifnamesCommaSep
                 withReply:(void (^)(BOOL, NSString *))reply
{
    if (redirPort <= 0 || redirPort > 65535) { reply(NO, @"redirPort 非法"); return; }
    if (dnsPort < 0 || dnsPort > 65535) { reply(NO, @"dnsPort 非法"); return; }

    // 网卡名要拼进规则文件，先做字符集校验（只允许字母数字和 .:-）。helper 是 root，
    // 放任任意字符串进 pf 规则等于让客户端间接改写整台机器的包过滤。
    // 条目形式：`en0` 或 `en0:7897`。带端口的那种表示「这张卡 rdr 到它自己那个 redir 入站」——
    // 出口网卡在核心里是 listener 的属性，同时接多条上行时每张卡各有一个入站（见应用侧
    // PfRules.h 的 NicPort）。**按最后一个冒号拆**，尾巴不是合法端口就把整串当网卡名，
    // 这样老形式（纯网卡名）逐字节等价，新旧客户端都能应付。
    NSMutableArray<NSString *> *ifs = [NSMutableArray array];
    NSMutableArray<NSNumber *> *ports = [NSMutableArray array];
    for (NSString *raw in [ifnamesCommaSep componentsSeparatedByString:@","]) {
        NSString *s = [raw stringByTrimmingCharactersInSet:
                                [NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (!s.length) continue;
        int perNic = 0;
        const NSRange colon = [s rangeOfString:@":" options:NSBackwardsSearch];
        if (colon.location != NSNotFound && colon.location + 1 < s.length) {
            NSString *tail = [s substringFromIndex:colon.location + 1];
            NSCharacterSet *nonDigit = [[NSCharacterSet decimalDigitCharacterSet] invertedSet];
            if ([tail rangeOfCharacterFromSet:nonDigit].location == NSNotFound) {
                const int p = tail.intValue;
                if (p > 0 && p <= 65535) {
                    perNic = p;
                    s = [s substringToIndex:colon.location];
                }
            }
        }
        // 网卡名照旧做字符集校验：helper 是 root，放任任意字符串进 pf 规则等于让客户端
        // 间接改写整台机器的包过滤。
        if (!s.length || s.length > 24 || !pfNameIsSafe(s)) {
            reply(NO, [NSString stringWithFormat:@"网卡名非法：%@", s]);
            return;
        }
        [ifs addObject:s];
        [ports addObject:@(perNic)];
    }
    if (!ifs.count) { [ifs addObject:@"en0"]; [ports addObject:@0]; }

    [self pfTeardown]; // 上次可能被 kill -9 打死，先清干净

    // 打开转发；原值存档，SIGKILL 走不到 remove 时靠它还原。
    NSString *oldFwd = sysctlReadStr(@"net.inet.ip.forwarding");
    [[NSFileManager defaultManager] createFileAtPath:kPfSysctlArchive
                                            contents:[(oldFwd ?: @"0") dataUsingEncoding:NSUTF8StringEncoding]
                                          attributes:@{NSFilePosixPermissions: @0600}];
    sysctlWriteStr(@"net.inet.ip.forwarding", @"1");

    // DNS 那条必须排在通配 rdr 之前，否则 53 会被一起截走、到不了核心的 DNS 监听。
    NSMutableString *rules = [NSMutableString stringWithString:@"table <coast_proxied> persist\n"];
    for (NSUInteger i = 0; i < ifs.count; ++i) {
        NSString *ifn = ifs[i];
        if (dnsPort > 0) {
            [rules appendFormat:@"rdr pass on %@ inet proto udp from <coast_proxied> "
                                 "to any port 53 -> 127.0.0.1 port %d\n", ifn, dnsPort];
        }
        // 这张卡带了专属端口就用它（每卡一个 redir 入站），否则用全局那个。
        const int p = ports[i].intValue > 0 ? ports[i].intValue : redirPort;
        [rules appendFormat:@"rdr pass on %@ inet proto tcp from <coast_proxied> "
                             "to any -> 127.0.0.1 port %d\n", ifn, p];
    }

    // ★ 必须写临时文件，**不能 `pfctl -f -` 从 stdin 喂** —— 后者静默失败（退出码 0、装 0 条）。
    //   与应用侧 PfRules.cpp 同一个坑，改这里时两边要一起看。
    NSString *tmp = @"/var/run/coast-pf-helper.conf";
    NSError *werr = nil;
    if (![rules writeToFile:tmp atomically:YES encoding:NSUTF8StringEncoding error:&werr]) {
        [self pfTeardown];
        reply(NO, werr.localizedDescription ?: @"写临时规则文件失败");
        return;
    }
    NSString *out = nil;
    const int rc = runTool(@"/sbin/pfctl", @[@"-a", @kPfAnchor, @"-f", tmp], &out);
    [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];
    if (rc != 0) {
        [self pfTeardown];
        reply(NO, [NSString stringWithFormat:@"pfctl 装载 anchor 失败：%@", out ?: @""]);
        return;
    }
    // pf 可能是关的（macOS 默认 Disabled）。已启用时 -e 返回非 0，属正常。
    runTool(@"/sbin/pfctl", @[@"-e"], NULL);

    // 回读核实：退出码 0 不代表真装上了（stdin 那个坑就是 0 却装 0 条）。
    NSString *check = nil;
    runTool(@"/sbin/pfctl", @[@"-a", @kPfAnchor, @"-s", @"nat"], &check);
    if (![check containsString:@"rdr"]) {
        [self pfTeardown];
        reply(NO, @"规则装载后回读为空");
        return;
    }
    // ★ 再核挂载点：没被主规则集引用的 anchor 装了也**永远不会被求值**，而 pfctl 全程不报错。
    //   macOS 默认 /etc/pf.conf 有 `rdr-anchor "com.apple/*"`，所以我们挂在 com.apple/ 下。
    NSString *mainNat = nil;
    runTool(@"/sbin/pfctl", @[@"-s", @"nat"], &mainNat);
    if (![mainNat containsString:@"rdr-anchor \"com.apple/"]) {
        [self pfTeardown];
        reply(NO, @"pf 主规则集缺少 `rdr-anchor \"com.apple/*\"` 挂载点，规则不会被求值");
        return;
    }
    reply(YES, @"");
}

- (void)pfSyncProxiedCommaSep:(NSString *)ipv4CommaSep withReply:(void (^)(BOOL, NSString *))reply
{
    NSMutableArray<NSString *> *ips = [NSMutableArray array];
    for (NSString *raw in [ipv4CommaSep componentsSeparatedByString:@","]) {
        NSString *s = [raw stringByTrimmingCharactersInSet:
                                [NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (!s.length) continue;
        if (!ipv4IsSafe(s)) { reply(NO, [NSString stringWithFormat:@"IP 非法：%@", s]); return; }
        [ips addObject:s];
    }
    // 整体替换（调用方给的是全集）。空集合要用 -T flush：-T replace 不接受空参数列表。
    NSMutableArray<NSString *> *args =
        [@[@"-a", @kPfAnchor, @"-t", @"coast_proxied", @"-T"] mutableCopy];
    if (!ips.count) [args addObject:@"flush"];
    else { [args addObject:@"replace"]; [args addObjectsFromArray:ips]; }

    NSString *out = nil;
    if (runTool(@"/sbin/pfctl", args, &out) != 0) {
        reply(NO, [NSString stringWithFormat:@"pfctl 更新 table 失败：%@", out ?: @""]);
        return;
    }
    reply(YES, @"");
}

- (void)pfRemoveWithReply:(void (^)(BOOL, NSString *))reply
{
    [self pfTeardown];
    reply(YES, @"");
}

// 拆规则 + 还原 forwarding。幂等。
// ★ **不执行 `pfctl -d`**：pf 可能是用户自己开着在用的（防火墙/其它 anchor），
//   关掉整个 pf 等于替用户关防火墙。只清我们自己的 anchor。
- (void)pfTeardown
{
    runTool(@"/sbin/pfctl", @[@"-a", @kPfAnchor, @"-F", @"all"], NULL);
    runTool(@"/sbin/pfctl", @[@"-a", @"coast", @"-F", @"all"], NULL); // 早期顶层名字的残留
    NSString *saved = [NSString stringWithContentsOfFile:kPfSysctlArchive
                                                encoding:NSUTF8StringEncoding error:nil];
    if (saved.length) {
        sysctlWriteStr(@"net.inet.ip.forwarding",
                       [saved stringByTrimmingCharactersInSet:
                                  [NSCharacterSet whitespaceAndNewlineCharacterSet]]);
        [[NSFileManager defaultManager] removeItemAtPath:kPfSysctlArchive error:nil];
    }
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
