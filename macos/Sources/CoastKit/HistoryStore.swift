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
        /// 界面上显示的名字。设备取台账里的名字；**空 key 三种维度含义不同，各给一句人话**
        /// （Qt `HistoryStore::topGroups` 的 `label`）。
        public let label: String
        public var id: String { key }

        public init(key: String, bytes: Int64, label: String? = nil) {
            self.key = key
            self.bytes = bytes
            self.label = label ?? key
        }
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
    /// 今日按维度分组的前 N 名。
    ///
    /// 与 Qt `topGroups()` 对齐的三件事，原来一件都没做：
    ///
    ///   1. **算上在途连接**。只查库的话，一个还没断的大下载在榜上完全看不见 ——
    ///      而「今天谁跑得最多」多半问的就是它。
    ///   2. **空 key 不丢，给名字**。原来 SQL 里写着 `\(column) != ''` 直接滤掉：
    ///      macOS 上绝大多数连接**没有 mac**（只有走透明代理的局域网设备才有），
    ///      于是「设备」这一维几乎永远是空的，而真相是那些流量都是本机的。
    ///      三种维度的「空」含义不同：设备 = 本机/未归属、进程 = 其它设备（进程名只有
    ///      本机连接才有）、域名 = 未知域名。
    ///   3. **设备维度显示台账里的名字**，不是一串 MAC。`deviceNames` 由调用方喂进来
    ///      （历史库不认识台账）。
    @MainActor
    public func todayTop(dimension: Dimension, scope: Scope = .proxyOnly, limit: Int = 5,
                         deviceNames: [String: String] = [:]) -> [GroupTotal] {
        let column: String
        switch dimension {
        case .process: column = "process"
        case .host: column = "host"
        case .device: column = "mac"
        }
        let (start, end) = Self.todayRange()
        var aggregate: [String: Int64] = [:]

        database?.query("""
            SELECT \(column), SUM(up + down) AS bytes FROM conn
            WHERE ended_at >= ? AND ended_at < ? \(Self.scopeClause(scope))
            GROUP BY \(column)
            """, [.int(start), .int(end)]) { row in
            aggregate[row.text(0), default: 0] += row.int(1)
        }

        for (record, _) in liveSnapshot() {
            guard scope == .all || Self.isProxied(record.chain) else { continue }
            let key: String
            switch dimension {
            case .process: key = record.process
            case .host: key = record.host.isEmpty ? record.destIP : record.host
            case .device: key = liveMAC(record)
            }
            aggregate[key, default: 0] += record.up + record.down
        }

        var totals: [GroupTotal] = []
        for (key, bytes) in aggregate where bytes > 0 {
            let label = Self.label(for: key, dimension: dimension, deviceNames: deviceNames)
            totals.append(GroupTotal(key: key, bytes: bytes, label: label))
        }
        // 字节相同时按 key 定序 —— 字典遍历顺序不定，否则两行会来回换位置。
        totals.sort { $0.bytes == $1.bytes ? $0.key < $1.key : $0.bytes > $1.bytes }
        return Array(totals.prefix(limit))
    }

    /// 标 `@MainActor` 是因为 `.t` 走的是主线程上的 `I18n` 单例；这几句是**给人看的名字**，
    /// 必须跟着语言走（而不是永远中文）。
    @MainActor
    static func label(for key: String, dimension: Dimension,
                      deviceNames: [String: String]) -> String {
        switch dimension {
        case .device:
            if let name = deviceNames[key], !name.isEmpty { return name }
            return key.isEmpty ? "本机 / 未归属".t : key
        case .process:
            // 进程名只有本机发起的连接才有 —— 局域网设备的进程在别人机器上。
            return key.isEmpty ? "其它设备".t : key
        case .host:
            return key.isEmpty ? "未知域名".t : key
        }
    }

    /// chain 算不算「走了代理」。与 `scopeClause` 是同一个判据的两种写法
    /// （SQL 里一份、内存里一份），改一个必须改另一个。
    static func isProxied(_ chain: String) -> Bool {
        !chain.isEmpty && chain != "DIRECT" && chain != "REJECT"
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

    /// 今日的上行 / 下行分开算。设备页概览条那句「今日 ↓x ↑y」要的是两个数，
    /// 而 `todayTotal` 只给合计。
    public func todayUpDown(scope: Scope = .all) -> (up: Int64, down: Int64) {
        guard let database else { return (0, 0) }
        let (start, end) = Self.todayRange()
        var result: (up: Int64, down: Int64) = (0, 0)
        database.query("""
            SELECT COALESCE(SUM(up),0), COALESCE(SUM(down),0) FROM conn
            WHERE ended_at >= ? AND ended_at < ? \(Self.scopeClause(scope))
            """, [.int(start), .int(end)]) { row in result = (row.int(0), row.int(1)) }
        return result
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

        // 今天这格再把在途连接加上 —— 与 `topDomains` 同一个理由：只查库的话，
        // 正开着的那条连接一个字节都不算，今天这根柱子会一直是 0。
        let todayKey = Self.dayKey(today)
        for (record, recordMAC) in liveSnapshot() where recordMAC == mac {
            var bucket = buckets[todayKey] ?? (0, 0)
            bucket.up += record.up
            bucket.down += record.down
            buckets[todayKey] = bucket
        }

        return (0..<days).reversed().map { offset in
            let date = calendar.date(byAdding: .day, value: -offset, to: today) ?? today
            let key = Self.dayKey(date)
            let bucket = buckets[key] ?? (0, 0)
            return DayTotal(day: key, up: bucket.up, down: bucket.down)
        }
    }

    /// 某台设备的常用域名（按累计字节降序）。
    /// 某台设备**近 `days` 天**用得最多的域名。
    ///
    /// 两处对齐 Qt（原来都缺）：
    ///   • **有时间窗**（Qt 传的是 7 天）。原来是把库里全部 30 天都算进来，
    ///     「常用域名」于是变成了「一个月里的常用域名」，答的不是同一个问题；
    ///   • **合并在途连接**。只查库的话正开着的连接一条都算不进去 ——
    ///     刚打开的那个网页在「常用域名」里看不见，看起来就像统计坏了（Qt 的原话）。
    public func topDomains(mac: String, days: Int = 7, limit: Int = 5) -> [GroupTotal] {
        guard !mac.isEmpty, limit > 0 else { return [] }
        let since = Int64(Date().addingTimeInterval(-Double(max(1, days)) * 86_400)
            .timeIntervalSince1970 * 1000)
        var aggregate: [String: Int64] = [:]
        database?.query("""
            SELECT host, SUM(up + down) AS bytes FROM conn
            WHERE mac = ? AND ended_at >= ? AND host != ''
            GROUP BY host
            """, [.text(mac), .int(since)]) { row in
            aggregate[row.text(0), default: 0] += row.int(1)
        }
        for (record, recordMAC) in liveSnapshot() where recordMAC == mac {
            let host = record.host.isEmpty ? record.destIP : record.host
            guard !host.isEmpty else { continue }
            aggregate[host, default: 0] += record.up + record.down
        }
        var totals = aggregate.map { GroupTotal(key: $0.key, bytes: $0.value) }
        totals.sort { $0.bytes == $1.bytes ? $0.key < $1.key : $0.bytes > $1.bytes }
        return Array(totals.prefix(limit))
    }

    /// 在途连接的快照（记录 + 解析出的设备 MAC）。锁着取一份就走，
    /// 别在锁里做排序/格式化。
    private func liveSnapshot() -> [(record: Record, mac: String)] {
        lock.lock()
        defer { lock.unlock() }
        return live.values.map { (record: $0.record, mac: deviceMap[$0.record.sourceIP] ?? "") }
    }

    private func liveMAC(_ record: Record) -> String {
        lock.lock()
        defer { lock.unlock() }
        return deviceMap[record.sourceIP] ?? ""
    }

    /// 某台设备的累计上/下行（保留期内的全部记录）。
    /// 今日每台设备的累计字节（上+下）。设备页拿它排序 —— 一次分组查询，
    /// 而不是每台设备查一遍。
    ///
    /// **排序键不能用实时速率**：速率每一拍都在变，拿它排序会让跑着流量的设备
    /// 一直换位置，列表抖个不停。今日累计是单调的，只会偶尔超车一次。
    public func todayByDevice() -> [String: Int64] {
        guard let database else { return [:] }
        let (start, end) = Self.todayRange()
        var result: [String: Int64] = [:]
        database.query("""
            SELECT mac, COALESCE(SUM(up + down), 0) FROM conn
            WHERE ended_at >= ? AND ended_at < ? AND mac != ''
            GROUP BY mac
            """, [.int(start), .int(end)]) { row in
            result[row.text(0)] = row.int(1)
        }
        return result
    }

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
