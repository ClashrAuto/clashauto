import Foundation

public struct SubscriptionSummary: Sendable, Equatable, Identifiable {
    public var name = ""
    public var url = ""
    public var type = ""
    public var use = false
    public var nodeCount = 0
    public var enabledNodeCount = 0
    /// 该订阅自己的自动更新周期（分钟）。0 = 沿用全局默认。
    public var updateTime = 0

    public var id: String { "\(name)\u{1}\(url)" }
    public init() {}
}

public struct SubscriptionNodeSummary: Sendable, Equatable, Identifiable {
    public var name = ""
    public var server = ""
    public var port = ""
    public var use = true

    public var id: String { "\(server):\(port)\u{1}\(name)" }
    public init() {}
}

/// `configDir/subscribe.yaml` 的读写：订阅增删改、节点启停、远程/本地拉取与增量更新。
/// 对齐 C++ `SubscriptionStore`。
public final class SubscriptionStore: @unchecked Sendable {

    public struct UpdateResult: Sendable {
        public let ok: Bool
        public let message: String
        /// false = 更新成功但内容与上次**逐字节一致**（含每个节点的启用状态）。
        /// 调用方据此跳过 full.yaml 重建与核心热重载 —— 自动更新周期短时这是常态。
        public let changed: Bool
    }

    private var config: AppConfig
    /// 配置目录。生产上就是 `AppPaths.configDir`；可注入是为了让测试跑在临时目录里 ——
    /// 这个类每个写操作都真的落盘，不隔离的话跑一次单测就把用户的订阅覆盖了。
    private let directory: URL

    public init(config: AppConfig, directory: URL = AppPaths.configDir) {
        self.config = config
        self.directory = directory
    }

    public func updateConfig(_ config: AppConfig) { self.config = config }

    public var path: URL { directory.appendingPathComponent("subscribe.yaml") }

    // MARK: - 读

    public func load() -> [SubscriptionSummary] {
        let document = ensureDocument()
        return document.blockRanges().map { block in
            var summary = SubscriptionSummary()
            summary.name = document.subscriptionValue("name", in: block) ?? ""
            summary.url = document.subscriptionValue("url", in: block) ?? ""
            summary.type = document.subscriptionValue("type", in: block) ?? ""
            summary.use = (document.subscriptionValue("use", in: block) ?? "").lowercased() == "true"
            summary.updateTime = Int(document.subscriptionValue("updateTime", in: block) ?? "") ?? 0
            let nodes = document.nodeRanges(in: block)
            summary.nodeCount = nodes.count
            summary.enabledNodeCount = nodes.filter {
                (document.nodeValue("use", in: $0) ?? "true").lowercased() != "false"
            }.count
            return summary
        }
    }

    public func nodes(at index: Int) -> [SubscriptionNodeSummary] {
        let document = ensureDocument()
        guard let block = document.blockRange(at: index) else { return [] }
        return document.nodeRanges(in: block).map { node in
            var summary = SubscriptionNodeSummary()
            summary.name = document.nodeValue("name", in: node) ?? ""
            summary.server = document.nodeValue("server", in: node) ?? ""
            summary.port = document.nodeValue("port", in: node) ?? ""
            summary.use = (document.nodeValue("use", in: node) ?? "true").lowercased() != "false"
            return summary
        }
    }

    // MARK: - 写：订阅级

    public func setSubscriptionEnabled(at index: Int, _ enabled: Bool) -> Bool {
        mutate { document in
            document.setSubscriptionValue("use", raw: enabled ? "true" : "false", in: index)
        }
    }

    public func setSubscriptionUpdateTime(at index: Int, minutes: Int) -> Bool {
        mutate { document in
            document.setSubscriptionValue("updateTime", raw: String(max(0, minutes)), in: index)
        }
    }

    /// 新增订阅。`type` 空则记为 `sub`；`name` 空则用 URL 顶上，避免列表里出现无名条目。
    public func addSubscription(name: String, url: String, type: String) -> Bool {
        guard !url.trimmingCharacters(in: .whitespaces).isEmpty else { return false }
        var document = ensureDocument()
        // 种子是字面量 `[]`（空列表）。往下追加条目前得先把它清掉，否则得到
        // `[]\n- name: …` 这种既是 flow 又是 block 的写法，YAML 解析器直接报错。
        if document.text.trimmingCharacters(in: .whitespacesAndNewlines) == "[]" {
            document.lines = []
        }
        while document.lines.last?.trimmingCharacters(in: .whitespaces).isEmpty == true {
            document.lines.removeLast()
        }
        let finalName = name.isEmpty ? url : name
        document.lines.append("- name: \(YAMLScalar.quote(finalName))")
        document.lines.append("  url: \(YAMLScalar.quote(url))")
        document.lines.append("  type: \(YAMLScalar.quote(type.isEmpty ? "sub" : type))")
        document.lines.append("  use: true")
        document.lines.append("  list:")
        return write(document)
    }

    public func editSubscription(at index: Int, name: String, url: String, type: String) -> Bool {
        mutate { document in
            guard document.blockRange(at: index) != nil else { return false }
            let finalName = name.isEmpty ? url : name
            _ = document.setSubscriptionValue("name", raw: YAMLScalar.quote(finalName), in: index)
            _ = document.setSubscriptionValue("url", raw: YAMLScalar.quote(url), in: index)
            _ = document.setSubscriptionValue("type", raw: YAMLScalar.quote(type.isEmpty ? "sub" : type), in: index)
            return true
        }
    }

    public func removeSubscription(at index: Int) -> Bool {
        mutate { document in
            guard let block = document.blockRange(at: index) else { return false }
            document.lines.removeSubrange(block)
            // 删光了就写回字面量空列表 —— 空文件不是合法 YAML，核心与我们自己都读不了。
            if document.blockRanges().isEmpty { document.lines = ["[]"] }
            return true
        }
    }

    // MARK: - 写：节点级

    public func setNodeEnabled(subscription: Int, node nodeIndex: Int, _ enabled: Bool) -> Bool {
        mutate { document in
            guard let block = document.blockRange(at: subscription) else { return false }
            let nodes = document.nodeRanges(in: block)
            guard nodeIndex >= 0, nodeIndex < nodes.count else { return false }
            document.setNodeUse(enabled, in: nodes[nodeIndex])
            return true
        }
    }

    /// 把「实时节点名」反查回订阅里的下标。
    ///
    /// 实时名 = `「订阅节点名 - 订阅名」`（见 `ConfigBuilder`）。Qt 版还会给某些订阅的节点
    /// 加 `[speedtest]` 后缀，那是它的遗留字段 —— 这里一并容忍，好让从 Qt 版迁移过来的
    /// 用户（其 full.yaml 里可能带着这种名字）也能对上。
    ///
    /// 写成**纯函数**：真正的禁用要写盘 + 重建配置，没法在测试里随便跑；
    /// 而「名字对不对得上」恰恰是最容易错、也最值得单独钉住的一环。
    public static func locateNode(liveName: String,
                                  in catalog: [(subscription: String, nodes: [String])])
        -> (subscription: Int, node: Int)? {
        for (subIndex, entry) in catalog.enumerated() {
            for (nodeIndex, node) in entry.nodes.enumerated() {
                let base = "\(node) - \(entry.subscription)"
                if liveName == base || liveName == base + "[speedtest]" {
                    return (subIndex, nodeIndex)
                }
            }
        }
        return nil
    }

    /// 禁用一个**正在使用**的节点：从订阅池里摘除。调用方负责随后重建配置。
    ///
    /// 返回是否真的改动了。找不到对应订阅节点时返回 false —— 这不是异常：
    /// 组名、`DIRECT`、`REJECT` 这些都不是订阅里的节点，点到它们时什么都不该发生。
    @discardableResult
    public func disableNode(liveName: String) -> Bool {
        let summaries = load()
        let catalog = summaries.enumerated().map { index, summary in
            (subscription: summary.name, nodes: nodes(at: index).map(\.name))
        }
        guard let hit = Self.locateNode(liveName: liveName, in: catalog) else { return false }
        return setNodeEnabled(subscription: hit.subscription, node: hit.node, false)
    }

    public func setAllNodesEnabled(subscription: Int, _ enabled: Bool) -> Bool {
        mutate { document in
            guard let block = document.blockRange(at: subscription) else { return false }
            // **从后往前**改：setNodeUse 在缺 use 键时会插行，从前往后会让后面所有区间整体位移。
            for node in document.nodeRanges(in: block).reversed() {
                document.setNodeUse(enabled, in: node)
            }
            return true
        }
    }

    // MARK: - 更新

    /// 拉取订阅并重建其节点列表。`url` 是 `file://` 或本地路径时读本地文件。
    ///
    /// 一律直接抓订阅原始 URL、节点在本地解析（`SubParser`），**不经远程 subconverter**
    /// —— 更快、更隐私，也不依赖第三方服务活着。
    public func updateSubscription(at index: Int) async -> UpdateResult {
        let summaries = load()
        guard index >= 0, index < summaries.count else {
            return UpdateResult(ok: false, message: "订阅不存在", changed: false)
        }
        let raw = summaries[index].url.trimmingCharacters(in: .whitespaces)
        guard !raw.isEmpty else {
            return UpdateResult(ok: false, message: "订阅地址为空", changed: false)
        }

        let text: String
        if let url = URL(string: raw), let scheme = url.scheme?.lowercased(),
           scheme == "http" || scheme == "https" {
            var request = URLRequest(url: url)
            request.setValue("coast-macos", forHTTPHeaderField: "User-Agent")
            request.timeoutInterval = 30
            do {
                let (data, response) = try await URLSession.shared.data(for: request)
                if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
                    return UpdateResult(ok: false, message: "HTTP \(http.statusCode)", changed: false)
                }
                text = String(data: data, encoding: .utf8) ?? ""
            } catch {
                return UpdateResult(ok: false, message: error.localizedDescription, changed: false)
            }
        } else {
            let localPath = raw.hasPrefix("file://") ? (URL(string: raw)?.path ?? raw) : raw
            guard let content = try? String(contentsOfFile: localPath, encoding: .utf8) else {
                return UpdateResult(ok: false, message: "读取本地订阅失败: \(localPath)", changed: false)
            }
            text = content
        }
        return updateSubscription(at: index, fromText: text)
    }

    /// 不走网络的更新入口（本地文件、剪贴板粘贴、以及测试都用它）。
    public func updateSubscription(at index: Int, fromText text: String) -> UpdateResult {
        let converted = SubParser.toClashProxies(text) ?? text
        var document = ensureDocument()
        guard let block = document.blockRange(at: index) else {
            return UpdateResult(ok: false, message: "订阅不存在", changed: false)
        }
        guard document.listLine(in: block) != nil else {
            return UpdateResult(ok: false, message: "订阅条目缺少 list: 字段", changed: false)
        }

        let previousUse = nodeUseByEndpoint(document: document, block: block)
        let previousYaml = nodeYamlByEndpoint(document: document, block: block)
        let (nodeLines, count) = buildNodeList(from: converted,
                                               previousUse: previousUse,
                                               previousYaml: previousYaml)
        guard count > 0 else {
            return UpdateResult(ok: false, message: "未解析到任何节点", changed: false)
        }

        let before = document.text
        let listLine = document.listLine(in: block)!
        document.lines.replaceSubrange(listLine..<block.upperBound,
                                       with: ["  list:"] + nodeLines)
        let after = document.text
        if after == before {
            return UpdateResult(ok: true, message: "订阅内容无变化（\(count) 个节点）", changed: false)
        }
        guard write(document) else {
            return UpdateResult(ok: false, message: "写入 subscribe.yaml 失败", changed: false)
        }
        return UpdateResult(ok: true, message: "已更新 \(count) 个节点", changed: true)
    }

    // MARK: - 节点列表构建

    /// 把 `proxies:` 块转成 subscribe.yaml 的节点列表行。
    ///
    /// 三件事和 C++ 逐条对齐：
    ///   1. **保留用户对每个节点的启停**：按 `server:port` 认人，不按节点名 —— 机场经常改名，
    ///      按名字认会让用户禁用过的节点在下次更新后全部复活。
    ///   2. 丢掉源里的 `use:`/`delay:`/`speed:` —— 那是我们自己的运行时字段，不是订阅内容。
    ///   3. `increment` 开启时，把这次没出现、上次有的节点原样留下（增量更新）。
    func buildNodeList(from proxiesYAML: String,
                       previousUse: [String: Bool],
                       previousYaml: [String: [String]]) -> (lines: [String], count: Int) {
        var output: [String] = []
        var seenEndpoints = Set<String>()
        var count = 0

        var inProxies = false
        var proxiesIndent = 0
        var currentNode: [String] = []

        func finishNode() {
            guard !currentNode.isEmpty else { return }
            defer { currentNode = [] }

            var name = "", server = "", port = ""
            var hasName = false
            for line in currentNode {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                if trimmed.hasPrefix("- name:") {
                    hasName = true; name = YAMLScalar.unquote(String(trimmed.dropFirst(7)))
                } else if trimmed.hasPrefix("name:") {
                    hasName = true; name = YAMLScalar.unquote(String(trimmed.dropFirst(5)))
                } else if trimmed.hasPrefix("server:") {
                    server = YAMLScalar.unquote(String(trimmed.dropFirst(7)))
                } else if trimmed.hasPrefix("port:") {
                    port = YAMLScalar.unquote(String(trimmed.dropFirst(5)))
                }
            }
            guard hasName, nodeAllowed(name) else { return }

            output += currentNode
            let endpoint = "\(server):\(port)"
            seenEndpoints.insert(endpoint)
            output.append("      use: \(previousUse[endpoint] ?? true ? "true" : "false")")
            count += 1
        }

        for rawLine in proxiesYAML.components(separatedBy: "\n") {
            var line = rawLine.replacingOccurrences(of: "\r", with: "")
            if line.first == "\u{FEFF}" { line.removeFirst() }

            if !inProxies {
                if line.trimmingCharacters(in: .whitespaces).hasPrefix("proxies:") {
                    inProxies = true
                    proxiesIndent = line.distance(from: line.startIndex,
                                                  to: line.firstIndex(of: "p") ?? line.startIndex)
                }
                continue
            }

            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }
            let indent = line.count - trimmed.count
            // 缩进退回到 proxies: 这一级且不是列表项 = proxies 块结束（后面是 rules: 之类）
            if indent <= proxiesIndent, !trimmed.hasPrefix("- ") {
                finishNode()
                break
            }

            if trimmed.hasPrefix("- ") {
                finishNode()
                let rest = String(trimmed.dropFirst(2)).trimmingCharacters(in: .whitespaces)
                currentNode.append(rest.hasPrefix("name:")
                    ? "    - name: \(rest.dropFirst(5).trimmingCharacters(in: .whitespaces))"
                    : "    - \(rest)")
                continue
            }

            if !currentNode.isEmpty, line.hasPrefix("  ") {
                if trimmed.hasPrefix("use:") || trimmed.hasPrefix("delay:") || trimmed.hasPrefix("speed:") {
                    continue
                }
                currentNode.append("      \(trimmed)")
            }
        }
        finishNode()

        if config.increment {
            for (endpoint, lines) in previousYaml.sorted(by: { $0.key < $1.key })
            where !seenEndpoints.contains(endpoint) {
                output += lines
                count += 1
            }
        }
        return (output, count)
    }

    /// 节点名过滤。允许规则不匹配 → 丢；排除规则匹配 → 丢。正则非法时**当作没设**，
    /// 不能因为用户在设置里敲错一个字符就让所有节点消失。
    func nodeAllowed(_ name: String) -> Bool {
        if config.allowRuleEnabled, !config.allowRule.isEmpty,
           let regex = try? NSRegularExpression(pattern: config.allowRule, options: .caseInsensitive) {
            let range = NSRange(name.startIndex..., in: name)
            if regex.firstMatch(in: name, range: range) == nil { return false }
        }
        if config.noAllowRuleEnabled, !config.noAllowRule.isEmpty,
           let regex = try? NSRegularExpression(pattern: config.noAllowRule, options: .caseInsensitive) {
            let range = NSRange(name.startIndex..., in: name)
            if regex.firstMatch(in: name, range: range) != nil { return false }
        }
        return true
    }

    private func nodeUseByEndpoint(document: SubscribeDocument, block: Range<Int>) -> [String: Bool] {
        var result: [String: Bool] = [:]
        for node in document.nodeRanges(in: block) {
            let server = document.nodeValue("server", in: node) ?? ""
            let port = document.nodeValue("port", in: node) ?? ""
            guard !server.isEmpty, !port.isEmpty else { continue }
            result["\(server):\(port)"] = (document.nodeValue("use", in: node) ?? "true").lowercased() != "false"
        }
        return result
    }

    private func nodeYamlByEndpoint(document: SubscribeDocument, block: Range<Int>) -> [String: [String]] {
        var result: [String: [String]] = [:]
        for node in document.nodeRanges(in: block) {
            let server = document.nodeValue("server", in: node) ?? ""
            let port = document.nodeValue("port", in: node) ?? ""
            guard !server.isEmpty, !port.isEmpty else { continue }
            result["\(server):\(port)"] = Array(document.lines[node])
        }
        return result
    }

    // MARK: - 文件

    /// 首次运行从种子落地。种子内容是字面量 `[]`；种子缺失时自己写一个。
    private func ensureDocument() -> SubscribeDocument {
        let target = path
        let fm = FileManager.default
        if !fm.fileExists(atPath: target.path) {
            try? fm.createDirectory(at: directory, withIntermediateDirectories: true)
            if let seed = Resources.seed("subscribe.yaml"), (try? fm.copyItem(at: seed, to: target)) != nil {
                AppPaths.makeWritable(target)
            } else {
                try? "[]\n".write(to: target, atomically: true, encoding: .utf8)
            }
        }
        return SubscribeDocument(text: (try? String(contentsOf: target, encoding: .utf8)) ?? "[]\n")
    }

    private func mutate(_ body: (inout SubscribeDocument) -> Bool) -> Bool {
        var document = ensureDocument()
        guard body(&document) else { return false }
        return write(document)
    }

    private func write(_ document: SubscribeDocument) -> Bool {
        var text = document.text
        if !text.hasSuffix("\n") { text += "\n" }
        AppPaths.makeWritable(path)
        do {
            try text.write(to: path, atomically: true, encoding: .utf8)
            return true
        } catch {
            return false
        }
    }
}
