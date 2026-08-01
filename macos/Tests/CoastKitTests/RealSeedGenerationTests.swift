import Foundation
import Testing
@testable import CoastKit

// 用**真实种子** default.yaml(8 万行) + plugin.yaml 跑一遍完整生成。
// 前面那些用例用的是精简骨架，验的是逻辑；这一条验的是「面对真种子的实际结构也不会写坏」。
@Suite("真实种子生成")
struct RealSeedGenerationTests {

    @Test("真种子跑通，且产出的 full.yaml 结构完整")
    func generatesFromRealSeeds() throws {
        try #require(Resources.seed("default.yaml") != nil, "找不到真实种子，跳过就没意义了")

        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-realseed-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        try """
        - name: 'A 机场'
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
        """.write(to: dir.appendingPathComponent("subscribe.yaml"), atomically: true, encoding: .utf8)

        var config = AppConfig()
        config.secret = "testsecret"
        let builder = ConfigBuilder(config: config, directory: dir)
        let path = try #require(builder.ensureFullConfig(tunEnabled: true))
        let yaml = try String(contentsOf: path, encoding: .utf8)

        // 想人工看生成结果时把它捞出来
        if let dump = ProcessInfo.processInfo.environment["COAST_DUMP_FULL_YAML"] {
            try? yaml.write(toFile: dump, atomically: true, encoding: .utf8)
        }

        #expect(yaml.contains("external-controller: '127.0.0.1:9191'"))
        #expect(yaml.contains("secret: 'testsecret'"))
        #expect(yaml.contains("name: '香港 01 - A 机场'"))
        #expect(yaml.contains("  - name: 'A 机场 订阅'"))
        #expect(yaml.contains("  - name: 'Auto - HK'"))
        #expect(ConfigBuilder.proxyNames(yaml) == ["香港 01 - A 机场", "日本 01 - A 机场"])

        // 主选择组引用的每一项都必须真实存在，否则核心拒绝加载整份配置
        let defined = Set(ConfigBuilder.existingGroupNames(yaml))
        let nodes = Set(ConfigBuilder.proxyNames(yaml))
        let first = try #require(YAMLSurgery.groupProxiesLine(yaml, occurrence: 0))
        for member in YAMLSurgery.groupMembers(yaml, at: first)
        where member != "DIRECT" && member != "REJECT" {
            #expect(defined.contains(member) || nodes.contains(member), "悬空引用: \(member)")
        }
    }
}
