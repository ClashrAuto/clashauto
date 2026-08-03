// 应用与特权 helper 之间共享的 XPC 协议定义（两侧的 .mm 都 #import 本文件）。
// Phase 1 只有 getVersion；Phase 2/3 会加 setSystemProxy / startCore / stopCore。
#pragma once

#import <Foundation/Foundation.h>

// launchd 广告的 mach service 名（= helper 的 bundle id / Label）。两侧必须一致。
#define CA_HELPER_MACH_SERVICE "com.yuehongsun.coast.helper"

// 允许连接本 helper 的客户端代码签名要求：必须是本应用、Apple 背书、且叶证书属于我们的 Team。
// helper 用 -[NSXPCConnection setCodeSigningRequirement:]（macOS 13+）据此拒绝任意进程驱动 root。
#define CA_CLIENT_CODE_REQUIREMENT \
    "identifier \"com.yuehongsun.coast\" and anchor apple generic and " \
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

// 以 root 打开并配置一个 BPF 设备(绑到 ifname)，把 fd 经 NSFileHandle 传回应用（透明网关二层收发）。
// /dev/bpf* 需 root，故由 helper 开好、配好(BIOCSETIF/PROMISC/IMMEDIATE/SHDRCMPLT)再传 fd，
// 应用即可非 root 收发帧。失败时 fh 为 nil、error 说明原因。
- (void)openBpfForInterface:(NSString *)ifname
                  withReply:(void (^)(NSFileHandle *_Nullable fh, NSString *_Nullable error))reply;

// ── 透明网关的 pf 数据面（rdr 重定向）。三件事都要 root，故整条生命周期由 helper 拥有 ──
//  · pfctl 要 root（/dev/pf 是 crw------- root:wheel，普通用户连 O_RDONLY 都打不开）；
//  · net.inet.ip.forwarding 要 root 才能写；
//  · 故 App（GUI，普通用户 uid）自己一件都做不了 —— 这不是"体验降级"而是功能完全不可用。
//
// ★ 接口只收**端口号 + 网卡名 + IP 列表**，规则文本由 helper 自己拼。不接受客户端传规则原文：
//   helper 是 root，放任客户端喂任意 pf 规则等于把整台机器的包过滤交出去。helper 还会校验
//   网卡名/IP 的字符集，避免被拼进规则文件里做注入。
- (void)pfInstallRedirPort:(int)redirPort
                   dnsPort:(int)dnsPort
           ifnamesCommaSep:(NSString *)ifnamesCommaSep
                 withReply:(void (^)(BOOL ok, NSString *error))reply;

// 把「当前应被接管的设备 IPv4 全集」整体替换进 pf table。空串 = 清空（等价于没开代理）。
- (void)pfSyncProxiedCommaSep:(NSString *)ipv4CommaSep
                    withReply:(void (^)(BOOL ok, NSString *error))reply;

// 拆掉 anchor 规则并还原 forwarding。幂等；helper 自身启动时也会先跑一遍清残留。
- (void)pfRemoveWithReply:(void (^)(BOOL ok, NSString *error))reply;

@end
