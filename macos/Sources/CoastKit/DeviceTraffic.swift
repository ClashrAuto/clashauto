import Foundation

/// 每台设备的**实时**速率与本次会话累计。设备详情窗上半部那三个数字加一张曲线全靠它。
///
/// 核心不按设备统计任何东西 —— `/connections` 里每条连接只带一个 `sourceIP`。所以口径与
/// `TrafficComposition` 一样：**每拍对每条连接算增量**，按 sourceIP 归到对应的桶里。
///
/// 纯值类型、无副作用，喂两拍快照就能验完所有分支 —— 而「攒增量」这类代码的错法
/// （重复计总数、断线倒扣、核心重启后回退）在真实网络上极难复现。
public struct DeviceTraffic: Sendable {

    public struct Sample: Sendable, Equatable {
        /// 最近一拍的速率（字节/秒）。轮询是每秒一拍，所以增量就是速率。
        public var rateUp: Int64 = 0
        public var rateDown: Int64 = 0
        /// 本次会话累计（app 启动至今）。
        public var sessionUp: Int64 = 0
        public var sessionDown: Int64 = 0
        /// 最近若干拍的速率，越靠后越新 —— 设备行背景那张流量图的数据源。
        public var upHistory: [Double] = []
        public var downHistory: [Double] = []

        public init() {}

        /// 「这台设备还没有任何采样」的零值。视图默认参数用它，免得每处各写一遍。
        public static let empty = Sample()
    }

    /// 历史保留多少拍。40 拍 ≈ 40 秒，与状态页那张带宽图同量级；
    /// **必须有上限** —— 一台设备挂一晚上就是几万个点，而行里只画得下几十个。
    public static let historyLength = 40

    /// sourceIP → 采样。
    public private(set) var byIP: [String: Sample] = [:]

    /// 连接 id → 上一拍看到的累计字节。
    private var lastSeen: [String: (up: Int64, down: Int64)] = [:]

    public init() {}

    public func sample(ip: String) -> Sample { byIP[ip] ?? Sample() }

    /// 并入一拍 `/connections` 快照。
    public mutating func observe(_ rows: [ConnectionRow]) {
        var rates: [String: (up: Int64, down: Int64)] = [:]
        var alive = Set<String>()

        for row in rows {
            guard !row.sourceIP.isEmpty else { continue }
            alive.insert(row.id)

            let previous = lastSeen[row.id]
            // 首次见到的连接：它此前的量一次性计入（就是它的全部）。
            // 回退（核心重启后 id 撞车）按新连接算，**绝不倒扣** —— 否则会出现负速率。
            var deltaUp = row.upload - (previous?.up ?? 0)
            var deltaDown = row.download - (previous?.down ?? 0)
            if deltaUp < 0 { deltaUp = row.upload }
            if deltaDown < 0 { deltaDown = row.download }
            lastSeen[row.id] = (row.upload, row.download)

            var bucket = rates[row.sourceIP] ?? (0, 0)
            bucket.up += deltaUp
            bucket.down += deltaDown
            rates[row.sourceIP] = bucket
        }

        // 断掉的连接不再累加，但要把它的记忆清掉 —— 留着的话 `lastSeen` 会一直涨，
        // 挂机一晚上就是几十万条死 id。
        lastSeen = lastSeen.filter { alive.contains($0.key) }

        // 这一拍没有流量的设备速率归零（而不是维持上一拍的数字 —— 那会让一台早就
        // 静默的设备在界面上永远显示着「正在以 3MB/s 下载」）。
        for ip in byIP.keys {
            byIP[ip]?.rateUp = 0
            byIP[ip]?.rateDown = 0
        }
        for (ip, delta) in rates {
            var sample = byIP[ip] ?? Sample()
            sample.rateUp = delta.up
            sample.rateDown = delta.down
            sample.sessionUp += delta.up
            sample.sessionDown += delta.down
            byIP[ip] = sample
        }

        // 每拍给**每台已知设备**都推一个点（没流量的推 0）—— 只给有流量的推的话，
        // 曲线的横轴就不是时间了，一台间歇跑量的设备会画出一条时间被压缩的假曲线。
        for ip in Array(byIP.keys) {
            guard var sample = byIP[ip] else { continue }
            sample.upHistory.append(Double(sample.rateUp))
            sample.downHistory.append(Double(sample.rateDown))
            if sample.upHistory.count > Self.historyLength {
                sample.upHistory.removeFirst()
                sample.downHistory.removeFirst()
            }
            byIP[ip] = sample
        }
    }
}
