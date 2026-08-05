import CoastHelperProtocol
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

    /// 这条流里**两种消息都有**：本类自己的编排消息（`.notice`/`.routine`）和核心
    /// 进程吐出来的原文（`.core`）。收集端靠 `LogKind` 分流 —— 以前它整条被当成
    /// 「核心日志」，于是主日志被核心刷屏、内核页里混着程序自己的动作。
    public var onLog: ((String, LogKind) -> Void)?
    /// 核心二进制缺失，UI 据此引导用户去「设置 → 系统」下载。
    public var onCoreMissing: ((String) -> Void)?

    /// 转发给 `CoreProcess`：日志页开着才收核心原文。见那边的说明。
    public var streamsCoreOutput: Bool {
        get { core.streamsCoreOutput }
        set { core.streamsCoreOutput = newValue }
    }

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
        core.onLog = { [weak self] message, kind in self?.onLog?(message, kind) }
        core.onCoreMissing = { [weak self] path in self?.onCoreMissing?(path) }
        core.onUnexpectedExit = { [weak self] in
            Task { await self?.handleUnexpectedCoreExit() }
        }
        Task { [weak self] in
            await self?.helper.setRedirectLostHandler { [weak self] in
                Task { await self?.handleRedirectLost() }
            }
        }
        // ★ 启动期自愈的**顺序不能反**：先收孤儿核心，再擦残留系统代理。
        //
        //   `clearStaleSystemProxy()` 的判据之一是「我们的 mixedPort **无人监听**」，
        //   而上一世遗留的孤儿核心**正占着那个端口**。若先清代理、后收孤儿，
        //   清代理那步会因为"有人在听"直接跳过 —— **两项自愈单独都对，
        //   凑在一起却互相抵消**。真机实测过这个组合场景：2 个孤儿占着 7890/9191，
        //   此时残留的系统代理擦不掉。
        core.reapOrphansAtStartup()
        clearStaleSystemProxy()
    }

    public var isCoreInstalled: Bool { core.isCoreInstalled }

    /// 正在运行的核心 PID。日志里对得上系统进程时排查快得多。
    public var coreProcessIdentifierForDiagnostics: Int32? { core.coreProcessIdentifier }

    // MARK: - 核心

    public func startCore() async {
        guard !isCoreRunning else { return }
        // 记下拉起时刻：意外退出时据此判断「这次是不是稳定跑了一阵才崩的」，是就把重启预算清零。
        coreStartedAt = Date()
        let path = buildFullConfig()
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

    /// 系统即将睡眠：撤销设备接管并让 helper 发复原 ARP，好让设备回到真网关。
    ///
    /// 与 `stopCore()` 的区别是**不动核心**：睡眠只是暂停，醒来还要接着用；
    /// 也**不动台账里的开关**（那是持久意图，`resumeDeviceTakeover()` 醒来会补回去）。
    /// 只把运行时的接管撤掉 —— 本机一睡就不转发了，设备的 ARP 还钉在这儿等于直接断网。
    public func withdrawTakeoverForSleep() async {
        guard !activeRedirectIPs.isEmpty else { return }
        log("撤销 \(activeRedirectIPs.count) 台设备的接管（系统即将睡眠）")
        try? await helper.stopRedirect()
        activeRedirectIPs = []
        v6GatewayActive = false
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
    ///
    /// ★ 但**失败之后要马上复查，别再等一个整轮**：判死之前的这段时间里，设备的流量还在被
    ///   rdr 到一个没人监听的端口上（connection refused，比超时更刺眼），窗口有多长就断多久。
    ///   稳态 5s 一次是为了省；一旦出现第一次失败，说明八成真出事了，改用 1s 间隔连查。
    ///   于是检测窗口从 5×3=15s 收到约 5+1+1=7s，而稳态的探测频率一点没变。
    ///
    /// ★ **「连接被拒」要当场判死，不要凑够三次**（真机压测实测收窄的关键）：
    ///   `kProbeFailsToDeclareDead = 3` 的理由是「单次超时可能只是核心在忙」——但那个理由
    ///   **只对超时成立**。`URLError.cannotConnectToHost` 是本地回环上的 TCP 连接被拒，
    ///   意味着**那个端口根本没有监听者**：一个忙到超时的核心仍然 listen 着，只有死掉的才拒连。
    ///   把这两类混为一谈，等于让确定的坏消息陪着不确定的一起多等两拍。
    ///   同时把稳态间隔 5s → 2s：一次本地回环 GET 而已，两秒一次的开销可以忽略。
    ///   真机实测（Mac 网关、三设备各 8 并发在跑时 `kill -9` 掉 root 核心）：
    ///   改之前设备侧**中断 6.1 秒**（探针 t=31.0 起连续失败、t=37.1 才恢复）；
    ///   按新策略最坏是 2s（稳态间隔）+ 一次判死，检测部分从 ~5~7s 收到 ~2s。
    ///   **注意这只是收窄，不是消除** —— 要真正做到近乎零，得让 helper 用 SIGCHLD
    ///   主动把核心死亡推给 app（需要新的 XPC 事件通道），那是另一件事。
    private static let kProbeIntervalSeconds: Double = 2
    private static let kProbeRetryIntervalSeconds: Double = 1
    private static let kProbeFailsToDeclareDead = 3

    /// 这次失败是不是「端口没人监听」——是就不必再等，核心已经不在了。
    private static func isConnectionRefused(_ error: Error) -> Bool {
        guard let urlError = error as? URLError else { return false }
        // cannotConnectToHost = 对端明确拒绝（回环上即 ECONNREFUSED）；
        // cannotFindHost / networkConnectionLost 之类不算，那些在回环上另有原因。
        return urlError.code == .cannotConnectToHost
    }

    private func startCoreLivenessProbe() {
        guard coreProbeTask == nil else { return }
        coreProbeTask = Task { [weak self] in
            var consecutiveFailures = 0
            while !Task.isCancelled {
                // 上一拍失败过就贴紧复查，否则按稳态节奏歇着。
                let wait = consecutiveFailures > 0 ? Self.kProbeRetryIntervalSeconds
                                                   : Self.kProbeIntervalSeconds
                try? await Task.sleep(for: .seconds(wait))
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
                    // 连接被拒 = 端口上没有监听者 = 核心已经不在，不用再等够三次（见上方论证）。
                    let refused = Self.isConnectionRefused(error)
                    if refused || consecutiveFailures >= Self.kProbeFailsToDeclareDead {
                        let reason = refused
                            ? "核心 REST 端口拒绝连接（无监听者），判定为意外退出"
                            : "核心 REST 连续 \(Self.kProbeFailsToDeclareDead) 次无响应，判定为意外退出"
                        consecutiveFailures = 0
                        self.log(reason)
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
        //
        // ★ 没接管过就**别去敲 helper**：`stopRedirect()` 在没有现成连接时会临时建一条，
        //   而 helper 不在/没响应时那次 XPC 要等满默认超时（15 秒）才返回 —— 退出路径上
        //   等于程序「点了退出退不掉」，自更新更是直接卡在「正在退出安装…」。
        //   判据与睡眠那条路一致（`withdrawTakeoverForSleep` 里也是先看这个集合）。
        if !activeRedirectIPs.isEmpty {
            try? await helper.stopRedirect()
        }
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
                log("Start sysproxy ok!", .routine)
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
            log("Start sysproxy ok!", .routine)
        } catch {
            log("设置系统代理失败：\(error)")
        }
    }

    /// 启动自愈：上一次会话被强杀/崩溃时来不及还原系统代理，会把整机流量指向一个
    /// **已经没人监听**的本机端口 —— 用户表现为"什么都打不开"，且完全无从得知原因。
    ///
    /// `stopProxy()` 开头那个 `guard systemProxyActive` 只看**本会话**是否设过代理，
    /// 上一世留下的残留它永远擦不掉。与 Qt 线的 `CoreController::clearStaleSystemProxy()`
    /// 同一设计（那边已修，本机实测有效：残留 `Enabled: Yes` → 启动后 `Enabled: No`），
    /// 这里对齐 —— 本机实测 Swift 端在同一场景下残留**依然还在**，是真缺口。
    ///
    /// 判据要求**两条同时成立**才动手，避免误删用户自己配的公司代理：
    ///   ① 系统代理指向 127.0.0.1，且端口 == 我们自己的 mixedPort；
    ///   ② 该端口当前**没有任何进程在监听**（连得上就说明有人在用，一律不动）。
    ///
    /// ★ 与 Qt 线的判据有一处**有意的不同**：这里用 `SCDynamicStoreCopyProxies` 读
    ///   **当前生效**的代理（即默认路由所在那条链路的设置），而 Qt 线是遍历
    ///   `networksetup -listallnetworkservices` 的**每一个**服务逐个查。
    ///   两者各有取舍：
    ///     · 读生效值：只处理真正影响上网的那一份，不会去碰用户在别的
    ///       （当前不活动的）网络服务上自己配的代理 —— **更保守、误伤面更小**；
    ///     · 遍历所有服务：能把不活动服务上的残留也擦掉，但那份残留本来也不影响上网。
    ///   本机实测踩到过这个差异：在 Wi-Fi 服务上造残留，而默认路由其实走 `utun4`
    ///   （另一个 VPN 的 TUN），于是 `scutil --proxy` 是 `HTTPEnable: 0`，
    ///   这里判定"没有生效的代理"直接返回 —— **是正确行为，不是漏修**。
    private func clearStaleSystemProxy() {
        let port = config.mixedPort
        guard port > 0 else { return }
        guard let cur = SystemProxy.currentHTTPProxy() else { return }
        guard cur.host == "127.0.0.1", cur.port == port else { return }   // 不是我们的，别碰
        // 条件②：能连上就说明有人在听（可能是用户手动起的核心），一律不动。
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = UInt16(port).bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        if fd >= 0 {
            var tv = timeval(tv_sec: 0, tv_usec: 150_000)
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
            let connected = withUnsafePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) == 0
                }
            }
            close(fd)
            if connected { return }   // 有人在听 → 不是残留
        }
        do {
            try systemProxy.disable()
            log("已清除上次异常退出残留的系统代理（127.0.0.1:\(port) 无人监听）")
        } catch {
            log("清除残留系统代理失败：\(error)")
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
        log("Stop sysproxy ok!", .routine)
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
            fullConfigPath = buildFullConfig()
        }

        log(isTunEnabled ? "已开启增强模式，正在以 root 重启核心以应用 TUN"
                         : "已关闭增强模式，正在重启核心")
        if isCoreRunning {
            await stopCore()
            await startCore()
        }
    }

    // MARK: - 配置重建

    /// 出口网卡（有序，index 0 = 主网卡），生成 full.yaml 那一刻定下来的那份。
    ///
    /// ★ 下发给 helper 时**必须用同一份**，不能各自再去探一次：每张卡的 redir 端口是按这个
    ///   次序算的（`DeviceStore.redirPort(forNic:)`），两处次序不一致 —— 比如两次探测之间
    ///   网线插拔了 —— PF 就会把设备的包 rdr 到一个没人监听的端口，那台设备直接断网。
    private var egressGateways: [LanTopology.Gateway] = []

    /// 生成 full.yaml。出口网卡表在这里现取并记下，配置与下发共用这一份次序。
    private func buildFullConfig() -> URL? {
        egressGateways = LanTopology.allGateways()
        builder.egressNics = egressGateways.map(\.interface)
        return builder.ensureFullConfig(tunEnabled: isTunEnabled)
    }

    /// 设置/规则/订阅变更后重新生成 full.yaml 并热重载。
    public func rebuildConfig() async {
        guard let path = buildFullConfig() else {
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
        // interface 也要带上：多网卡时它决定这台设备归哪张卡（台账里由扫描侧 recordSeen 写）。
        let requestedFull = store.proxiedDevices()
            .map { (ip: $0.lastIP, mac: $0.mac, interface: $0.interface) }
        let requested = requestedFull.map { (ip: $0.ip, mac: $0.mac) }

        // ★ 安全闸门:下发前把网关与本机剔掉。
        //
        //   界面上禁用开关是不够的 —— 台账按 MAC 存、开关会一直留着,而网络是会变的:
        //   在 A 网把 192.168.1.50 设成代理,换到 B 网时那个地址可能正是路由器。
        //   真给路由器发「你自己的地址在我这儿」,污染的是它对自身地址的 ARP,
        //   整个局域网都可能被打瘫。这一道必须在这里、在下发之前。
        //   多网卡时**每张卡各有一个网关**，全都要剔 —— 只剔主网卡那个，等于允许把另一张卡的
        //   路由器当设备接管，那个网络会被打瘫。
        // 用生成配置时那份网关表（见 egressGateways 的注释），不再现探一次。
        let gateways = egressGateways
        let localMACs = LanTopology.localMACs()
        var targets = requested
        if gateways.isEmpty {
            targets = RedirectTargets.allowed(targets, gatewayIP: "", gatewayMAC: "",
                                              localMACs: localMACs)
        } else {
            for gateway in gateways {
                targets = RedirectTargets.allowed(targets, gatewayIP: gateway.ip,
                                                  gatewayMAC: gateway.mac, localMACs: localMACs)
            }
        }
        let gatewayNow = gateways.first
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

        guard gatewayNow != nil else {
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

        // —— 按网卡分组下发 ——
        // 每张卡一组：自己的网关、自己那批设备、自己的 redir 入站端口（出口网卡是**入站**的
        // 属性，见 core 的 component/dialer/egress.go）。一张卡就是长度 1 的数组。
        // 设备归哪张卡看台账里的 interface；归不出来的（还没扫到 / 那张卡不在网关表里）
        // 算主网卡 —— 宁可从默认出口出去，也不能没人接管。
        let interfaceByMAC = Dictionary(requestedFull.map { ($0.mac, $0.interface) },
                                        uniquingKeysWith: { first, _ in first })
        var grouped: [Int: [(ip: String, mac: String)]] = [:]
        for target in targets {
            let iface = interfaceByMAC[target.mac] ?? ""
            let index = gateways.firstIndex { $0.interface == iface } ?? 0
            grouped[index, default: []].append(target)
        }
        // v6 拓扑只对**它自己那张卡**成立（defaultGatewayV6 返回的 interface）。别的卡不带 v6 ——
        // 带着别人的路由器 LL 去投毒，设备会把 v6 默认路由指到一个不转发它的地方。
        let nics: [RedirectNicSpec] = grouped.keys.sorted().compactMap { index in
            guard index < gateways.count, let devices = grouped[index], !devices.isEmpty
            else { return nil }
            let gateway = gateways[index]
            let sameNicV6 = gateway6?.interface == gateway.interface
            return RedirectNicSpec(
                interface: gateway.interface,
                gatewayIP: gateway.ip,
                gatewayMAC: gateway.mac,
                redirPort: DeviceStore.redirPort(forNic: index),
                routerLL6: sameNicV6 ? (gateway6?.routerLL ?? "") : "",
                routerMAC6: sameNicV6 ? (gateway6?.routerMAC ?? "") : "",
                deviceIPs: devices.map(\.ip),
                deviceMACs: devices.map(\.mac),
                deviceV6s: sameNicV6 ? deviceV6s : [])
        }
        guard !nics.isEmpty else {
            log("没有任何设备能归到已知网卡上，未接管")
            v6GatewayActive = false
            return
        }
        do {
            try await helper.startRedirect(nics: nics, dnsPort: DeviceStore.dnsPort)
            activeRedirectIPs = ips
            v6GatewayActive = gateway6 != nil
            let v6Note = gateway6 == nil ? "" : "（含 IPv6，\(deviceV6s.count) 个 v6 源）"
            let nicNote = nics.count > 1 ? "，跨 \(nics.count) 张网卡" : ""
            log("正在接管 \(targets.count) 台设备的流量\(nicNote)\(v6Note)")
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
            log("Clash 配置已重载", .routine)
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

    /// 默认 `.notice`：本类的消息都是程序自己的动作/结论。每次开关都会刷一条的
    /// 例行回执显式标 `.routine`（页脚不显示它们，日志页照收）。
    private func log(_ message: String, _ kind: LogKind = .notice) { onLog?(message, kind) }
}
