import Foundation
import Testing
@testable import CoastKit

/// 从 Qt 版迁移过来的数据文件，Swift 版能不能原样吃下。
///
/// 这不是假想场景：这次是**替换**掉 Qt 版，用户的 `~/Library/Application Support/` 下就是
/// Qt 写出来的 `subscribe.yaml`。解析器要是在某个不认识的字段上翻车，用户升级后订阅全丢，
/// 而且丢得毫无征兆 —— 界面只会显示「没有订阅」。
@Suite("Qt 版数据文件的迁移兼容")
struct QtMigrationTests {

    /// 与 `clashauto-c++/src/SubscriptionStore.cpp` 写出的块**逐字段一致**。
    /// 其中 `speedtest` / `proxy` / `updateTime` 是 Swift 侧没有声明的字段。
    private let qtFormat = """
    - name: '机场A'
      url: 'https://example.com/sub'
      type: 'sub'
      use: true
      updateTime: 15
      speedtest: false
      proxy: false
      list:
        - name: '香港01'
          server: '1.2.3.4'
          port: 443
          use: true
        - name: '日本02'
          server: '5.6.7.8'
          port: 443
          use: false
    - name: '机场B'
      url: 'https://example.org/sub'
      type: 'sub'
      use: false
      updateTime: 30
      speedtest: true
      proxy: false
      list: []
    """

    private func store(with text: String) throws -> (SubscriptionStore, URL) {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-migrate-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try text.write(to: dir.appendingPathComponent("subscribe.yaml"),
                       atomically: true, encoding: .utf8)
        var config = AppConfig()
        config.secret = "x"
        return (SubscriptionStore(config: config, directory: dir), dir)
    }

    @Test("★ Qt 写的 subscribe.yaml:两条订阅、节点与启用状态都读得出来")
    func readsQtSubscriptions() throws {
        let (store, dir) = try store(with: qtFormat)
        defer { try? FileManager.default.removeItem(at: dir) }

        let subs = store.load()
        #expect(subs.count == 2, "订阅条数不对:\(subs.count) —— 用户升级后会看到订阅丢失")
        guard subs.count == 2 else { return }
        #expect(subs[0].name == "机场A")
        #expect(subs[0].use, "use: true 没读出来")
        #expect(subs[1].name == "机场B")
        #expect(subs[1].use == false, "use: false 没读出来")
        let nodes = store.nodes(at: 0)
        #expect(nodes.count == 2, "节点没读全:\(nodes.count)")
        guard nodes.count == 2 else { return }
        #expect(nodes[0].name == "香港01")
        #expect(nodes[0].use, "节点 use: true 没读出来")
        #expect(nodes[1].use == false, "单个节点的 use: false 没读出来")
        #expect(subs[0].nodeCount == 2)
        #expect(subs[0].enabledNodeCount == 1, "启用节点计数不对")
    }

    @Test("★ 不认识的字段(speedtest/proxy/updateTime)改写后不能丢")
    func preservesUnknownFields() throws {
        let (store, dir) = try store(with: qtFormat)
        defer { try? FileManager.default.removeItem(at: dir) }

        // 改一下启用状态 → 触发回写
        _ = store.setSubscriptionEnabled(at: 1, true)
        let text = try String(contentsOf: dir.appendingPathComponent("subscribe.yaml"),
                              encoding: .utf8)
        // 这几个字段 Swift 用不上，但**不能因为回写就把它们抹掉** —— 用户若装回 Qt 版
        // （或将来 Swift 版要用），这些信息就没了。文本手术的意义正在于此。
        for field in ["speedtest:", "proxy:", "updateTime:"] {
            #expect(text.contains(field), "回写后丢了 \(field)")
        }
        #expect(store.load().count == 2, "回写后订阅条数变了")
    }

    @Test("Swift 生成的 subscribe.yaml，Qt 的字段布局也认得（两格缩进、list: 四格列表项）")
    func writesQtCompatibleLayout() throws {
        let (store, dir) = try store(with: "")
        defer { try? FileManager.default.removeItem(at: dir) }
        _ = store.addSubscription(name: "新机场", url: "https://x.example/sub", type: "sub")
        let text = try String(contentsOf: dir.appendingPathComponent("subscribe.yaml"),
                              encoding: .utf8)
        #expect(text.contains("- name:"), "顶层条目不是 `- name:`")
        #expect(text.contains("\n  url:"), "字段不是两格缩进 —— Qt 的解析器按缩进分层")
        #expect(text.contains("\n  list:"), "缺 list: 段")
    }
}

/// `rules.json` 的迁移。Qt 的 schema：
/// `{"area":[{name,type,rule}], "rule":[{type,node,value}]}`（见 ConfigBuilder.cpp:331-333）。
extension QtMigrationTests {

    @Test("★ Qt 写的 rules.json:区域组与自定义规则都能读出来并生成进 full.yaml")
    func readsQtRulesJSON() throws {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-rules-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        // 与 Qt 完全一致的 JSON（键序也照抄，避免「只是恰好能解析」）
        let qtJSON = """
        {"area":[{"name":"我的港区","type":"url-test","rule":"香港|HK"}],
         "rule":[{"type":"DOMAIN-SUFFIX","node":"DIRECT","value":"example.com"},
                 {"type":"MATCH","node":"🚀 节点选择","value":""}]}
        """
        try qtJSON.write(to: dir.appendingPathComponent("rules.json"),
                         atomically: true, encoding: .utf8)

        // 经 RulesStore 读出来
        let store = RulesStore(directory: dir)
        let (rules, areas) = store.load()
        #expect(areas.count == 1, "区域组没读出来")
        #expect(areas.first?.name == "我的港区")
        #expect(areas.first?.rule == "香港|HK")
        #expect(areas.first?.type == "url-test")
        #expect(rules.count == 2, "自定义规则没读全:\(rules.count)")
        #expect(rules.first?.type == "DOMAIN-SUFFIX")
        #expect(rules.first?.value == "example.com")
        // MATCH 只有两段 —— 这条最容易在拼装时出错
        #expect(rules.last?.ruleLine == "MATCH,🚀 节点选择")
    }
}
