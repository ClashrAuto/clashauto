import Foundation
import Testing
@testable import CoastKit

/// 每个用例一个临时库。历史库真的落盘，不隔离会污染开发者自己的 coast.db。
private final class TempHistory {
    let directory: URL
    let store: HistoryStore

    init() {
        directory = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-hist-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        store = HistoryStore(configDir: directory)
    }

    deinit {
        store.flush()
        try? FileManager.default.removeItem(at: directory)
    }
}

/// 造一条 `/connections` 里的连接。
private func connection(id: String, host: String = "", destIP: String = "1.2.3.4",
                        chain: String = "🚀 节点选择", process: String = "Safari",
                        up: Int64 = 0, down: Int64 = 0) -> [String: Any] {
    [
        "id": id,
        "chains": [chain],
        "upload": NSNumber(value: up),
        "download": NSNumber(value: down),
        "metadata": [
            "host": host,
            "destinationIP": destIP,
            "network": "tcp",
            "process": process,
        ] as [String: Any],
    ]
}

@Suite("历史库：连接生命周期")
struct HistoryLifecycleTests {

    @Test("连接还在时不落库，消失了才落一条")
    func writesOnDisappear() {
        let temp = TempHistory()
        #expect(temp.store.isOpen)

        temp.store.observe([connection(id: "a", host: "example.com", down: 1000)])
        temp.store.flush()
        #expect(temp.store.recordCount() == 0)   // 还在途，不该落库

        temp.store.observe([])                    // a 消失 = 已断开
        temp.store.flush()
        #expect(temp.store.recordCount() == 1)
    }

    @Test("退出时 flush(includingLive:) 把在途的长连接也落下来")
    func flushIncludesLive() {
        let temp = TempHistory()
        temp.store.observe([connection(id: "long", host: "a.com", down: 5000)])
        temp.store.flush(includingLive: true)
        // 一条挂了几小时的连接，不这样处理就永远进不了库
        #expect(temp.store.recordCount() == 1)
    }

    @Test("host 迟到时要跟上，但空值不能把已有域名抹回 IP")
    func hostArrivesLate() {
        let temp = TempHistory()
        // 第一拍还没嗅探出域名
        temp.store.observe([connection(id: "a", host: "", destIP: "93.184.216.34", down: 10)])
        // 第二拍 sniffer 出结果了
        temp.store.observe([connection(id: "a", host: "example.com", destIP: "93.184.216.34", down: 20)])
        // 第三拍又是空的（核心偶尔如此）——不能覆盖回去
        temp.store.observe([connection(id: "a", host: "", destIP: "93.184.216.34", down: 30)])
        temp.store.observe([])
        temp.store.flush()

        let top = temp.store.todayTop(dimension: .host, scope: .all)
        #expect(top.map(\.key) == ["example.com"])
    }

    @Test("完全没嗅到域名时用目标 IP 兜底，不留空记录")
    func fallsBackToDestIP() {
        let temp = TempHistory()
        temp.store.observe([connection(id: "a", host: "", destIP: "9.9.9.9", down: 10)])
        temp.store.observe([])
        temp.store.flush()
        #expect(temp.store.todayTop(dimension: .host, scope: .all).map(\.key) == ["9.9.9.9"])
    }

    @Test("字节数取连接最后一次的累计值")
    func usesLastByteCount() {
        let temp = TempHistory()
        temp.store.observe([connection(id: "a", host: "x.com", down: 100)])
        temp.store.observe([connection(id: "a", host: "x.com", down: 999)])
        temp.store.observe([])
        temp.store.flush()
        #expect(temp.store.todayTotal(scope: .all) == 999)
    }
}

@Suite("历史库：统计口径")
struct HistoryAggregationTests {

    @Test("proxyOnly 排除 DIRECT/REJECT，也排除 chain 为空的")
    func proxyOnlyScope() {
        let temp = TempHistory()
        temp.store.observe([
            connection(id: "p", host: "proxied.com", chain: "🚀 节点选择", down: 100),
            connection(id: "d", host: "direct.com", chain: "DIRECT", down: 200),
            connection(id: "r", host: "rejected.com", chain: "REJECT", down: 400),
        ])
        // chain 为空的：状态不明，不该被算进「代理流量」
        var unknown = connection(id: "u", host: "unknown.com", down: 800)
        unknown["chains"] = [String]()
        temp.store.observe([
            connection(id: "p", host: "proxied.com", chain: "🚀 节点选择", down: 100),
            connection(id: "d", host: "direct.com", chain: "DIRECT", down: 200),
            connection(id: "r", host: "rejected.com", chain: "REJECT", down: 400),
            unknown,
        ])
        temp.store.observe([])
        temp.store.flush()

        #expect(temp.store.todayTotal(scope: .all) == 1500)
        #expect(temp.store.todayTotal(scope: .proxyOnly) == 100)
    }

    @Test("Top N 按字节降序、能按进程或域名分组")
    func topGrouping() {
        let temp = TempHistory()
        temp.store.observe([
            connection(id: "1", host: "a.com", process: "Safari", down: 100),
            connection(id: "2", host: "b.com", process: "Safari", down: 300),
            connection(id: "3", host: "c.com", process: "Chrome", down: 50),
        ])
        temp.store.observe([])
        temp.store.flush()

        let byHost = temp.store.todayTop(dimension: .host, scope: .all)
        #expect(byHost.map(\.key) == ["b.com", "a.com", "c.com"])

        let byProcess = temp.store.todayTop(dimension: .process, scope: .all)
        #expect(byProcess.map(\.key) == ["Safari", "Chrome"])
        #expect(byProcess.first?.bytes == 400)   // 两条 Safari 合并
    }

    @Test("Top N 尊重 limit")
    func topRespectsLimit() {
        let temp = TempHistory()
        let many = (1...10).map { connection(id: "c\($0)", host: "h\($0).com", down: Int64($0 * 10)) }
        temp.store.observe(many)
        temp.store.observe([])
        temp.store.flush()
        #expect(temp.store.todayTop(dimension: .host, scope: .all, limit: 3).count == 3)
    }

    @Test("24 个小时桶，合计等于今日总量")
    func hourlyBuckets() {
        let temp = TempHistory()
        temp.store.observe([connection(id: "a", host: "x.com", down: 123)])
        temp.store.observe([])
        temp.store.flush()
        let buckets = temp.store.todayHourly(scope: .all)
        #expect(buckets.count == 24)
        #expect(buckets.reduce(0, +) == 123)
    }

    @Test("库是空的时候所有查询都返回零值，不崩")
    func emptyDatabase() {
        let temp = TempHistory()
        #expect(temp.store.recordCount() == 0)
        #expect(temp.store.todayTotal() == 0)
        #expect(temp.store.todayTop(dimension: .host).isEmpty)
        #expect(temp.store.todayHourly() == [Int64](repeating: 0, count: 24))
    }
}

@Suite("历史库：时间处理")
struct HistoryTimeTests {

    @Test("今日区间用本地时区 —— 用 UTC 的话东八区早上 8 点前看到的「今日」是昨天")
    func todayRangeIsLocal() {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(secondsFromGMT: 8 * 3600)!
        // 北京时间 2026-07-31 03:00（此刻 UTC 还是 7-30）
        var components = DateComponents()
        components.year = 2026; components.month = 7; components.day = 31; components.hour = 3
        components.timeZone = calendar.timeZone
        let now = calendar.date(from: components)!

        let (start, end) = HistoryStore.todayRange(now: now, calendar: calendar)
        let startDate = Date(timeIntervalSince1970: Double(start) / 1000)
        #expect(calendar.component(.day, from: startDate) == 31)   // 是 31 号 00:00，不是 30 号
        #expect(end - start == 86_400_000)
    }

    @Test("RFC3339 起始时间：带/不带小数秒都要认，认不出返回 nil 由调用方兜底")
    func parsesStart() {
        #expect(HistoryStore.parseStart("2026-07-31T10:00:00.123456789Z") != nil)
        #expect(HistoryStore.parseStart("2026-07-31T10:00:00Z") != nil)
        #expect(HistoryStore.parseStart("") == nil)
        #expect(HistoryStore.parseStart("不是时间") == nil)
    }
}
