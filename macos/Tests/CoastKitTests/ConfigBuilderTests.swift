import Foundation
import Testing
@testable import CoastKit

// full.yaml 是核心真正吃的东西，写坏了核心直接拒绝加载整份配置 —— 这里是整条链路上
// 最该被钉住的一环。用一份精简的 default.yaml 骨架来验，结构与真种子一致。

private let baseYAML = """
mixed-port: 7890
allow-lan: true
dns:
  enable: true
  nameserver:
    - 223.5.5.5
proxies: null
proxy-groups:
  - name: '🚀 节点选择'
    type: select
    proxies:
      - DIRECT
  - name: '自动选择'
    type: url-test
    proxies:
      - DIRECT
rules:
  - 'GEOIP,CN,DIRECT'
  - 'MATCH,🚀 节点选择'
"""

private let subscribeYAML = """
- name: 'A'
  url: 'https://a.example.com'
  use: true
  list:
    - name: '香港 01'
      type: trojan
      server: '1.1.1.1'
      port: 443
      password: 'pw'
      use: true
    - name: '日本 01'
      type: trojan
      server: '2.2.2.2'
      port: 443
      password: 'pw'
      use: true
    - name: '美国 01'
      type: trojan
      server: '3.3.3.3'
      port: 443
      password: 'pw'
      use: false
- name: 'B'
  url: 'https://b.example.com'
  use: false
  list:
    - name: '新加坡 01'
      type: ss
      server: '4.4.4.4'
      port: 8388
      use: true
"""

/// 临时 configDir，避免动到开发者自己的配置。
private final class TempBuilder {
    let directory: URL
    let builder: ConfigBuilder

    init(config: AppConfig = AppConfig(), files: [String: String] = [:]) {
        directory = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-cfg-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for (name, content) in files {
            try? content.write(to: directory.appendingPathComponent(name),
                               atomically: true, encoding: .utf8)
        }
        builder = ConfigBuilder(config: config, directory: directory)
    }

    deinit { try? FileManager.default.removeItem(at: directory) }
}

@Suite("YAML 手术")
struct YAMLSurgeryTests {

    @Test("改已有顶层标量")
    func replaceScalar() {
        let out = YAMLSurgery.setScalar(baseYAML, key: "mixed-port", value: "1080")
        #expect(out.contains("mixed-port: 1080"))
        #expect(out.contains("mixed-port: 7890") == false)
    }

    @Test("缺失的顶层标量前置到开头，而不是追加到末尾")
    func missingScalarGoesToTop() {
        // 追加到末尾会掉进最后一个块的缩进范围，变成那个块的子键
        let out = YAMLSurgery.setScalar(baseYAML, key: "secret", value: "'abc'")
        #expect(out.hasPrefix("secret: 'abc'\n"))
    }

    @Test("两级标量：有则改，无则补")
    func nestedScalar() {
        let changed = YAMLSurgery.setNestedScalar(baseYAML, section: "dns", key: "enable", value: "false")
        #expect(changed.contains("  enable: false"))

        let added = YAMLSurgery.setNestedScalar(baseYAML, section: "dns", key: "listen", value: "127.0.0.1:1053")
        #expect(added.contains("  listen: 127.0.0.1:1053"))
        // 补的键要落在 dns 块里，不能跑到 proxies: 后面去
        let dnsRange = YAMLSurgery.blockRange(added, key: "dns")!
        #expect(added[dnsRange].contains("listen:"))
    }

    @Test("section 不存在时整块追加")
    func nestedScalarCreatesSection() {
        let out = YAMLSurgery.setNestedScalar("foo: 1\n", section: "tun", key: "enable", value: "true")
        #expect(out.contains("tun:\n  enable: true"))
    }

    @Test("加引号：默认单引号，带 \\u 的改双引号并保留转义")
    func quoting() {
        #expect(YAMLSurgery.quote("plain") == "'plain'")
        #expect(YAMLSurgery.quote("") == "''")
        #expect(YAMLSurgery.quote("it's") == "'it''s'")
        // 有些机场的节点名里是字面量 \uXXXX；单引号 YAML 不解转义，写出去核心读到的会是那六个字符
        #expect(YAMLSurgery.quote("a\\u4e2d") == "\"a\\u4e2d\"")
    }
}

@Suite("plugin 合并与标量修补")
struct ConfigBuilderPatchTests {

    @Test("plugin 的 dns/tun 整块覆盖 base")
    func mergePlugin() {
        let plugin = """
        dns:
          enable: true
          enhanced-mode: fake-ip
        tun:
          stack: gvisor
          enable: true
        """
        let out = ConfigBuilder.mergePlugin(base: baseYAML, plugin: plugin)
        #expect(out.contains("enhanced-mode: fake-ip"))
        #expect(out.contains("stack: gvisor"))
        // base 里 dns 块原有的 nameserver 被整块替换掉了（这是刻意的：plugin 是 DNS/TUN 的真值来源）
        #expect(out.contains("nameserver:") == false)
    }

    @Test("plugin 为空时原样返回")
    func mergeEmptyPlugin() {
        #expect(ConfigBuilder.mergePlugin(base: baseYAML, plugin: "   \n ") == baseYAML)
    }

    @Test("proxies: null 要归一，否则核心拒绝加载")
    func normalizeNullProxies() {
        let out = ConfigBuilder.normalizeEmptyProxies("proxies: null\nproxy-groups:\n  - name: g\n    proxies: null\n")
        #expect(out.contains("proxies: []"))
        #expect(out.contains("    proxies:\n      - DIRECT"))
    }

    @Test("注入 proxy-server-nameserver，且幂等")
    func proxyServerNameserver() {
        let once = ConfigBuilder.ensureProxyServerNameserver(baseYAML)
        #expect(once.contains("  proxy-server-nameserver:"))
        #expect(once.contains("    - 223.5.5.5"))
        // 用户自己配了就不再注入
        let twice = ConfigBuilder.ensureProxyServerNameserver(once)
        #expect(twice == once)
    }

    @Test("没有 dns 块时不动，免得破坏结构")
    func proxyServerNameserverNeedsDNS() {
        let input = "mixed-port: 7890\n"
        #expect(ConfigBuilder.ensureProxyServerNameserver(input) == input)
    }
}

@Suite("地区自动分组")
struct AutoGroupTests {

    @Test("按关键词分组，组名按字典序 —— 顺序直接体现在 UI 上")
    func groupsSorted() {
        let groups = ConfigBuilder.autoGroups(["香港 01", "日本 02", "US-3", "韩国 4", "Netflix 专线"])
        #expect(groups.map(\.name) == ["Auto - HK", "Auto - JP", "Auto - KR", "Auto - Netflix", "Auto - US"])
    }

    @Test("同一节点可以进多个组")
    func nodeCanJoinMultipleGroups() {
        let groups = ConfigBuilder.autoGroups(["香港 Netflix 01"])
        #expect(groups.map(\.name) == ["Auto - HK", "Auto - Netflix"])
    }

    @Test("没有匹配的节点时不产生空组")
    func noEmptyGroups() {
        #expect(ConfigBuilder.autoGroups(["随便一个名字"]).isEmpty)
    }

    @Test("组内成员去重且保序")
    func membersDeduplicatedInOrder() {
        let groups = ConfigBuilder.autoGroups(["香港 01", "香港 02", "香港 01"])
        #expect(groups.first?.members == ["香港 01", "香港 02"])
    }
}

@Suite("私网直连规则")
struct PrivateNetworkTests {

    @Test("前插到 rules: 顶部")
    func insertedAtTop() {
        let out = ConfigBuilder.applyPrivateNetworkRules(baseYAML, extraPrefixes: [])
        let lines = out.components(separatedBy: "\n")
        let rulesIndex = lines.firstIndex { $0 == "rules:" }!
        #expect(lines[rulesIndex + 1].contains("10.0.0.0/8"))
        // 原有规则还在，只是被顶下去了
        #expect(out.contains("GEOIP,CN,DIRECT"))
    }

    @Test("已经写过的同一条不重复插（幂等）")
    func idempotent() {
        let once = ConfigBuilder.applyPrivateNetworkRules(baseYAML, extraPrefixes: [])
        let twice = ConfigBuilder.applyPrivateNetworkRules(once, extraPrefixes: [])
        #expect(twice == once)
    }

    @Test("动态 v6 前缀也会被插进去")
    func includesDynamicIPv6() {
        let prefix = "IP-CIDR6,240e:390:abc::/64,DIRECT,no-resolve"
        let out = ConfigBuilder.applyPrivateNetworkRules(baseYAML, extraPrefixes: [prefix])
        #expect(out.contains(prefix))
    }

    @Test("没有 rules: 锚点时不注入")
    func needsRulesAnchor() {
        let input = "proxies: []\n"
        #expect(ConfigBuilder.applyPrivateNetworkRules(input, extraPrefixes: []) == input)
    }
}

@Suite("订阅注入")
struct ApplySubscriptionsTests {

    private func build() -> (String, TempBuilder) {
        let temp = TempBuilder(files: ["subscribe.yaml": subscribeYAML])
        let subs = temp.builder.readSubscriptions()
        return (temp.builder.applySubscriptions(baseYAML, subscriptions: subs), temp)
    }

    @Test("只收启用订阅里启用的节点，节点名带订阅后缀")
    func onlyEnabled() {
        let (out, _) = build()
        #expect(out.contains("name: '香港 01 - A'"))
        #expect(out.contains("name: '日本 01 - A'"))
        #expect(out.contains("美国 01") == false)      // 节点 use: false
        #expect(out.contains("新加坡 01") == false)    // 订阅 use: false
    }

    @Test("主选择组含「订阅组 + 地区组 + 全部节点」，自动选择组只含节点")
    func groupMembership() {
        let (out, _) = build()
        let first = YAMLSurgery.groupProxiesLine(out, occurrence: 0)!
        let firstMembers = YAMLSurgery.groupMembers(out, at: first)
        #expect(firstMembers.contains("DIRECT"))       // 原有成员保留
        #expect(firstMembers.contains("A 订阅"))
        #expect(firstMembers.contains("Auto - HK"))
        #expect(firstMembers.contains("香港 01 - A"))

        let second = YAMLSurgery.groupProxiesLine(out, occurrence: 1)!
        let secondMembers = YAMLSurgery.groupMembers(out, at: second)
        #expect(secondMembers == ["香港 01 - A", "日本 01 - A"])
    }

    @Test("订阅组与地区组的定义都插在 rules: 之前")
    func groupDefinitionsBeforeRules() {
        let (out, _) = build()
        let subGroup = out.range(of: "  - name: 'A 订阅'")!
        let rules = out.range(of: "\nrules:")!
        #expect(subGroup.lowerBound < rules.lowerBound)
        #expect(out.contains("type: url-test"))
        // 刻意不写 lazy: false —— 那会让核心启动时同步健康检查，节点不可达就卡住启动
        #expect(out.contains("lazy:") == false)
    }

    @Test("引用的组名都有对应定义，不能有悬空引用")
    func noDanglingGroupReferences() {
        let (out, _) = build()
        let defined = Set(ConfigBuilder.existingGroupNames(out))
        let nodes = Set(ConfigBuilder.proxyNames(out))
        let first = YAMLSurgery.groupProxiesLine(out, occurrence: 0)!
        for member in YAMLSurgery.groupMembers(out, at: first) where member != "DIRECT" && member != "REJECT" {
            // 每个成员要么是已定义的组，要么是已存在的节点 —— 否则核心拒绝加载整份配置
            #expect(defined.contains(member) || nodes.contains(member), "悬空引用: \(member)")
        }
    }

    @Test("没有任何可用节点时不动 proxies")
    func noNodesLeavesYAMLAlone() {
        let temp = TempBuilder(files: ["subscribe.yaml": "[]\n"])
        let out = temp.builder.applySubscriptions(baseYAML, subscriptions: temp.builder.readSubscriptions())
        #expect(out == baseYAML)
    }
}

@Suite("自定义规则")
struct CustomRulesTests {

    private let rulesJSON = """
    {"area":[{"name":"我的港区","type":"url-test","rule":"香港"}],
     "rule":[{"type":"DOMAIN-SUFFIX","node":"DIRECT","value":"example.com"},
             {"type":"MATCH","node":"🚀 节点选择","value":""}]}
    """

    @Test("area 生成自定义组，并补进主选择组")
    func areaGroups() {
        let temp = TempBuilder(files: ["subscribe.yaml": subscribeYAML, "rules.json": rulesJSON])
        var yaml = temp.builder.applySubscriptions(baseYAML, subscriptions: temp.builder.readSubscriptions())
        yaml = temp.builder.applyCustomRules(yaml)
        #expect(yaml.contains("  - name: '我的港区'"))
        let first = YAMLSurgery.groupProxiesLine(yaml, occurrence: 0)!
        #expect(YAMLSurgery.groupMembers(yaml, at: first).contains("我的港区"))
    }

    @Test("rule 前插到 rules: 顶部；MATCH 只有两段")
    func customRuleLines() {
        let temp = TempBuilder(files: ["subscribe.yaml": subscribeYAML, "rules.json": rulesJSON])
        let yaml = temp.builder.applyCustomRules(baseYAML)
        #expect(yaml.contains("  - 'DOMAIN-SUFFIX,example.com,DIRECT'"))
        #expect(yaml.contains("  - 'MATCH,🚀 节点选择'"))
    }

    @Test("正则匹配不到节点时不生成空组")
    func noMatchNoGroup() {
        let json = #"{"area":[{"name":"火星区","type":"url-test","rule":"火星"}]}"#
        let temp = TempBuilder(files: ["subscribe.yaml": subscribeYAML, "rules.json": json])
        let yaml = temp.builder.applyCustomRules(baseYAML)
        #expect(yaml.contains("火星区") == false)
    }

    @Test("没有 rules.json 时原样返回")
    func noRulesFile() {
        let temp = TempBuilder()
        #expect(temp.builder.applyCustomRules(baseYAML) == baseYAML)
    }
}

@Suite("ensureFullConfig 全流程")
struct EnsureFullConfigTests {

    private func makeTemp(config: AppConfig = {
        var c = AppConfig()
        c.mixedPort = 7891
        c.uiPort = 9192
        c.secret = "deadbeef"
        return c
    }()) -> TempBuilder {
        TempBuilder(config: config, files: [
            "default.yaml": baseYAML,
            "plugin.yaml": "dns:\n  enable: true\n  enhanced-mode: fake-ip\ntun:\n  stack: gvisor\n  enable: true\n",
            "subscribe.yaml": subscribeYAML,
        ])
    }

    @Test("产出 full.yaml，端口/控制器/secret 都按配置写入")
    func writesFullConfig() throws {
        let temp = makeTemp()
        let path = try #require(temp.builder.ensureFullConfig(tunEnabled: false))
        let yaml = try String(contentsOf: path, encoding: .utf8)
        #expect(yaml.contains("mixed-port: 7891"))
        #expect(yaml.contains("external-controller: '127.0.0.1:9192'"))
        #expect(yaml.contains("secret: 'deadbeef'"))
        #expect(yaml.contains("allow-lan: false"))       // 不能变成开放代理
        #expect(yaml.contains("find-process-mode: always"))
    }

    @Test("tun.enable 跟随入参")
    func tunFlag() throws {
        let temp = makeTemp()
        let off = try String(contentsOf: #require(temp.builder.ensureFullConfig(tunEnabled: false)), encoding: .utf8)
        #expect(off.contains("  enable: false"))
        let on = try String(contentsOf: #require(temp.builder.ensureFullConfig(tunEnabled: true)), encoding: .utf8)
        #expect(on.contains("  enable: true"))
    }

    @Test("sniffer 与 profile 块都在，且重复生成不会叠加")
    func snifferAndProfileAreIdempotent() throws {
        let temp = makeTemp()
        _ = temp.builder.ensureFullConfig(tunEnabled: false)
        let yaml = try String(contentsOf: #require(temp.builder.ensureFullConfig(tunEnabled: false)), encoding: .utf8)
        #expect(yaml.components(separatedBy: "sniffer:\n").count == 2)   // 只出现一次
        #expect(yaml.components(separatedBy: "profile:\n").count == 2)
        #expect(yaml.contains("parse-pure-ip: true"))
        #expect(yaml.contains("store-selected: true"))
    }

    @Test("同一份输入连跑两次结果一致 —— 否则每次热重载都在改配置")
    func deterministic() throws {
        let temp = makeTemp()
        let first = try String(contentsOf: #require(temp.builder.ensureFullConfig(tunEnabled: false)), encoding: .utf8)
        let second = try String(contentsOf: #require(temp.builder.ensureFullConfig(tunEnabled: false)), encoding: .utf8)
        #expect(first == second)
    }

    @Test("★ 私网直连必须压在自定义规则之上")
    func privateRulesOutrankCustomRules() throws {
        let json = #"{"rule":[{"type":"DOMAIN-SUFFIX","node":"🚀 节点选择","value":"example.com"}]}"#
        let temp = TempBuilder(files: [
            "default.yaml": baseYAML,
            "plugin.yaml": "",
            "subscribe.yaml": subscribeYAML,
            "rules.json": json,
        ])
        let yaml = try String(contentsOf: #require(temp.builder.ensureFullConfig(tunEnabled: false)), encoding: .utf8)
        let privateRule = try #require(yaml.range(of: "10.0.0.0/8"))
        let customRule = try #require(yaml.range(of: "DOMAIN-SUFFIX,example.com"))
        // 两者都前插到 rules: 顶部，后插的在上面。私网要赢 —— 否则 policy 指向代理的规则会把
        // 「访问自己家路由器后台 / 内网 NAS」也发到代理节点上。
        #expect(privateRule.lowerBound < customRule.lowerBound)
    }
}
