import AppKit
import CoastKit
import Observation
import SwiftUI

/// UI 与后端之间唯一的门面 —— C++ `QmlBridge` 的对应物。
///
/// 职责和那边一样只有三件：把后端状态暴露成视图能读的属性、把界面动作转成后端调用、
/// 持有几个列表模型。**业务规则不写在这里**，那些在 `CoastController` / `ClashService`。
@MainActor
@Observable
public final class AppState {

    public enum Page: Int, CaseIterable, Identifiable {
        case status, nodes, devices, subscriptions, settings, logs, about
        public var id: Int { rawValue }

        /// 侧栏标题。`@MainActor` 是因为 `.t` 要读全局 I18n 单例 —— 语言表只在主线程改，
        /// 让翻译只发生在主线程比给单例加锁更简单也更快。
        @MainActor
        var title: String {
            switch self {
            case .status: return "状态".t
            case .nodes: return "节点".t
            case .devices: return "设备".t
            case .subscriptions: return "订阅".t
            case .settings: return "设置".t
            case .logs: return "日志".t
            case .about: return "关于".t
            }
        }

        /// Remix 图标码点（`IconFont.remix` 字体渲染）。与 Qt `qml/Main.qml` 的
        /// navIcons 逐项相同：dashboard / global / device / rss /
        /// settings-3 / file-list-3 / information。
        var icon: String {
            switch self {
            case .status: return "\u{EC14}"
            case .nodes: return "\u{EDCF}"
            case .devices: return "\u{EC2E}"
            case .subscriptions: return "\u{F09F}"
            case .settings: return "\u{F0E6}"
            case .logs: return "\u{ECEF}"
            case .about: return "\u{EE59}"
            }
        }
    }

    /// 起始页。`COAST_INITIAL_PAGE=<0..6>` 可指定 —— 开发期截图/验证某一页时不必手点，
    /// 也让「这页渲染对不对」这类检查可复现。不设时就是状态页。
    public var currentPage: Page = {
        guard let raw = ProcessInfo.processInfo.environment["COAST_INITIAL_PAGE"],
              let index = Int(raw), let page = Page(rawValue: index) else { return .status }
        return page
    }()

    /// 设备详情窗当前显示的那台设备。
    ///
    /// 与 Qt 的 `devices.selectedDevice` 同一个意思：详情窗显示的**永远是当前选中的那台**，
    /// 窗口开着时点列表里另一台，窗口内容跟着换，不用来回开关窗口。
    /// 详情窗是独立顶层窗（不在主窗的 environment 链里），所以这份「选中」必须放在共享状态上。
    public struct SelectedDevice: Sendable, Equatable {
        public var discovered: LanBrowser.Device
        public var online: Bool
        public var rejection: RedirectTargets.Rejection?

        public init(discovered: LanBrowser.Device, online: Bool,
                    rejection: RedirectTargets.Rejection?) {
            self.discovered = discovered
            self.online = online
            self.rejection = rejection
        }
    }

    public var selectedDevice: SelectedDevice?

    /// 供**独立窗口** scene 取用的那一份 `AppState`。
    ///
    /// SwiftUI 的 `Window` scene 不在主窗的 environment 链里，拿不到 `RootView` 注入的
    /// 那个实例。而这个应用**只有一份状态**（核心、轮询、台账都只能有一份），
    /// 所以主窗建好时把它登记在这里，独立窗口按需取。
    @MainActor public static var sharedForWindows: AppState?

    // MARK: 后端

    public private(set) var config: AppConfig
    public let controller: CoastController
    public let clash: ClashService
    public let subscriptions: SubscriptionStore
    public let history: HistoryStore
    public let devices: DeviceStore
    public let theme: Theme
    /// 「延迟」卡的四个数。
    public let latency = LatencyMonitor()

    // MARK: 今日流量（数据来自历史库，跨重启保留）

    public private(set) var todayHourly: [Int64] = []
    public private(set) var todayTop: [HistoryStore.GroupTotal] = []
    public private(set) var todayTotal: Int64 = 0

    /// 实时连接列表(连接查看器用)。每拍 /connections 快照解析而来,不额外发请求。
    public private(set) var connections: [ConnectionRow] = []

    /// 连接账本：在上面那份**活的**快照之外，还记住已经断掉的连接（标为离线）。
    /// 连接窗要的是「刚才那条连到哪去了」，只看活快照答不了 —— 断掉的下一拍就没了。
    /// 与 Qt 的 `ConnectionsModel` 同语义，打开窗口时 reset 一次。
    public private(set) var connectionLedger = ConnectionLedger()
    /// 本次会话的流量构成(直连 vs 代理)。同一份快照攒出来,不额外发请求。
    public private(set) var composition = TrafficComposition()

    /// 每台设备的实时速率与会话累计（设备详情窗用）。同样是同一份快照攒出来的。
    public private(set) var deviceTraffic = DeviceTraffic()
    /// 口径：只算走代理的流量。与 Qt 版的默认一致。
    public var trafficProxyOnly = true { didSet { refreshTodayTraffic() } }
    /// 维度：进程 / 域名。（Qt 版还有「设备」，那一维依赖设备台账，见 PLAN 阶段 6/9。）
    public var trafficDimension: HistoryStore.Dimension = .process { didSet { refreshTodayTraffic() } }

    /// 被代理设备的 (IP, 别名) 列表，供连接行标注发起设备。
    /// 每帧都查一次数据库太浪费，而这份名单只在设备开关变动时才会变。
    public var proxiedDeviceLabels: [(ip: String, alias: String)] {
        // ★ **全部台账设备**，不只是「正在被代理」的那些 —— Qt 的 `deviceNameFor()`
        //   是拿整张台账去匹配源 IP 的。只看代理中的话，一台认得出名字、但此刻没开代理的
        //   设备，它的连接在列表里只会显示成一串 IP。
        // 没起过备注名的用扫描存下来的主机名顶上 —— 台账现在存了身份，
        // 连接行不必再为一台明明认得出名字的设备显示一串 IP。
        devices.all().filter { !$0.lastIP.isEmpty }
            .map { (ip: $0.lastIP, alias: $0.alias.isEmpty ? $0.hostname : $0.alias) }
    }

    // MARK: 局域网安全告警

    /// 当前成立的 ARP 欺骗告警。设备页据此显示警示条。
    public private(set) var securityAlerts: [ArpWatch.Alert] = []
    private let arpWatch = ArpWatch()
    private let arpThrottle = ArpAlertThrottle()
    /// 告警留存：一条威胁出现后在横幅里待到连续 150 秒没再观察到为止（Qt 的 `kSecTtlMs`）。
    /// 直接用每轮的检测结果替换的话，横幅会随 ARP 绑定来回翻而每隔几秒闪一次。
    private let arpRetention = ArpAlertRetention()
    /// v6 争抢检测的基线：设备 v6 → 设备真实 MAC，在**观察到干净绑定时**逐步攒起来
    /// （见 `runArpWatchOnce`）。不能每轮从当前邻居表现取——地址一旦被投毒就锚不住了。
    private var v6Baseline: [String: String] = [:]

    // MARK: 日志

    /// 「主日志」标签页：**只有程序自己的**动作与结论（`.notice` / `.routine`）。
    ///
    /// 以前核心的 stdout 也整条塞进来，于是这一页在核心 debug 刷屏时根本没法读 ——
    /// 「程序做了什么」被淹在几千行连接日志里。两条流现在按 `LogKind` 彻底分开。
    ///
    /// 环形，**必须有上限**：无界数组跑一晚上就是几百 MB，而日志页只看得到最近几屏。
    /// **索引 0 = 最新**，与 Qt 的 `LogEntryModel` 一致（它 prepend 到第 0 行、
    /// 界面最新置顶）。
    public private(set) var logs: [LogEntry] = []

    /// 「Clash 内核」标签页：**只有核心进程吐出来的原文**。单独存一份而不是每次渲染
    /// `logs.filter` —— 过滤要扫全部 2000 条，而这个数组每条日志只写一次。
    public private(set) var coreLogs: [LogEntry] = []

    /// 页脚那一行。只放**程序侧、值得看见的**那些（`.notice`）——
    /// 核心原文和例行回执都不进来，理由见 `LogKind`。
    public private(set) var lastLog = ""
    private static let logCapacity = 2000

    /// 日志页是否正开着。核心原文只在开着时才收 —— 见 `setLogsPageVisible`。
    private var logsPageVisible = false

    public struct LogEntry: Identifiable, Sendable {
        public let id = UUID()
        /// 已格式化的时间戳。与 Qt 一样在 append 时定型（`yyyy-MM-dd HH:mm:ss`），
        /// 不在渲染时算 —— 滚动时每帧跑 DateFormatter 是纯浪费。
        public let time: String
        public let message: String
        public let severity: LogSeverity
    }

    /// 固定格式、固定历法的时间戳。**必须锁 `en_US_POSIX` + 公历**：
    /// 跟随用户区域设置的话，日本历/佛历用户看到的年份会是 R8、2569 这种，
    /// 而这一栏的用途是和核心日志、系统日志对时间。
    private static let logTimeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.calendar = Calendar(identifier: .gregorian)
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f
    }()

    // MARK: 更新检查
    //
    // 放在 AppState 而不是关于页的 `@State` 里，是因为**两处**要读它：关于页的版本行，
    // 以及侧栏版本行右上角那两颗角标（"new" / "core"）。Qt 那边同样是一个共享的
    // `about` 控制器同时喂这两处 —— 各自查一遍的话，两处会显示不一致的结论。

    public private(set) var appRelease: UpdateChecker.Release?
    public private(set) var checkingUpdate = false
    /// 一次性提示：已最新 / 检查失败。对齐 `qml/AboutPage.qml` 的 `page.hint`。
    public private(set) var updateHint = ""
    public private(set) var updateHintIsError = false
    /// 内核有新版。**查不到本地内核版本时恒为 false** —— 宁可漏报也不误报一颗常亮的角标。
    public private(set) var coreUpdateAvailable = false

    public var appUpdateAvailable: Bool {
        guard let appRelease else { return false }
        return UpdateChecker.isNewer(remote: appRelease.tag, than: AppInfo.version)
    }

    /// 查程序 + 内核两条。核心在跑时经本机代理出网 —— 直连 api.github.com 在部分网络下必然 403/超时。
    public func checkUpdates() async {
        checkingUpdate = true
        updateHint = ""
        defer { checkingUpdate = false }

        let checker = UpdateChecker(includePrerelease: config.receiveBeta,
                                    proxyPort: controller.isCoreRunning ? config.mixedPort : nil)
        do {
            appRelease = try await checker.latestAppRelease()
            updateHintIsError = false
            updateHint = appUpdateAvailable ? "" : "已是最新版本".t
        } catch {
            updateHintIsError = true
            updateHint = "检查失败：".t + error.localizedDescription
        }

        // 内核那条**单独失败、单独静默**：程序更新查成功而内核查失败时，不该把
        // 关于页那行提示改写成「检查失败」—— 用户关心的主要是程序自身那条。
        // 注意 `try?` 作用在返回 `String?` 的抛出函数上会**压平**成一层 `String?` ——
        // 「网络失败」和「上游没给 tag」在这里合并成同一个 nil，两者的处置本来就一样。
        // 内核用「不一样即可更新」而非「更大」（`coreHasUpdate` 的注释解释了为什么）。
        if let localCore = CoreVersion.local(), let remote = try? await checker.latestCoreTag() {
            coreUpdateAvailable = UpdateChecker.coreHasUpdate(remote: remote, local: localCore)
        } else {
            coreUpdateAvailable = false
        }
    }

    /// 台账改动的版本号。加一次 = 「设备表变了，谁在显示它谁就重读」。
    ///
    /// 详情窗是**独立窗口**，它改了备注名/类型/策略，设备页那份 `ledger` 快照并不知道 ——
    /// 原来要等下一轮扫描（几秒）列表里的图标才跟着变，中间那几秒像是「点了没反应」。
    /// `DeviceStore` 是普通的 SQL 层、不可观察，所以由这里出一个可观察的信号。
    public private(set) var ledgerRevision = 0

    public func ledgerDidChange() {
        ledgerRevision &+= 1
        refreshHistoryDeviceMap()
    }

    /// 把台账里的 IP→MAC 映射喂给历史库 —— 它在**落盘那一刻**把连接归到设备头上。
    /// 设备开关变动或扫描完一轮时调一次即可，不必每拍。
    public func refreshHistoryDeviceMap() {
        var map: [String: String] = [:]
        for device in devices.all() where !device.lastIP.isEmpty {
            map[device.lastIP] = device.mac
        }
        history.setDeviceMap(map)
    }

    /// 连接窗每次打开时清空账本 —— 否则上次会话的离线连接会一直挂在列表里。
    public func resetConnectionLedger() { connectionLedger.reset() }

    /// 打开发布页。关于页的入口，也是更新窗在**开发期直跑**（没有可替换的 .app）时的回退 ——
    /// 正常安装形态下的程序更新走 `AppUpdater` 自动下载替换，不经这里。
    public func openReleasePage() {
        guard let appRelease,
              let url = URL(string: "https://github.com/ClashrAuto/clashauto/releases/tag/\(appRelease.tag)")
        else { return }
        NSWorkspace.shared.open(url)
    }

    // MARK: 派生显示值

    public var upText: String { Formatting.rate(clash.up) }
    public var downText: String { Formatting.rate(clash.down) }
    public var connectionsCount: Int { clash.connectionsCount }
    public var totalDownText: String { Formatting.bytes(clash.downloadTotal) }

    /// 采样节拍。每推进一拍就 +1 —— 带宽折线靠它知道「新点是什么时候进来的」，
    /// 才能把整条曲线**连续左滑**过去，而不是每来一个样本整条跳一格。
    ///
    /// 用计数器而不是让视图自己比对数组：数组到了上限之后长度不再变，`onChange(of:count)`
    /// 从此再也不触发；而末位的值经常连着好几拍都是 0，比值也认不出「来了新的一拍」。
    public private(set) var pollTick: UInt64 = 0

    /// 带宽图的采样序列。长度与图上的点数一致（`BandwidthChart.pointCount` = 42，
    /// 即 40 个可见 + 2 个富余）—— **横轴就是「最近 40 秒」，与 Qt 相同**。
    /// 原来留 60 拍，同样的宽度里塞 60 秒，一次尖峰看起来比 Qt 窄一截。
    public private(set) var bandwidthSamples: [(up: Double, down: Double)] = []
    private static let bandwidthWindow = BandwidthChart.pointCount

    public init() {
        let loaded = (try? AppConfigLoader.load()) ?? AppConfig()
        config = loaded
        theme = Theme(dark: loaded.theme.lowercased() != "light")
        controller = CoastController(config: loaded)
        clash = ClashService(config: loaded)
        subscriptions = SubscriptionStore(config: loaded)
        history = HistoryStore()
        devices = DeviceStore()

        controller.deviceStore = devices
        // 这条流里既有 controller 自己的编排消息、也有核心进程的 stdout，靠 `LogKind`
        // 分流（以前整条被当成「核心日志」，两个标签页于是都不纯）。
        controller.onLog = { [weak self] message, kind in self?.append(log: message, kind: kind) }
        // `COAST_LOG_FILE` 是无头排障的口子（见 `mirrorToFile`），核心原文正是那时最要紧的
        // 东西 —— 那种场景下没有界面、日志页永远打不开，所以订阅直接常开。
        controller.streamsCoreOutput = Self.logFilePath != nil
        // 核心意外死亡是必须打扰用户的少数几件事之一：系统代理刚被撤掉，网络行为会突变,
        // 不说一声的话用户只会觉得「网怎么突然不走代理了」。
        controller.onCoreUnexpectedlyExited = {
            Notifier.post(title: "核心意外退出".t, body: "已自动关闭系统代理以避免断网".t)
        }
        clash.onLog = { [weak self] message, kind in self?.append(log: message, kind: kind) }
        // 切节点成功 → 按「切换节点时弹出通知」决定弹不弹(nodeSwitchNote 之前是死开关)
        clash.onNodeSelected = { [weak self] name in
            guard let self, self.config.nodeSwitchNote else { return }
            Notifier.post(title: "已切换节点".t, body: name)
        }
        // 历史库消费**同一份** /connections 快照，不为此多发一次请求。
        clash.onConnectionsSnapshot = { [weak self] connections in
            self?.history.observe(connections)
            let rows = ConnectionRow.parse(connections)
            self?.connections = rows
            self?.connectionLedger.merge(rows)
            // 本机自己发出的连接归到「本机」那一行 —— 否则全机器最忙的一台恒显示 0。
            self?.deviceTraffic.observe(rows, localIP: DeviceStore.localLANAddress() ?? "")
            // 「用量最多」那一列要显示设备名，而设备名只有 AppState 认得（台账在它手上）。
            let proxied = self?.proxiedDeviceLabels ?? []
            self?.composition.observe(connections) { sourceIP, _ in
                ConnectionRow.deviceLabel(sourceIP: sourceIP, proxied: proxied)
            }
        }
    }

    /// 启动：接上流量采样、拉起核心、开始轮询。
    ///
    /// `COAST_NO_AUTOSTART=1` 时**不自动起核心** —— 本地调 UI 时不想每次都真的把系统代理
    /// 改掉，无头冒烟测试也需要这个。
    public func start() {
        clash.start()
        startBandwidthSampling()
        startTodayTrafficRefresh()
        applySystemAppearanceIfNeeded()   // 启动时若跟随系统,先对齐一次
        startSystemAppearanceObserver()   // 之后系统外观变了也跟上
        startSubscriptionAutoUpdate()     // 定时自动更新订阅
        startArpWatch()                   // 盯着有没有别人在做 ARP 欺骗
        startLatencyMonitor()             // 直连 / 到路由 / DNS / 代理 四个延迟
        refreshHistoryDeviceMap()         // 历史库落盘时才认得出「这条连接是哪台设备的」
        // 启动就查一次更新 —— 侧栏的 "new" / "core" 角标是**不进关于页也要看得见**的，
        // 那正是它存在的意义。Qt 同样在启动路径上调 `AboutController::check()`。
        // 延后 3 秒:核心刚起来时经它出网的成功率高得多(直连 api.github.com 常年 403)。
        Task { [weak self] in
            try? await Task.sleep(for: .seconds(3))
            await self?.checkUpdates()
        }
        // .app 被替换过(应用内更新/从 DMG 拖覆盖)就重注册 helper —— 否则 status() 照报
        // enabled，XPC 却永远无人应答。详见 MacHelperClient.ensureRegisteredForCurrentBuild。
        // 放进 Task：自愈可能要注销 + 退避重试，最坏几秒钟，不能卡在启动路径上。
        Task { [weak self] in
            let heal = await MacHelperClient.ensureRegisteredForCurrentBuild()
            if heal.isNoteworthy {
                self?.append(log: String(format: "免密助手：%@".t, "\(heal)"))
            }
        }

        guard ProcessInfo.processInfo.environment["COAST_NO_AUTOSTART"] != "1" else {
            append(log: "COAST_NO_AUTOSTART=1，跳过自动启动核心".t)
            return
        }
        Task {
            await controller.startCore()
            // 台账里还开着代理的设备要重新接管 —— 接管随上次退出被复原了，只有开关状态是持久的。
            await controller.resumeDeviceTakeover()
            await startGatewayTestDeviceIfRequested()
        }
    }

    /// 真机联调钩子：`COAST_GATEWAY_TESTDEV=<设备IP>` 启动即接管该设备，免去点界面开关。
    ///
    /// 与 Qt 版 `COAST_GATEWAY_TESTDEV` 同名同语义。走的是**与界面开关完全相同**的那条路
    /// （写台账 → `rebuildConfig()` → `syncRedirect()` → helper），所以它验的是真实实现，
    /// 不是一条测试专用的旁路。MAC 从本机 ARP 表解析 —— 解析不到就不接管（接管的硬前提）。
    private func startGatewayTestDeviceIfRequested() async {
        let env = ProcessInfo.processInfo.environment
        guard let ip = env["COAST_GATEWAY_TESTDEV"], !ip.isEmpty else { return }
        guard let mac = LanBrowser.arpMap()[ip] else {
            append(log: "COAST_GATEWAY_TESTDEV=\(ip)：ARP 表里没有它，先让它发点流量再试") // i18n-ignore: 只在联调环境变量下出现
            return
        }
        _ = devices.setProxyEnabled(mac: mac, true, ip: ip)
        append(log: "COAST_GATEWAY_TESTDEV=\(ip)（\(mac)）：已开代理，正在接管") // i18n-ignore: 只在联调环境变量下出现
        await controller.rebuildConfig()
    }

    public func shutdown() async {
        clash.stop()
        latency.stop()
        // 在途的长连接也各落一条 —— 否则一条挂了几小时的连接永远进不了库。
        history.flush(includingLive: true)
        await controller.stopCore()
    }

    // MARK: 跟随系统深浅色

    /// 跟随系统时,把主题的明暗对齐当前系统外观。手选主题(autoTheme 关)时不动。
    func applySystemAppearanceIfNeeded() {
        guard config.autoTheme else { return }
        theme.dark = Self.systemIsDark()
    }

    static func systemIsDark() -> Bool {
        // 用 `NSApplication.shared`(恒返回共享实例)而不是全局 `NSApp` —— 后者在 app 启动
        // 完成前是 nil,早期读它会崩。`AppleInterfaceStyle` 兜底:那是系统外观在全局域里的标记
        // ("Dark"=深色,不存在=浅色),完全不依赖 AppKit 是否就绪。
        if let match = NSApplication.shared.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) {
            return match == .darkAqua
        }
        return UserDefaults.standard.string(forKey: "AppleInterfaceStyle")?.lowercased() == "dark"
    }

    /// 系统深浅色切换会发这个分布式通知(改「系统设置 → 外观」时)。只在跟随模式下响应。
    private func startSystemAppearanceObserver() {
        DistributedNotificationCenter.default.addObserver(
            forName: Notification.Name("AppleInterfaceThemeChangedNotification"),
            object: nil, queue: .main) { [weak self] _ in
            self?.applySystemAppearanceIfNeeded()
        }
    }

    // MARK: 订阅自动更新

    /// 每 `autoUpdateMinutes` 分钟自动更新所有订阅;内容变了才重建配置。0 = 关。
    /// 周期短时「内容没变就不重建」很关键 —— 否则每次到点都白重载一次核心。
    private func startSubscriptionAutoUpdate() {
        let minutes = config.autoUpdateMinutes
        guard minutes > 0 else { return }
        Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(Double(minutes) * 60))
                guard let self else { return }
                var changed = false
                for index in self.subscriptions.load().indices {
                    let result = await self.subscriptions.updateSubscription(at: index)
                    changed = changed || result.changed
                }
                if changed {
                    self.append(log: "订阅自动更新:有变化,已重建配置".t)
                    await self.controller.rebuildConfig()
                }
            }
        }
    }

    /// 「延迟」卡：喂给它网关与当前节点，然后让它自己按 10 秒一轮跑。
    ///
    /// 网关与当前节点都会变（换网络、切节点），所以要持续同步 —— 只在启动时喂一次的话，
    /// 换了 Wi-Fi 之后「到路由」还在测老网关，显示的是一个早就不相干的数。
    private func startLatencyMonitor() {
        latency.start()
        Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.latency.gatewayIP = LanTopology.defaultGateway()?.ip ?? ""
                // 当前选中节点的延迟直接取核心的测速结果，不自己再连一遍。
                if let current = self.clash.nodes.first(where: { $0.active }) {
                    self.latency.proxyName = current.now.isEmpty ? current.name : current.now
                    self.latency.proxyDelay = current.delay > 0 ? current.delay : LatencyProbe.unknown
                } else {
                    self.latency.proxyName = ""
                    self.latency.proxyDelay = LatencyProbe.untested
                }
                try? await Task.sleep(for: .seconds(5))
            }
        }
    }

    /// ARP 欺骗巡检。
    ///
    /// 30 秒一轮：ARP 表本身就是缓存，变化不会比这更快被本机学到；而且这条路径要跑
    /// `arp -an` 子进程，更密没有意义。**接管没开时也要跑** —— 「有人在冒充网关」
    /// 与我们开没开代理无关，恰恰是这种时候用户更需要知道。
    private func startArpWatch() {
        Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.runArpWatchOnce()
                try? await Task.sleep(for: .seconds(30))
            }
        }
        startProxiedAddressFollow()
        startPowerWatch()
    }

    /// 系统睡眠 / 唤醒。**Swift 版此前一行都没有**（Qt 版有 `PowerWatcher_mac.mm`），
    /// 而 MacBook 合盖睡眠恰恰是这个功能最高频的场景。
    ///
    /// 不处理的后果不是「慢一点」：本机一睡，转发就停了，而被接管设备的 ARP 还钉在本机 MAC 上、
    /// PF 的 rdr 规则也还挂着 —— 那些设备**直接断网**，而且要等 ARP 老化才恢复（macOS 的表项能
    /// 存活十几分钟）。关机路径早就有还原，睡眠这条一直是空的。
    ///
    /// `NSWorkspace.willSleepNotification` 是**同步**投递的：系统会等观察者回调返回后才真正挂起
    /// （有几秒预算），所以在回调里把「撤接管 + 发复原 ARP」做完是安全的，这正是我们要的。
    /// 两个通知都只在真正的系统睡眠/唤醒时发，不含屏幕休眠。
    private func startPowerWatch() {
        let center = NSWorkspace.shared.notificationCenter
        center.addObserver(forName: NSWorkspace.willSleepNotification,
                           object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.suspended = true   // 置位后周期性任务不再把接管挂回去（见 resume 侧说明）
                self.append(log: "系统即将睡眠：撤销设备接管，让它们回到真网关".t)
            }
            // 撤销要在挂起**之前**完成。通知是同步投递的，这里等它做完再返回。
            let semaphore = DispatchSemaphore(value: 0)
            Task { @MainActor [weak self] in
                await self?.controller.withdrawTakeoverForSleep()
                semaphore.signal()
            }
            _ = semaphore.wait(timeout: .now() + 3)
        }
        center.addObserver(forName: NSWorkspace.didWakeNotification,
                           object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.suspended = false  // 先解封，否则下面这次重挂会被自己拦掉
                self.append(log: "系统已唤醒：重新接管设备".t)
                Task { await self.controller.resumeDeviceTakeover() }
            }
        }
        installPowerTestHook()
    }

    /// `COAST_POWER_TESTHOOK=1` 时用 SIGUSR1/SIGUSR2 手动触发睡眠/唤醒路径（与 Qt 版同名同语义）。
    ///
    /// 为什么必须有：这条路只有真让机器睡下去才走得到，而**真睡一台远程测试机是有去无回的赌**
    /// （SSH 断开，唤不回来就没了）。有了这个钩子，撤接管/复原 ARP/重新接管这几步都能在
    /// 不睡机器的前提下验证。只在设了环境变量时安装，正式使用不受影响。
    private func installPowerTestHook() {
        guard ProcessInfo.processInfo.environment["COAST_POWER_TESTHOOK"] == "1" else { return }
        // signal(2) 的处理器里能做的事极少，所以只用 DispatchSource 把信号转成主线程上的回调。
        for (sig, isSleep) in [(SIGUSR1, true), (SIGUSR2, false)] {
            signal(sig, SIG_IGN) // 交给 DispatchSource 处理，别让默认行为把进程杀了
            let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
            source.setEventHandler { [weak self] in
                guard let self else { return }
                let center = NSWorkspace.shared.notificationCenter
                self.append(log: isSleep ? "SIGUSR1(testhook) → 模拟睡眠" : "SIGUSR2(testhook) → 模拟唤醒") // i18n-ignore: 只在 COAST_POWER_TESTHOOK 下出现
                center.post(name: isSleep ? NSWorkspace.willSleepNotification
                                          : NSWorkspace.didWakeNotification, object: nil)
            }
            source.resume()
            powerTestSources.append(source)
        }
    }

    /// DispatchSource 必须被持有，否则 setEventHandler 之后就被回收、信号再也不触发。
    private var powerTestSources: [DispatchSourceSignal] = []

    /// 是否处于「已挂起」状态。置位期间周期性的地址跟随/重挂一律跳过 ——
    /// 否则刚发出去的复原帧会被自己的重挂盖掉（Linux 那侧真机抓包实测过：复原后 961ms
    /// 重投毒帧就上来了，设备最终仍指着睡着的本机）。
    private var suspended = false

    /// 跟随被代理设备的地址变化（DHCP 续约 / 断线重连）。
    ///
    /// ★ **不修的话是静默彻底断网**，不是"慢一点"：PF 的 rdr 规则写的是
    ///   `from <设备IP> -> 127.0.0.1:redir`，ARP 投毒也按 IP 单播。设备一换地址，
    ///   规则全部落空 —— 而 ARP 那边它仍把本机当网关，于是包发过来、我们既不重定向也不 NAT，
    ///   有去无回。真机实测（虚拟设备从 .201 换到 .251）：**成功率直接 0%**，而台账、
    ///   PF 规则 30 秒后仍是旧地址。（Linux 那侧同样的场景还能靠普通转发跑到 67.6%，
    ///   macOS 这条更脆。）
    ///
    /// 为什么不能指望原有路径：台账的地址只在 `LanBrowser.scan()` 的结果里，而那个扫描
    /// **写在设备页的视图里**（`DevicesPage.scan()`）—— 用户不打开设备页就永远不跑；
    /// 而且 `DeviceStore.updateAddress` 全项目**没有任何调用者**。也就是说地址跟随这件事
    /// 此前根本没接线。
    ///
    /// 这里复用 ArpWatch 已经在读的系统邻居表（`arp -an`），不额外发探测；
    /// **只在真有设备被代理时才跑**，否则整段跳过，空载零开销。
    private func startProxiedAddressFollow() {
        Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(5))
                guard let self else { return }
                guard !self.suspended else { continue } // 挂起期间不跟随、不重挂
                let proxied = self.devices.proxiedDevices()
                guard !proxied.isEmpty else { continue }
                // 系统邻居表是 ip→mac；这里要的是「某个 MAC 现在在哪个 IP」，反过来建一张。
                //
                // ★ 一个 MAC 对应多个表项时不能随手取一个：邻居表里常有还没老化的陈旧条目，
                //   而字典反转的取值顺序是不定的 —— 真机就抖过，同一台设备被依次学成
                //   .251 → .239 → .201，中间那次是别的机器的地址、直接把接管挂错了地方。
                //
                //   但也**不能一律跳过**：设备换址之后「旧条目还在 + 新条目也在」恰恰是**常态**
                //   （macOS 的 ARP 条目要十几分钟才老化），一律跳过等于永远不跟随 —— 实测就是
                //   换址 14 秒后台账、PF 规则仍是旧地址、设备 0%。
                //
                //   ⚠️ **已知缺口，别当它修完了**：歧义时这里只能跳过，于是「旧条目尚未老化」
                //   这个最常见的 DHCP 场景仍然跟不上（实测换址 14s 后台账/PF 规则仍是旧地址、
                //   设备 0%），要等旧条目自己老化（macOS 上十几分钟）才会恢复。
                //   试过在歧义时用 TCP 探活来分辨死活，**不成立** —— `LatencyProbe.tcpRTT`
                //   把「连接被拒(RST，说明主机活着)」和「超时(主机不在)」都归成 unknown，
                //   拿它判会把活着的设备判死。app 这一层根本没有可靠的死活信号。
                //
                //   真正无歧义的证据在**帧**里：源 MAC 与源 IP 同时出现，一眼就知道谁在哪。
                //   Linux 那侧正是这么修的（`GatewayWorker::learnDeviceV4`，3 秒内跟上）。
                //   macOS 上只有 **helper** 拿得到帧（它为 ARP/NDP 投毒开着 BPF），
                //   所以完整的修法是让 helper 学到之后经 XPC 通知 app —— 那要动 XPC 契约，
                //   是独立的一件事，没有和这次一起做。
                var ipsOfMac: [String: [String]] = [:]
                for (ip, mac) in LanBrowser.arpMap() { ipsOfMac[mac.lowercased(), default: []].append(ip) }
                let ipOfMac = ipsOfMac.compactMapValues { $0.count == 1 ? $0[0] : nil }

                var changed = false
                for device in proxied {
                    guard let current = ipOfMac[device.mac.lowercased()],
                          !current.isEmpty, current != device.lastIP else { continue }
                    // 这个地址已经属于**另一台**被代理的设备 → 不认（旧址被 DHCP 分给别人，
                    // 或链路上有源 MAC/IP 不一致的帧）。认了会把 A 的接管改挂到 B 的地址上。
                    if proxied.contains(where: { $0.mac != device.mac && $0.lastIP == current }) {
                        continue
                    }
                    _ = self.devices.setProxyEnabled(mac: device.mac, true, ip: current)
                    self.append(log: String(format: "设备 %@ 地址变了：%@ → %@，重新接管".t,
                                            device.mac, device.lastIP, current))
                    changed = true
                }
                if changed {
                    // 重建配置（规则里的 SRC-IP 要跟着走）并把接管按新地址重新下发。
                    await self.controller.rebuildConfig()
                }
            }
        }
    }

    private func runArpWatchOnce() {
        guard let gateway = LanTopology.defaultGateway() else { return }
        let localMACs = LanTopology.localMACs()
        // 只盯**正在代理**的设备：没接管的设备其 ARP 变化与我们无关，报了只是噪音。
        var proxied: [String: String] = [:]
        for device in devices.proxiedDevices() where !device.lastIP.isEmpty {
            proxied[device.lastIP] = device.mac
        }
        let snapshot = ArpWatch.Snapshot(arp: LanBrowser.arpMap(),
                                         gatewayIP: gateway.ip, gatewayMAC: gateway.mac,
                                         localMACs: localMACs,
                                         proxiedDevices: proxied)
        var alerts = arpWatch.evaluate(snapshot)

        // —— IPv6（NDP）监视：本网络有 v6 路由器才做（与 v4 共用 Alert/节流/留存/界面）——
        if let gw6 = LanTopology.defaultGatewayV6() {
            let neighbors = LanTopology.ndpNeighborMap()
            let proxiedMACs = Set(devices.proxiedDevices().map { $0.mac.lowercased() })
            // 基线：把当前绑到**代理设备 MAC** 的 v6 记进去（只在干净时攒），并剔除已不再代理的。
            for (v6, mac) in neighbors where proxiedMACs.contains(mac) { v6Baseline[v6] = mac }
            v6Baseline = v6Baseline.filter { proxiedMACs.contains($0.value.lowercased()) }
            let snap6 = ArpWatch.SnapshotV6(neighbors: neighbors,
                                            routerLL: gw6.routerLL, routerMAC: gw6.routerMAC,
                                            localMACs: localMACs, proxiedV6: v6Baseline)
            alerts += arpWatch.evaluateV6(snap6)
        }

        securityAlerts = arpRetention.absorb(alerts)
        // 通知仍只对**本轮真的观察到**的那些发 —— 留存是给界面看的，不该让 TTL 内的
        // 旧告警反复触发通知。
        for alert in arpThrottle.filter(alerts) {
            switch alert.kind {
            case .gatewaySpoofed:
                let body = String(format: "%@ 正在冒充网关，可能在监听或代理你的流量".t,
                                  alert.offenderMAC)
                append(log: body)
                Notifier.post(title: "检测到 ARP 欺骗".t, body: body)
            case .deviceContended:
                let body = String(format: "%@ 也在劫持你代理的设备 %@".t,
                                  alert.offenderMAC, alert.subjectIP)
                append(log: body)
                Notifier.post(title: "设备被争抢".t, body: body)
            }
        }
    }

    /// 今日流量卡的刷新。**不跟着每秒轮询走** —— 那是几条 GROUP BY 聚合查询，
    /// 每秒跑一次纯属浪费；这张卡的数字慢几秒没有任何影响。
    private func startTodayTrafficRefresh() {
        Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.refreshTodayTraffic()
                try? await Task.sleep(for: .seconds(10))
            }
        }
    }

    private func refreshTodayTraffic() {
        let scope: HistoryStore.Scope = trafficProxyOnly ? .proxyOnly : .all
        todayHourly = history.todayHourly(scope: scope)
        // 设备维度要显示台账里的名字而不是一串 MAC —— 历史库不认识台账，喂给它。
        let names = Dictionary(devices.all().map { ($0.mac, $0.alias.isEmpty ? $0.hostname : $0.alias) },
                               uniquingKeysWith: { first, _ in first })
        todayTop = history.todayTop(dimension: trafficDimension, scope: scope,
                                    deviceNames: names)
        todayTotal = history.todayTotal(scope: scope)
    }

    private func startBandwidthSampling() {
        Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.pollTick &+= 1
                self.bandwidthSamples.append((Double(self.clash.up), Double(self.clash.down)))
                if self.bandwidthSamples.count > Self.bandwidthWindow {
                    self.bandwidthSamples.removeFirst(self.bandwidthSamples.count - Self.bandwidthWindow)
                }
                try? await Task.sleep(for: .seconds(1))
            }
        }
    }

    // MARK: 动作

    /// 设置页点「应用」后把新配置推给各后端组件。
    /// 只更新内存里的那份 —— 落盘由设置页逐键 `persist` 负责（那样才不会覆盖用户手写的注释）。
    public func applyConfig(_ newConfig: AppConfig) {
        config = newConfig
        controller.updateConfig(newConfig)
        subscriptions.updateConfig(newConfig)
        theme.dark = newConfig.theme.lowercased() != "light"
    }

    /// 新设备提醒开关。**立即落盘** —— 这是个开关不是草稿，不该等到别处点「应用」。
    public func setNewDeviceAlert(_ on: Bool) {
        config.newDeviceAlert = on
        AppConfigLoader.persist(key: "newDevice", bool: on)
    }

    /// 已经见过的设备 MAC。首轮扫描**只记录不提醒** ——
    /// 否则第一次打开设备页会把网络里所有设备一次性全弹出来。
    private var knownDeviceMACs: Set<String> = []
    private var deviceBaselineTaken = false

    /// 设备页每轮扫描后调用：有没见过的设备就提醒。
    public func noticeDevices(_ macs: [String]) {
        let seen = Set(macs.filter { !$0.isEmpty })
        guard deviceBaselineTaken else {
            knownDeviceMACs = seen
            deviceBaselineTaken = true
            return
        }
        let fresh = seen.subtracting(knownDeviceMACs)
        knownDeviceMACs.formUnion(seen)
        guard config.newDeviceAlert, !fresh.isEmpty else { return }
        for mac in fresh.sorted() {
            append(log: String(format: "发现新设备 %@".t, mac))
        }
        Notifier.post(title: "发现新设备".t,
                      body: String(format: "局域网里出现了 %d 台没见过的设备".t, fresh.count))
    }

    public func toggleCore() { Task { await controller.toggleCore() } }
    public func toggleProxy() { Task { await controller.toggleProxy() } }
    public func toggleTun() { Task { await controller.toggleTun() } }

    public func setMode(_ mode: String) { clash.setMode(mode) }
    public func closeConnection(id: String) { clash.closeConnection(id: id) }
    public func closeAllConnections() { clash.clearConnections() }
    public func selectNode(_ name: String) { clash.selectNode(name) }

    /// 禁用**正在使用**的那个节点：从订阅池摘除 → 重建配置 → 立刻重拉列表。
    ///
    /// 组行的 `now` 非空时禁的是它的叶子（`节点选择 → 自动选择 → HK-6` 禁的是 HK-6）——
    /// 禁掉组本身没有意义，用户想踢掉的是那个具体的、此刻正在服务的节点。
    ///
    /// 重拉列表这一步不能省：不拉的话被禁的节点还留在界面上，再点一次就切到一个
    /// 配置里已经没有的节点，核心直接报错。
    public func disableCurrentNode(_ node: NodeInfo) {
        let live = node.now.isEmpty ? node.name : node.now
        guard subscriptions.disableNode(liveName: live) else {
            append(log: String(format: "「%@」不在任何订阅里,无法禁用".t, live))
            return
        }
        append(log: String(format: "已禁用节点「%@」,正在重建配置".t, live))
        Task {
            await controller.rebuildConfig()
            clash.refreshNodes()
        }
    }

    /// 页脚模式下拉的档位。
    ///
    /// `clash.mode` 存的是规范值（Rule/Global/Direct），而下拉显示的是本地化文案 ——
    /// 不能拿显示串去 `indexOf`，切语言后会回显错档。
    public var modeIndex: Int {
        switch clash.mode {
        case "Global": return 1
        case "Direct": return 2
        default: return 0
        }
    }

    /// 页脚模式下拉的显示文案。**必须是计算属性而不是 `static let`** —— 常量在首次访问时
    /// 求值并永久缓存，语言切换后不会再变。
    @MainActor
    public static var modeTitles: [String] { ["规则".t, "全局".t, "直连".t] }

    /// 追加一条日志，按 `LogKind` 分流：
    ///
    /// | kind | 主日志 | Clash 内核 | 页脚 |
    /// |---|---|---|---|
    /// | `.notice`  | ✓ | | ✓ |
    /// | `.routine` | ✓ | | |
    /// | `.core`    | | ✓（且仅在日志页开着时）| |
    ///
    /// 以前是一个 `isCore` 布尔，核心日志**两边都进**、且随便哪条都能占住页脚 ——
    /// 三处显示于是都在讲同一件事：最近刷屏的那一行。
    private func append(log message: String, kind: LogKind = .notice) {
        // 核心原文：没人在看就**到此为止**，连时间戳都不格式化。
        // （`COAST_LOG_FILE` 是无头排障的口子，开着时照旧落盘，不受页面开关影响。）
        if kind == .core, !logsPageVisible, Self.logFilePath == nil { return }

        let trimmed = message.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        let entry = LogEntry(time: Self.logTimeFormatter.string(from: Date()),
                             message: trimmed,
                             severity: LogSeverity.of(trimmed))
        switch kind {
        case .core:
            if logsPageVisible { Self.prepend(entry, into: &coreLogs) }
        case .notice, .routine:
            Self.prepend(entry, into: &logs)
        }
        if kind == .notice { lastLog = trimmed }
        Self.mirrorToFile(entry.time + " " + trimmed)
    }

    /// 日志页开/关。**只影响核心那条流** —— 主日志得一直收着，页脚那一行要用它。
    ///
    /// 核心原文才是需要省的那一头：mihomo 在 debug 级别下每秒几十行，而这些行在
    /// 没人看的时候会一路走完「切行 → 跳主 actor → 格式化时间戳 → 判严重级别 →
    /// 插进 2000 条数组的第 0 位」，最后被下一屏挤掉，谁也没看见。
    public func setLogsPageVisible(_ visible: Bool) {
        guard logsPageVisible != visible else { return }
        logsPageVisible = visible
        // `COAST_LOG_FILE` 开着时订阅常开 —— 无头排障没有界面，日志页永远打不开，
        // 而那正是最需要核心原文的场景。
        controller.streamsCoreOutput = visible || Self.logFilePath != nil
        // 关页时把这一份丢掉：它只在页面开着的这段时间里有意义，留着白占内存。
        if !visible { coreLogs = [] }
    }

    /// `COAST_LOG_FILE=<路径>` 时把日志**同时**追加到该文件。
    ///
    /// 为什么需要它：app 的日志平时只进「日志」页，无头排障时一个字都看不到。而 macOS 上
    /// 想从外面接住 app 的输出出奇地难 —— 真机上逐个试过并都失败：
    ///   · 在 `launchctl asuser …` 外层重定向：那条命令会把进程重新挂进用户的 launchd 会话，
    ///     **stdio 被脱离**，日志文件恒为 0 字节；
    ///   · 启动脚本内部 `exec > file` 再 `print`：Swift 的 stdout 重定向到文件是**全缓冲**，
    ///     几行短日志会一直躺在缓冲区里不落盘；
    ///   · 改用 `NSLog`：统一日志里也搜不到（GUI app 的子系统过滤）。
    /// 于是改成 app **自己**往文件写：路径由我们决定（用户目录下，权限没问题），
    /// 每条 open→write→close，**不带缓冲**，进程被强杀也不会丢最后几行。
    ///
    /// 只在设了环境变量时才开，正常使用零开销、零副作用。
    private static let logFilePath: String? = {
        ProcessInfo.processInfo.environment["COAST_LOG_FILE"].flatMap { $0.isEmpty ? nil : $0 }
    }()

    private static func mirrorToFile(_ line: String) {
        guard let path = logFilePath else { return }
        guard let data = (line + "\n").data(using: .utf8) else { return }
        if !FileManager.default.fileExists(atPath: path) {
            FileManager.default.createFile(atPath: path, contents: nil)
        }
        guard let handle = FileHandle(forWritingAtPath: path) else { return }
        defer { try? handle.close() }
        try? handle.seekToEnd()
        try? handle.write(contentsOf: data)
    }

    /// 最新置顶 + 封顶。`insert(at: 0)` 是 O(n) 的搬移，但 n 封顶 2000、每条日志只搬一次，
    /// 远比渲染侧每帧倒序一遍便宜。
    private static func prepend(_ entry: LogEntry, into list: inout [LogEntry]) {
        list.insert(entry, at: 0)
        if list.count > logCapacity {
            list.removeLast(list.count - logCapacity)
        }
    }
}
