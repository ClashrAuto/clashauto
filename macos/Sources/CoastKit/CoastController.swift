import Foundation
import Observation

/// 把核心进程、系统代理、TUN、配置生成串起来的编排层 —— C++ `CoreController` 的对应物。
///
/// `CoreProcess` 只管进程本身，`SystemProxy` / `MacHelperClient` 只管各自那一件事；
/// 「开代理要不要先起核心」「翻 TUN 要不要重启」这类**策略**都收在这里。
@MainActor
@Observable
public final class CoastController {

    public private(set) var isCoreRunning = false
    public private(set) var isProxyEnabled = false
    public private(set) var isTunEnabled = false
    /// 核心是否由特权 helper 以 root 启动。TUN 只有在这个为 true 时才真正生效。
    public private(set) var isPrivileged = false

    public var onLog: ((String) -> Void)?
    /// 核心二进制缺失，UI 据此引导用户去「设置 → 系统」下载。
    public var onCoreMissing: ((String) -> Void)?

    private var config: AppConfig
    private let core: CoreProcess
    private let builder: ConfigBuilder
    private let systemProxy = SystemProxy()
    private let helper = MacHelperClient()

    private var fullConfigPath: URL?
    /// 本会话是否真的设过系统代理。退出路径上 stop 会被调用多次，没设过就别做无谓的还原动作。
    private var systemProxyActive = false

    public init(config: AppConfig) {
        self.config = config
        self.core = CoreProcess(config: config)
        self.builder = ConfigBuilder(config: config)
        isProxyEnabled = config.webProxy
        isTunEnabled = config.tun

        core.privilegedLauncher = helper
        core.onLog = { [weak self] message in self?.log(message) }
        core.onCoreMissing = { [weak self] path in self?.onCoreMissing?(path) }
    }

    public var isCoreInstalled: Bool { core.isCoreInstalled }

    // MARK: - 核心

    public func startCore() async {
        guard !isCoreRunning else { return }
        let path = builder.ensureFullConfig(tunEnabled: isTunEnabled)
        guard let path else {
            log("生成 full.yaml 失败")
            return
        }
        fullConfigPath = path

        let result = await core.start(tunEnabled: isTunEnabled, fullConfigPath: path)
        isCoreRunning = core.isRunning
        isPrivileged = core.isPrivileged
        if case .success = result, isProxyEnabled {
            await startProxy()
        }
    }

    public func stopCore() async {
        await stopProxy()
        await core.stop()
        isCoreRunning = core.isRunning
        isPrivileged = core.isPrivileged
    }

    public func toggleCore() async {
        if isCoreRunning { await stopCore() } else { await startCore() }
    }

    // MARK: - 系统代理

    public func toggleProxy() async {
        isProxyEnabled.toggle()
        AppConfigLoader.persist(key: "web", bool: isProxyEnabled)
        if isProxyEnabled { await startProxy() } else { await stopProxy() }
    }

    private func startProxy() async {
        // helper 就绪就走它：root 提交网络配置不需要授权，**全程免密**。
        if await helper.isEnabled {
            do {
                try await helper.setSystemProxy(enabled: true, host: config.host, port: config.mixedPort)
                systemProxyActive = true
                log("Start sysproxy ok!")
                return
            } catch {
                log("设置系统代理失败（helper）：\(error)")
                return
            }
        }
        // 回退：本进程 + 一次性授权，会弹一次密码框。
        do {
            try systemProxy.enable(host: config.host, port: config.mixedPort)
            systemProxyActive = true
            log("Start sysproxy ok!")
        } catch {
            log("设置系统代理失败：\(error)")
        }
    }

    private func stopProxy() async {
        guard systemProxyActive else { return }
        if await helper.isEnabled {
            do { try await helper.setSystemProxy(enabled: false, host: config.host, port: config.mixedPort) }
            catch { log("还原系统代理失败（helper）：\(error)") }
        } else {
            do { try systemProxy.disable() }
            catch { log("还原系统代理失败：\(error)") }
        }
        systemProxyActive = false
        log("Stop sysproxy ok!")
    }

    // MARK: - TUN

    /// 翻 TUN（增强模式）。
    ///
    /// **macOS 上必须重启核心，不能热重载** —— 两条原因叠加，缺一不可：
    ///   1. 建/拆 utun + 改默认路由必须 **root**，而 root 只在核心由特权 helper **冷启动**时才有；
    ///   2. mihomo 的 `PUT /configs` 默认**不重载 general/tun 段**（要 `?force=true` 才会），
    ///      所以热重载改 `tun.enable` 核心根本不理会。
    ///
    /// 于是运行中翻开关既没权限建 utun、核心也不重读 tun → 表现为「开了 TUN 却不全局」。
    /// 这里改成把核心停掉再起：`startCore()` 经 helper 以 root 冷启动，读到刚写入的 `tun.enable`，
    /// utun 与全局路由才真正建立。helper 没启用时 `CoreProcess` 会回退非 root 并明确记一条
    /// 「TUN 将不生效」，把真正原因暴露出来而不是静默失败。
    public func toggleTun() async {
        isTunEnabled.toggle()
        AppConfigLoader.persist(key: "use", bool: isTunEnabled)

        if let path = fullConfigPath {
            _ = builder.writeTunEnabled(at: path, enabled: isTunEnabled)
        } else {
            fullConfigPath = builder.ensureFullConfig(tunEnabled: isTunEnabled)
        }

        log(isTunEnabled ? "已开启增强模式，正在以 root 重启核心以应用 TUN"
                         : "已关闭增强模式，正在重启核心")
        if isCoreRunning {
            await stopCore()
            await startCore()
        }
    }

    // MARK: - 配置重建

    /// 设置/规则/订阅变更后重新生成 full.yaml 并热重载。
    public func rebuildConfig() async {
        guard let path = builder.ensureFullConfig(tunEnabled: isTunEnabled) else {
            log("生成 full.yaml 失败")
            return
        }
        fullConfigPath = path
        await reloadConfig()
    }

    /// 热重载。
    ///
    /// **先用核心自己的测试模式（`mihomo -t`）校验**再 PUT：校验不过就不重载，保留当前正在跑的
    /// 好配置。否则一份坏配置会把核心打到失效状态，而用户只会看到「突然全断网」。
    /// 核心不在时跳过校验（别因为缺核心反而不重载）。
    private func reloadConfig() async {
        guard isCoreRunning, let path = fullConfigPath else { return }

        let executable = AppPaths.coreExecutable
        if FileManager.default.isExecutableFile(atPath: executable.path) {
            let check = Process()
            check.executableURL = executable
            check.arguments = ["-t", "-d", AppPaths.userDir.path, "-f", path.path]
            check.standardOutput = FileHandle.nullDevice
            check.standardError = FileHandle.nullDevice
            try? check.run()
            check.waitUntilExit()
            guard check.terminationStatus == 0 else {
                log("配置校验未通过，已跳过热重载（保留当前运行配置）")
                return
            }
        }

        let api = ClashAPI(host: config.host, port: config.uiPort,
                           mixedPort: config.mixedPort, secret: config.secret)
        do {
            try await api.reloadConfig(path: path.path)
            log("Clash 配置已重载")
        } catch {
            log("重载 Clash 配置失败: \(error)")
        }
    }

    // MARK: - 配置变更

    public func updateConfig(_ config: AppConfig) {
        self.config = config
        core.updateConfig(config)
        builder.updateConfig(config)
    }

    // MARK: - helper

    public nonisolated var helperStatus: MacHelperClient.RegistrationStatus {
        MacHelperClient.status()
    }

    public func installHelper() async -> String {
        do {
            let status = try MacHelperClient.register()
            switch status {
            case .enabled:
                log("免密助手已安装并启用")
                return "已安装并启用"
            case .requiresApproval:
                // 这个中间态必须和「没装」区分开，否则用户会一直点安装而不知道要去批准。
                log("免密助手已注册，等待在「系统设置 → 登录项」中批准")
                MacHelperClient.openLoginItemsSettings()
                return "已注册，请在「系统设置 → 登录项」中批准"
            default:
                return "注册后状态异常：\(status)"
            }
        } catch {
            log("安装免密助手失败：\(error)")
            return "安装失败：\(error.localizedDescription)"
        }
    }

    public func uninstallHelper() async -> String {
        do {
            try MacHelperClient.unregister()
            log("免密助手已卸载")
            return "已卸载"
        } catch {
            return "卸载失败：\(error.localizedDescription)"
        }
    }

    private func log(_ message: String) { onLog?(message) }
}
