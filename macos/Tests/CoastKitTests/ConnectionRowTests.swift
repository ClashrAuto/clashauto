import Foundation
import Testing
@testable import CoastKit

@Suite("连接解析")
struct ConnectionRowTests {
    private func conn(_ id: String, host: String = "", ip: String = "1.2.3.4",
                      chain: String = "🚀 节点选择", up: Int64 = 0, down: Int64 = 0,
                      process: String = "Safari") -> [String: Any] {
        ["id": id, "chains": [chain], "upload": NSNumber(value: up), "download": NSNumber(value: down),
         "metadata": ["host": host, "destinationIP": ip, "network": "tcp", "process": process] as [String: Any]]
    }

    @Test("解析字段;host 空时回退目标 IP")
    func parsesFields() {
        let rows = ConnectionRow.parse([conn("a", host: "example.com", down: 100)])
        #expect(rows.count == 1)
        #expect(rows[0].host == "example.com")
        #expect(rows[0].chain == "🚀 节点选择")
        #expect(rows[0].isProxied)
        let noHost = ConnectionRow.parse([conn("b", host: "", ip: "9.9.9.9")])
        #expect(noHost[0].host == "9.9.9.9")   // 回退 IP
    }

    @Test("按总流量降序")
    func sortsByTraffic() {
        let rows = ConnectionRow.parse([
            conn("small", host: "a", down: 10),
            conn("big", host: "b", down: 9999),
            conn("mid", host: "c", up: 500),
        ])
        #expect(rows.map(\.host) == ["b", "c", "a"])
    }

    @Test("DIRECT/REJECT/空 chain 不算代理")
    func proxiedFlag() {
        #expect(ConnectionRow.parse([conn("a", chain: "DIRECT")])[0].isProxied == false)
        #expect(ConnectionRow.parse([conn("b", chain: "REJECT")])[0].isProxied == false)
        var noChain = conn("c"); noChain["chains"] = [String]()
        #expect(ConnectionRow.parse([noChain])[0].isProxied == false)
    }

    @Test("缺 id 跳过,不崩")
    func skipsMissingID() {
        #expect(ConnectionRow.parse([["metadata": [:] as [String: Any]], ["id": ""]]).isEmpty)
    }
}
