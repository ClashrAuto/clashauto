import Foundation

/// 上网历史库（`coast.db` 的 `conn` 表）——「什么时候、连了哪个域名、走的哪个节点、多少字节」。
///
/// 为什么要落盘：核心的 `/connections` 只给**当前活动**连接，断了就没了。要能回看、要能算
/// 「今日流量 Top 域名/进程」，就必须存起来；而这个量级（每天几万条、按时间和域名查）
/// 用 JSON 是自找麻烦。
///
/// 写入时机是**连接结束时落一条**，不是跟着每秒轮询逐条写：
///   • 每秒把 `/connections` 快照喂进 `observe()`，本类维护一张「在途连接」表；
///   • 某个 id 在新快照里消失 = 连接已断 → 用它最后一次的累计字节生成记录进待写缓冲；
///   • 缓冲攒够或定时到点，**一个事务**批量写盘。
///
/// 退出时 `flush()`：仍在途的长连接也各落一条，否则一条挂了几小时的连接永远进不了库。
public final class HistoryStore: @unchecked Sendable {

    public struct Record: Sendable, Equatable {
        public var host = ""       // 域名（sniffer 之后多数有值），没有则是目标 IP
        public var destIP = ""
        public var chain = ""      // 出口策略/节点（chains[0]）
        public var network = ""    // tcp/udp
        public var process = ""    // 发起进程
        /// 发起方 IP。**不入库**，只用来在落盘那一刻解析成设备 MAC（见 `deviceResolver`）。
        public var sourceIP = ""
        public var startedAt: Int64 = 0   // unix ms
        public var endedAt: Int64 = 0
        public var up: Int64 = 0
        public var down: Int64 = 0
    }

    /// 「某一天 → 上/下行」聚合（设备详情的近 7 天柱状图）。
    public struct DayTotal: Sendable, Identifiable, Equatable {
        /// `yyyy-MM-dd`。
        public let day: String
        public let up: Int64
        public let down: Int64
        public var id: String { day }
        public var total: Int64 { up + down }
    }

    /// 「某一项 → 字节」聚合（今日流量卡的 Top N）。
    public struct GroupTotal: Sendable, Identifiable {
        public let key: String
        public let bytes: Int64
        public var id: String { key }
    }

    /// 统计口径：全部流量，还是只算走了代理的（chain 既不是 DIRECT 也不是 REJECT）。
    public enum Scope: Sendable { case all, proxyOnly }
    /// 分组维度。
    /// 「今日流量」的聚合维度。
    ///
    /// `device` 用的是 `conn.mac` —— 这一列建表时就在写，却一直没有任何查询用它，
    /// 于是「这些流量是哪台设备跑的」这个问题在界面上无从回答（Qt 那边有这个 tab）。
    public enum Dimension: Sendable { case process, host, device }

    /// 保留天数。超过就删 —— 无限增长的库迟早会让「今日流量」查询慢到几秒。
    public static let retentionDays = 30

    private let database: SQLiteDatabase?
    /// 在途连接：id → 最后一次看到的样子。
    private var live: [String: Live] = [:]
    private var pending: [Record] = []
    /// `sourceIP` → 设备 MAC。**在落盘那一刻解析**（与 Qt 同）—— 查询时再解析的话，
    /// 设备换了 IP（DHCP 续租）就把历史流量算到别的设备头上了。
    ///
    /// 由 `AppState` 用 `DeviceStore` 的台账定期喂进来。没喂时 `mac` 列留空，
    /// 「按设备」的聚合自然什么都查不到 —— 宁可查不到，也不写出错误数据。
    private var deviceMap: [String: String] = [:]
    private let lock = NSLock()

    /// 攒够这么多条才写一次盘。
    private static let flushThreshold = 64

    private struct Live {
        var record = Record()
        var seen = false
    }

    /// 更新 IP→MAC 映射。设备开关变动或扫描完一轮时调一次即可。
    public func setDeviceMap(_ map: [String: String]) {
        lock.lock()
        defer { lock.unlock() }
        deviceMap = map
    }

    public var isOpen: Bool { database?.isOpen ?? false }

    public init(configDir: URL = AppPaths.configDir) {
        let path = SQLitePaths.databasePath(configDir: configDir)
        database = try? SQLiteDatabase(path: path)
        createSchema()
        purgeOld()
    }

    private func createSchema() {
        guard let database else { return }
        database.exec("""
            CREATE TABLE IF NOT EXISTS conn (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              mac TEXT NOT NULL DEFAULT '',
              host TEXT NOT NULL DEFAULT '',
              dest_ip TEXT NOT NULL DEFAULT '',
              chain TEXT NOT NULL DEFAULT '',
              network TEXT NOT NULL DEFAULT '',
              process TEXT NOT NULL DEFAULT '',
              started_at INTEGER NOT NULL DEFAULT 0,
              ended_at INTEGER NOT NULL DEFAULT 0,
              up INTEGER NOT NULL DEFAULT 0,
              down INTEGER NOT NULL DEFAULT 0)
            """)
        // 查询就两类：「某时间段」和「按域名/进程聚合」，索引照着来。
        database.exec("CREATE INDEX IF NOT EXISTS conn_ended ON conn(ended_at)")
        // 设备详情要按 mac 查「近 7 天」和「常用域名」，没有这条索引就是全表扫。
        database.exec("CREATE INDEX IF NOT EXISTS conn_mac ON conn(mac, ended_at)")
        database.exec("PRAGMA user_version=1")
    }

    // MARK: - 观察

    /// 喂一份 `/connections` 快照。**每秒调一次**，与 `ClashService` 的轮询同拍。
    public func observe(_ connections: [[String: Any]]) {
        guard isOpen else { return }
        lock.lock()
        defer { lock.unlock() }

        for key in live.keys { live[key]?.seen = false }

        let now = Int64(Date().timeIntervalSince1970 * 1000)
        for connection in connections {
            guard let id = connection["id"] as? String, !id.isEmpty else { continue }
            let metadata = connection["metadata"] as? [String: Any] ?? [:]

            var entry = live[id] ?? Live()
            if entry.record.startedAt == 0 {
                // 第一次见到这条连接：填那些不会变的字段
                entry.record.startedAt = Self.parseStart(connection["start"] as? String) ?? now
                entry.record.destIP = metadata["destinationIP"] as? String ?? ""
                entry.record.network = metadata["network"] as? String ?? ""
                entry.record.process = metadata["process"] as? String ?? ""
                entry.record.sourceIP = metadata["sourceIP"] as? String ?? ""
            }
            // host 可能**迟到** —— sniffer 嗅出域名后才填上。所以每拍都跟一次；
            // 但空值不覆盖已有值，否则会把已经拿到的域名又抹回 IP。
            if let host = metadata["host"] as? String, !host.isEmpty {
                entry.record.host = host
            }
            if let chains = connection["chains"] as? [String], let first = chains.first {
                entry.record.chain = first
            }
            entry.record.up = (connection["upload"] as? NSNumber)?.int64Value ?? entry.record.up
            entry.record.down = (connection["download"] as? NSNumber)?.int64Value ?? entry.record.down
            entry.seen = true
            live[id] = entry
        }

        // 这一拍没出现的 id = 连接已断，落一条。
        for (id, entry) in live where !entry.seen {
            var record = entry.record
            record.endedAt = now
            if record.host.isEmpty { record.host = record.destIP }
            pending.append(record)
            live.removeValue(forKey: id)
        }

        if pending.count >= Self.flushThreshold { flushLocked() }
    }

    /// 把待写缓冲落盘。退出前必须调 —— 否则挂着的长连接与最后一批记录全丢。
    public func flush(includingLive: Bool = false) {
        lock.lock()
        defer { lock.unlock() }
        if includingLive {
            let now = Int64(Date().timeIntervalSince1970 * 1000)
            for (_, entry) in live {
                var record = entry.record
                record.endedAt = now
                if record.host.isEmpty { record.host = record.destIP }
                pending.append(record)
            }
            live.removeAll()
        }
        flushLocked()
    }

    /// 调用方必须已持有 `lock`。
    private func flushLocked() {
        guard let database, !pending.isEmpty else { return }
        let sql = """
            INSERT INTO conn (mac, host, dest_ip, chain, network, process, started_at, ended_at, up, down)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """
        let statements = pending.map { record in
            (sql, [
                SQLiteDatabase.Value.text(deviceMap[record.sourceIP] ?? ""),
                .text(record.host),
                .text(record.destIP),
                .text(record.chain),
                .text(record.network),
                .text(record.process),
                .int(record.startedAt),
                .int(record.endedAt),
                .int(record.up),
                .int(record.down),
            ])
        }
        if database.transaction(statements) { pending.removeAll() }
    }

    private func purgeOld() {
        guard let database else { return }
        let cutoff = Int64(Date().addingTimeInterval(-Double(Self.retentionDays) * 86400)
            .timeIntervalSince1970 * 1000)
        database.run("DELETE FROM conn WHERE ended_at < ?", [.int(cutoff)])
    }

    // MARK: - 查询

    public func recordCount() -> Int64 {
        var count: Int64 = 0
        database?.query("SELECT COUNT(*) FROM conn") { row in count = row.int(0) }
        return count
    }

    /// 今日按小时的字节数（24 个桶，索引 0..23 = 本地时间的 0..23 点）。
    public func todayHourly(scope: Scope = .proxyOnly) -> [Int64] {
        var buckets = [Int64](repeating: 0, count: 24)
        guard let database else { return buckets }
        let (start, end) = Self.todayRange()
        database.query("""
            SELECT ended_at, up + down FROM conn
            WHERE ended_at >= ? AND ended_at < ? \(Self.scopeClause(scope))
            """, [.int(start), .int(end)]) { row in
            let date = Date(timeIntervalSince1970: Double(row.int(0)) / 1000)
            let hour = Calendar.current.component(.hour, from: date)
            if hour >= 0, hour < 24 { buckets[hour] += row.int(1) }
        }
        return buckets
    }

    /// 今日 Top N（按进程或域名）。
    public func todayTop(dimension: Dimension, scope: Scope = .proxyOnly, limit: Int = 5) -> [GroupTotal] {
        guard let database else { return [] }
        let column: String
        switch dimension {
        case .process: column = "process"
        case .host: column = "host"
        case .device: column = "mac"
        }
        let (start, end) = Self.todayRange()
        var result: [GroupTotal] = []
        database.query("""
            SELECT \(column), SUM(up + down) AS bytes FROM conn
            WHERE ended_at >= ? AND ended_at < ? AND \(column) != '' \(Self.scopeClause(scope))
            GROUP BY \(column) ORDER BY bytes DESC LIMIT ?
            """, [.int(start), .int(end), .int(Int64(limit))]) { row in
            result.append(GroupTotal(key: row.text(0), bytes: row.int(1)))
        }
        return result
    }

    public func todayTotal(scope: Scope = .proxyOnly) -> Int64 {
        guard let database else { return 0 }
        let (start, end) = Self.todayRange()
        var total: Int64 = 0
        database.query("""
            SELECT COALESCE(SUM(up + down), 0) FROM conn
            WHERE ended_at >= ? AND ended_at < ? \(Self.scopeClause(scope))
            """, [.int(start), .int(end)]) { row in total = row.int(0) }
        return total
    }

    // MARK: - 按设备（设备详情窗）

    /// 某台设备近 N 天的每日上/下行。返回**恰好 N 项**、按日期升序，没有记录的那天补 0 ——
    /// 缺天不补的话柱状图的横轴会跳着走（7 根柱子只画出 3 根，日期还不连续）。
    public func recentDays(mac: String, days: Int = 7) -> [DayTotal] {
        var buckets: [String: (up: Int64, down: Int64)] = [:]
        let calendar = Calendar.current
        let today = calendar.startOfDay(for: Date())

        if let database, !mac.isEmpty {
            let start = calendar.date(byAdding: .day, value: -(days - 1), to: today) ?? today
            database.query("""
                SELECT ended_at, up, down FROM conn
                WHERE mac = ? AND ended_at >= ?
                """, [.text(mac), .int(Int64(start.timeIntervalSince1970 * 1000))]) { row in
                let date = Date(timeIntervalSince1970: Double(row.int(0)) / 1000)
                let key = Self.dayKey(calendar.startOfDay(for: date))
                var bucket = buckets[key] ?? (0, 0)
                bucket.up += row.int(1)
                bucket.down += row.int(2)
                buckets[key] = bucket
            }
        }

        return (0..<days).reversed().map { offset in
            let date = calendar.date(byAdding: .day, value: -offset, to: today) ?? today
            let key = Self.dayKey(date)
            let bucket = buckets[key] ?? (0, 0)
            return DayTotal(day: key, up: bucket.up, down: bucket.down)
        }
    }

    /// 某台设备的常用域名（按累计字节降序）。
    public func topDomains(mac: String, limit: Int = 5) -> [GroupTotal] {
        guard let database, !mac.isEmpty else { return [] }
        var result: [GroupTotal] = []
        database.query("""
            SELECT host, SUM(up + down) AS bytes FROM conn
            WHERE mac = ? AND host != ''
            GROUP BY host ORDER BY bytes DESC LIMIT ?
            """, [.text(mac), .int(Int64(limit))]) { row in
            result.append(GroupTotal(key: row.text(0), bytes: row.int(1)))
        }
        return result
    }

    /// 某台设备的累计上/下行（保留期内的全部记录）。
    public func total(mac: String) -> (up: Int64, down: Int64) {
        guard let database, !mac.isEmpty else { return (0, 0) }
        var result: (up: Int64, down: Int64) = (0, 0)
        database.query("SELECT COALESCE(SUM(up),0), COALESCE(SUM(down),0) FROM conn WHERE mac = ?",
                       [.text(mac)]) { row in result = (row.int(0), row.int(1)) }
        return result
    }

    /// `yyyy-MM-dd`。锁 `en_US_POSIX` + 公历 —— 跟随区域设置的话，
    /// 和历/佛历用户的 key 会是 R8-08-01 之类，与库里既有的对不上。
    static func dayKey(_ date: Date) -> String {
        formatterForDayKey.string(from: date)
    }

    private static let formatterForDayKey: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.calendar = Calendar(identifier: .gregorian)
        f.dateFormat = "yyyy-MM-dd"
        return f
    }()

    // MARK: - 工具

    /// 「只算走代理的」= chain 既不是 DIRECT 也不是 REJECT。
    /// chain 为空的（还没拿到链路信息）也排除，否则会把状态不明的算进代理流量里。
    static func scopeClause(_ scope: Scope) -> String {
        scope == .proxyOnly ? "AND chain NOT IN ('DIRECT', 'REJECT', '')" : ""
    }

    /// 本地时区的今天 [00:00, 次日 00:00) 的 unix 毫秒。
    ///
    /// 必须用本地时区 —— 用 UTC 的话，东八区用户在早上 8 点前看到的「今日」其实是昨天。
    static func todayRange(now: Date = Date(), calendar: Calendar = .current) -> (Int64, Int64) {
        let start = calendar.startOfDay(for: now)
        let end = calendar.date(byAdding: .day, value: 1, to: start) ?? start.addingTimeInterval(86400)
        return (Int64(start.timeIntervalSince1970 * 1000), Int64(end.timeIntervalSince1970 * 1000))
    }

    /// 核心给的 start 是 RFC3339（带小数秒与时区）。解析不了就返回 nil，调用方用「此刻」兜底 ——
    /// 起始时间不准只影响时长显示，不该让整条记录丢掉。
    static func parseStart(_ raw: String?) -> Int64? {
        guard let raw, !raw.isEmpty else { return nil }
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let date = formatter.date(from: raw) { return Int64(date.timeIntervalSince1970 * 1000) }
        formatter.formatOptions = [.withInternetDateTime]
        if let date = formatter.date(from: raw) { return Int64(date.timeIntervalSince1970 * 1000) }
        return nil
    }
}
