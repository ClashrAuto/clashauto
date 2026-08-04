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
        // 透明代理口：PF 把被代理设备的 TCP 重定向到这里。**不对外广播**，
        // 只接受经内核重定向进来的连接，所以开着它不等于开放代理。
        yaml = YAMLSurgery.setScalar(yaml, key: "redir-port", value: String(DeviceStore.redirPort))
        // DNS 监听口：被代理设备的 UDP :53 也被重定向过来。不转投的话设备会拿着它自己配的
        // DNS（常是路由器）去查，经我们转发出去解析不到，表现为「直连 IP 通、用域名全超时」。
        // 转过来之后它拿到 fake-ip，域名规则才匹配得上。
        //
        // ★ **只有真在当网关时才绑 `0.0.0.0`。** 一台设备都没接管的时候绑全网卡，等于在局域网上
        //   开了一个谁都能查的 DNS —— 实测从另一台机器 `dig @<本机> -p 1053` 直接有应答，
        //   而且回的是 **fake-ip**（198.18.x.x），对局域网里其他主机毫无意义：谁误用了谁的
        //   域名解析就坏掉。redir-port 那行注释说的「不对外广播」对它自己成立（核心把 redir
        //   绑在 127.0.0.1），但对这行**不成立**，两者不能共用一套说法。
        //   Qt 端同一处一直是 `127.0.0.1:1053`（它走用户态栈中继，回环就够）；
        //   macOS 这条线走 PF rdr，包是内核改写目的地送进来的，当网关时确实需要非回环地址。
        let actingAsGateway = !DeviceStore(configDir: directory).proxiedDevices().isEmpty
        yaml = YAMLSurgery.setNestedScalar(
            yaml, section: "dns", key: "listen",
            value: (actingAsGateway ? "0.0.0.0:" : "127.0.0.1:") + String(DeviceStore.dnsPort))
        // 顶层 ipv6:true —— 让核心**接受并拨出** IPv6 连接。透明网关会把被接管设备的 v6 TCP
        // 经 PF rdr 送进 redir 口(见 Redirector 的 inet6 规则);ipv6:false 时核心会直接丢弃这些
        // v6 连接,整条 v6 接管等于白做。dns.ipv6 仍保持 plugin.yaml 里的 false —— AAAA 回空 →
        // 域名流量自然回落 v4(已被 v4 rdr 走 fake-ip 代理),只有字面量 v6 / 设备自带 v6 解析器
        // 拿到的真实 AAAA 才走 v6 redir。这样既补齐 v6、又不动已验证的 v4 DNS/fake-ip 路径。
        yaml = YAMLSurgery.setScalar(yaml, key: "ipv6", value: "true")
        yaml = YAMLSurgery.setNestedScalar(yaml, section: "tun", key: "enable",
                                           value: tunEnabled ? "true" : "false")
        // 必须排在 ensureProxyServerNameserver **之前**：剔完才知道还剩几条（与 Qt 同序）。
        yaml = Self.pruneUnreachableDns(yaml)
        yaml = Self.ensureProxyServerNameserver(yaml)
        yaml = Self.normalizeEmptyProxies(yaml)
        yaml = applySubscriptions(yaml, subscriptions: readSubscriptions())
        yaml = applyCustomRules(yaml)
        yaml = applyDevicePolicies(yaml)
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

    /// 剔掉订阅 dns 段里**实测确认不可达**的 DoH —— 与 Qt 的 `pruneUnreachableDns` 同一份名单、
    /// 同一套规则。这条线一直没有这一步，于是订阅带来的那几条一直留在配置里。
    ///
    /// 后果比「少一个解析器」严重得多：核心按列表顺序挨个试，每撞一条不可达的就等一次连接
    /// 超时，而这发生在**每条新连接**的名字解析上。
    ///
    /// 本机实测（2026-08-04，同一份核心、同一份配置，只换 DNS 列表，用 `/dns/query`
    /// 隔离掉节点质量，各两轮）：**中位 0.19s → 0.03~0.04s，约 5 倍**。
    /// Qt 注释里 Windows 上记的是 32 倍 —— 差别在于本机默认路由挂在别的代理的 TUN 上，
    /// `1.0.0.1` / `public.dns.iij.jp` 借到了路，只有 rubyfish / twnic 真的在超时。
    /// 换句话说 5 倍是**这台机器的下限**，裸网络只会更糟。
    ///
    /// 只剔固定名单，不做运行时探测：探测本身要花时间、还会因网络抖动误杀，
    /// 而这几条是长期失效。名单之外一概不动，保持「订阅说什么就是什么」。
    static func pruneUnreachableDns(_ yaml: String) -> String {
        // ★ `default-nameserver` 里的 1.0.0.1 是**明文 UDP**（不是 DoH），墙内可达，
        //   不能按同一把尺子砍 —— 故只在这三张表里剔。
        let unreachable = ["dns.rubyfish.cn", "1.0.0.1", "dns.twnic.tw", "public.dns.iij.jp"]
        let targetLists: Set<String> = ["nameserver", "fallback", "proxy-server-nameserver"]

        var out: [String] = []
        var inDns = false
        var currentList = ""
        var removed = 0
        for line in yaml.split(separator: "\n", omittingEmptySubsequences: false).map(String.init) {
            if !line.hasPrefix(" ") && !line.hasPrefix("\t") {
                inDns = line.hasPrefix("dns:")      // 顶格键 → 进/出 dns 块
                currentList = ""
            } else if inDns, let name = Self.listKey(line) {
                currentList = name
            }
            if inDns, targetLists.contains(currentList),
               line.trimmingCharacters(in: .whitespaces).hasPrefix("-"),
               unreachable.contains(where: { line.contains($0) }) {
                removed += 1
                continue
            }
            out.append(line)
        }
        guard removed > 0 else { return yaml }
        // 剔空的列表补回可用项：空列表在 mihomo 里等于「没有解析器」，比留着慢的还糟。
        return Self.refillEmptyDnsLists(out.joined(separator: "\n"))
    }

    /// `  nameserver:` 这种「两空格 + 键 + 冒号 + 行尾」的子列表头，返回键名。
    private static func listKey(_ line: String) -> String? {
        guard line.hasPrefix("  "), !line.hasPrefix("   ") else { return nil }
        let body = line.dropFirst(2)
        guard body.hasSuffix(":") || body.trimmingCharacters(in: .whitespaces).hasSuffix(":")
        else { return nil }
        let name = body.prefix(while: { $0 != ":" })
        guard !name.isEmpty, name.allSatisfy({ $0.isLowercase || $0 == "-" }) else { return nil }
        return String(name)
    }

    /// 把被剔空的 nameserver / fallback 补回两条实测可用的。
    private static func refillEmptyDnsLists(_ yaml: String) -> String {
        let good = ["    - https://223.5.5.5/dns-query", "    - https://dns.pub/dns-query"]
        var lines = yaml.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        var inDns = false
        var index = 0
        while index < lines.count {
            let line = lines[index]
            if !line.hasPrefix(" ") && !line.hasPrefix("\t") { inDns = line.hasPrefix("dns:") }
            if inDns, let name = Self.listKey(line), name == "nameserver" || name == "fallback" {
                // 下一行不是本列表的条目 → 这张表空了
                let next = index + 1 < lines.count ? lines[index + 1] : ""
                if !next.trimmingCharacters(in: .whitespaces).hasPrefix("-") {
                    lines.insert(contentsOf: good, at: index + 1)
                    index += good.count
                }
            }
            index += 1
        }
        return lines.joined(separator: "\n")
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
            // 拼法只此一处 —— 用 `RulesStore.Rule.ruleLine`。
            //
            // 这里原本自己写了一遍同样的三元表达式，而 `ruleLine` 的注释写着「与
            // ConfigBuilder.applyCustomRules 的拼法一致」—— 把一致性当约定，没有任何东西
            // 保证它。更糟的是测试测的是 `ruleLine`（**产品代码里没人调用**），
            // 给真正上线的这一份提供了虚假信心：改坏这里，测试照样全绿。
            let rule = RulesStore.Rule(type: type, node: node, value: value).ruleLine
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

    // MARK: - 局域网设备代理

    /// 为「开着代理」的设备生成每设备的 `SRC-IP-CIDR` 规则。
    ///
    /// 透明重定向下我们看不到任何凭据，**只能按源 IP 认设备**，所以策略是
    /// `SRC-IP-CIDR,<ip>/32,<目标>`（Qt 版走 `IN-USER`，那是它经带认证 SOCKS 拨号的产物）。
    ///
    /// 规则**前插到 `rules:` 顶部**，压过默认规则表；但会被随后前插的私网直连再压一层 ——
    /// 那是对的：被代理设备访问自家路由器后台也该直连，不该绕一圈出国。
    ///
    /// 注意这里**只管分流策略**。「把流量弄过来」是 ARP 欺骗 + PF 重定向的事，
    /// 两者分开：配置生成是纯函数、可测；改系统状态的那部分需要 root。
    func applyDevicePolicies(_ yaml: String) -> String {
        let devices = DeviceStore(configDir: directory).proxiedDevices()
        guard !devices.isEmpty else { return yaml }

        var ruleLines = ""
        for device in devices {
            let target: String
            switch device.policyMode {
            case .follow, .rule: continue   // 不加专属规则 = 跟随全局 / 走默认规则表
            case .direct: target = "DIRECT"
            case .reject: target = "REJECT"
            case .global:
                guard !device.policyTarget.isEmpty else { continue }
                target = device.policyTarget
            }
            ruleLines += "  - " + YAMLSurgery.quote("SRC-IP-CIDR,\(device.lastIP)/32,\(target)") + "\n"
        }
        guard !ruleLines.isEmpty, let insertion = YAMLSurgery.rulesInsertionPoint(yaml) else { return yaml }
        var out = yaml
        out.insert(contentsOf: ruleLines, at: insertion)
        return out
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
