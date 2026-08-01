import Foundation
import Testing
@testable import CoastKit

/// YAML 注入实证:构造含恶意字符的节点名/订阅名,跑完整 ConfigBuilder,
/// 用 ConfigBuilder 自己的反解(proxyNames)验证「恶意内容没越权成额外的 proxy/键」。
///
/// 纯 Swift、进 CI。另有一个 PyYAML 交叉验证脚本(scripts/verify_yaml.py)作为本地额外保险。
@Suite("YAML 注入防护(实证)")
struct YAMLInjectionTests {

    /// 一批想越权的节点名。每个都试图从 YAML 值里"逃逸"出去。
    static let malicious = [
        "正常香港",
        "换行注入\n  - name: EVIL\n    server: 1.2.3.4",   // 想新增一个 proxy
        "单引号'注入",                                       // 想提前闭合单引号
        "双引号\"注入",
        "冒号: 值",                                          // YAML 键值分隔符
        "井号#注释",                                          // YAML 注释
        "锚点&a别名*a",                                       // YAML 锚点/别名
        "管道|折叠>标量",                                     // 块标量指示符
        "逗号,分隔,字段",                                     // 规则字段分隔符
        "反斜杠\\u000A转义",                                  // 想让解析器解码成换行
        "前导空格   尾随空格   ",
    ]

    private func dir(withNodes names: [String], subName: String = "机场") -> URL {
        let d = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-inject-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        // 直接写 proxies 块(避开订阅解析,聚焦 ConfigBuilder 的注入面)
        var nodes = ""
        for (i, name) in names.enumerated() {
            nodes += "    - name: \(SubParser.yq(name))\n"
            nodes += "      type: trojan\n      server: 10.0.0.\(i+1)\n      port: 443\n"
            nodes += "      password: pw\n      use: true\n"
        }
        let sub = "- name: \(SubParser.yq(subName))\n  url: 'x'\n  use: true\n  list:\n" + nodes
        try? sub.write(to: d.appendingPathComponent("subscribe.yaml"), atomically: true, encoding: .utf8)
        return d
    }

    private let base = """
    proxies: []
    proxy-groups:
      - name: '🚀 节点选择'
        type: select
        proxies:
          - DIRECT
    rules:
      - 'MATCH,🚀 节点选择'
    """

    @Test("恶意节点名不越权成额外 proxy,数量精确等于输入数")
    func maliciousNamesDoNotSplit() {
        let d = dir(withNodes: Self.malicious)
        let builder = ConfigBuilder(config: AppConfig(), directory: d)
        let out = builder.applySubscriptions(base, subscriptions: builder.readSubscriptions())

        let names = ConfigBuilder.proxyNames(out)
        // 核心不变式:每个恶意名字恰好产出**一个** proxy —— 没有一个因为换行/引号逃逸而
        // 分裂成多个。注意:"EVIL" 作为某个节点名的**子串**是无害的(那就是它的名字);
        // 危险的是它变成**独立的一行 `- name:`**,让 proxy 数量超过输入数。数量对 = 没越权。
        #expect(names.count == Self.malicious.count,
                "期望 \(Self.malicious.count) 个 proxy,实得 \(names.count):\(names)")
        // 每个恶意输入都还在(作为完整值),没有一个被截断丢失
        #expect(names.contains { $0.hasPrefix("正常香港") })
        #expect(names.contains { $0.contains("EVIL") && $0.contains("机场") },
                "含 EVIL 的名字应作为**一个**完整节点名存在(带订阅后缀),而不是被劈开")
        try? FileManager.default.removeItem(at: d)
    }

    @Test("恶意订阅名不破坏策略组结构")
    func maliciousSubNameSafe() {
        let d = dir(withNodes: ["节点A"], subName: "订阅\n  - name: INJECTED")
        let builder = ConfigBuilder(config: AppConfig(), directory: d)
        let out = builder.applySubscriptions(base, subscriptions: builder.readSubscriptions())
        // 订阅名进了「<名> 订阅」组名;注入的 INJECTED 不该成为一个独立的 proxy 或组
        #expect(ConfigBuilder.proxyNames(out) == ["节点A - 订阅\n  - name: INJECTED"]
                || ConfigBuilder.proxyNames(out).count == 1)
        #expect(ConfigBuilder.existingGroupNames(out).contains { $0.contains("INJECTED") && !$0.contains("订阅") } == false)
        try? FileManager.default.removeItem(at: d)
    }

    @Test("YAMLSurgery.quote:单引号翻倍,不提前闭合")
    func quoteEscapesSingleQuote() {
        #expect(YAMLSurgery.quote("a'b") == "'a''b'")
        #expect(YAMLSurgery.quote("") == "''")
        // 含 \u 的走双引号支,保留字面转义序列
        #expect(YAMLSurgery.quote("a\\u4e2d").hasPrefix("\""))
    }
}
