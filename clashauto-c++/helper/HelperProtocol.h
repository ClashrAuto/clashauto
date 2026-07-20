// 应用与特权 helper 之间共享的 XPC 协议定义（两侧的 .mm 都 #import 本文件）。
// Phase 1 只有 getVersion；Phase 2/3 会加 setSystemProxy / startCore / stopCore。
#pragma once

#import <Foundation/Foundation.h>

// launchd 广告的 mach service 名（= helper 的 bundle id / Label）。两侧必须一致。
#define CA_HELPER_MACH_SERVICE "com.clashrauto.clashauto.helper"

// 允许连接本 helper 的客户端代码签名要求：必须是本应用、Apple 背书、且叶证书属于我们的 Team。
// helper 用 -[NSXPCConnection setCodeSigningRequirement:]（macOS 13+）据此拒绝任意进程驱动 root。
#define CA_CLIENT_CODE_REQUIREMENT \
    "identifier \"com.clashrauto.clashauto\" and anchor apple generic and " \
    "certificate leaf[subject.OU] = \"6AXTRT5TV4\""

@protocol CAHelperProtocol <NSObject>

// 返回 helper 自身版本（与应用版本比对，用于「装的 helper 是否过期需重装」）。
- (void)getVersionWithReply:(void (^)(NSString *version))reply;

// 以 root 设置/清除系统代理（HTTP/HTTPS/SOCKS 都指向 host:port）。root 提交网络配置无需授权。
// bypassCommaSep：逗号分隔的旁路域名/网段（避免 NSXPC 的 NSArray 安全解码白名单）。
- (void)setSystemProxyEnabled:(BOOL)enable
                          host:(NSString *)host
                          port:(int)port
                bypassCommaSep:(NSString *)bypassCommaSep
                     withReply:(void (^)(BOOL ok, NSString *error))reply;

// 以 root 启动 mihomo（-d userDir -f configPath），stdout/stderr → userDir/logs/core.log。
// 已在跑则先停旧的。TUN 靠 root 身份的 mihomo 自建 utun/改路由，helper 不额外做网络配置。
- (void)startCoreExec:(NSString *)execPath
               config:(NSString *)configPath
              userDir:(NSString *)userDir
            withReply:(void (^)(BOOL ok, NSString *error))reply;

// 停止 helper 启动的核心。
- (void)stopCoreWithReply:(void (^)(BOOL ok, NSString *error))reply;

@end
