import Foundation

/// 生成核心真正吃的 `full.yaml`。
///
/// 流水线（顺序有讲究，见 `ensureFullConfig`）：
///   base(default.yaml) → 合并 plugin.yaml 的 dns/tun 块 → 覆盖端口/控制器/secret 等标量
///   → 注入订阅节点与策略组 → 自定义区域组与规则 → 私网直连 → sniffer → profile
public final class ConfigBuilder: @unchecked Sendable {

    struct SubscriptionNode {
        var name: String
        var yaml: String
    }

    struct Subscription {
        var name = ""
        var use = false
        /// 名字带 `[speedtest]` 后缀 —— 旧项目的约定，某些机场用它标可测速节点。
        var speedtest = false
        var nodes: [SubscriptionNode] = []
    }

    private var config: AppConfig
    /// 可注入，便于测试跑在临时目录（同 `SubscriptionStore`）。
    private let directory: URL

    public init(config: AppConfig, directory: URL = AppPaths.configDir) {
        self.config = config
        self.directory = directory
    }

    public func updateConfig(_ config: AppConfig) { self.config = config }

    public var fullConfigPath: URL { directory.appendingPathComponent("full.yaml") }

    // MARK: - 入口

    /// 生成 `full.yaml` 并返回路径。启动核心前必须先调它。
    @discardableResult
    public func ensureFullConfig(tunEnabled: Bool) -> URL? {
        seedIfNeeded("default.yaml")
        seedIfNeeded("plugin.yaml")

        guard let base = readSeeded("default.yaml") else { return nil }
        let plugin = readSeeded("plugin.yaml") ?? ""

        var yaml = Self.mergePlugin(base: base, plugin: plugin)
        yaml = YAMLSurgery.setScalar(yaml, key: "mixed-port", value: String(config.mixedPort))
        yaml = YAMLSurgery.setScalar(yaml, key: "external-controller",
                                     value: "'\(config.host):\(config.uiPort)'")
        if !config.secret.isEmpty {
            // 给 REST API 设访问密钥。default.yaml 通常没有这个键，setScalar 会补上。
            yaml = YAMLSurgery.setScalar(yaml, key: "secret", value: "'\(config.secret)'")
        }
        // 混合端口只监听本机 —— allow-lan 开着等于把自己变成开放代理。
        yaml = YAMLSurgery.setScalar(yaml, key: "allow-lan", value: "false")
        // 让核心把发起连接的进程名填进 metadata.process（连接窗口要显示）。
        // 用 always 而不是 strict：strict 由核心自行决定要不要查，没有 PROCESS-NAME 规则时它就不查了，
        // 于是 UI 永远拿不到进程名。代价是每条新连接多一次本机套接字表查询，核心内部有缓存。
        yaml = YAMLSurgery.setScalar(yaml, key: "find-process-mode", value: "always")
        yaml = YAMLSurgery.setNestedScalar(yaml, section: "tun", key: "enable",
                                           value: tunEnabled ? "true" : "false")
        yaml = Self.ensureProxyServerNameserver(yaml)
        yaml = Self.normalizeEmptyProxies(yaml)
        yaml = applySubscriptions(yaml, subscriptions: readSubscriptions())
        yaml = applyCustomRules(yaml)
        // ★ 私网直连必须**最后前插** —— 它和自定义规则都插在 rules: 顶部，后插的在最上面。
        //   私网要压过自定义规则：否则「访问自己家路由器后台 / 内网 NAS」也会被发到代理节点上。
        yaml = Self.applyPrivateNetworkRules(yaml, extraPrefixes: Self.localGlobal6Prefixes())
        yaml = Self.applySniffer(yaml)
        yaml = Self.applyProfilePersistence(yaml)

        let target = fullConfigPath
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try yaml.write(to: target, atomically: true, encoding: .utf8)
        } catch {
            return nil
        }
        return target
    }

    /// 只翻 `tun.enable` 这一个键（切换增强模式时热改，不重建整份配置）。
    /// 返回 false 表示文件读不到或值本来就一样 —— 调用方据此跳过热重载。
    public func writeTunEnabled(at path: URL, enabled: Bool) -> Bool {
        guard let yaml = try? String(contentsOf: path, encoding: .utf8), !yaml.isEmpty else { return false }
        let updated = YAMLSurgery.setNestedScalar(yaml, section: "tun", key: "enable",
                                                  value: enabled ? "true" : "false")
        guard updated != yaml else { return false }
        return (try? updated.write(to: path, atomically: true, encoding: .utf8)) != nil
    }

    // MARK: - plugin 合并

    /// 用 plugin.yaml 里的 `dns:` / `tun:` 块**整块覆盖** base 里的同名块。
    /// 只这两个键 —— plugin.yaml 的定位是「DNS 与 TUN 的可调参数」，不是全量覆盖层。
    static func mergePlugin(base: String, plugin: String) -> String {
        guard !plugin.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return base }
        var merged = base
        for key in ["dns", "tun"] {
            guard let range = YAMLSurgery.blockRange(plugin, key: key) else { continue }
            let block = String(plugin[range]).trimmingCharacters(in: .whitespacesAndNewlines) + "\n"
            merged = YAMLSurgery.replaceOrAppendBlock(merged, key: key, with: block)
        }
        return merged
    }

    // MARK: - 标量修补

    /// `proxies: null` → `proxies: []`，组里的 `proxies: null` → 至少给个 DIRECT。
    /// 空订阅时 default.yaml 会留下 null，核心对此直接报错拒绝加载。
    static func normalizeEmptyProxies(_ yaml: String) -> String {
        var out = yaml.replacingOccurrences(of: "(?m)^proxies:\\s*null\\s*$", with: "proxies: []",
                                            options: .regularExpression)
        out = out.replacingOccurrences(of: "(?m)^    proxies:\\s*null\\s*$",
                                       with: "    proxies:\n      - DIRECT",
                                       options: .regularExpression)
        return out
    }

    /// 注入 `dns.proxy-server-nameserver`（境内明文 DNS）。
    ///
    /// 开 TUN(auto-route) 后核心要先解析「代理服务器域名」才能拨代理。若这一步走 dns.fallback 的
    /// 境外 DoH，那条 DoH 连接会被核心自己的 TUN 路由捕获 → 命中规则丢回代理 → 拨代理又得先解析
    /// 代理服务器域名 → **死循环**，日志报 `dns resolve failed: couldn't find ip`，
    /// 表现为「所有节点无延迟、境外全打不开」。
    ///
    /// 境内明文 DNS 恒可直连、应答快，代理服务器域名通常未被污染，足以打破环路。TUN 关时也无副作用。
    /// 用户自己配了就不覆盖。
    static func ensureProxyServerNameserver(_ yaml: String) -> String {
        if yaml.range(of: "(?m)^  proxy-server-nameserver:", options: .regularExpression) != nil {
            return yaml
        }
        guard let dnsHead = yaml.range(of: "(?m)^dns:\\n", options: .regularExpression) else {
            return yaml   // 没有 dns 块（理论上不会）：不动，免得破坏结构
        }
        var out = yaml
        out.insert(contentsOf: "  proxy-server-nameserver:\n    - 223.5.5.5\n    - 119.29.29.29\n",
                   at: dnsHead.upperBound)
        return out
    }

    // MARK: - 订阅

    /// 读 `subscribe.yaml`，只留**启用的订阅**里**启用的节点**，并把节点改名成
    /// `<原名> - <订阅名>[speedtest]` —— 多个订阅撞名时不至于互相覆盖。
    func readSubscriptions() -> [Subscription] {
        let path = directory.appendingPathComponent("subscribe.yaml")
        guard let text = try? String(contentsOf: path, encoding: .utf8) else { return [] }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, trimmed != "[]" else { return [] }

        let document = SubscribeDocument(text: text)
        var result: [Subscription] = []
        for block in document.blockRanges() {
            var subscription = Subscription()
            subscription.name = document.subscriptionValue("name", in: block) ?? ""
            subscription.use = (document.subscriptionValue("use", in: block) ?? "").lowercased() == "true"
            subscription.speedtest = (document.subscriptionValue("speedtest", in: block) ?? "").lowercased() == "true"
            guard subscription.use else { continue }

            for node in document.nodeRanges(in: block) {
                // 用户禁用的节点直接不进 full.yaml
                let enabled = (document.nodeValue("use", in: node) ?? "true").lowercased() != "false"
                guard enabled else { continue }
                let name = document.nodeValue("name", in: node) ?? ""
                guard !name.isEmpty else { continue }

                let suffix = subscription.speedtest ? "[speedtest]" : ""
                let fullName = "\(name) - \(subscription.name)\(suffix)"
                var body = "  - name: \(YAMLSurgery.quote(fullName))\n"
                for index in node {
                    let line = document.lines[index]
                    guard line.hasPrefix("      ") else { continue }
                    let field = line.trimmingCharacters(in: .whitespaces)
                    // name 已经用改过的写在上面了；use/delay/speed/loading 是我们自己的运行时字段
                    if field.hasPrefix("name:") || field.hasPrefix("use:")
                        || field.hasPrefix("delay:") || field.hasPrefix("speed:")
                        || field.hasPrefix("loading:") { continue }
                    body += "    \(field)\n"
                }
                subscription.nodes.append(SubscriptionNode(name: fullName, yaml: body))
            }
            if !subscription.nodes.isEmpty { result.append(subscription) }
        }
        return result
    }

    /// 把订阅节点写进 `proxies:`，并把「订阅组 + 地区自动组 + 全部节点」灌进策略组。
    ///
    /// 两个策略组的分工（沿用 default.yaml 的既定结构）：
    ///   第 1 个 = 主选择组（🚀 节点选择）：成员是**组名 + 全部节点**，用户在这里选；
    ///   第 2 个 = 自动选择：成员只有全部节点。
    func applySubscriptions(_ yaml: String, subscriptions: [Subscription]) -> String {
        var allNames: [String] = []
        var usedNames = Set<String>()
        var proxyBlock = "proxies:"

        for subscription in subscriptions {
            for node in subscription.nodes {
                let name = node.name
                // 撞名的直接丢：核心遇到重名 proxy 会拒绝整份配置。
                guard !name.isEmpty, !usedNames.contains(name) else { continue }
                usedNames.insert(name)
                allNames.append(name)
                var body = node.yaml
                while body.hasSuffix("\n") { body.removeLast() }
                proxyBlock += "\n" + body + "\n"
            }
        }
        guard !allNames.isEmpty else { return yaml }

        var out = yaml
        if let range = YAMLSurgery.topLevelProxiesRange(out) {
            out = out.replacingCharacters(in: range, with: proxyBlock + "\n")
        }

        let autoGroupNames = Self.autoGroups(allNames).map(\.name)
        if let firstProxies = YAMLSurgery.groupProxiesLine(out, occurrence: 0) {
            var members = YAMLSurgery.groupMembers(out, at: firstProxies)
            members += subscriptions.filter { !$0.nodes.isEmpty }.map { "\($0.name) 订阅" }
            members += autoGroupNames
            members += allNames
            out = YAMLSurgery.replaceGroupMembers(out, at: firstProxies, with: deduplicated(members))
        }
        if let secondProxies = YAMLSurgery.groupProxiesLine(out, occurrence: 1) {
            out = YAMLSurgery.replaceGroupMembers(out, at: secondProxies, with: allNames)
        }
        return appendSubscriptionGroups(out, subscriptions: subscriptions, allNames: allNames)
    }

    /// 在 `rules:` 之前追加「每个订阅一个 url-test 组」+「每个地区一个 url-test 组」。
    private func appendSubscriptionGroups(_ yaml: String, subscriptions: [Subscription],
                                          allNames: [String]) -> String {
        guard let anchor = YAMLSurgery.rulesAnchor(yaml) else { return yaml }

        var groups = ""
        for subscription in subscriptions where !subscription.nodes.isEmpty {
            groups += urlTestGroup(name: "\(subscription.name) 订阅",
                                   members: subscription.nodes.map(\.name))
        }
        for group in Self.autoGroups(deduplicated(allNames)) {
            groups += urlTestGroup(name: group.name, members: group.members)
        }
        guard !groups.isEmpty else { return yaml }

        var out = yaml
        out.insert(contentsOf: groups, at: anchor)
        return out
    }

    /// **刻意不写 `lazy: false`**（默认 true）：lazy:false 会让核心在启动时同步做健康检查，
    /// 节点不可达时卡住启动、REST API 迟迟不监听。延迟改由应用起来后异步测速填充。
    private func urlTestGroup(name: String, members: [String]) -> String {
        var block = "  - name: \(YAMLSurgery.quote(name))\n"
        block += "    type: url-test\n"
        block += "    url: 'http://www.gstatic.com/generate_204'\n"
        block += "    interval: 300\n"
        block += "    proxies:\n"
        for member in members {
            block += "      - \(YAMLSurgery.quote(member))\n"
        }
        return block
    }

    /// 按节点名里的地区关键词自动分组。
    ///
    /// 返回值**按组名排序**：C++ 用的是 `QMap`（天然按键排序），策略组在配置里的先后顺序
    /// 会直接体现在 UI 列表上，顺序变了用户会以为分组乱了。
    static func autoGroups(_ nodeNames: [String]) -> [(name: String, members: [String])] {
        let patterns: [(String, String)] = [
            ("Auto - HK", "(香港|港|HK|Hong\\s*Kong)"),
            ("Auto - TW", "(台湾|台|TW|Taiwan)"),
            ("Auto - JP", "(日本|日|JP|Japan|Tokyo|Osaka)"),
            ("Auto - SG", "(新加坡|狮城|SG|Singapore)"),
            ("Auto - US", "(美国|美|US|USA|United\\s*States|Los\\s*Angeles|LA)"),
            ("Auto - KR", "(韩国|韩|KR|Korea|Seoul)"),
            ("Auto - Netflix", "(nf|netflix|奈飞|奈飛)"),
        ]
        var grouped: [String: [String]] = [:]
        for (groupName, pattern) in patterns {
            guard let regex = try? NSRegularExpression(pattern: pattern, options: .caseInsensitive) else { continue }
            for node in nodeNames {
                let range = NSRange(node.startIndex..., in: node)
                if regex.firstMatch(in: node, range: range) != nil {
                    grouped[groupName, default: []].append(node)
                }
            }
        }
        return grouped.keys.sorted().map { key in
            (name: key, members: deduplicatedStatic(grouped[key] ?? []))
        }
    }

    // MARK: - 自定义规则

    /// 消费设置页写的 `configDir/rules.json`：
    ///   `area: [{name, type, rule}]` → 按正则匹配节点名生成自定义策略组，并加进主选择组；
    ///   `rule: [{type, node, value}]` → 前插到 `rules:` 顶部。
    func applyCustomRules(_ yaml: String) -> String {
        let path = directory.appendingPathComponent("rules.json")
        guard let data = try? Data(contentsOf: path),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return yaml }
        let areas = root["area"] as? [[String: Any]] ?? []
        let customRules = root["rule"] as? [[String: Any]] ?? []
        guard !areas.isEmpty || !customRules.isEmpty else { return yaml }

        var out = yaml
        let nodeNames = Self.proxyNames(out)
        var existing = Set(Self.existingGroupNames(out))

        var groupBlock = ""
        var newGroupNames: [String] = []
        for area in areas {
            let name = (area["name"] as? String ?? "").trimmingCharacters(in: .whitespaces)
            var type = (area["type"] as? String ?? "").trimmingCharacters(in: .whitespaces)
            let rule = (area["rule"] as? String ?? "").trimmingCharacters(in: .whitespaces)
            guard !name.isEmpty, !rule.isEmpty, !existing.contains(name) else { continue }
            if type.isEmpty { type = "url-test" }
            guard let regex = try? NSRegularExpression(pattern: rule) else { continue }

            let matched = deduplicated(nodeNames.filter {
                regex.firstMatch(in: $0, range: NSRange($0.startIndex..., in: $0)) != nil
            })
            guard !matched.isEmpty else { continue }

            existing.insert(name)
            newGroupNames.append(name)
            groupBlock += "  - name: \(YAMLSurgery.quote(name))\n"
            groupBlock += "    type: \(type)\n"
            if type != "select" {
                groupBlock += "    url: 'http://www.gstatic.com/generate_204'\n"
                groupBlock += "    interval: 300\n"
            }
            groupBlock += "    proxies:\n"
            for node in matched {
                groupBlock += "      - \(YAMLSurgery.quote(node))\n"
            }
        }

        // 只在有 rules: 锚点时注入：否则会出现「主选择组引用了组名、但组定义插不进去」的悬空引用，
        // 核心直接拒绝加载整份配置。
        if !newGroupNames.isEmpty, YAMLSurgery.rulesAnchor(out) != nil {
            out = addToFirstGroup(out, names: newGroupNames)
            if let anchor = YAMLSurgery.rulesAnchor(out) {
                out.insert(contentsOf: groupBlock, at: anchor)
            }
        }

        var ruleLines = ""
        for entry in customRules {
            let type = (entry["type"] as? String ?? "").trimmingCharacters(in: .whitespaces)
            let node = (entry["node"] as? String ?? "").trimmingCharacters(in: .whitespaces)
            let value = (entry["value"] as? String ?? "").trimmingCharacters(in: .whitespaces)
            guard !type.isEmpty, !node.isEmpty else { continue }
            // MATCH 是兜底规则，只有两段
            let rule = type == "MATCH" ? "\(type),\(node)" : "\(type),\(value),\(node)"
            ruleLines += "  - \(YAMLSurgery.quote(rule))\n"
        }
        if !ruleLines.isEmpty, let insertion = YAMLSurgery.rulesInsertionPoint(out) {
            out.insert(contentsOf: ruleLines, at: insertion)
        }
        return out
    }

    /// 把组名补进主选择组（去重后追加到末尾）。
    func addToFirstGroup(_ yaml: String, names: [String]) -> String {
        guard !names.isEmpty, let firstProxies = YAMLSurgery.groupProxiesLine(yaml, occurrence: 0) else {
            return yaml
        }
        var members = YAMLSurgery.groupMembers(yaml, at: firstProxies)
        for name in names where !members.contains(name) { members.append(name) }
        return YAMLSurgery.replaceGroupMembers(yaml, at: firstProxies, with: members)
    }

    // MARK: - 私网直连

    /// 私网/回环/链路本地一律 DIRECT，前插到 `rules:` 顶部。
    ///
    /// `extraPrefixes` 传本机的 IPv6 全局单播前缀：IPv6 **没有** RFC1918 那种固定私网段，
    /// 家用 v6 内网用的就是运营商 RA 下发的全局单播前缀（如 `240e:…/64`），换网络/换 ISP 就变，
    /// 静态列表根本写不出来 —— `fc00::/7`(ULA) 和 `fe80::/10`(链路本地) 一条都盖不住它。
    static func applyPrivateNetworkRules(_ yaml: String, extraPrefixes: [String]) -> String {
        let networks = [
            "IP-CIDR,10.0.0.0/8,DIRECT,no-resolve",
            "IP-CIDR,172.16.0.0/12,DIRECT,no-resolve",
            "IP-CIDR,192.168.0.0/16,DIRECT,no-resolve",
            "IP-CIDR,127.0.0.0/8,DIRECT,no-resolve",
            "IP-CIDR,169.254.0.0/16,DIRECT,no-resolve",
            "IP-CIDR6,fc00::/7,DIRECT,no-resolve",
            "IP-CIDR6,fe80::/10,DIRECT,no-resolve",
        ] + extraPrefixes

        guard let insertion = YAMLSurgery.rulesInsertionPoint(yaml) else { return yaml }
        var block = ""
        for rule in networks {
            // 幂等：种子里可能已经手写了同一条。完全相同的行不重复插；
            // 不同写法的重复无害 —— 先命中的赢，目标策略都是 DIRECT。
            guard !yaml.contains(rule) else { continue }
            block += "  - \(YAMLSurgery.quote(rule))\n"
        }
        guard !block.isEmpty else { return yaml }
        var out = yaml
        out.insert(contentsOf: block, at: insertion)
        return out
    }

    // MARK: - sniffer / profile

    /// 域名嗅探。纯 IP 连接（没有域名可匹配）靠它从 TLS SNI / HTTP Host 还原域名后再分流。
    static func applySniffer(_ yaml: String) -> String {
        var block = "sniffer:\n"
        block += "  enable: true\n"
        // 默认只对「已有域名」的连接做校正，纯 IP 压根不嗅探 —— 而我们要修的正是纯 IP。
        block += "  parse-pure-ip: true\n"
        // 用嗅出的域名覆盖连接目标，规则匹配才拿得到域名；不覆盖就只修了显示、没修分流。
        block += "  override-destination: true\n"
        block += "  sniff:\n"
        block += "    HTTP:\n"
        block += "      ports: [80, 8080-8880]\n"
        block += "    TLS:\n"
        block += "      ports: [443, 8443]\n"
        block += "    QUIC:\n"
        block += "      ports: [443, 8443]\n"
        // 已知会被嗅探搞坏的：小米/米家设备的云通道用自签证书 + 非常规 SNI，覆盖目标后连不上。
        block += "  skip-domain:\n"
        block += "    - Mijia Cloud\n"
        block += "    - dlg.io.mi.com\n"
        return YAMLSurgery.replaceOrAppendBlock(yaml, key: "sniffer", with: block)
    }

    /// 持久化 fake-ip 映射与节点选择，扛住热重载 —— 否则每次改配置用户选的节点都会被重置。
    static func applyProfilePersistence(_ yaml: String) -> String {
        let block = "profile:\n  store-selected: true\n  store-fake-ip: true\n"
        return YAMLSurgery.replaceOrAppendBlock(yaml, key: "profile", with: block)
    }

    // MARK: - 静态查询（规则编辑器的下拉要用）

    public static func proxyNames(_ yaml: String) -> [String] {
        guard let start = yaml.range(of: "(?m)^proxies:", options: .regularExpression) else { return [] }
        let end = yaml.range(of: "\nproxy-groups:", range: start.lowerBound..<yaml.endIndex)?.lowerBound
            ?? yaml.endIndex
        return YAMLSurgery.listItemNames(in: String(yaml[start.lowerBound..<end]))
    }

    public static func existingGroupNames(_ yaml: String) -> [String] {
        guard let start = yaml.range(of: "\nproxy-groups:") else { return [] }
        let end = yaml.range(of: "\nrules:", range: start.lowerBound..<yaml.endIndex)?.lowerBound
            ?? yaml.endIndex
        return YAMLSurgery.listItemNames(in: String(yaml[start.lowerBound..<end]))
    }

    // MARK: - 工具

    private func deduplicated(_ values: [String]) -> [String] { Self.deduplicatedStatic(values) }

    /// 保序去重。`Set` 会打乱顺序，而这些列表的顺序直接决定 UI 里的节点/组顺序。
    static func deduplicatedStatic(_ values: [String]) -> [String] {
        var seen = Set<String>()
        return values.filter { seen.insert($0).inserted }
    }

    private func seedIfNeeded(_ name: String) {
        let target = directory.appendingPathComponent(name)
        guard !FileManager.default.fileExists(atPath: target.path),
              let seed = Resources.seed(name) else { return }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try? FileManager.default.copyItem(at: seed, to: target)
        // 种子只读，区域/规则编辑器要写回，必须补权限
        AppPaths.makeWritable(target)
    }

    private func readSeeded(_ name: String) -> String? {
        let target = directory.appendingPathComponent(name)
        if let text = try? String(contentsOf: target, encoding: .utf8) { return text }
        guard let seed = Resources.seed(name) else { return nil }
        return try? String(contentsOf: seed, encoding: .utf8)
    }
}
