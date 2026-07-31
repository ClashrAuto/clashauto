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

    public init() {}

    public var totalBytes: Int64 { directBytes + proxyBytes }

    /// 吃一份 /connections 快照,累加增量。
    public mutating func observe(_ connections: [[String: Any]]) {
        var seen = Set<String>()
        for c in connections {
            guard let id = c["id"] as? String, !id.isEmpty else { continue }
            seen.insert(id)
            let up = (c["upload"] as? NSNumber)?.int64Value ?? 0
            let down = (c["download"] as? NSNumber)?.int64Value ?? 0
            let cumulative = up + down
            let previous = lastBytes[id] ?? 0
            // 增量只可能 ≥0(连接字节只增不减)。核心偶发把已有 id 的计数清零重来时,
            // previous > cumulative,delta 取 0,不倒扣 —— 宁可少算一点也不出现负流量。
            let delta = max(0, cumulative - previous)
            lastBytes[id] = cumulative

            let chain = (c["chains"] as? [String])?.first ?? ""
            if chain.isEmpty || chain == "DIRECT" || chain == "REJECT" {
                directBytes += delta
            } else {
                proxyBytes += delta
            }
        }
        // 断开的连接:从 lastBytes 里清掉,免得无界增长(长会话里连接来来去去)。
        // 它最后一次的增量已经计入,清掉不影响已累计的总数。
        lastBytes = lastBytes.filter { seen.contains($0.key) }
    }
}
