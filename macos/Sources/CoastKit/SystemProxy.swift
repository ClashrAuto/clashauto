import Foundation
import Security
import SystemConfiguration

/// macOS 系统代理开关。
///
/// 走 **SystemConfiguration** 直接改网络配置，而不是逐个网络服务起 `networksetup` 子进程：
/// 一次 Lock → 逐服务改 Proxies 字典 → Commit → Apply，既省掉 N 次 fork/exec，
/// 也把授权收敛成**整会话最多弹一次**密码框。
///
/// 两条路径，优先级如注释：
///   1. **特权 helper**（root）—— 全程免密，是首选。由 `CoreController` 在 helper 就绪时选用。
///   2. 本进程 + 一次性 `AuthorizationRef` —— helper 没装/没批准时的回退，会弹一次密码框。
///
/// 本类实现的是第 2 条；第 1 条在 `MacHelperClient`。
public final class SystemProxy: @unchecked Sendable {

    public enum ProxyError: Error, Sendable {
        case authorizationDenied(OSStatus)
        case preferencesUnavailable
        case lockFailed
        case noNetworkServices
        case commitFailed
    }

    /// 旁路列表。回环、私网、`.local` 不该走代理 —— 否则访问自家路由器后台、局域网 NAS、
    /// Bonjour 名字都会被丢到代理节点上。
    public static let defaultBypass = [
        "localhost", "127.0.0.1", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "*.local",
    ]

    /// 本会话的授权句柄。**整个进程生命周期复用** —— 开代理时弹一次，之后关代理、
    /// 改端口都不再弹。这正是替代 networksetup 的目的。
    private var authorization: AuthorizationRef?

    public init() {}

    deinit {
        if let authorization {
            AuthorizationFree(authorization, AuthorizationFlags())
        }
    }

    // MARK: - 开关

    public func enable(host: String, port: Int, bypass: [String] = SystemProxy.defaultBypass) throws {
        try apply(enable: true, host: host, port: port, bypass: bypass)
    }

    /// 关代理时**只翻 `*Enable` 位、保留 host/port** —— 下次秒开，也不动用户自己配的其它代理键。
    public func disable() throws {
        try apply(enable: false, host: "", port: 0, bypass: [])
    }

    private func apply(enable: Bool, host: String, port: Int, bypass: [String]) throws {
        let auth = try ensureAuthorization()

        guard let prefs = SCPreferencesCreateWithAuthorization(nil, "Coast" as CFString, nil, auth) else {
            throw ProxyError.preferencesUnavailable
        }
        guard SCPreferencesLock(prefs, true) else {
            throw ProxyError.lockFailed
        }
        defer { SCPreferencesUnlock(prefs) }

        guard let services = SCNetworkServiceCopyAll(prefs) as? [SCNetworkService] else {
            throw ProxyError.noNetworkServices
        }

        for service in services {
            // 跳过已停用的网络服务：给它们写代理没有意义，还会在「网络」面板里留下噪声。
            guard SCNetworkServiceGetEnabled(service),
                  let serviceID = SCNetworkServiceGetServiceID(service) as String? else { continue }

            let path = "/NetworkServices/\(serviceID)/Proxies" as CFString
            // 在**现有字典**上改，不是新建一个：FTP/PAC 等我们不管的键要原样保留，
            // 整份替换会把用户自己配的东西抹掉。
            var proxies = (SCPreferencesPathGetValue(prefs, path) as? [String: Any]) ?? [:]

            if enable {
                // 混合端口同时服务 HTTP/HTTPS/SOCKS，三种都指向它
                proxies[kCFNetworkProxiesHTTPEnable as String] = 1
                proxies[kCFNetworkProxiesHTTPProxy as String] = host
                proxies[kCFNetworkProxiesHTTPPort as String] = port
                proxies[kCFNetworkProxiesHTTPSEnable as String] = 1
                proxies[kCFNetworkProxiesHTTPSProxy as String] = host
                proxies[kCFNetworkProxiesHTTPSPort as String] = port
                proxies[kCFNetworkProxiesSOCKSEnable as String] = 1
                proxies[kCFNetworkProxiesSOCKSProxy as String] = host
                proxies[kCFNetworkProxiesSOCKSPort as String] = port
                proxies[kCFNetworkProxiesExceptionsList as String] = bypass
            } else {
                proxies[kCFNetworkProxiesHTTPEnable as String] = 0
                proxies[kCFNetworkProxiesHTTPSEnable as String] = 0
                proxies[kCFNetworkProxiesSOCKSEnable as String] = 0
            }
            SCPreferencesPathSetValue(prefs, path, proxies as CFDictionary)
        }

        guard SCPreferencesCommitChanges(prefs), SCPreferencesApplyChanges(prefs) else {
            throw ProxyError.commitFailed
        }
    }

    // MARK: - 授权

    /// 预授权 `system.services.systemconfiguration.network`：弹一次密码框，成功后授权缓存在
    /// `AuthorizationRef` 上，后续 Commit 在有效期内不再弹。
    private func ensureAuthorization() throws -> AuthorizationRef {
        if let authorization { return authorization }

        var ref: AuthorizationRef?
        var status = AuthorizationCreate(nil, nil, AuthorizationFlags(), &ref)
        guard status == errAuthorizationSuccess, let created = ref else {
            throw ProxyError.authorizationDenied(status)
        }

        status = "system.services.systemconfiguration.network".withCString { name in
            var item = AuthorizationItem(name: name, valueLength: 0, value: nil, flags: 0)
            return withUnsafeMutablePointer(to: &item) { itemPointer in
                var rights = AuthorizationRights(count: 1, items: itemPointer)
                let flags: AuthorizationFlags = [.interactionAllowed, .preAuthorize, .extendRights]
                return AuthorizationCopyRights(created, &rights, nil, flags, nil)
            }
        }
        guard status == errAuthorizationSuccess else {
            // 用户取消或权限不足：放弃，不退回逐次弹窗的 networksetup
            AuthorizationFree(created, AuthorizationFlags())
            throw ProxyError.authorizationDenied(status)
        }

        authorization = created
        return created
    }

    // MARK: - 自检

    /// 机制自检：读当前值 → 设成测试值 → 读回核对 → **还原原值**。
    ///
    /// 单独做这个钩子的理由：改系统代理这条路只有在**真实桌面会话**里才验得了，CI 和无头机都跑不了，
    /// 而它一旦出错的后果是「本机上不了网」。让用户拿 GUI 去试代价太大 —— 这个几秒钟就能知道
    /// 机制在这台机器上到底通不通。不建 GUI、不起核心。
    public static func selfTest(host: String = "127.0.0.1", port: Int = 7890) -> (ok: Bool, message: String) {
        let proxy = SystemProxy()
        let before = currentHTTPProxy()
        do {
            try proxy.enable(host: host, port: port)
        } catch {
            return (false, "开启失败: \(error)")
        }
        let readBack = currentHTTPProxy()
        do {
            try proxy.disable()
        } catch {
            return (false, "已开启但还原失败（系统代理可能仍指向 \(host):\(port)）: \(error)")
        }
        guard readBack?.host == host, readBack?.port == port else {
            return (false, "写入后读回不一致：期望 \(host):\(port)，实际 \(describe(readBack))")
        }
        return (true, "系统代理机制可用（原值 \(describe(before))），已还原")
    }

    private static func describe(_ proxy: (host: String, port: Int)?) -> String {
        guard let proxy else { return "未设置" }
        return "\(proxy.host):\(proxy.port)"
    }

    /// 读当前生效的 HTTP 代理（用 `SCDynamicStore` 的合成视图，即系统实际在用的那份）。
    public static func currentHTTPProxy() -> (host: String, port: Int)? {
        guard let settings = SCDynamicStoreCopyProxies(nil) as? [String: Any],
              (settings[kCFNetworkProxiesHTTPEnable as String] as? Int) == 1,
              let host = settings[kCFNetworkProxiesHTTPProxy as String] as? String,
              let port = settings[kCFNetworkProxiesHTTPPort as String] as? Int else { return nil }
        return (host, port)
    }
}
