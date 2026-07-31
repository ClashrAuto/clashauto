import Foundation

/// `configDir/rules.json` 的读写 —— 设置页里「自定义规则」与「自定义区域分组」的持久层。
///
/// 这个文件的**消费者是 `ConfigBuilder.applyCustomRules`**：
///   • `rule` → 前插到 `full.yaml` 的 `rules:` 顶部；
///   • `area` → 按正则匹配节点名生成策略组，并加进主选择组。
/// 两边的字段名必须完全一致，改一个就要改另一个。
public struct RulesStore: Sendable {

    /// 一条自定义路由规则。
    public struct Rule: Sendable, Equatable, Identifiable, Codable {
        /// `DOMAIN-SUFFIX` / `IP-CIDR` / `PROCESS-NAME` / `MATCH` …
        public var type: String
        /// 目标策略：节点名、策略组名，或 `DIRECT` / `REJECT`。
        public var node: String
        /// 匹配值。`MATCH` 是兜底规则，没有这一段。
        public var value: String

        public var id: String { "\(type)\u{1}\(value)\u{1}\(node)" }

        public init(type: String = "DOMAIN-SUFFIX", node: String = "DIRECT", value: String = "") {
            self.type = type
            self.node = node
            self.value = value
        }

        /// 生成到 full.yaml 里的那一行（与 `ConfigBuilder.applyCustomRules` 的拼法一致）。
        public var ruleLine: String {
            type == "MATCH" ? "\(type),\(node)" : "\(type),\(value),\(node)"
        }
    }

    /// 一个按正则匹配节点名的自定义策略组。
    public struct Area: Sendable, Equatable, Identifiable, Codable {
        public var name: String
        /// `url-test` / `select` / `fallback` …
        public var type: String
        /// 匹配节点名的正则。
        public var rule: String

        public var id: String { name }

        public init(name: String = "", type: String = "url-test", rule: String = "") {
            self.name = name
            self.type = type
            self.rule = rule
        }
    }

    /// 规则类型下拉的选项。与 mihomo 支持的规则类型对齐；`MATCH` 排最后 ——
    /// 它是兜底规则，放前面容易被误选成普通规则。
    public static let ruleTypes = [
        "DOMAIN", "DOMAIN-SUFFIX", "DOMAIN-KEYWORD",
        "IP-CIDR", "IP-CIDR6", "GEOIP", "GEOSITE",
        "SRC-IP-CIDR", "DST-PORT", "SRC-PORT",
        "PROCESS-NAME", "PROCESS-PATH",
        "MATCH",
    ]

    public static let areaTypes = ["url-test", "select", "fallback", "load-balance"]

    private let directory: URL

    public init(directory: URL = AppPaths.configDir) {
        self.directory = directory
    }

    public var path: URL { directory.appendingPathComponent("rules.json") }

    // MARK: - 读写

    public func load() -> (rules: [Rule], areas: [Area]) {
        guard let data = try? Data(contentsOf: path),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return ([], []) }

        let rules = (root["rule"] as? [[String: Any]] ?? []).map { entry in
            Rule(type: entry["type"] as? String ?? "",
                 node: entry["node"] as? String ?? "",
                 value: entry["value"] as? String ?? "")
        }
        let areas = (root["area"] as? [[String: Any]] ?? []).map { entry in
            Area(name: entry["name"] as? String ?? "",
                 type: entry["type"] as? String ?? "url-test",
                 rule: entry["rule"] as? String ?? "")
        }
        return (rules, areas)
    }

    @discardableResult
    public func save(rules: [Rule], areas: [Area]) -> Bool {
        // 手写字典而不是 JSONEncoder：这个文件**与 Qt 版共用**，键名和结构必须逐字对齐，
        // 交给 Encoder 去推导反而容易在某次重构里悄悄改掉字段名。
        let root: [String: Any] = [
            "rule": rules.map { ["type": $0.type, "node": $0.node, "value": $0.value] },
            "area": areas.map { ["name": $0.name, "type": $0.type, "rule": $0.rule] },
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: root,
                                                     options: [.prettyPrinted, .sortedKeys])
        else { return false }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return (try? data.write(to: path, options: .atomic)) != nil
    }

    // MARK: - 校验

    /// 保存前的校验。返回 nil 表示没问题，否则是给用户看的原因。
    ///
    /// 这一步不能省：坏规则会让核心**整份配置**加载失败，而用户只会看到「突然全断网」，
    /// 完全无从关联到自己刚才加的那条规则。
    public static func validate(_ rule: Rule) -> String? {
        let type = rule.type.trimmingCharacters(in: .whitespaces)
        let node = rule.node.trimmingCharacters(in: .whitespaces)
        let value = rule.value.trimmingCharacters(in: .whitespaces)

        if type.isEmpty { return "请选择规则类型" }
        if node.isEmpty { return "请填写目标策略" }
        // MATCH 是兜底规则，只有两段；其余都必须有匹配值
        if type != "MATCH", value.isEmpty { return "请填写匹配值" }
        // 逗号会把一行规则切成更多段，直接写坏 full.yaml 的规则表
        if value.contains(",") || node.contains(",") { return "匹配值与目标策略里不能有逗号" }

        if type == "IP-CIDR" || type == "IP-CIDR6" || type == "SRC-IP-CIDR" {
            guard value.contains("/") else { return "\(type) 需要写成网段，例如 10.0.0.0/8" }
        }
        if type == "DST-PORT" || type == "SRC-PORT" {
            guard let port = Int(value), port > 0, port <= 65535 else { return "端口要在 1..65535" }
        }
        return nil
    }

    public static func validate(_ area: Area) -> String? {
        let name = area.name.trimmingCharacters(in: .whitespaces)
        let rule = area.rule.trimmingCharacters(in: .whitespaces)
        if name.isEmpty { return "请填写分组名" }
        if rule.isEmpty { return "请填写匹配节点名的正则" }
        // 正则非法时 ConfigBuilder 会静默跳过这个组 —— 用户加了组却什么都没发生，
        // 不如在这里当场说清楚。
        guard (try? NSRegularExpression(pattern: rule)) != nil else { return "正则表达式无效" }
        return nil
    }
}
