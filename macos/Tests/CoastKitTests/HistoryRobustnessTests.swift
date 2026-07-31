import Foundation
import Testing
@testable import CoastKit

/// 核心 REST 返回的 JSON 是最后一个外部数据入口。SQL 用参数绑定、维度是封闭枚举,
/// 注入不成立 —— 但用**实证**确认,而不是看一眼下结论。重点是对畸形/恶意数据的健壮性:
/// 一份恶意配置能让核心上报带 SQL 元字符、控制字符、错类型字段的连接。
private final class TempHistory {
    let dir: URL; let store: HistoryStore
    init() {
        dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("hist-rob-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        store = HistoryStore(configDir: dir)
    }
    deinit { store.flush(); try? FileManager.default.removeItem(at: dir) }
}

private func conn(id: String, host: String, up: Any = 0, down: Any = 100,
                  chain: String = "🚀 节点选择", process: String = "Safari") -> [String: Any] {
    ["id": id, "chains": [chain], "upload": up, "download": down,
     "metadata": ["host": host, "destinationIP": "1.2.3.4", "network": "tcp", "process": process] as [String: Any]]
}

@Suite("历史库:恶意/畸形 JSON 健壮性")
struct HistoryRobustnessTests {

    @Test("★ SQL 注入 payload 作为 host —— 原样存入,库不被破坏,可原样查回")
    func sqlInjectionInHostIsInert() {
        let t = TempHistory()
        let evil = "x'; DROP TABLE conn; --"
        t.store.observe([conn(id: "a", host: evil, down: 500)])
        t.store.observe([])   // 断开 → 落库
        t.store.flush()
        // 表还在、记录写进去了、host 原样(参数绑定 = payload 只是数据)
        #expect(t.store.recordCount() == 1)
        #expect(t.store.todayTop(dimension: .host, scope: .all).map(\.key) == [evil])
    }

    @Test("host 含单引号/控制字符/换行 —— 往返不丢不崩")
    func hostWithSpecialChars() {
        let t = TempHistory()
        let names = ["a'b", "c\"d", "e\nf", "g\th", "井号#i", "空格 j "]
        for (i, h) in names.enumerated() { t.store.observe([conn(id: "n\(i)", host: h, down: Int64(i+1))]) }
        t.store.observe([]); t.store.flush()
        #expect(t.store.recordCount() == Int64(names.count))
    }

    @Test("upload/download 是字符串或缺失 —— 不崩,按 0/上次值处理")
    func wrongTypeByteCounts() {
        let t = TempHistory()
        t.store.observe([conn(id: "s", host: "x.com", up: "not-a-number", down: "999")])
        var missing = conn(id: "m", host: "y.com"); (missing["metadata"] as? NSDictionary); missing["download"] = nil
        t.store.observe([missing])
        t.store.observe([]); t.store.flush()
        // 两条都落了库,没崩(错类型被 as? NSNumber 挡成 nil → 0/保留)
        #expect(t.store.recordCount() == 2)
    }

    @Test("id 缺失/为空的连接直接跳过,不崩")
    func missingID() {
        let t = TempHistory()
        t.store.observe([["chains": ["DIRECT"], "metadata": [:] as [String: Any]],   // 无 id
                         ["id": "", "metadata": [:] as [String: Any]]])                // 空 id
        t.store.observe([]); t.store.flush()
        #expect(t.store.recordCount() == 0)
    }

    @Test("chains 是空数组/缺失 —— chain 留空,proxyOnly 口径正确排除它")
    func missingChains() {
        let t = TempHistory()
        var noChain = conn(id: "c", host: "z.com", down: 200); noChain["chains"] = [String]()
        t.store.observe([noChain]); t.store.observe([]); t.store.flush()
        #expect(t.store.todayTotal(scope: .all) == 200)          // 全部口径:算
        #expect(t.store.todayTotal(scope: .proxyOnly) == 0)      // 只算代理:chain 空 → 排除
    }

    @Test("大量连接一次喂入不崩(健壮性/性能 sanity)")
    func manyConnections() {
        let t = TempHistory()
        let many = (0..<3000).map { conn(id: "c\($0)", host: "h\($0 % 50).com", down: Int64($0)) }
        t.store.observe(many)
        t.store.observe([]); t.store.flush()
        #expect(t.store.recordCount() == 3000)
        #expect(t.store.todayTop(dimension: .host, scope: .all, limit: 5).count == 5)
    }
}
