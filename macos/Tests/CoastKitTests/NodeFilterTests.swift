import Foundation
import Testing
@testable import CoastKit

@Suite("节点搜索与筛选")
struct NodeFilterTests {

    private static func node(_ name: String, delay: Int) -> NodeInfo {
        var node = NodeInfo(name: name)
        node.delay = delay
        return node
    }

    private let nodes = [
        node("香港01 - HK Airport", delay: 88),
        node("日本02 - JP Tokyo", delay: 0),
        node("美国03 - US West", delay: 260),
        node("DIRECT", delay: 0),
    ]

    @Test("空关键词 + 不筛可用 = 原样返回")
    func passthrough() {
        #expect(NodeFilter.apply(nodes, keyword: "", onlyAvailable: false).count == 4)
        #expect(NodeFilter.apply(nodes, keyword: "   ", onlyAvailable: false).count == 4)
    }

    @Test("★ 搜索不区分大小写(中英混排的节点名是常态)")
    func caseInsensitive() {
        #expect(NodeFilter.apply(nodes, keyword: "hk", onlyAvailable: false).map(\.name)
                == ["香港01 - HK Airport"])
        #expect(NodeFilter.apply(nodes, keyword: "TOKYO", onlyAvailable: false).count == 1)
    }

    @Test("中文关键词也能搜")
    func chineseKeyword() {
        #expect(NodeFilter.apply(nodes, keyword: "美国", onlyAvailable: false).count == 1)
    }

    @Test("★ 仅可用:delay == 0 是「还没测过」,会被滤掉——这正是要提醒用户先测延迟的地方")
    func onlyAvailableFiltersUntested() {
        let visible = NodeFilter.apply(nodes, keyword: "", onlyAvailable: true)
        #expect(visible.count == 2, "应只剩测过且通的两个:\(visible.map(\.name))")
        #expect(visible.allSatisfy { $0.delay > 0 })
    }

    @Test("搜索与筛选叠加")
    func combined() {
        #expect(NodeFilter.apply(nodes, keyword: "0", onlyAvailable: true).count == 2)
        #expect(NodeFilter.apply(nodes, keyword: "日本", onlyAvailable: true).isEmpty,
                "日本节点没测过,开了「仅可用」就该看不到")
    }

    @Test("关键词首尾空格不影响结果(从别处粘过来常常带空格)")
    func trimsKeyword() {
        #expect(NodeFilter.apply(nodes, keyword: "  hk  ", onlyAvailable: false).count == 1)
    }
}
