import Foundation
import Testing
@testable import CoastKit

@Suite("会话流量构成")
struct TrafficCompositionTests {
    private func conn(_ id: String, chain: String, up: Int64 = 0, down: Int64 = 0) -> [String: Any] {
        ["id": id, "chains": [chain], "upload": NSNumber(value: up), "download": NSNumber(value: down)]
    }

    @Test("按出口链分直连/代理桶")
    func splitsByChain() {
        var t = TrafficComposition()
        t.observe([conn("a", chain: "🚀 节点选择", down: 100),
                   conn("b", chain: "DIRECT", down: 200),
                   conn("c", chain: "REJECT", down: 50)])
        #expect(t.proxyBytes == 100)
        #expect(t.directBytes == 250)   // DIRECT + REJECT
    }

    @Test("★ 按连接攒增量,不是每拍重复计总数")
    func accumulatesDeltas() {
        var t = TrafficComposition()
        t.observe([conn("a", chain: "🚀", down: 100)])   // +100
        t.observe([conn("a", chain: "🚀", down: 300)])   // +200(增量)
        t.observe([conn("a", chain: "🚀", down: 300)])   // +0(没变)
        #expect(t.proxyBytes == 300)   // 不是 100+300+300
    }

    @Test("新连接的全部字节算增量")
    func newConnectionFullDelta() {
        var t = TrafficComposition()
        t.observe([conn("a", chain: "DIRECT", down: 500)])
        t.observe([conn("a", chain: "DIRECT", down: 500),
                   conn("b", chain: "DIRECT", down: 999)])   // b 新出现,+999
        #expect(t.directBytes == 1499)
    }

    @Test("连接断开后不再累加,总数保留")
    func disconnectStopsAccumulation() {
        var t = TrafficComposition()
        t.observe([conn("a", chain: "🚀", down: 100)])
        t.observe([])                                 // a 断开
        t.observe([conn("b", chain: "🚀", down: 50)]) // 新连接
        #expect(t.proxyBytes == 150)                  // a 的 100 保留 + b 的 50
    }

    @Test("核心把已有连接计数清零时不倒扣(不出现负流量)")
    func counterResetDoesNotUnderflow() {
        var t = TrafficComposition()
        t.observe([conn("a", chain: "DIRECT", down: 1000)])
        t.observe([conn("a", chain: "DIRECT", down: 10)])   // 清零重来:delta=0 不是 -990
        #expect(t.directBytes == 1000)
    }

    @Test("上下行都计入")
    func countsBothDirections() {
        var t = TrafficComposition()
        t.observe([conn("a", chain: "DIRECT", up: 30, down: 70)])
        #expect(t.directBytes == 100)
    }
}
