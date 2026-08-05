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

    /// 收拾干净并退出本进程，让 launchd 下次按需拉起**新版**。
    ///
    /// 为什么需要它：daemon 一旦被拉起就 `RunLoop.main.run()` 常驻，重新注册**不会**把它换掉
    /// （实测：.app 换成新版、重注册成功、`getVersion` 仍返回旧版本，PID 也没变）。
    /// 结果是程序更新之后，以 root 运行的仍旧是旧代码，且可能长期如此。
    ///
    /// 退出前必须先还原接管、再停核心 —— 直接 exit 会把局域网留在被 ARP 接管的状态，
    /// 并留下一个以 root 运行的孤儿核心进程。那比「跑着旧版 helper」糟得多。
    func terminate(withReply reply: @escaping () -> Void)

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

    /// 开始接管指定设备的流量（零配置透明代理）。以 root 做的事：
    ///   1. `net.inet.ip.forwarding=1`（有 v6 拓扑时再加 `net.inet6.ip6.forwarding=1`）；
    ///   2. 装 PF anchor：把这些源 IP 的 TCP 重定向到 `redirPort`、UDP :53 重定向到 `dnsPort`
    ///      （v4 走 `inet`、v6 走 `inet6`）；
    ///   3. 周期性向这些设备发 ARP 应答宣称「网关 IP 在本机 MAC」，以及（有 v6 拓扑时）发
    ///      NDP 邻居通告宣称「路由器链路本地地址在本机 MAC」。
    ///
    /// ★ **欺骗循环整个跑在 helper 里，而不是把 BPF fd 传回 app**（Qt 版是后者）。
    ///   理由是安全收尾：被欺骗的设备把本机当网关，一旦我们不再转发它就**直接断网**，
    ///   而且它的 ARP/邻居 缓存要十几分钟才过期。只有 helper 能保证「app 崩了也复原」——
    ///   XPC 连接一断它就自己收尾（见 helper 里的 invalidationHandler）。
    ///   fd 传给 app 的话，app 被 SIGKILL 时没有任何人来发那几个复原包。
    ///
    /// `deviceIPsCommaSep` / `deviceMACsCommaSep`：逗号分隔、**一一对应**的设备 IPv4 与 MAC
    /// （用两个字符串而不是数组，避免 NSXPC 的容器类白名单样板）。
    ///
    /// ★ **MAC 必须传，且欺骗必须单播到它**。发广播 ARP 会被整个局域网上缓存里有网关条目的
    ///   设备处理 —— 用户只选了一台，却把全网流量都引过来了。单播只影响目标那一台。
    ///
    /// —— IPv6 参数（都为空 = 该网络没有 v6 默认路由 → 完全跳过 v6，行为与旧版一致）——
    /// `routerLL6`：v6 默认路由器的**链路本地**地址（`fe80::…`，NDP 投毒的 target）。
    /// `routerMAC6`：真路由器的 MAC（复原时用；同一路由器 v4/v6 常共用一个 NIC MAC，但仍单列）。
    /// `deviceV6sCommaSep`：被接管设备的**全局/ULA** v6 源地址（逗号分隔，跨设备扁平合并，顺序无关
    ///   —— PF 规则按源地址匹配，不需要与 MAC 对齐）。它决定哪些 v6 源被 rdr 进核心；查不到 v6 的
    ///   设备照样会被 NDP 投毒（无害），只是其 v6 走转发而非代理，直到下次同步补上。
    ///   NDP 投毒**只需设备 MAC + 路由器 LL/MAC**（组播 target + L2 单播），不依赖这份地址表。
    /// `nicsJSON`：`[RedirectNicSpec]` 的 JSON（见该结构）。**每张网卡一组** ——
    /// 一张卡就是长度 1 的数组，没有单网卡这个特例。
    /// `dnsPort`：核心的 DNS 监听口，全机一个，不随网卡变。
    func startRedirect(nicsJSON: String,
                       dnsPort: Int,
                       withReply reply: @escaping (Bool, String) -> Void)

    /// 停止接管并**把设备复原**：向它们发真网关 MAC 的 ARP 应答，卸掉 PF anchor。
    func stopRedirect(withReply reply: @escaping (Bool, String) -> Void)
}
