import AppKit
import CoastHelperProtocol
import Foundation
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
        return status()
    }

    nonisolated public static func unregister() throws {
        try SMAppService.daemon(plistName: plistName).unregister()
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

    /// 每次调用建一条连接、用完即弃。helper 是按需拉起的 daemon，长连接除了让状态更难推理
    /// 没有别的好处；这几个调用都不在热路径上。
    private func withProxy<T: Sendable>(
        timeout: Duration = .seconds(15),
        _ body: @escaping @Sendable (CoastHelperProtocol, @escaping @Sendable (Result<T, Error>) -> Void) -> Void
    ) async throws -> T {
        let connection = NSXPCConnection(machServiceName: HelperConstants.machServiceName,
                                         options: .privileged)
        connection.remoteObjectInterface = NSXPCInterface(with: CoastHelperProtocol.self)
        connection.resume()
        defer { connection.invalidate() }

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

    public func version() async throws -> String {
        try await withProxy { helper, done in
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

    public func startRedirect(deviceIPs: [String], interface: String,
                              gatewayIP: String, gatewayMAC: String,
                              redirPort: Int, dnsPort: Int) async throws {
        let joined = deviceIPs.joined(separator: ",")
        let _: Bool = try await withProxy { helper, done in
            helper.startRedirect(deviceIPsCommaSep: joined, interface: interface,
                                 gatewayIP: gatewayIP, gatewayMAC: gatewayMAC,
                                 redirPort: redirPort, dnsPort: dnsPort) { ok, error in
                done(ok ? .success(true) : .failure(HelperError.remote(error)))
            }
        }
    }

    public func stopRedirect() async throws {
        let _: Bool = try await withProxy { helper, done in
            helper.stopRedirect { ok, error in
                done(ok ? .success(true) : .failure(HelperError.remote(error)))
            }
        }
    }
}

/// 一次性认领标记。XPC 的错误处理块与正常回复块都可能触发，`CheckedContinuation`
/// 被 resume 两次会直接崩，用它保证只有第一个赢。
private final class OnceFlag: @unchecked Sendable {
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
