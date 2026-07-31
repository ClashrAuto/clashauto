import Foundation

/// 一条活动连接的类型化视图。核心 `/connections` 返回的是 `[[String: Any]]`,
/// 在**进入 UI 之前**就地解析成这个结构 —— UI 不碰裸字典,也便于单测解析逻辑。
public struct ConnectionRow: Sendable, Equatable, Identifiable {
    public let id: String
    public let host: String        // 域名(sniffer 之后多有值),否则目标 IP
    public let network: String     // tcp/udp
    public let process: String     // 发起进程(仅本机连接查得到)
    public let chain: String       // 出口链首段(chains[0]),如 "🚀 节点选择"
    public let upload: Int64
    public let download: Int64

    /// 是否走了代理(chain 既非 DIRECT 也非 REJECT)。UI 据此标色。
    public var isProxied: Bool { !chain.isEmpty && chain != "DIRECT" && chain != "REJECT" }

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
            return ConnectionRow(
                id: id,
                host: host,
                network: meta["network"] as? String ?? "",
                process: meta["process"] as? String ?? "",
                chain: chains.first ?? "",
                upload: (c["upload"] as? NSNumber)?.int64Value ?? 0,
                download: (c["download"] as? NSNumber)?.int64Value ?? 0)
        }
        .sorted { ($0.upload + $0.download) > ($1.upload + $1.download) }
    }
}
