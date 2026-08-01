import Foundation
import Testing
@testable import CoastKit

/// 「禁用当前节点」的反查。
///
/// 实时节点名是 `ConfigBuilder` 拼出来的（`「订阅节点名 - 订阅名」`），
/// 而禁用要写回订阅文件里的**下标**。名字对不上就会禁错节点 —— 或者更常见的：
/// 什么都不发生，用户点了没反应且毫无提示。
@Suite("禁用当前节点:实时名反查")
struct DisableNodeTests {

    private let catalog = [
        (subscription: "机场A", nodes: ["香港01", "日本02"]),
        (subscription: "机场B", nodes: ["香港01", "美国03"]),
    ]

    @Test("★ 同名节点分属不同订阅时,靠订阅名区分,不能串")
    func disambiguatesBySubscription() {
        let a = SubscriptionStore.locateNode(liveName: "香港01 - 机场A", in: catalog)
        let b = SubscriptionStore.locateNode(liveName: "香港01 - 机场B", in: catalog)
        #expect(a?.subscription == 0 && a?.node == 0)
        #expect(b?.subscription == 1 && b?.node == 0, "串到了别的订阅上:\(String(describing: b))")
    }

    @Test("★ 容忍 Qt 遗留的 [speedtest] 后缀(从 Qt 版迁移来的 full.yaml 里可能带着)")
    func toleratesSpeedtestSuffix() {
        let hit = SubscriptionStore.locateNode(liveName: "日本02 - 机场A[speedtest]", in: catalog)
        #expect(hit?.subscription == 0 && hit?.node == 1)
    }

    @Test("★ 组名 / DIRECT / REJECT 查不到 —— 这不是异常,点到它们时什么都不该发生")
    func nonSubscriptionNamesMiss() {
        #expect(SubscriptionStore.locateNode(liveName: "DIRECT", in: catalog) == nil)
        #expect(SubscriptionStore.locateNode(liveName: "🚀 节点选择", in: catalog) == nil)
        #expect(SubscriptionStore.locateNode(liveName: "", in: catalog) == nil)
    }

    @Test("节点名里本身带「 - 」也不会切错")
    func nodeNameContainingSeparator() {
        let tricky = [(subscription: "机场C", nodes: ["香港 - 家宽 01"])]
        let hit = SubscriptionStore.locateNode(liveName: "香港 - 家宽 01 - 机场C", in: tricky)
        #expect(hit?.node == 0, "节点名里带分隔符时反查失败")
    }

    @Test("★ 真写盘:禁用后该节点 use 变 false,其余不动")
    func disablesOnDisk() throws {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-disable-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        try """
        - name: '机场A'
          url: 'https://e.example/sub'
          type: 'sub'
          use: true
          list:
            - name: '香港01'
              server: '1.1.1.1'
              port: 443
              use: true
            - name: '日本02'
              server: '2.2.2.2'
              port: 443
              use: true
        """.write(to: dir.appendingPathComponent("subscribe.yaml"),
                  atomically: true, encoding: .utf8)

        var config = AppConfig(); config.secret = "x"
        let store = SubscriptionStore(config: config, directory: dir)
        #expect(store.disableNode(liveName: "香港01 - 机场A"), "禁用没生效")

        let nodes = store.nodes(at: 0)
        #expect(nodes.first { $0.name == "香港01" }?.use == false, "目标节点没被禁用")
        #expect(nodes.first { $0.name == "日本02" }?.use == true, "把别的节点也一起禁了")
        #expect(store.disableNode(liveName: "不存在 - 机场A") == false)
    }
}
