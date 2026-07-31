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

        /// SF Symbol。顺序与 Qt 版侧栏一致：状态/节点/设备/订阅/设置/日志/关于。
        var symbol: String {
            switch self {
            case .status: return "gauge.with.dots.needle.33percent"
            case .nodes: return "globe"
            case .devices: return "laptopcomputer.and.iphone"
            case .subscriptions: return "dot.radiowaves.up.forward"
            case .settings: return "gearshape"
            case .logs: return "list.bullet.rectangle"
            case .about: return "info.circle"
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
    /// 本次会话的流量构成(直连 vs 代理)。同一份快照攒出来,不额外发请求。
    public private(set) var composition = TrafficComposition()
    /// 口径：只算走代理的流量。与 Qt 版的默认一致。
    public var trafficProxyOnly = true { didSet { refreshTodayTraffic() } }
    /// 维度：进程 / 域名。（Qt 版还有「设备」，那一维依赖设备台账，见 PLAN 阶段 6/9。）
    public var trafficDimension: HistoryStore.Dimension = .process { didSet { refreshTodayTraffic() } }

    /// 被代理设备的 (IP, 别名) 列表，供连接行标注发起设备。
    /// 每帧都查一次数据库太浪费，而这份名单只在设备开关变动时才会变。
    public var proxiedDeviceLabels: [(ip: String, alias: String)] {
        devices.proxiedDevices().map { (ip: $0.lastIP, alias: $0.alias) }
    }

    // MARK: 局域网安全告警

    /// 当前成立的 ARP 欺骗告警。设备页据此显示警示条。
    public private(set) var securityAlerts: [ArpWatch.Alert] = []
    private let arpWatch = ArpWatch()
    private let arpThrottle = ArpAlertThrottle()

    // MARK: 日志

    /// 环形日志。**必须有上限** —— 核心在 debug 级别下每秒能刷几十条，无界数组跑一晚上
    /// 就是几百 MB，而日志页只看得到最近几屏。
    public private(set) var logs: [LogEntry] = []
    public private(set) var lastLog = ""
    private static let logCapacity = 2000

    public struct LogEntry: Identifiable, Sendable {
        public let id = UUID()
        public let time: Date
        public let message: String
    }

    // MARK: 派生显示值

    public var upText: String { Formatting.rate(clash.up) }
    public var downText: String { Formatting.rate(clash.down) }
    public var connectionsCount: Int { clash.connectionsCount }
    public var totalDownText: String { Formatting.bytes(clash.downloadTotal) }

    /// 带宽图的采样序列。只留最近 60 拍（约 1 分钟），图上也就画这么多。
    public private(set) var bandwidthSamples: [(up: Double, down: Double)] = []
    private static let bandwidthWindow = 60

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
        controller.onLog = { [weak self] message in self?.append(log: message) }
        // 核心意外死亡是必须打扰用户的少数几件事之一：系统代理刚被撤掉，网络行为会突变,
        // 不说一声的话用户只会觉得「网怎么突然不走代理了」。
        controller.onCoreUnexpectedlyExited = {
            Notifier.post(title: "核心意外退出".t, body: "已自动关闭系统代理以避免断网".t)
        }
        clash.onLog = { [weak self] message in self?.append(log: message) }
        // 切节点成功 → 按「切换节点时弹出通知」决定弹不弹(nodeSwitchNote 之前是死开关)
        clash.onNodeSelected = { [weak self] name in
            guard let self, self.config.nodeSwitchNote else { return }
            Notifier.post(title: "已切换节点".t, body: name)
        }
        // 历史库消费**同一份** /connections 快照，不为此多发一次请求。
        clash.onConnectionsSnapshot = { [weak self] connections in
            self?.history.observe(connections)
            self?.connections = ConnectionRow.parse(connections)
            self?.composition.observe(connections)
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
        Task { await controller.startCore() }
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
    }

    private func runArpWatchOnce() {
        guard let gateway = LanTopology.defaultGateway() else { return }
        // 只盯**正在代理**的设备：没接管的设备其 ARP 变化与我们无关，报了只是噪音。
        var proxied: [String: String] = [:]
        for device in devices.proxiedDevices() where !device.lastIP.isEmpty {
            proxied[device.lastIP] = device.mac
        }
        let snapshot = ArpWatch.Snapshot(arp: LanBrowser.arpMap(),
                                         gatewayIP: gateway.ip, gatewayMAC: gateway.mac,
                                         localMACs: LanTopology.localMACs(),
                                         proxiedDevices: proxied)
        let alerts = arpWatch.evaluate(snapshot)
        securityAlerts = alerts
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
        todayTop = history.todayTop(dimension: trafficDimension, scope: scope)
        todayTotal = history.todayTotal(scope: scope)
    }

    private func startBandwidthSampling() {
        Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
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

    public func toggleCore() { Task { await controller.toggleCore() } }
    public func toggleProxy() { Task { await controller.toggleProxy() } }
    public func toggleTun() { Task { await controller.toggleTun() } }

    public func setMode(_ mode: String) { clash.setMode(mode) }
    public func closeConnection(id: String) { clash.closeConnection(id: id) }
    public func closeAllConnections() { clash.clearConnections() }
    public func selectNode(_ name: String) { clash.selectNode(name) }

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

    private func append(log message: String) {
        let trimmed = message.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        logs.append(LogEntry(time: Date(), message: trimmed))
        if logs.count > Self.logCapacity {
            logs.removeFirst(logs.count - Self.logCapacity)
        }
        lastLog = trimmed
    }
}
