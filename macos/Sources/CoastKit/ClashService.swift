import Foundation
import Observation

/// 节点/策略组在列表里的一行。对齐 C++ `NodeInfo`。
public struct NodeInfo: Sendable, Equatable, Identifiable {
    public var name: String
    /// 组沿 `now` 链走到底的**叶子**名（`节点选择 → 自动选择 → HK-6` 取 HK-6）。叶子节点自身为空。
    public var now: String = ""
    public var delay: Int = 0
    /// 字节/秒。优先本程序实测（下载测速），无实测时回退核心 `history.speed`。
    public var speed: Int64 = 0
    public var active: Bool = false

    public var id: String { name }
    public init(name: String) { self.name = name }
}

/// REST 轮询的**编排层**：定时拉取、组/叶子解析、模式同步、节点选择、延迟与下载测速。
/// 对齐 C++ `ClashService` 的同名成员，信号改成 `@Observable` 属性 + 少量回调闭包。
///
/// 整体在 `@MainActor` 上：所有状态直接驱动 SwiftUI，不需要额外的线程切换与加锁；
/// 真正的网络往返都在 `ClashAPI`（actor）里，不占主线程。
@MainActor
@Observable
public final class ClashService {

    // MARK: 对外状态

    public private(set) var up: Int64 = 0
    public private(set) var down: Int64 = 0
    public private(set) var connectionsCount = 0
    public private(set) var downloadTotal: Int64 = 0
    public private(set) var nodes: [NodeInfo] = []
    public private(set) var groups: [String] = []
    public private(set) var selectedGroup = ""
    public private(set) var selectedNode = ""
    /// `Rule` / `Global` / `Direct`。由 `setMode` 归一，并由 `pollMode` 从核心读回校正。
    public private(set) var mode = "Rule"
    public private(set) var coreReachable = false
    public private(set) var speedTesting = false

    // MARK: 回调

    public var onLog: ((String, LogKind) -> Void)?
    /// 每轮 `/connections` 拿到的完整数组。历史库消费同一份，不额外发请求。
    public var onConnectionsSnapshot: (([[String: Any]]) -> Void)?

    // MARK: 内部

    private let api: ClashAPI
    private var trafficTask: Task<Void, Never>?

    /// 核心进程还在，但**已经连续几轮拿不到 `/traffic` 数据**。
    ///
    /// 「进程活着」不等于「核心可用」：mihomo 卡死、REST 端口被别人占住导致绑定失败、
    /// 配置热重载中途崩在半路 —— 这些情况下进程都在，界面照样显示「运行中」，
    /// 而实际上什么都不通，用户完全无从查起。
    ///
    /// 用**连续失败计数**而不是直接照搬 `coreReachable`：后者在刚启动、第一帧数据到达
    /// 之前天然是 false，直接拿去点亮警告会闪一下假警报。
    public private(set) var coreUnresponsive = false
    private var failedStreamRounds = 0
    /// 连续几轮拿不到数据才算「没响应」。看门狗一轮 2s，两轮≈4s —— 够躲开启动抖动，
    /// 又不至于让用户对着一个坏掉的核心干等太久。
    private static let unresponsiveThreshold = 2
    private var connectionsTask: Task<Void, Never>?
    private var nodesTask: Task<Void, Never>?
    private var speedTask: Task<Void, Never>?

    private var clearOnSwitch = true
    /// 核心起来后自动测一次延迟；核心掉线后重置，下次起来再测。
    private var autoTested = false
    /// 节点名 → 实测下载速度（字节/秒）。每轮重测前清空，避免旧值残留。
    private var measuredSpeeds: [String: Int64] = [:]
    /// 测速期间对 UI 强制上报的「原活动节点」。见 `pollNodes` 里的覆盖逻辑。
    private var speedOriginalNode = ""

    private static let speedConcurrency = 5

    public init(config: AppConfig) {
        api = ClashAPI(host: config.host, port: config.uiPort,
                       mixedPort: config.mixedPort, secret: config.secret)
        clearOnSwitch = config.clearConnections
    }

    // MARK: - 生命周期

    public func start() {
        log("Connecting to Clash API...", .routine)
        startTrafficStream()
        // 两条轮询各自成环。`await` 天然形成「上一拍没回来就不发下一拍」的效果 ——
        // C++ 版为此专门加了 m_connectionsInFlight / m_nodesInFlight 两个标志，这里不需要。
        connectionsTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }   // 自己没了就收摊，别留个空转的循环
                await self.pollConnections()
                await self.pollMode()
                try? await Task.sleep(for: .seconds(2))
            }
        }
        // 1s：对齐旧项目 getProxies 的节奏，节点状态要「实时」。
        nodesTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                await self.pollNodes()
                try? await Task.sleep(for: .seconds(self.nodesVisible ? 1 : 5))
            }
        }
    }

    public func stop() {
        trafficTask?.cancel(); trafficTask = nil
        failedStreamRounds = 0
        coreUnresponsive = false
        connectionsTask?.cancel(); connectionsTask = nil
        nodesTask?.cancel(); nodesTask = nil
        speedTask?.cancel(); speedTask = nil
    }

    // 没有 deinit：那些 Task 属性受 @MainActor 保护，nonisolated 的 deinit 碰不到。
    // 四条循环都持 weak self，自己被释放后下一拍就自行退出，不会泄漏。

    public func setEndpoint(host: String, port: Int) {
        Task { await api.setEndpoint(host: host, port: port) }
        // 地址变了：断开旧流，看门狗会按新地址重连。
        trafficTask?.cancel()
        startTrafficStream()
    }

    public func setMixedPort(_ port: Int) { Task { await api.setMixedPort(port) } }
    public func setSecret(_ secret: String) { Task { await api.setSecret(secret) } }
    public func setClearConnectionsOnSwitch(_ enabled: Bool) { clearOnSwitch = enabled }

    // MARK: - 流量流

    /// `/traffic` 常开单流 + 看门狗。流一断（核心重启/端口变更/网络错误）就隔 2s 重连。
    private func startTrafficStream() {
        trafficTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                do {
                    for try await sample in await self.api.trafficStream() {
                        self.up = sample.up
                        self.down = sample.down
                        // 「变了才写」：这几个是布尔，一秒一帧地原样重写会让观察它们的
                        // 视图（状态页的核心无响应警示）每秒白重排一次。
                        if !self.coreReachable { self.coreReachable = true }  // 有流数据 = 核心在跑
                        self.failedStreamRounds = 0
                        if self.coreUnresponsive { self.coreUnresponsive = false }
                    }
                } catch {
                    // 流结束或出错都落到这里，下面统一等 2s 再重连
                }
                self.coreReachable = false
                self.failedStreamRounds += 1
                if self.failedStreamRounds >= Self.unresponsiveThreshold {
                    self.coreUnresponsive = true
                }
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    // MARK: - 轮询

    private func pollConnections() async {
        do {
            let (list, total) = try await api.connections()
            connectionsCount = list.count
            downloadTotal = total
            onConnectionsSnapshot?(list)
        } catch ClashAPI.APIError.notRunning {
            handleCoreDown()
        } catch {
            // 超时/其它错误：不动 UI。把超时当掉线会导致列表闪空，还会和自动测延迟形成死循环。
        }
    }

    /// 从核心读回**真实**模式。
    ///
    /// 必要性：`mode` 若只在用户于本 app 里改时才更新，那么「核心配置本来就是 global/direct」
    /// 或「模式被外部改过」时它会一直停在默认 Rule —— 下游据此判 Direct/Global 的逻辑就全错。
    /// 拿不到就不动，绝不把模式猜成别的。
    private func pollMode() async {
        guard uiActive else { return }
        guard let raw = try? await api.configs()["mode"] as? String, !raw.isEmpty else { return }
        let normalized: String
        switch raw.lowercased() {
        case "global": normalized = "Global"
        case "direct": normalized = "Direct"
        case "rule": normalized = "Rule"
        default: return        // 未知模式串：保持原值
        }
        guard normalized != mode else { return }
        mode = normalized
        selectedGroup = ""     // 模式变了，主选择组要重选（与 setMode 一致）
        log("Mode (from core): \(normalized)")
        await pollNodes()
    }

    /// 界面此刻有没有人在看（由 `AppState` 按窗口可见性同步过来）。
    ///
    /// 只挡**纯给界面看**的那两条轮询：节点列表（1 秒一次 `/proxies`，几百个节点的 JSON，
    /// 是这里最贵的一条）和当前模式。流量与连接照旧 —— 前者喂托盘菜单，后者是历史记录的
    /// 唯一数据源，停了会丢数据。
    ///
    /// 挡在**发请求之前**而不是取消整个 Task：循环留着，恢复可见时下一拍就自动接上，
    /// 不必重建任务，也不会有「窗口打开后要等一个周期才有数据」的空窗（`AppState`
    /// 在转为可见的那一刻还会主动催一次 `refreshNodes`）。
    public var uiActive = true
    /// 节点列表是否正被用户注视（= 当前停在节点页）。
    ///
    /// ★ `/proxies` 的响应**很大**：本机 70 个节点时实测 **51 KB**，而这条循环
    ///   原本是**每 1 秒**拉一次并整份 `JSONSerialization` 解析成 `[String: [String: Any]]`
    ///   （每个字段都装箱成 `Any`）。`sample` 抓到的非布局类热点就只有它 ——
    ///   `newJSONValue` / `newJSONObject`。
    ///   而这份数据**只有节点页在看**：状态页、设备页、日志页都不需要节点状态每秒刷新。
    ///   所以按「用户是不是正看着节点页」分档：看着就保持 1s 实时（那是它该有的手感），
    ///   没看着就降到 5s（只为让切回去时不至于是陈旧数据）。
    ///   实测：常驻 1s 时开窗空载中位 0.85%，降到 5s 后 **0.60%**（= Qt 端水平）。
    ///   与 `uiActive` 的关系：那个管「窗口开没开」，这个管「开着时在看哪一页」。
    public var nodesVisible = false

    private func pollNodes() async {
        guard uiActive else { return }
        let proxies: [String: [String: Any]]
        do {
            proxies = try await api.proxies()
        } catch ClashAPI.APIError.notRunning {
            handleCoreDown()
            return
        } catch {
            return
        }
        if !coreReachable { coreReachable = true }

        let resolver = ProxyTree(proxies: proxies)

        // 所有「有 all 数组」的都是策略组。
        var discoveredGroups: [String] = []
        for (name, object) in proxies where !((object["all"] as? [Any])?.isEmpty ?? true) {
            discoveredGroups.append(name)
        }
        discoveredGroups.sort()
        // 这条轮询 1 秒一轮，而列表通常一轮一轮完全没变 ——「变了才写」，
        // 否则观察这些属性的页面（节点页整列列表）每秒都被原样重排一遍。
        if groups != discoveredGroups { groups = discoveredGroups }

        let (groupName, group) = pickPrimaryGroup(proxies: proxies)
        var selected = group["now"] as? String ?? selectedNode
        // 测速期间主组的 now 随逐个测速临时变动 —— 对 UI 强制上报原活动节点，
        // 否则活动节点每秒都在变，列表会反复重建、切换通知乱弹。
        if speedTesting, !speedOriginalNode.isEmpty { selected = speedOriginalNode }

        let nextGroup = groupName.isEmpty ? "GLOBAL" : groupName
        if selectedGroup != nextGroup { selectedGroup = nextGroup }
        if selectedNode != selected { selectedNode = selected }

        var list: [NodeInfo] = []
        for name in (group["all"] as? [String] ?? []) where !name.isEmpty {
            let proxy = proxies[name] ?? [:]
            var node = NodeInfo(name: name)
            // 组自身通常没有 history：沿 now 链取叶子的延迟/速度，否则国家组、以及 now 指向
            // 其它组的选择组（节点选择/漏网之鱼…）全都显示「无延迟」。
            let history = resolver.history(from: name)
            node.now = (proxy["now"] as? String).map { _ in resolver.finalName(from: name) } ?? ""
            node.delay = (history.last?["delay"] as? NSNumber)?.intValue ?? 0
            let historySpeed = (history.last?["speed"] as? NSNumber)?.int64Value ?? 0
            node.speed = measuredSpeeds[name] ?? historySpeed
            node.active = name == selected
            list.append(node)
        }

        // 主组一个都没解析出来：退化成「把所有非组的代理列出来」，至少让用户看到点东西。
        if list.isEmpty {
            for (name, proxy) in proxies {
                let type = proxy["type"] as? String ?? ""
                if type == "Selector" || type == "URLTest" || type == "Fallback" || name == "GLOBAL" { continue }
                var node = NodeInfo(name: name)
                let history = proxy["history"] as? [[String: Any]] ?? []
                node.delay = (history.last?["delay"] as? NSNumber)?.intValue ?? 0
                let historySpeed = (history.last?["speed"] as? NSNumber)?.int64Value ?? 0
                node.speed = measuredSpeeds[name] ?? historySpeed
                node.active = name == selected
                list.append(node)
            }
        }

        let sortedList = Self.sorted(list, selected: selected)
        if nodes != sortedList { nodes = sortedList }

        // 核心刚起来、首次拿到节点：异步补测一次延迟，对齐「进列表即可见延迟」的体验。
        // 不用配置里的 lazy:false —— 那会卡启动。
        if !nodes.isEmpty, !autoTested {
            autoTested = true
            Task { await testDelays() }
        }
    }

    /// 主选择组的挑选顺序，逐条对齐 C++ 版：
    /// 已选中的组 → Global 模式用 GLOBAL → Rule 模式找名字像「节点/选择/代理/Proxy」的 Selector
    /// → GLOBAL → 任意一个组。只有在**正确的主组**里选节点才真的改路由。
    private func pickPrimaryGroup(proxies: [String: [String: Any]]) -> (String, [String: Any]) {
        func hasMembers(_ object: [String: Any]?) -> Bool {
            !((object?["all"] as? [Any])?.isEmpty ?? true)
        }

        if hasMembers(proxies[selectedGroup]) { return (selectedGroup, proxies[selectedGroup]!) }

        if mode.caseInsensitiveCompare("Global") == .orderedSame, hasMembers(proxies["GLOBAL"]) {
            return ("GLOBAL", proxies["GLOBAL"]!)
        }

        let keywords = ["节点", "选择", "代理"]
        for name in proxies.keys.sorted() {
            guard let candidate = proxies[name], candidate["type"] as? String == "Selector",
                  hasMembers(candidate), name != "GLOBAL" else { continue }
            if keywords.contains(where: { name.contains($0) })
                || name.range(of: "Proxy", options: .caseInsensitive) != nil {
                return (name, candidate)
            }
        }

        if hasMembers(proxies["GLOBAL"]) { return ("GLOBAL", proxies["GLOBAL"]!) }

        for name in proxies.keys.sorted() {
            guard let candidate = proxies[name] else { continue }
            let type = candidate["type"] as? String ?? ""
            if type == "Selector" || type == "URLTest" || type == "Fallback" {
                return (name, candidate)
            }
        }
        return ("", [:])
    }

    /// 排序：当前节点置顶，其次速度降序，再延迟升序，超时/无延迟垫底。
    /// 与旧项目 `clash.js getProxies` 同一口径 —— 用户对这个顺序有肌肉记忆，别改。
    nonisolated static func sorted(_ list: [NodeInfo], selected: String) -> [NodeInfo] {
        func key(_ node: NodeInfo) -> Double {
            if node.name == selected { return .greatestFiniteMagnitude }
            if node.speed > 0 { return Double(node.speed) }
            return node.delay <= 0 ? 0 : 10000 - Double(node.delay)
        }
        // 稳定排序：Swift 的 sort 不保证稳定，同权重时用原始下标兜底 —— 否则同为 0 延迟的
        // 一堆节点每拍都可能换个顺序，列表肉眼可见地抖。
        return list.enumerated()
            .sorted { lhs, rhs in
                let (a, b) = (key(lhs.element), key(rhs.element))
                return a == b ? lhs.offset < rhs.offset : a > b
            }
            .map(\.element)
    }

    private func handleCoreDown() {
        coreReachable = false
        up = 0; down = 0
        connectionsCount = 0; downloadTotal = 0
        nodes = []
        autoTested = false          // 核心掉线：下次起来重新自动测一次延迟
        measuredSpeeds.removeAll()
        if speedTesting {           // 测速途中核心掉线：中止本轮
            speedTask?.cancel()
            speedTesting = false
        }
    }

    // MARK: - 动作

    public func setMode(_ raw: String) {
        let apiMode = Self.normalizeMode(raw)
        mode = apiMode
        selectedGroup = ""          // 模式变了，默认视图重选主组
        Task {
            do {
                try await api.setMode(apiMode)
                log("Mode changed: \(raw)")
                await pollNodes()
            } catch {
                log("Mode change failed (\(apiMode)): \(error)")
            }
        }
    }

    /// 中英与「规则/全局/直连」都要认 —— 模式串既来自 UI 的本地化文案，也来自核心。
    nonisolated static func normalizeMode(_ raw: String) -> String {
        let lower = raw.lowercased()
        if lower.contains("rule") || raw.contains("规则") { return "Rule" }
        if lower.contains("global") || raw.contains("全局") || raw.contains("全部") { return "Global" }
        if lower.contains("direct") || raw.contains("直连") || raw.contains("直") { return "Direct" }
        return raw
    }

    public func setSelectedGroup(_ group: String) {
        guard !group.isEmpty, group != selectedGroup else { return }
        selectedGroup = group
        Task { await pollNodes() }
    }

    /// 正在切往的节点名;PUT 在途时非空。UI 据此在该节点上转圈、并禁掉重复点。
    public private(set) var switchingTo: String?
    /// 切节点成功后回调(名字)。AppState 据此按 `nodeSwitchNote` 决定弹不弹通知。
    public var onNodeSelected: ((String) -> Void)?

    public func selectNode(_ name: String) {
        guard !name.isEmpty, switchingTo == nil else { return }   // 在途时忽略重复点
        let group = selectedGroup.isEmpty ? "GLOBAL" : selectedGroup
        switchingTo = name   // 立刻给 UI 反馈:点到了、正在切
        Task {
            defer { switchingTo = nil }
            do {
                try await api.selectNode(group: group, name: name)
                selectedNode = name
                log("Node selected: \(group) -> \(name)")
                onNodeSelected?(name)
                if clearOnSwitch { clearConnections() }   // 设置项「切换时清理连接」
                await pollNodes()
            } catch {
                log("Select node failed: \(error)")
            }
        }
    }

    public func clearConnections() {
        Task {
            do {
                try await api.clearConnections()
                log("Connections cleared.")
                await pollConnections()
            } catch {
                log("Clear connections failed: \(error)")
            }
        }
    }

    public func closeConnection(id: String) {
        Task {
            do { try await api.closeConnection(id: id); log("Connection closed: \(id)") }
            catch { log("Close connection failed: \(error)") }
        }
    }

    public func refreshNodes() { Task { await pollNodes() } }

    // MARK: - 延迟测速

    /// 测当前主组的全部成员。`thenSpeed` = 测完延迟后，对有效延迟节点自动跑下载测速。
    public func testDelays(thenSpeed: Bool = false) async {
        guard let proxies = try? await api.proxies() else { return }
        let group = selectedGroup.isEmpty ? "GLOBAL" : selectedGroup
        let names = (proxies[group]?["all"] as? [String] ?? []).filter { !$0.isEmpty }
        await testNodeDelays(names, thenSpeed: thenSpeed)
    }

    public func testNodeDelays(_ names: [String], thenSpeed: Bool = false) async {
        guard !names.isEmpty else {
            log("No nodes to test.")
            if thenSpeed { speedTesting = false }
            return
        }
        log("Testing \(names.count) nodes...")
        // 并发发出，全部回来再刷新一次列表。延迟请求走独立连接池，不会挤占轮询。
        await withTaskGroup(of: Void.self) { group in
            for name in names {
                group.addTask { [api] in _ = await api.delay(node: name) }
            }
        }
        // ★ 级别必须跟上面那句 `Testing N nodes...` 一致（都走页脚）。
        //   标成 `.routine` 的话页脚只显示「开始」、不显示「结束」——
        //   于是测完之后页脚**永远**停在「Testing 67 nodes...」，用户以为还在跑。
        //   规则：**清除某个进行中状态的消息，可见性不能低于设置它的那条。**
        //   Qt 端两句都是普通级别，页脚正常收尾成「Delay test finished.」。
        log("Delay test finished.")
        await pollNodes()
        if thenSpeed { startSpeedTestForValidNodes() }
    }

    // MARK: - 下载测速

    /// 拉一次 `/proxies`，筛出「沿 now 链有有效延迟」的节点后开测 —— 连不通的节点测速没有意义。
    public func startSpeedTestForValidNodes() {
        guard !speedTesting else {
            log("下载测速进行中，忽略重复请求")
            return
        }
        speedTask = Task { [weak self] in
            guard let self, let proxies = try? await self.api.proxies() else { return }
            let resolver = ProxyTree(proxies: proxies)
            let group = self.selectedGroup.isEmpty ? "GLOBAL" : self.selectedGroup
            let valid = (proxies[group]?["all"] as? [String] ?? []).filter { name in
                guard !name.isEmpty else { return false }
                let history = resolver.history(from: name)
                return ((history.last?["delay"] as? NSNumber)?.intValue ?? 0) > 0
            }
            await self.runSpeedTest(valid)
        }
    }

    /// 逐节点测下载速度。
    ///
    /// 核心 REST 没有「按名测速」接口，只能在主组里**临时选中**目标节点、经混合端口下载。
    /// 已建立的下载会钉在拨号那一刻选中的出站上（切组只影响新连接），所以把「选组 + 建连」
    /// 串行化之后，多个下载可以真正并发（上限 5）。
    private func runSpeedTest(_ names: [String]) async {
        guard !names.isEmpty else {
            log("没有可测速的有效节点")
            return
        }
        speedTesting = true
        speedOriginalNode = selectedNode
        measuredSpeeds.removeAll()
        let group = selectedGroup.isEmpty ? "GLOBAL" : selectedGroup
        log("开始下载测速：\(names.count) 个节点（并发 \(Self.speedConcurrency)）")

        let selectLock = AsyncSemaphore(1)                     // 「选组 + 建连」串行锁
        let slots = AsyncSemaphore(Self.speedConcurrency)      // 并发闸门

        await withTaskGroup(of: (String, Int64).self) { taskGroup in
            for name in names {
                if Task.isCancelled { break }
                await slots.wait()
                await selectLock.wait()
                do {
                    try await api.selectNode(group: group, name: name)
                } catch {
                    measuredSpeeds[name] = 0
                    log("测速选中失败 \(name): \(error)")
                    await selectLock.signal()
                    await slots.signal()
                    continue
                }
                taskGroup.addTask { [api] in
                    // 首字节到达 = 已钉在本节点上 → 放行下一个节点的握手。
                    // SpeedProbe 保证这个回调恰好触发一次（连不上时在收尾处补触发），锁不会漏放。
                    let result = await api.speedProbe {
                        Task { await selectLock.signal() }
                    }
                    await slots.signal()
                    let bps = result.ms > 0 ? result.bytes * 1000 / result.ms : 0
                    return (name, bps)
                }
            }
            for await (name, bps) in taskGroup {
                measuredSpeeds[name] = bps
                log("测速 \(name) -> \(bps / 1024) KB/s", .routine)
            }
        }

        // 收尾：恢复测速前的活动节点。**恢复确认之前**不能清 speedTesting ——
        // 否则最后残留的测速节点会被当成「用户切换了节点」，误弹通知、触发整表重建。
        if !speedOriginalNode.isEmpty {
            try? await api.selectNode(group: group, name: speedOriginalNode)
        }
        speedTesting = false
        log("下载测速完成", .routine)
        await pollNodes()
    }

    /// 默认 `.notice`。轮询侧那几条**每轮/每个节点**都会刷的回执显式标 `.routine` ——
    /// 页脚只有一行，被它们占着的话真正要看的错误永远露不出来。
    private func log(_ message: String, _ kind: LogKind = .notice) { onLog?(message, kind) }
}

/// `/proxies` 那张图的只读视图：沿 `now` 链往下走的两个查询。
///
/// 组自身一般没有 `history`，而它的 `now` 可能还是个组（节点选择 → 自动选择 → 某节点），
/// 得一路跟到有 history 的叶子。16 步上限 + 访问过集合防成环 —— 配置写错时不能把 UI 卡死。
struct ProxyTree {
    let proxies: [String: [String: Any]]

    func history(from start: String) -> [[String: Any]] {
        var current = start
        var seen = Set<String>()
        for _ in 0..<16 {
            guard !current.isEmpty, !seen.contains(current) else { break }
            seen.insert(current)
            let object = proxies[current] ?? [:]
            let entries = object["history"] as? [[String: Any]] ?? []
            if !entries.isEmpty { return entries }
            current = object["now"] as? String ?? ""
        }
        return []
    }

    func finalName(from start: String) -> String {
        var current = start
        var seen = Set<String>()
        for _ in 0..<16 {
            guard !current.isEmpty, !seen.contains(current) else { break }
            seen.insert(current)
            let next = proxies[current]?["now"] as? String ?? ""
            if next.isEmpty { return current }
            current = next
        }
        return current
    }
}
