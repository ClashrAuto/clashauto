import Foundation
import Testing
@testable import CoastKit

private final class TempRules {
    let directory: URL
    let store: RulesStore

    init() {
        directory = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-rules-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        store = RulesStore(directory: directory)
    }

    deinit { try? FileManager.default.removeItem(at: directory) }
}

@Suite("rules.json 读写")
struct RulesStoreIOTests {

    @Test("往返：写进去什么读出来什么")
    func roundTrip() {
        let temp = TempRules()
        let rules = [
            RulesStore.Rule(type: "DOMAIN-SUFFIX", node: "DIRECT", value: "example.com"),
            RulesStore.Rule(type: "MATCH", node: "🚀 节点选择", value: ""),
        ]
        let areas = [RulesStore.Area(name: "我的港区", type: "url-test", rule: "香港|HK")]
        #expect(temp.store.save(rules: rules, areas: areas))

        let loaded = temp.store.load()
        #expect(loaded.rules == rules)
        #expect(loaded.areas == areas)
    }

    @Test("字段名必须和 ConfigBuilder 消费的一致")
    func fieldNamesMatchConsumer() throws {
        let temp = TempRules()
        _ = temp.store.save(rules: [RulesStore.Rule(type: "DOMAIN", node: "DIRECT", value: "a.com")],
                            areas: [RulesStore.Area(name: "g", type: "select", rule: "x")])
        let data = try Data(contentsOf: temp.store.path)
        let root = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
        // ConfigBuilder.applyCustomRules 读的就是这几个键，改一个两边就对不上了
        let rule = try #require((root["rule"] as? [[String: Any]])?.first)
        #expect(rule["type"] as? String == "DOMAIN")
        #expect(rule["node"] as? String == "DIRECT")
        #expect(rule["value"] as? String == "a.com")
        let area = try #require((root["area"] as? [[String: Any]])?.first)
        #expect(area["name"] as? String == "g")
        #expect(area["type"] as? String == "select")
        #expect(area["rule"] as? String == "x")
    }

    @Test("文件不存在时返回空，不崩")
    func missingFile() {
        let temp = TempRules()
        let loaded = temp.store.load()
        #expect(loaded.rules.isEmpty)
        #expect(loaded.areas.isEmpty)
    }

    @Test("文件是坏 JSON 时也返回空，不崩")
    func corruptFile() {
        let temp = TempRules()
        try? "{ 这不是 json".write(to: temp.store.path, atomically: true, encoding: .utf8)
        #expect(temp.store.load().rules.isEmpty)
    }
}

@Suite("规则校验")
struct RuleValidationTests {

    @Test("合法规则通过")
    func validRules() {
        #expect(RulesStore.validate(RulesStore.Rule(type: "DOMAIN-SUFFIX", node: "DIRECT", value: "a.com")) == nil)
        #expect(RulesStore.validate(RulesStore.Rule(type: "IP-CIDR", node: "DIRECT", value: "10.0.0.0/8")) == nil)
        #expect(RulesStore.validate(RulesStore.Rule(type: "DST-PORT", node: "REJECT", value: "443")) == nil)
    }

    @Test("MATCH 是兜底规则，不需要匹配值")
    func matchNeedsNoValue() {
        #expect(RulesStore.validate(RulesStore.Rule(type: "MATCH", node: "DIRECT", value: "")) == nil)
        // 其它类型缺匹配值就不行
        #expect(RulesStore.validate(RulesStore.Rule(type: "DOMAIN", node: "DIRECT", value: "")) != nil)
    }

    @Test("★ 逗号必须拦住 —— 它会把一行规则切成更多段，写坏整张规则表")
    func rejectsComma() {
        // 坏规则会让核心**整份配置**加载失败，用户只看到「突然全断网」，
        // 完全无从关联到自己刚加的这条。所以宁可在这里啰嗦。
        #expect(RulesStore.validate(RulesStore.Rule(type: "DOMAIN", node: "DIRECT", value: "a.com,b.com")) != nil)
        #expect(RulesStore.validate(RulesStore.Rule(type: "DOMAIN", node: "DIRECT,REJECT", value: "a.com")) != nil)
    }

    @Test("网段类型要求带 /，端口要在范围内")
    func typeSpecificChecks() {
        #expect(RulesStore.validate(RulesStore.Rule(type: "IP-CIDR", node: "DIRECT", value: "10.0.0.0")) != nil)
        #expect(RulesStore.validate(RulesStore.Rule(type: "DST-PORT", node: "DIRECT", value: "70000")) != nil)
        #expect(RulesStore.validate(RulesStore.Rule(type: "DST-PORT", node: "DIRECT", value: "abc")) != nil)
    }

    @Test("空类型/空目标都要拦")
    func rejectsEmpty() {
        #expect(RulesStore.validate(RulesStore.Rule(type: "", node: "DIRECT", value: "a")) != nil)
        #expect(RulesStore.validate(RulesStore.Rule(type: "DOMAIN", node: "", value: "a")) != nil)
    }

    @Test("区域分组：正则非法当场报，而不是让它静默失效")
    func areaValidation() {
        #expect(RulesStore.validate(RulesStore.Area(name: "港区", type: "url-test", rule: "香港|HK")) == nil)
        // ConfigBuilder 遇到非法正则会静默跳过这个组 —— 用户加了组却什么都没发生
        #expect(RulesStore.validate(RulesStore.Area(name: "港区", type: "url-test", rule: "([未闭合")) != nil)
        #expect(RulesStore.validate(RulesStore.Area(name: "", type: "url-test", rule: "x")) != nil)
        #expect(RulesStore.validate(RulesStore.Area(name: "g", type: "url-test", rule: "")) != nil)
    }

    @Test("生成的规则行与 ConfigBuilder 的拼法一致")
    func ruleLineMatchesBuilder() {
        #expect(RulesStore.Rule(type: "DOMAIN-SUFFIX", node: "DIRECT", value: "a.com").ruleLine
                == "DOMAIN-SUFFIX,a.com,DIRECT")
        #expect(RulesStore.Rule(type: "MATCH", node: "🚀 节点选择", value: "").ruleLine
                == "MATCH,🚀 节点选择")
    }
}

@Suite("规则编辑器与 ConfigBuilder 的闭环")
struct RulesEndToEndTests {

    @Test("编辑器写的 rules.json，ConfigBuilder 能原样消费出来")
    func editorOutputIsConsumable() throws {
        let temp = TempRules()
        _ = temp.store.save(
            rules: [RulesStore.Rule(type: "DOMAIN-SUFFIX", node: "DIRECT", value: "example.com")],
            areas: [RulesStore.Area(name: "我的港区", type: "url-test", rule: "香港")])

        // 造一份最小 full.yaml 骨架喂给 ConfigBuilder
        let base = """
        proxies:
          - name: '香港 01'
            type: trojan
            server: '1.1.1.1'
            port: 443
        proxy-groups:
          - name: '🚀 节点选择'
            type: select
            proxies:
              - DIRECT
        rules:
          - 'MATCH,🚀 节点选择'
        """
        let builder = ConfigBuilder(config: AppConfig(), directory: temp.directory)
        let out = builder.applyCustomRules(base)

        // 规则被前插到 rules: 顶部
        #expect(out.contains("- 'DOMAIN-SUFFIX,example.com,DIRECT'"))
        // 区域分组被生成，且加进了主选择组
        #expect(out.contains("- name: '我的港区'"))
        let first = try #require(YAMLSurgery.groupProxiesLine(out, occurrence: 0))
        #expect(YAMLSurgery.groupMembers(out, at: first).contains("我的港区"))
    }
}
