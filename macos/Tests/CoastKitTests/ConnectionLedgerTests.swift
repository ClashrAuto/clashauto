import Foundation
import Testing

@testable import CoastKit

/// 连接账本的合并语义。这是连接查看器能回答「刚才那条连到哪去了」的全部依据 ——
/// 核心的 `/connections` 只给还活着的连接，断掉的下一拍就没了。
struct ConnectionLedgerTests {

    private func row(_ id: String, host: String = "example.com",
                     up: Int64 = 0, down: Int64 = 0) -> ConnectionRow {
        ConnectionRow(id: id, host: host, network: "tcp", type: "HTTP", process: "curl",
                      chain: "🚀 节点选择", upload: up, download: down,
                      start: .distantPast, sourceIP: "127.0.0.1")
    }

    @Test func newConnectionsArriveOnline() {
        var ledger = ConnectionLedger()
        ledger.merge([row("a"), row("b")])
        #expect(ledger.entries.count == 2)
        #expect(ledger.onlineCount == 2)
        #expect(ledger.offlineCount == 0)
    }

    /// ★ 这条是这个类型存在的理由：快照里没了的连接**不能跟着消失**，
    /// 要留在列表里标成离线。
    @Test func vanishedConnectionsBecomeOfflineInsteadOfDisappearing() {
        var ledger = ConnectionLedger()
        ledger.merge([row("a", down: 100), row("b", down: 50)])
        ledger.merge([row("a", down: 200)])          // b 断了

        #expect(ledger.entries.count == 2)
        #expect(ledger.onlineCount == 1)
        #expect(ledger.offlineCount == 1)
        let b = ledger.entries.first { $0.id == "b" }
        #expect(b?.offline == true)
        // ★ 数值保留最后一次的。清零的话用户会以为它压根没跑过流量。
        #expect(b?.row.download == 50)
    }

    /// 在线条目每拍都要**用新值覆盖**：流量在涨，host 也可能迟到
    /// （sniffer 嗅出域名后才填上，之前显示的是 IP）。
    @Test func liveEntriesTakeTheFreshValues() {
        var ledger = ConnectionLedger()
        ledger.merge([row("a", host: "1.2.3.4", down: 10)])
        ledger.merge([row("a", host: "example.com", down: 999)])

        #expect(ledger.entries.count == 1)
        #expect(ledger.entries[0].row.host == "example.com")
        #expect(ledger.entries[0].row.download == 999)
    }

    /// 一条断掉的连接**又回来了**（同一个 id）要重新算在线。
    @Test func returningConnectionGoesBackOnline() {
        var ledger = ConnectionLedger()
        ledger.merge([row("a")])
        ledger.merge([])
        #expect(ledger.entries[0].offline == true)
        ledger.merge([row("a")])
        #expect(ledger.entries[0].offline == false)
        #expect(ledger.entries.count == 1)   // 不是新增一条
    }

    /// 进程名是迟到的（find-process-mode 头几拍常常还是空的）。空值**不能**覆盖已有值，
    /// 否则那枚徽标会一闪一闪地出现又消失。
    @Test func emptyProcessDoesNotWipeAKnownOne() {
        var ledger = ConnectionLedger()
        ledger.merge([row("a")])                       // process = "curl"
        ledger.merge([rowWithoutProcess("a")])
        #expect(ledger.entries[0].row.process == "curl")
    }

    private func rowWithoutProcess(_ id: String) -> ConnectionRow {
        ConnectionRow(id: id, host: "example.com", network: "tcp", type: "HTTP", process: "",
                      chain: "🚀 节点选择", upload: 0, download: 0,
                      start: .distantPast, sourceIP: "127.0.0.1")
    }

    @Test func sortedByTotalBytesDescending() {
        var ledger = ConnectionLedger()
        ledger.merge([row("small", down: 1), row("big", down: 1000), row("mid", up: 100)])
        #expect(ledger.entries.map(\.id) == ["big", "mid", "small"])
    }

    @Test func resetClearsEverything() {
        var ledger = ConnectionLedger()
        ledger.merge([row("a")])
        ledger.reset()
        #expect(ledger.entries.isEmpty)
    }

    @Test func filterByOnlineOffline() {
        var ledger = ConnectionLedger()
        ledger.merge([row("a"), row("b")])
        ledger.merge([row("a")])   // b 离线

        #expect(ledger.filtered(online: true, offline: false, query: "").map(\.id) == ["a"])
        #expect(ledger.filtered(online: false, offline: true, query: "").map(\.id) == ["b"])
        #expect(ledger.filtered(online: false, offline: false, query: "").isEmpty)
        #expect(ledger.filtered(online: true, offline: true, query: "").count == 2)
    }

    /// 关键字对 host / 出口链 / 进程三处匹配，且大小写无关。
    @Test func filterByQueryCoversHostChainAndProcess() {
        var ledger = ConnectionLedger()
        ledger.merge([row("a", host: "GitHub.com"), row("b", host: "example.org")])

        #expect(ledger.filtered(online: true, offline: true, query: "github").map(\.id) == ["a"])
        #expect(ledger.filtered(online: true, offline: true, query: "节点选择").count == 2)
        #expect(ledger.filtered(online: true, offline: true, query: "curl").count == 2)
        #expect(ledger.filtered(online: true, offline: true, query: "nope").isEmpty)
    }
}
