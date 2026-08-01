import AppKit
import CoastHelperProtocol
import Foundation
import Security
import ServiceManagement

/// 特权 helper 的应用侧封装：`SMAppService` 注册/注销 + XPC 客户端。
///
/// helper 就绪时，**设系统代理**与**以 root 起核心**都走它，全程免密；TUN 完全依赖这条路
/// —— 非 root 起的 mihomo 建不了 utun。
public actor MacHelperClient: PrivilegedCoreLauncher {

    public enum RegistrationStatus: Sendable, Equatable {
        case notRegistered
        case enabled
        /// 已注册但**等用户在「系统设置 → 登录项」里批准**。这个中间态很常见，
        /// UI 必须把它和「没装」区分开，否则用户会一直点「安装」而不知道要去批准。
        case requiresApproval
        case notFound
        case unknown
    }

    public enum HelperError: Error, Sendable {
        case notEnabled
        case connectionFailed(String)
        case remote(String)
        case timedOut
    }

    /// daemon 的 plist 文件名，必须与 `Contents/Library/LaunchDaemons/` 下的实际文件同名。
    private static let plistName = "\(HelperConstants.machServiceName).plist"

    public init() {}

    // MARK: - 注册

    nonisolated public static func status() -> RegistrationStatus {
        let service = SMAppService.daemon(plistName: plistName)
        switch service.status {
        case .notRegistered: return .notRegistered
        case .enabled: return .enabled
        case .requiresApproval: return .requiresApproval
        case .notFound: return .notFound
        @unknown default: return .unknown
        }
    }

    /// 注册 daemon。首次会让系统弹出「后台项已添加」并要求用户批准。
    nonisolated public static func register() throws -> RegistrationStatus {
        try SMAppService.daemon(plistName: plistName).register()
        // 记下这次注册的是哪一份 helper，供 `ensureRegisteredForCurrentBuild()` 比对。
        // 不记的话，安装后第一次启动会白白重注册一次（并多要一次用户批准）。
        if let hash = helperCDHash() {
            UserDefaults.standard.set(hash, forKey: registeredIdentityKey)
        }
        return status()
    }

    nonisolated public static func unregister() throws {
        try SMAppService.daemon(plistName: plistName).unregister()
    }

    // MARK: - 重新安装后的自愈

    /// `UserDefaults` 里记住「上次注册的是哪一份 helper」。
    private static let registeredIdentityKey = "coast.helper.registeredCDHash"

    /// helper 可执行文件的 cdhash —— 代码签名对内容的唯一摘要。
    ///
    /// 用它而不是 `CFBundleVersion` 当身份：同一个版本号重新构建、重新签名后 cdhash 就变了，
    /// 而 launchd 认的恰恰是签名，不是版本号。版本号相同的替换正是最容易漏掉的那一类。
    nonisolated private static func helperCDHash() -> String? {
        let executable = Bundle.main.bundleURL
            .appendingPathComponent("Contents/MacOS/\(HelperConstants.machServiceName)")
        var staticCode: SecStaticCode?
        guard SecStaticCodeCreateWithPath(executable as CFURL, [], &staticCode) == errSecSuccess,
              let code = staticCode else { return nil }
        var info: CFDictionary?
        guard SecCodeCopySigningInformation(code, [], &info) == errSecSuccess,
              let dict = info as? [String: Any],
              let unique = dict[kSecCodeInfoUnique as String] as? Data else { return nil }
        return unique.map { String(format: "%02x", $0) }.joined()
    }

    /// 确保当前这份 helper 真的是 launchd 注册的那一份；不是就重注册。
    ///
    /// **为什么必须有这个**：`SMAppService` 把注册和注册时那份包的代码签名绑在一起。
    /// 原地替换 .app（应用内更新、从 DMG 拖覆盖、开发期重新打包）之后，旧注册**既不失效
    /// 也拉不起来** —— `status()` 照报 `.enabled`，`isEnabled` 因此判为可用，而每一次 XPC
    /// 调用都无人应答，挂满 15s 超时才失败。表现是「界面显示助手已启用，增强模式和系统代理
    /// 却全哑了」，且日志里没有任何线索。这是实测出来的，不是推测。
    ///
    /// `ensureRegisteredForCurrentBuild` 的结局。**必须分得这么细** —— 第一版把
    /// 「没装过」「取不到 cdhash」「确实没变」「重注册失败」全塌缩成一个 `false`，
    /// 结果实测时看到「报未变更、状态却从 enabled 掉到 notRegistered」而无从判断走了哪条路。
    public enum HealOutcome: Sendable, CustomStringConvertible {
        /// 没安装过 helper —— 安装是用户的显式动作，不在这里替他决定。
        case notInstalled
        /// 读不到 helper 的代码签名摘要，无法判断是否变更；保持现状。
        case identityUnavailable
        /// helper 二进制未变，无需处理。
        case unchanged
        /// 检测到变更，已重新注册并启用。
        case reregistered
        /// 已重新注册，但需要用户去「系统设置 → 登录项」重新批准。
        case needsApproval
        /// 检测到变更，但重注册失败 —— helper 此刻不可用。
        case failed(String)

        public var description: String {
            switch self {
            case .notInstalled: return "未安装 helper"
            case .identityUnavailable: return "读不到 helper 签名摘要"
            case .unchanged: return "helper 未变更"
            case .reregistered: return "helper 已变更，已重新注册并启用"
            case .needsApproval: return "helper 已变更，已重新注册，待用户批准"
            case .failed(let why): return "helper 已变更，重注册失败：\(why)"
            }
        }

        /// 是否需要提醒用户 —— 只有真的做了事或真的坏了才值得打扰他。
        public var isNoteworthy: Bool {
            switch self {
            case .notInstalled, .unchanged, .identityUnavailable: return false
            case .reregistered, .needsApproval, .failed: return true
            }
        }
    }

    nonisolated public static func debugCurrentCDHash() -> String? { helperCDHash() }
    nonisolated public static func debugRecordedCDHash() -> String? {
        UserDefaults.standard.string(forKey: registeredIdentityKey)
    }

    /// 轻量连通性探测：真发一次只读请求。
    ///
    /// **判据只能是行为，不能是 `status()`** —— 实测中 `status()` 报 `.enabled`
    /// 而 XPC 无人应答的情况真实存在（.app 被替换后 launchd 还拽着旧注册）。
    /// 超时取 3s：这只是判活，不值得让启动流程等满默认的 15s。
    nonisolated private static func pingSucceeds() async -> Bool {
        (try? await MacHelperClient().version(timeout: .seconds(3))) != nil
    }

    @discardableResult
    nonisolated public static func ensureRegisteredForCurrentBuild() async -> HealOutcome {
        let recorded = UserDefaults.standard.string(forKey: registeredIdentityKey)
        // 判据是「以前装过」，不是「现在还 enabled」：替换 .app 后系统有时会自己把注册作废。
        guard recorded != nil || status() == .enabled else { return .notInstalled }
        guard let current = helperCDHash() else { return .identityUnavailable }
        if recorded == current, status() == .enabled, await pingSucceeds() { return .unchanged }

        // 第一步：**只** register()，绝不先注销。
        //
        // 第一版是「先 unregister 再 register」，实测的结果是：注销成功、紧接着的注册报
        // “Operation not permitted”（launchd 的注销是异步的），于是用户从「有一个陈旧但
        // 还能用的 helper」变成「完全没有 helper」—— 自愈把事情弄得更糟。
        // 而对已 enabled 的服务重复 register() 是安全的（实测返回 enabled，XPC 照常）。
        if let status = try? register(), status == .enabled, await pingSucceeds() {
            UserDefaults.standard.set(current, forKey: registeredIdentityKey)
            // 重注册**不会**替换已在运行的 daemon（实测：版本与 PID 都不变）。
            // 请它自行收拾干净并退出，launchd 下次按需拉起的就是新版。
            // 旧版 helper 没有 terminate 方法，失败是预期的 —— 忽略即可。
            try? await MacHelperClient().terminate()
            return .reregistered
        }

        // 第二步：确实需要重来一遍时才注销，并带退避重试 —— 注销后立刻注册必然失败。
        try? unregister()
        for delay in [Duration.milliseconds(500), .seconds(2), .seconds(4)] {
            try? await Task.sleep(for: delay)
            guard let status = try? register() else { continue }
            UserDefaults.standard.set(current, forKey: registeredIdentityKey)
            guard status == .enabled else { return .needsApproval }
            return await pingSucceeds() ? .reregistered : .failed("已注册但 XPC 仍不通")
        }
        return .failed("重注册失败，请在设置页重新安装免密助手")
    }

    /// 打开「系统设置 → 登录项」，引导用户批准。注册后停在 `requiresApproval` 时必须给这条出路。
    nonisolated public static func openLoginItemsSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }

    /// 判据用「已启用」而**不是**「ping 得通」。
    ///
    /// 冷启动的 daemon 首个 XPC 偶发慢/超时，用 ping 当门槛会让 helper 明明装好了却被判为不可用
    /// —— 表现是「装了 helper、开了增强、核心却仍非 root，TUN 不生效」，且毫无线索。
    public var isEnabled: Bool {
        get async { Self.status() == .enabled }
    }

    // MARK: - XPC

    /// **接管期专用的长连接。**
    ///
    /// helper 把「撤销设备接管」绑在连接失效上（app 崩了就把设备放回去，那是命门）。
    /// 于是接管必须由一条**活着的**连接持有 —— 用完即弃的短连接一返回就 invalidate，
    /// helper 立刻就把刚建立的接管撤了。这条连接从 `startRedirect` 活到 `stopRedirect`。
    private var redirectConnection: NSXPCConnection?

    /// 接管期的那条连接断了 —— 说明 **helper 侧**没了（崩溃 / 被强杀 / 被系统回收）。
    ///
    /// 这是上一条修复的镜像：那条修的是「helper 误以为 app 没了」，这条管的是
    /// 「app 不知道 helper 没了」。不处理的话 `activeRedirectIPs` 会一直挂着，
    /// 界面继续显示那些设备「已代理」，而实际上接管早就随进程消失了。
    private var onRedirectLost: (@Sendable () -> Void)?

    public func setRedirectLostHandler(_ handler: @escaping @Sendable () -> Void) {
        onRedirectLost = handler
    }

    private func makeConnection() -> NSXPCConnection {
        let connection = NSXPCConnection(machServiceName: HelperConstants.machServiceName,
                                         options: .privileged)
        connection.remoteObjectInterface = NSXPCInterface(with: CoastHelperProtocol.self)
        connection.resume()
        return connection
    }

    /// 默认每次调用建一条连接、用完即弃。helper 是按需拉起的 daemon，长连接除了让状态更难
    /// 推理没有别的好处；这几个调用都不在热路径上。
    ///
    /// 传 `on:` 可复用一条已有连接（接管期那条），此时**不**在返回时 invalidate。
    private func withProxy<T: Sendable>(
        timeout: Duration = .seconds(15),
        on existing: NSXPCConnection? = nil,
        _ body: @escaping @Sendable (CoastHelperProtocol, @escaping @Sendable (Result<T, Error>) -> Void) -> Void
    ) async throws -> T {
        let connection = existing ?? makeConnection()
        defer { if existing == nil { connection.invalidate() } }

        return try await withThrowingTaskGroup(of: T.self) { group in
            group.addTask {
                try await withCheckedThrowingContinuation { continuation in
                    // 只 resume 一次：错误回调和正常回复都可能来，先到先得。
                    let resumed = OnceFlag()
                    let proxy = connection.remoteObjectProxyWithErrorHandler { error in
                        if resumed.claim() {
                            continuation.resume(throwing: HelperError.connectionFailed(error.localizedDescription))
                        }
                    }
                    guard let helper = proxy as? CoastHelperProtocol else {
                        if resumed.claim() {
                            continuation.resume(throwing: HelperError.connectionFailed("远端对象不符合协议"))
                        }
                        return
                    }
                    body(helper) { result in
                        guard resumed.claim() else { return }
                        continuation.resume(with: result)
                    }
                }
            }
            group.addTask {
                try await Task.sleep(for: timeout)
                throw HelperError.timedOut
            }
            guard let first = try await group.next() else { throw HelperError.timedOut }
            group.cancelAll()
            return first
        }
    }

    /// 请求 helper 收拾干净并退出。旧版 helper 没有这个方法，调用会失败 —— 那是预期的，
    /// 调用方当作「换不掉」处理即可，不应视为错误。
    public func terminate(timeout: Duration = .seconds(8)) async throws {
        let _: Bool = try await withProxy(timeout: timeout) { helper, done in
            helper.terminate { done(.success(true)) }
        }
    }

    public func version(timeout: Duration = .seconds(15)) async throws -> String {
        try await withProxy(timeout: timeout) { helper, done in
            helper.getVersion { version in done(.success(version)) }
        }
    }

    public func setSystemProxy(enabled: Bool, host: String, port: Int,
                               bypass: [String] = SystemProxy.defaultBypass) async throws {
        let joined = bypass.joined(separator: ",")
        let _: Bool = try await withProxy { helper, done in
            helper.setSystemProxy(enabled: enabled, host: host, port: port,
                                  bypassCommaSeparated: joined) { ok, error in
                done(ok ? .success(true) : .failure(HelperError.remote(error)))
            }
        }
    }

    // MARK: - PrivilegedCoreLauncher

    public func startCore(executable: URL, config: URL, userDir: URL) async throws {
        let _: Bool = try await withProxy { helper, done in
            helper.startCore(executable: executable.path, config: config.path,
                             userDir: userDir.path) { ok, error in
                done(ok ? .success(true) : .failure(HelperError.remote(error)))
            }
        }
    }

    public func stopCore() async throws {
        let _: Bool = try await withProxy { helper, done in
            helper.stopCore { ok, error in
                done(ok ? .success(true) : .failure(HelperError.remote(error)))
            }
        }
    }

    // MARK: - 透明代理接管

    public func startRedirect(devices: [(ip: String, mac: String)], interface: String,
                              gatewayIP: String, gatewayMAC: String,
                              redirPort: Int, dnsPort: Int,
                              routerLL6: String = "", routerMAC6: String = "",
                              deviceV6s: [String] = []) async throws {
        // IP 与 MAC 用两个逗号串平行传，下标一一对应（helper 侧 zip 回去）
        let ips = devices.map(\.ip).joined(separator: ",")
        let macs = devices.map(\.mac).joined(separator: ",")
        // v6 源地址是扁平集合（PF 按源匹配，与 MAC 无需对齐）；空 = 无 v6。
        let v6s = deviceV6s.joined(separator: ",")
        // 建一条**活着的**连接并留住它 —— 见 `redirectConnection` 的说明。
        redirectConnection?.invalidate()
        let connection = makeConnection()
        redirectConnection = connection
        // 只有**非我方主动**断开才算「丢了」：stopRedirect 里会先把 handler 摘掉。
        connection.interruptionHandler = { [weak self] in
            Task { await self?.redirectVanished() }
        }
        // 启动失败就别留着这条连接 —— 它对应的接管根本没建立起来，留着只是个活着的
        // 空连接，还会让下一次 startRedirect 白白多 invalidate 一次。
        var started = false
        defer {
            if !started {
                connection.invalidate()
                redirectConnection = nil
            }
        }
        let _: Bool = try await withProxy(on: connection) { helper, done in
            helper.startRedirect(deviceIPsCommaSep: ips, deviceMACsCommaSep: macs,
                                 interface: interface,
                                 gatewayIP: gatewayIP, gatewayMAC: gatewayMAC,
                                 redirPort: redirPort, dnsPort: dnsPort,
                                 routerLL6: routerLL6, routerMAC6: routerMAC6,
                                 deviceV6sCommaSep: v6s) { ok, error in
                done(ok ? .success(true) : .failure(HelperError.remote(error)))
            }
        }
        started = true
    }

    /// 接管连接被中断时走这里。摘掉自己再回调，避免 invalidate 时又触发一次。
    private func redirectVanished() {
        guard let connection = redirectConnection else { return }
        connection.interruptionHandler = nil
        redirectConnection = nil
        onRedirectLost?()
    }

    public func stopRedirect() async throws {
        // 用接管期那条连接发停止请求；没有它（app 重启后想清理残留）就临时建一条。
        let connection = redirectConnection
        connection?.interruptionHandler = nil   // 我方主动停，不该当成「丢了」
        defer {
            connection?.invalidate()
            redirectConnection = nil
        }
        let _: Bool = try await withProxy(on: connection) { helper, done in
            helper.stopRedirect { ok, error in
                done(ok ? .success(true) : .failure(HelperError.remote(error)))
            }
        }
    }
}

/// 一次性认领标记。XPC 的错误处理块与正常回复块都可能触发，`CheckedContinuation`
/// 被 resume 两次会直接崩，用它保证只有第一个赢。
/// 只放行一次的标志。多个回调都可能先到（正常回复 / 错误 / 超时），
/// 而 `CheckedContinuation` 恢复两次会直接崩。
final class OnceFlag: @unchecked Sendable {
    private var claimed = false
    private let lock = NSLock()

    func claim() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if claimed { return false }
        claimed = true
        return true
    }
}
