import Foundation

/// 一条活动连接的类型化视图。核心 `/connections` 返回的是 `[[String: Any]]`,
/// 在**进入 UI 之前**就地解析成这个结构 —— UI 不碰裸字典,也便于单测解析逻辑。
public struct ConnectionRow: Sendable, Equatable, Identifiable {
    public let id: String
    public let host: String        // 域名(sniffer 之后多有值),否则目标 IP
    public let network: String     // tcp/udp
    /// 连接类型：核心的 `metadata.type`（HTTP / Socks5 / Redir …），空则退回 `network`。
    /// 连接窗行首那个 `[type]` 就是它 —— 与 Qt `QmlBridge::refreshConnections` 同一套回退。
    public let type: String
    /// 发起进程（核心的 find-process-mode 填的，仅本机连接查得到）。
    ///
    /// ★ **`inboundUser` 以 `dev-` 开头时强制留空**：那是局域网设备经透明网关进来的连接，
    /// 从 127.0.0.1 发起，查到的进程必然是 Coast 自己 —— 把「某台手机在访问 X」标成
    /// 「Coast 在访问 X」比留空更糟。宁可空着也不误标（与 Qt 同）。
    public let process: String
    public let chain: String       // 出口链首段(chains[0]),如 "🚀 节点选择"
    public let upload: Int64
    public let download: Int64
    /// 连接建立时刻。核心给的是 RFC3339 字符串。
    ///
    /// 「最近连接」按它排 —— 与「用量最多」是**两个不同的榜**：
    /// 刚建立的连接往往还没跑量，只看流量榜的话它一条都不会出现，
    /// 而用户想确认「我刚点的那下到底连出去没有」时，看的正是这个。
    public let start: Date
    /// 发起方 IP（局域网设备走透明代理时是设备的 IP，本机连接则是本机地址）。
    public let sourceIP: String

    /// 是否走了代理(chain 既非 DIRECT 也非 REJECT)。UI 据此标色。
    public var isProxied: Bool { !chain.isEmpty && chain != "DIRECT" && chain != "REJECT" }

    public var totalBytes: Int64 { upload + download }

    /// 把一份 `/connections` 快照解析成行,按总流量降序(用户最关心跑量大的)。
    public static func parse(_ connections: [[String: Any]]) -> [ConnectionRow] {
        connections.compactMap { c in
            guard let id = c["id"] as? String, !id.isEmpty else { return nil }
            let meta = c["metadata"] as? [String: Any] ?? [:]
            let host: String = {
                let h = meta["host"] as? String ?? ""
                return h.isEmpty ? (meta["destinationIP"] as? String ?? "") : h
            }()
            let chains = c["chains"] as? [String] ?? []
            let inboundUser = meta["inboundUser"] as? String ?? ""
            let type: String = {
                let t = meta["type"] as? String ?? ""
                return t.isEmpty ? (meta["network"] as? String ?? "") : t
            }()
            // 核心给的是 RFC3339（常带小数秒）。解不出来时退回 `.distantPast` 而不是 now：
            // 用 now 的话解析失败的连接会永远排在「最近」榜首，把真正新建的挤掉。
            let start = Self.parseTimestamp(c["start"] as? String ?? "")
            return ConnectionRow(
                id: id,
                host: host,
                network: meta["network"] as? String ?? "",
                type: type,
                process: inboundUser.hasPrefix("dev-") ? "" : (meta["process"] as? String ?? ""),
                // 出口链为空时显示 "-" 而不是空白 —— 徽标空着会让人以为渲染坏了（与 Qt 同）。
                chain: chains.first ?? "-",
                upload: (c["upload"] as? NSNumber)?.int64Value ?? 0,
                download: (c["download"] as? NSNumber)?.int64Value ?? 0,
                start: start,
                sourceIP: meta["sourceIP"] as? String ?? "")
        }
        .sorted { $0.totalBytes > $1.totalBytes }
    }

    /// RFC3339 解析。核心的时间戳**带小数秒**（`2026-07-31T21:33:33.878493+08:00`），
    /// 而 `.withInternetDateTime` 单独用时解不了小数秒 —— 少了 `.withFractionalSeconds`
    /// 会整条解析失败、全部退成 `distantPast`，「最近连接」于是变成一个乱序的列表。
    static func parseTimestamp(_ raw: String) -> Date {
        guard !raw.isEmpty else { return .distantPast }
        let withFraction = ISO8601DateFormatter()
        withFraction.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let date = withFraction.date(from: raw) { return date }
        let plain = ISO8601DateFormatter()
        plain.formatOptions = [.withInternetDateTime]
        return plain.date(from: raw) ?? .distantPast
    }

    /// 这条连接的发起设备名。
    ///
    /// 本机发起的一律留空（与 Qt 一致）：本机是默认情形，每行都标上「本机」只是噪音；
    /// 局域网设备走透明代理时才有必要指明「这条是谁的流量」。
    /// 查不到台账记录时退回 IP —— 有个 IP 也比空着强，至少还能对得上是哪台。
    ///
    /// 写成纯函数：状态页有两张卡（最近连接 / 用量最多）都要用它，
    /// 各写一份的话迟早会漂移，而漂移了没有任何东西会报警。
    public static func deviceLabel(for row: ConnectionRow,
                                   proxied: [(ip: String, alias: String)]) -> String {
        let ip = row.sourceIP
        guard !ip.isEmpty, !ip.hasPrefix("127."), ip != "::1" else { return "" }
        guard let match = proxied.first(where: { $0.ip == ip }) else { return ip }
        return match.alias.isEmpty ? ip : match.alias
    }

    /// 最近建立的若干条。
    public static func recent(_ rows: [ConnectionRow], limit: Int = 5) -> [ConnectionRow] {
        Array(rows.sorted { $0.start > $1.start }.prefix(limit))
    }

    /// 跑量最多的若干条。`parse` 已按流量降序，这里只截断 —— 但**不假设**调用方传进来的
    /// 一定是 `parse` 的原始输出（上层可能过滤过），所以仍然自己排一次。
    public static func top(_ rows: [ConnectionRow], limit: Int = 5) -> [ConnectionRow] {
        Array(rows.sorted { $0.totalBytes > $1.totalBytes }.prefix(limit))
    }
}
