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
        // ★ 曾经写的是 250（DIRECT + REJECT）—— 那是照着当时的实现写的断言，
        //   而实现本身与 Qt 相反：Qt 明确「REJECT 既没出网也没流量，两桶都不该记」。
        //   被拦掉的流量算进直连桶，等于把「拦住了」显示成「直接出去了」。
        #expect(t.directBytes == 200)   // 只有 DIRECT
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

@Suite("用量最多（按 host 累计）")
struct TopHostsTests {

    private func conn(_ id: String, host: String, chain: String,
                      up: Int64, down: Int64, source: String = "") -> [String: Any] {
        ["id": id, "chains": [chain], "upload": NSNumber(value: up), "download": NSNumber(value: down),
         "metadata": ["host": host, "sourceIP": source]]
    }

    @Test("★ 同一个 host 的多条连接要合成一行——按连接排的话一个域名能占满整张榜")
    func aggregatesByHost() {
        var c = TrafficComposition()
        c.observe([conn("1", host: "a.com", chain: "P", up: 0, down: 100),
                   conn("2", host: "a.com", chain: "P", up: 0, down: 200),
                   conn("3", host: "b.com", chain: "P", up: 0, down: 250)])
        let top = c.topHosts()
        #expect(top.map(\.host) == ["a.com", "b.com"])
        #expect(top.first?.stat.bytes == 300)
    }

    @Test("★ 连接断了仍留在榜上——问的是「这次运行谁跑得最多」")
    func survivesDisconnect() {
        var c = TrafficComposition()
        c.observe([conn("1", host: "big.com", chain: "P", up: 0, down: 9_000)])
        c.observe([])   // 全断开
        #expect(c.topHosts().first?.host == "big.com")
        #expect(c.topHosts().first?.stat.bytes == 9_000)
    }

    @Test("★ REJECT 两桶都不记——原来算进了直连，把「被拦掉」显示成了「直连出去」")
    func rejectCountsNowhere() {
        var c = TrafficComposition()
        c.observe([conn("1", host: "ad.com", chain: "REJECT", up: 10, down: 90),
                   conn("2", host: "ad2.com", chain: "REJECT-DROP", up: 10, down: 90)])
        #expect(c.directBytes == 0)
        #expect(c.proxyBytes == 0)
        #expect(c.topHosts().isEmpty)
    }

    @Test("chains 为空按代理算（与 Qt 的 `direct = outbound == \"DIRECT\"` 一字不差）")
    func emptyChainIsProxy() {
        var c = TrafficComposition()
        c.observe([["id": "1", "chains": [String](), "upload": NSNumber(value: 0),
                    "download": NSNumber(value: 100), "metadata": ["host": "x.com"]]])
        #expect(c.directBytes == 0)
        #expect(c.proxyBytes == 100)
    }

    @Test("一个字节都没跑过的目标不占榜位")
    func zeroBytesExcluded() {
        var c = TrafficComposition()
        c.observe([conn("1", host: "idle.com", chain: "P", up: 0, down: 0)])
        #expect(c.topHosts().isEmpty)
    }
}
