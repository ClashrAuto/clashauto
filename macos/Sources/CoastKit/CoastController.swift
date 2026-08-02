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
    /// 当前是否正在接管被代理设备的 **IPv6** 流量：网络有 v6 默认路由器、且确有设备被接管着。
    /// UI 据此如实区分「v6 也拐过来了」与「本网络只有 v4」。没接管任何设备 / 本网络无 v6 /
    /// 助手未启用时都是 false（见 `syncRedirect` 各分支）。
    public private(set) var v6GatewayActive = false

    public var onLog: ((String) -> Void)?
    /// 核心二进制缺失，UI 据此引导用户去「设置 → 系统」下载。
    public var onCoreMissing: ((String) -> Void)?

    private var config: AppConfig
    private let core: CoreProcess
    private let builder: ConfigBuilder
    private let systemProxy = SystemProxy()
    private let helper = MacHelperClient()

    private var fullConfigPath: URL?
    /// 台账（生成设备规则时用）。由 app 注入 —— controller 不该自己开库。
    public var deviceStore: DeviceStore?
    /// 当前已下发给 helper 的被接管设备 IP 集合。用来判断「需不需要重新下发」。
    private var activeRedirectIPs: Set<String> = []
    /// 本会话是否真的设过系统代理。退出路径上 stop 会被调用多次，没设过就别做无谓的还原动作。
    private var systemProxyActive = false
    /// 核心意外退出后的有界自愈状态（见 `handleUnexpectedCoreExit`）。
    private var coreRestarts = 0
    private var coreStartedAt: Date?
    /// helper 起的核心没有进程句柄，只能靠 REST 探活（见 `startCoreLivenessProbe`）。
    private var coreProbeTask: Task<Void, Never>?
    /// 只用来探活（`/configs`），与 UI 那条轮询各用各的，互不影响。
    private let api: ClashAPI

    public init(config: AppConfig) {
        self.config = config
        self.core = CoreProcess(config: config)
        self.builder = ConfigBuilder(config: config)
        self.api = ClashAPI(host: config.host, port: config.uiPort,
                            mixedPort: config.mixedPort, secret: config.secret)
        isProxyEnabled = config.webProxy
        isTunEnabled = config.tun

        core.privilegedLauncher = helper
        core.onLog = { [weak self] message in self?.log(message) }
        core.onCoreMissing = { [weak self] path in self?.onCoreMissing?(path) }
        core.onUnexpectedExit = { [weak self] in
            Task { await self?.handleUnexpectedCoreExit() }
        }
        Task { [weak self] in
            await self?.helper.setRedirectLostHandler { [weak self] in
                Task { await self?.handleRedirectLost() }
            }
        }
    }

    public var isCoreInstalled: Bool { core.isCoreInstalled }

    /// 正在运行的核心 PID。日志里对得上系统进程时排查快得多。
    public var coreProcessIdentifierForDiagnostics: Int32? { core.coreProcessIdentifier }

    // MARK: - 核心

    public func startCore() async {
        guard !isCoreRunning else { return }
        // 记下拉起时刻：意外退出时据此判断「这次是不是稳定跑了一阵才崩的」，是就把重启预算清零。
        coreStartedAt = Date()
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
        // helper 起的核心归 helper 所有，本进程收不到它的退出通知 —— 靠 REST 探活补上。
        if isCoreRunning && isPrivileged { startCoreLivenessProbe() }
    }

    /// 核心自己死了（崩溃 / 被 kill / 配置热重载踩到 panic）时的收尾。
    ///
    /// **必须主动把系统恢复原状**，光把状态置回去是不够的：
    /// - 系统代理还指着一个已经没人监听的端口 —— 那不是「代理失效」，是**彻底断网**，
    ///   而界面还显示「运行中」，用户完全无从下手。
    /// - 被接管的设备还在把流量发给一个不再转发的网关 —— 那几台设备同样直接断网。
    ///
    /// 另外 `isCoreRunning` 是本类的存储属性，只在 startCore/stopCore 里更新；不在这里
    /// 置回去的话它会一直是 `true`，于是 `startCore()` 的 `guard !isCoreRunning` 永远
    /// 提前返回 —— 核心**再也起不起来**，只能靠用户先点一次「停止」。
    private func handleUnexpectedCoreExit() async {
        guard isCoreRunning else { return }   // stopCore 已经处理过就别重复来一遍
        isCoreRunning = false
        isPrivileged = false
        // ★ 也要把 CoreProcess 自己的状态复位。helper 起的核心它收不到退出通知，`isRunning`
        //   会一直是 true，下面那次 `startCore()` 会被它开头的 guard 直接早退并**报成功** ——
        //   自愈就成了空转。详见 `CoreProcess.markDead()` 的说明。
        core.markDead()
        log("核心意外退出：正在撤销系统代理与设备接管，避免断网")
        // ★ 先通知，再收拾。
        //
        //   收拾这一步里 `stopRedirect` 要走 XPC —— 没装 helper 时它会**等满 15 秒超时**。
        //   把通知排在后面的话，用户要在「网突然坏了」的状态里干等十几秒才收到解释。
        //   通知本身不依赖收拾的结果，没有理由排队等它。
        onCoreUnexpectedlyExited?()
        try? await helper.stopRedirect()
        activeRedirectIPs = []
        v6GatewayActive = false
        await stopProxy()

        // ★ 有界自愈：把核心重新拉起来。
        //
        //   上面这段撤接管是对的（fail-safe：设备掉回真网关走直连，而不是被劫持到一个不存在的
        //   出口上断网）。但**撤完之后没有任何人再把核心拉起来** —— app 还活着、界面上那些开关
        //   还亮着，而网关就此永久停摆，直到用户去点一次「启动」。被代理设备的主人根本看不到
        //   这个界面，他只会静默失去代理。Qt 那边同一个缺口已在 CoreController 里修过，
        //   两端保持一致的语义。
        //
        //   **有界**是关键：连续最多 kMaxRestarts 次；只要有一次活过 kStableSeconds 就认为稳住了、
        //   预算清零（"跑了一天后崩一次"不该因为历史计数而不救）。超预算就停手并明确记一条 ——
        //   那时多半是配置或内核本身坏了，无脑重启只会刷屏并反复扰动设备。
        //   主动停走的是 `stopCore()`，根本不会到这里，所以不需要额外的"是不是我自己停的"标志。
        let now = Date()
        if let started = coreStartedAt, now.timeIntervalSince(started) >= Self.kStableSeconds {
            coreRestarts = 0
        }
        guard coreRestarts < Self.kMaxRestarts else {
            log("核心连续 \(coreRestarts) 次异常退出，已停止自动重启——请检查配置或内核")
            return
        }
        coreRestarts += 1
        log("核心异常退出，2 秒后自动重启（第 \(coreRestarts)/\(Self.kMaxRestarts) 次）")
        try? await Task.sleep(for: .seconds(2))   // 别贴着崩溃点立刻重来，给端口/句柄让位
        guard !isCoreRunning else { return }
        await startCore()
        // 核心起来之后把台账里还开着代理的设备重新接管回去（startCore 自己不做这件事）。
        await resumeDeviceTakeover()
    }

    /// 连续自动重启的次数上限，以及「活多久算稳住了」。理由见 `handleUnexpectedCoreExit`。
    private static let kMaxRestarts = 3
    private static let kStableSeconds: TimeInterval = 60

    // MARK: - 核心存活探测（helper 起的核心专用）

    /// ★ **helper 起的核心死了没人知道** —— 这条探测就是为了补上它。
    ///
    /// `CoreProcess` 只有 `launchPlain` 那条路挂了 `terminationHandler`；而 macOS 的**默认出货
    /// 配置**是经 helper 以 root 启动（TUN 要 root），那条路只是 `isRunning = true` 就返回了，
    /// 进程归 helper 所有，本进程手里**没有任何句柄**，于是核心崩了、被杀了、OOM 了，app 全然不知：
    /// 界面照显「运行中」，PF 的 rdr 规则**照旧挂着**，被接管设备的流量继续被重定向到一个
    /// 已经没人监听的端口 —— 那不是「代理失效」，是**设备被切断**。
    /// 真机实测（负载中 kill 掉 root 核心）：设备侧 **87.6% 的连接直接 connection refused**，
    /// 而 app 这边一条日志都没有、规则一条没撤。
    ///
    /// 探测用 REST（`/configs`）而不是查 pid：我们真正关心的是「核心还能不能干活」，
    /// 端口没人听和进程没了对使用者是同一件事，而 REST 一次往返本来就是本地回环、成本可忽略。
    /// 连续失败 `kProbeFailsToDeclareDead` 次才判死 —— 单次超时可能只是核心在忙（比如正在
    /// 热重载一份大配置），一次抖动就宣布死亡会把自动重启变成自伤。
    private static let kProbeIntervalSeconds: UInt64 = 5
    private static let kProbeFailsToDeclareDead = 3

    private func startCoreLivenessProbe() {
        guard coreProbeTask == nil else { return }
        coreProbeTask = Task { [weak self] in
            var consecutiveFailures = 0
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(Double(Self.kProbeIntervalSeconds)))
                guard let self else { return }
                // 只在「我们认为核心正在跑、且是 helper 拥有的」时候探测。非特权那条路有
                // terminationHandler，不需要这一层；核心本来就停着时更不该探。
                guard self.isCoreRunning, self.isPrivileged else {
                    consecutiveFailures = 0
                    continue
                }
                do {
                    _ = try await self.api.configs()
                    consecutiveFailures = 0
                } catch {
                    consecutiveFailures += 1
                    if consecutiveFailures >= Self.kProbeFailsToDeclareDead {
                        consecutiveFailures = 0
                        self.log("核心 REST 连续 \(Self.kProbeFailsToDeclareDead) 次无响应，判定为意外退出")
                        await self.handleUnexpectedCoreExit()
                    }
                }
            }
        }
    }

    /// 核心意外退出后通知上层（弹通知 / 提示用户）。
    public var onCoreUnexpectedlyExited: (() -> Void)?

    public func stopCore() async {
        // 主动停：先把探活停掉，免得它把这次"消失"当成崩溃再拉一次起来。
        coreProbeTask?.cancel()
        coreProbeTask = nil
        coreRestarts = 0
        // 退出/停核心前先复原被接管的设备。放在最前面：哪怕后面出错，设备也已经被放回去了。
        try? await helper.stopRedirect()
        activeRedirectIPs = []
        v6GatewayActive = false
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
        await syncRedirect()
    }

    /// 启动时把台账里**已经开着代理**的设备重新接管起来。
    ///
    /// 台账是持久的，接管不是 —— 接管活在 helper 的进程状态里，app 一退出（正常退出、崩溃、
    /// 关机）就随 XPC 断开被复原掉。所以重开 app 之后，界面上那些开关还都亮着，实际却一台
    /// 都没在接管：**用户以为在代理，其实没有**，而且没有任何提示。这里补上这一次同步。
    ///
    /// ★ **必须在核心确实起来之后才做**。被接管的设备没有"直连"这条路 —— 本机就是它的默认
    ///   网关，它的每个包（含最终命中 DIRECT 的）都要经核心才出得去。核心没起就先去投毒，
    ///   等于把设备劫持到一个不存在的出口上，那不是"代理失效"而是**彻底断网**。
    public func resumeDeviceTakeover() async {
        guard isCoreRunning else { return }
        await syncRedirect()
    }

    /// 把「哪些设备该被接管」下发给 helper。
    ///
    /// 每次设备开关/策略变更后经 `rebuildConfig` 调到这里。逻辑很简单：
    ///   • 有设备开代理 → 让 helper（重新）开始接管这批 IP；
    ///   • 一台都没有 → 让 helper 停下并**复原**。
    ///
    /// helper 侧的 start 是幂等的（换 IP 列表时先干净收上一轮），所以这里无脑重发也没问题；
    /// 但仍然比对一下集合，没变就不打扰 —— 避免每次热重载都触发一轮 ARP 重置。
    private func syncRedirect() async {
        guard let store = deviceStore else { return }
        // 每台设备的 (IP, MAC)。MAC 是接管的硬前提 —— 单播欺骗要它、复原要它。
        // proxiedDevices 已保证 lastIP 非空；mac 是主键，恒有值。
        let requested = store.proxiedDevices().map { (ip: $0.lastIP, mac: $0.mac) }

        // ★ 安全闸门:下发前把网关与本机剔掉。
        //
        //   界面上禁用开关是不够的 —— 台账按 MAC 存、开关会一直留着,而网络是会变的:
        //   在 A 网把 192.168.1.50 设成代理,换到 B 网时那个地址可能正是路由器。
        //   真给路由器发「你自己的地址在我这儿」,污染的是它对自身地址的 ARP,
        //   整个局域网都可能被打瘫。这一道必须在这里、在下发之前。
        let gatewayNow = LanTopology.defaultGateway()
        let targets = RedirectTargets.allowed(requested,
                                              gatewayIP: gatewayNow?.ip ?? "",
                                              gatewayMAC: gatewayNow?.mac ?? "",
                                              localMACs: LanTopology.localMACs())
        if targets.count != requested.count {
            let dropped = requested.count - targets.count
            log("已跳过 \(dropped) 台不可接管的设备(网关/本机/离线)")
        }
        let ips = Set(targets.map(\.ip))

        guard ips != activeRedirectIPs else { return }

        // helper 没启用就没法接管（BPF/PF/sysctl 都要 root）。有设备开着却没 helper，
        // 明确记一条 —— 否则用户开了开关却毫无反应，无从查起。
        guard await helper.isEnabled else {
            if !ips.isEmpty { log("已开启设备代理，但免密助手未启用 —— 无法接管（需 root）") }
            v6GatewayActive = false
            return
        }

        if targets.isEmpty {
            try? await helper.stopRedirect()
            activeRedirectIPs = []
            v6GatewayActive = false
            log("已停止接管所有设备并复原")
            return
        }

        guard let gateway = gatewayNow else {
            log("取不到默认网关，无法接管设备")
            v6GatewayActive = false
            return
        }
        // —— IPv6（尽力）——：拿得到 v6 默认路由器就一并接管设备的 v6，拿不到就只做 v4。
        //   设备 v6 源地址只在 v6 拓扑存在时才查（省一次 `ndp -an`）。这份地址表进 helper 的
        //   PF `inet6 from <v6>` 规则；查不到 v6 的设备照样被 NDP 投毒，只是走转发而非代理。
        let gateway6 = LanTopology.defaultGatewayV6()
        let deviceV6s = gateway6 == nil
            ? []
            : LanTopology.deviceV6Addresses(ofMACs: Set(targets.map(\.mac)))
        do {
            try await helper.startRedirect(devices: targets,
                                           interface: gateway.interface,
                                           gatewayIP: gateway.ip,
                                           gatewayMAC: gateway.mac,
                                           redirPort: DeviceStore.redirPort,
                                           dnsPort: DeviceStore.dnsPort,
                                           routerLL6: gateway6?.routerLL ?? "",
                                           routerMAC6: gateway6?.routerMAC ?? "",
                                           deviceV6s: deviceV6s)
            activeRedirectIPs = ips
            v6GatewayActive = gateway6 != nil
            let v6Note = gateway6 == nil ? "" : "（含 IPv6，\(deviceV6s.count) 个 v6 源）"
            log("正在接管 \(targets.count) 台设备的流量\(v6Note)")
        } catch {
            v6GatewayActive = false
            log("接管设备失败：\(error)")
        }
    }

    /// helper 在接管期间没了（崩溃 / 被强杀）。
    ///
    /// 接管随进程消失，但**设备侧的开关没变** —— 用户当初打开它就是授权了接管。
    /// 新的 helper 由 launchd 按需拉起，且它启动时已经把上一条命遗留的内核状态收拾干净
    /// （见 `Redirector.recoverFromCrashIfNeeded`），所以这里直接重新接管一次即可。
    /// 不重来的话：`activeRedirectIPs` 一直挂着，界面显示那几台设备「已代理」，实际没有。
    private func handleRedirectLost() async {
        guard !activeRedirectIPs.isEmpty else { return }
        log("免密助手在接管期间退出，正在重新接管")
        activeRedirectIPs = []
        await syncRedirect()
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
