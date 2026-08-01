import Foundation

/// 连接查看器的账本 —— 对齐 Qt `ConnectionsModel` 的**在线 / 离线**语义。
///
/// 核心的 `/connections` 只返回**当前还活着**的连接：一条连接断掉，下一拍就从快照里消失。
/// 而连接查看器要能回答「刚才那条连到哪去了」，所以消失的条目不能跟着消失，
/// 要留在列表里标成**离线**（圆点转灰、✕ 置灰不可点），直到用户重开窗口。
///
/// 纯值类型、无副作用 —— 这套合并逻辑的每一条分支都能直接喂快照来测，
/// 不必真的等一条连接断掉。
public struct ConnectionLedger: Sendable {

    /// 账本里的一行：连接本体 + 是否已离线。
    public struct Entry: Sendable, Equatable, Identifiable {
        public var row: ConnectionRow
        public var offline: Bool
        public var id: String { row.id }
    }

    /// 索引 0 起按「总流量降序」排列的全部条目（在线 + 离线）。
    public private(set) var entries: [Entry] = []

    public init() {}

    public var onlineCount: Int { entries.lazy.filter { !$0.offline }.count }
    public var offlineCount: Int { entries.lazy.filter(\.offline).count }

    /// 并入一份新快照。
    ///
    /// - 快照里有、账本里没有 → 新增（在线）；
    /// - 两边都有 → **用快照的值覆盖**（流量在涨，host 也可能迟到：sniffer 嗅出域名后才填上），
    ///   但**空的进程名不覆盖已有的**（见 `keepingProcess`）；
    /// - 账本里有、快照里没有 → 标离线，**保留最后一次的数值**（清零的话用户会以为它没跑过流量）。
    public mutating func merge(_ snapshot: [ConnectionRow]) {
        let live = Dictionary(snapshot.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        var seen = Set<String>()

        for index in entries.indices {
            let id = entries[index].id
            if let fresh = live[id] {
                entries[index] = Entry(row: Self.keepingProcess(fresh, previous: entries[index].row),
                                       offline: false)
                seen.insert(id)
            } else {
                entries[index].offline = true
            }
        }
        // 新出现的**追加到末尾**，已有的行**一律不动**。
        //
        // ★ 原来这里每次合并都按总流量降序重排一遍。后果是：只要有连接在跑流量，
        //   这张表就一直在换位置 —— 想点某一行的「断开」，手还没到它就已经挪走了。
        //   Qt 的 `ConnectionsModel::recompute()` 整套 reconcile（删已消失 → 原地刷存活 →
        //   新的追加到末尾）存在的全部意义就是**让行不动**：核心的 `/connections`
        //   是 Go map 的快照、原始顺序本来就是随机的，靠位置稳定来抵消它。
        //   要找「跑得最多」的那条，用窗口自带的搜索框，而不是让整张表跳。
        for row in snapshot where !seen.contains(row.id) {
            entries.append(Entry(row: row, offline: false))
        }
    }

    /// 进程名是**迟到**的：核心的 find-process-mode 要查本机套接字表，头几拍常常还是空的。
    /// 所以新值为空、旧值非空时**保留旧值** —— 否则那枚徽标会一闪一闪地出现又消失。
    /// （Qt 的 `refreshConnections` 里同一句注释：「迟到的进程名也补上，已有的不清空」。）
    static func keepingProcess(_ fresh: ConnectionRow, previous: ConnectionRow) -> ConnectionRow {
        guard fresh.process.isEmpty, !previous.process.isEmpty else { return fresh }
        return ConnectionRow(id: fresh.id, host: fresh.host, network: fresh.network,
                             type: fresh.type, process: previous.process, chain: fresh.chain,
                             upload: fresh.upload, download: fresh.download,
                             start: fresh.start, sourceIP: fresh.sourceIP)
    }

    /// 清空。Qt 在**每次打开窗口时**调它，免得上次会话的离线连接残留。
    public mutating func reset() { entries.removeAll() }

    /// 按「在线 / 离线 / 关键字」筛。关键字对 host、出口链、进程名三处做大小写无关匹配。
    public func filtered(online: Bool, offline: Bool, query: String) -> [Entry] {
        let needle = query.trimmingCharacters(in: .whitespaces).lowercased()
        return entries.filter { entry in
            guard entry.offline ? offline : online else { return false }
            guard !needle.isEmpty else { return true }
            return entry.row.host.lowercased().contains(needle)
                || entry.row.chain.lowercased().contains(needle)
                || entry.row.process.lowercased().contains(needle)
        }
    }
}
