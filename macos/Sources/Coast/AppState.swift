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

    // MARK: 今日流量（数据来自历史库，跨重启保留）

    public private(set) var todayHourly: [Int64] = []
    public private(set) var todayTop: [HistoryStore.GroupTotal] = []
    public private(set) var todayTotal: Int64 = 0
    /// 口径：只算走代理的流量。与 Qt 版的默认一致。
    public var trafficProxyOnly = true { didSet { refreshTodayTraffic() } }
    /// 维度：进程 / 域名。（Qt 版还有「设备」，那一维依赖设备台账，见 PLAN 阶段 6/9。）
    public var trafficDimension: HistoryStore.Dimension = .process { didSet { refreshTodayTraffic() } }

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

        controller.onLog = { [weak self] message in self?.append(log: message) }
        clash.onLog = { [weak self] message in self?.append(log: message) }
        // 历史库消费**同一份** /connections 快照，不为此多发一次请求。
        clash.onConnectionsSnapshot = { [weak self] connections in
            self?.history.observe(connections)
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

        guard ProcessInfo.processInfo.environment["COAST_NO_AUTOSTART"] != "1" else {
            append(log: "COAST_NO_AUTOSTART=1，跳过自动启动核心".t)
            return
        }
        Task { await controller.startCore() }
    }

    public func shutdown() async {
        clash.stop()
        // 在途的长连接也各落一条 —— 否则一条挂了几小时的连接永远进不了库。
        history.flush(includingLive: true)
        await controller.stopCore()
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
