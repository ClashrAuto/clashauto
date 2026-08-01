import Foundation

/// 本次会话的流量构成:直连 vs 走代理,各累计多少字节。对齐 Qt `QmlBridge` 的
/// `directBytes`/`proxyBytes`。
///
/// 核心的 `downloadTotal` 只有一个总数,答不了「有多少走了代理」;要拆开就得**按连接攒增量**:
/// 每拍拿到 /connections 快照,对每条连接算「这拍比上拍多了多少字节」,按它的出口链累进
/// 直连桶或代理桶。连接断开后不再有增量(它最后一次的增量早已计入)。
///
/// 单独成类(不塞进 HistoryStore)是因为口径不同:这个是**本次运行内的实时累计**,
/// HistoryStore 是**落盘的历史**。两者数据源同一份快照,但一个在内存、一个进库。
public struct TrafficComposition: Sendable, Equatable {
    public private(set) var directBytes: Int64 = 0
    public private(set) var proxyBytes: Int64 = 0

    /// 每条在途连接上一次看到的累计字节(up+down)。用来算增量。
    private var lastBytes: [String: Int64] = [:]

    /// 目标 host → 本会话累计用量。对齐 Qt 的 `m_hostBytes`。
    ///
    /// 「用量最多」问的是**这次运行里谁跑得最多**，所以必须按 host 跨连接累加、
    /// 且连接断了仍留在榜上。原来这张卡直接拿**当前在途连接**排前 5：
    /// 同一个域名的十几条连接会各占一行、刚下完的大文件一断就从榜上消失 —— 答的是另一个问题。
    public struct HostStat: Sendable, Equatable {
        public var bytes: Int64 = 0
        public var device = ""
        public var direct = false
    }
    private var hostBytes: [String: HostStat] = [:]

    /// host 表只增不减，给它一个上限：满了就只留用量最大的一半（被裁掉的都是零头）。
    /// 与 Qt 的 `kMaxHostStats` 同值。
    static let maxHostStats = 512

    public init() {}

    public var totalBytes: Int64 { directBytes + proxyBytes }

    /// 吃一份 /connections 快照,累加增量。
    ///
    /// `deviceName` 把发起方（sourceIP / inboundUser）翻成设备名，用于「用量最多」那一列；
    /// 与 Qt 的 `deviceNameFor()` 同一个位置。
    public mutating func observe(_ connections: [[String: Any]],
                                 deviceName: (_ sourceIP: String, _ inboundUser: String) -> String
                                    = { _, _ in "" }) {
        var seen = Set<String>()
        for c in connections {
            guard let id = c["id"] as? String, !id.isEmpty else { continue }
            seen.insert(id)

            let chain = (c["chains"] as? [String])?.first ?? ""
            // ★ REJECT 系列**两桶都不记**（Qt：既没出网也没流量）。原来把它算进了直连桶，
            //   于是「被规则拦掉」的流量被显示成了「直连出去的」—— 意思正好相反。
            //   用前缀匹配：还有 `REJECT-DROP`。
            if chain.hasPrefix("REJECT") { continue }

            let up = (c["upload"] as? NSNumber)?.int64Value ?? 0
            let down = (c["download"] as? NSNumber)?.int64Value ?? 0
            let cumulative = up + down
            let previous = lastBytes[id] ?? 0
            // 增量只可能 ≥0(连接字节只增不减)。核心偶发把已有 id 的计数清零重来时,
            // previous > cumulative,delta 取 0,不倒扣 —— 宁可少算一点也不出现负流量。
            let delta = max(0, cumulative - previous)
            lastBytes[id] = cumulative

            // 与 Qt 一字不差：只有 `DIRECT` 算直连，其余（含 chains 为空的）都算代理。
            let direct = chain == "DIRECT"
            if direct { directBytes += delta } else { proxyBytes += delta }

            let metadata = c["metadata"] as? [String: Any] ?? [:]
            var host = metadata["host"] as? String ?? ""
            if host.isEmpty { host = metadata["destinationIP"] as? String ?? "" }
            if !host.isEmpty {
                var stat = hostBytes[host] ?? HostStat()
                stat.bytes += delta
                stat.device = deviceName(metadata["sourceIP"] as? String ?? "",
                                         metadata["inboundUser"] as? String ?? "")
                stat.direct = direct
                hostBytes[host] = stat
            }
        }
        pruneHostStats()
        // 断开的连接:从 lastBytes 里清掉,免得无界增长(长会话里连接来来去去)。
        // 它最后一次的增量已经计入,清掉不影响已累计的总数。
        lastBytes = lastBytes.filter { seen.contains($0.key) }
    }

    /// 本会话用量最多的若干个目标。一个字节都没跑过的不占榜位（Qt 同）。
    public func topHosts(limit: Int = 5) -> [(host: String, stat: HostStat)] {
        hostBytes.filter { $0.value.bytes > 0 }
            // 字节相同时按 host 定序 —— 否则字典遍历顺序会让两行来回换位置。
            .sorted { $0.value.bytes == $1.value.bytes ? $0.key < $1.key
                                                       : $0.value.bytes > $1.value.bytes }
            .prefix(limit)
            .map { (host: $0.key, stat: $0.value) }
    }

    private mutating func pruneHostStats() {
        guard hostBytes.count > Self.maxHostStats else { return }
        let keep = hostBytes.sorted { $0.value.bytes > $1.value.bytes }
            .prefix(Self.maxHostStats / 2)
        hostBytes = Dictionary(uniqueKeysWithValues: keep.map { ($0.key, $0.value) })
    }
}
