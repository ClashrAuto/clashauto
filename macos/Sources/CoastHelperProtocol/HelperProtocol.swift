import Foundation

/// 应用与特权 helper 之间共享的 XPC 契约。**两侧只能有这一份定义** ——
/// 协议方法一旦两边不一致，XPC 在运行期直接拒绝调用，而错误信息只有一句含糊的
/// "does not conform to protocol"。
///
/// 名字沿用 Qt 版：launchd Label = mach service 名 = helper bundle id = codesign `-i`
/// **四者必须完全一致**，否则 daemon 起不来或连不上。换名字等于让已装好 helper 的老用户
/// 重新走一遍系统设置里的批准流程。
public enum HelperConstants {
    /// launchd 广告的 mach service 名。
    public static let machServiceName = "com.yuehongsun.coast.helper"

    /// 允许连接本 helper 的客户端代码签名要求。
    ///
    /// helper 用 `NSXPCConnection.setCodeSigningRequirement(_:)`（macOS 13+）据此拒绝任意进程。
    /// 没有这道门，**任何**本机进程都能连上一个 root 服务让它改网络配置、以 root 起任意程序 ——
    /// 这不是加固，是必需品。
    public static let clientCodeRequirement = """
        identifier "com.yuehongsun.coast" and anchor apple generic and \
        certificate leaf[subject.OU] = "6AXTRT5TV4"
        """
}

/// XPC 接口。`@objc` 是 NSXPCConnection 的硬要求（它靠 Obj-C 运行时做方法转发）。
///
/// 所有方法都是「回调式」而非 async：`NSXPCInterface` 只认 reply block 形式。
/// 客户端那边再包成 async（见 `MacHelperClient`）。
@objc public protocol CoastHelperProtocol {

    /// helper 自身版本，用来判断「装着的 helper 是否过期需重装」。
    func getVersion(withReply reply: @escaping (String) -> Void)

    /// 以 root 设置/清除系统代理（HTTP/HTTPS/SOCKS 都指向 host:port）。
    /// root 提交网络配置无需 Authorization，这正是 helper 能做到全程免密的原因。
    ///
    /// `bypassCommaSeparated` 传逗号分隔的串而不是数组：NSXPC 对容器类型要求显式声明
    /// 允许的类内容白名单，一个字符串省掉那套样板，也少一处能出错的地方。
    func setSystemProxy(enabled: Bool,
                        host: String,
                        port: Int,
                        bypassCommaSeparated: String,
                        withReply reply: @escaping (Bool, String) -> Void)

    /// 以 root 启动 mihomo（`-d userDir -f configPath`），stdout/stderr 重定向到
    /// `userDir/logs/core.log`。已在跑则先停旧的。
    ///
    /// TUN 靠 **root 身份的 mihomo 自己**建 utun、改路由，helper 不额外做网络配置。
    func startCore(executable: String,
                   config: String,
                   userDir: String,
                   withReply reply: @escaping (Bool, String) -> Void)

    /// 停止由本 helper 启动的核心。
    func stopCore(withReply reply: @escaping (Bool, String) -> Void)
}
