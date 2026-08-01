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
    /// 上一拍的时刻。速率要按实测间隔归一化（见 `observe`）。
    private var lastObservedAt: Date?

    public init() {}

    public func sample(ip: String) -> Sample { byIP[ip] ?? Sample() }

    /// 并入一拍 `/connections` 快照。
    ///
    /// - Parameter localIP: 本机那一行在设备列表里用的 IP。**本机自己发出的连接
    ///   （回环 / TUN 的 198.18.0.1 / 本机任一网卡）会归到它名下**。
    ///
    ///   ★ 不这么做的话，本机这台的速率与用量**恒为 0** —— 全机器最忙的一台反而
    ///     永远显示没流量。Qt 那边同一处的注释写着：「以前这类全部落进『未归属』丢掉」。
    ///     macOS 上这尤其普遍：本机的连接 sourceIP 多半是 127.0.0.1 或 TUN 地址，
    ///     而设备行认的是它的局域网 IP，两者对不上。
    /// - Parameter now: 这一拍的时刻。速率按**实测的两拍间隔**归一化，
    ///   而不是把「一拍的增量」直接当成「每秒」——轮询晚到时（系统忙、睡眠唤醒、
    ///   核心卡住）攒了 3 秒的量会被报成 3 倍的速率。Qt 那边是 `delta * 1000 / dt`。
    public mutating func observe(_ rows: [ConnectionRow], localIP: String = "",
                                 now: Date = Date()) {
        var rates: [String: (up: Int64, down: Int64)] = [:]
        var alive = Set<String>()

        for row in rows {
            guard !row.sourceIP.isEmpty else { continue }
            alive.insert(row.id)
            let key = (!localIP.isEmpty && DeviceStore.isLocalMachineIP(row.sourceIP))
                ? localIP : row.sourceIP

            let previous = lastSeen[row.id]
            // 首次见到的连接：它此前的量一次性计入（就是它的全部）。
            // 回退（核心重启后 id 撞车）按新连接算，**绝不倒扣** —— 否则会出现负速率。
            var deltaUp = row.upload - (previous?.up ?? 0)
            var deltaDown = row.download - (previous?.down ?? 0)
            if deltaUp < 0 { deltaUp = row.upload }
            if deltaDown < 0 { deltaDown = row.download }
            lastSeen[row.id] = (row.upload, row.download)

            var bucket = rates[key] ?? (0, 0)
            bucket.up += deltaUp
            bucket.down += deltaDown
            rates[key] = bucket
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
        // 两拍间隔（秒）。第一拍没有上一拍可比，按标称的 1 秒算；
        // 间隔离谱地小（同一拍被喂了两次）时也按 1 秒，避免除出一个天文数字。
        let elapsed = lastObservedAt.map { now.timeIntervalSince($0) } ?? 1
        let seconds = elapsed > 0.05 ? elapsed : 1
        lastObservedAt = now

        for (ip, delta) in rates {
            var sample = byIP[ip] ?? Sample()
            sample.rateUp = Int64(Double(delta.up) / seconds)
            sample.rateDown = Int64(Double(delta.down) / seconds)
            // **累计量用的是原始增量**，不除时间 —— 那是「一共跑了多少字节」，与间隔无关。
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
