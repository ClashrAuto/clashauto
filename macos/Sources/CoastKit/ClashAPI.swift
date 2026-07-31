import Foundation

/// mihomo REST API 的**低层**客户端：一个方法一个接口，不含任何轮询/编排策略
/// （那些在 `ClashService`）。对齐 Qt 端 `ClashService` 里 `sendGet`/`sendJsonRequest`/`sendDelete`
/// 那一层。
///
/// 三个连接池分开，理由与 C++ 版逐条相同：
///   • `session`      —— 轮询与切换。
///   • `delaySession` —— 延迟测速。几十个节点同时测，混在一起会把 6 连接/主机的池占满，
///                       导致 `/proxies` 轮询和 selectNode 的 PUT 一起排队卡死。
///   • `speedSession` —— 下载测速，**整个 session 走 HTTP 代理指向混合端口**，
///                       这样下载才会经核心、按规则回到主选择组、钉在被测节点上。
public actor ClashAPI {

    public struct Traffic: Sendable, Equatable {
        public let up: Int64
        public let down: Int64
    }

    public enum APIError: Error, Sendable {
        case badStatus(Int, String)
        case notRunning          // 连接被拒/对端关闭 = 核心没在跑（与超时区分开，见 ClashService）
        case transport(String)
    }

    private var host: String
    private var port: Int
    private var mixedPort: Int
    private var secret: String

    private let session: URLSession
    private let delaySession: URLSession

    public init(host: String = "127.0.0.1", port: Int = 9191, mixedPort: Int = 7890, secret: String = "") {
        self.host = host
        self.port = port
        self.mixedPort = mixedPort
        self.secret = secret

        // 8s 兜底：任何请求最多挂 8s。C++ 版靠 setTransferTimeout 防「卡死接口耗尽连接池」，
        // 这里同理 —— 一个挂死的 /connections 会让后面每一拍轮询都排在它后面。
        let base = URLSessionConfiguration.ephemeral
        base.timeoutIntervalForRequest = 8
        base.waitsForConnectivity = false
        session = URLSession(configuration: base)

        let delayConfig = URLSessionConfiguration.ephemeral
        delayConfig.timeoutIntervalForRequest = 9   // 略大于核心侧 5s，免得延迟请求自己先超时
        delayConfig.httpMaximumConnectionsPerHost = 16
        delaySession = URLSession(configuration: delayConfig)
    }

    // MARK: - 端点

    public func setEndpoint(host: String, port: Int) {
        if !host.isEmpty { self.host = host }
        if port > 0 { self.port = port }
    }

    public func setMixedPort(_ port: Int) {
        if port > 0 { mixedPort = port }
    }

    public func setSecret(_ secret: String) { self.secret = secret }

    /// 下载测速用的 session 配置：**整个 session 走 HTTP/HTTPS 代理指向核心的混合端口**。
    /// 测速请求必须经核心出去，才会按规则回到主选择组、钉在当前被测节点上；直连出去测的是
    /// 本机带宽，与节点无关。测速目标是 https，故 HTTPS 那一半（CONNECT 隧道）才是真正生效的。
    nonisolated static func speedSessionConfig(host: String, mixedPort: Int) -> URLSessionConfiguration {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 6   // 停顿兜底：6s 无数据即中止，防止卡死占用并发槽
        config.httpMaximumConnectionsPerHost = 8
        config.connectionProxyDictionary = [
            kCFNetworkProxiesHTTPEnable as String: 1,
            kCFNetworkProxiesHTTPProxy as String: host,
            kCFNetworkProxiesHTTPPort as String: mixedPort,
            "HTTPSEnable": 1,
            "HTTPSProxy": host,
            "HTTPSPort": mixedPort,
        ]
        return config
    }

    // MARK: - 接口

    /// `/traffic` 是**流式**接口：mihomo 每秒推一行 JSON，连接常开、永不结束。
    ///
    /// 绝不能像普通接口那样每秒 GET 一次 —— C++ 版踩过这个坑：每次泄漏一个连接，几秒内耗尽
    /// 连接池，之后所有请求（含 selectNode 的 PUT）永久排队，表现为「列表不刷新、点应用没反应」。
    /// 这里用 `URLSession.bytes` 按行读，流断了由调用方（`ClashService` 的看门狗）重连。
    public func trafficStream() -> AsyncThrowingStream<Traffic, Error> {
        AsyncThrowingStream { continuation in
            let task = Task {
                do {
                    let (bytes, response) = try await session.bytes(for: request(path: "/traffic"))
                    if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                        throw APIError.badStatus(http.statusCode, "/traffic")
                    }
                    for try await line in bytes.lines {
                        guard let data = line.data(using: .utf8),
                              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
                        else { continue }
                        continuation.yield(Traffic(up: int64(object["up"]), down: int64(object["down"])))
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    /// `/connections` 全量快照。数量、`downloadTotal`、以及历史库要的连接数组都从这一份来，
    /// 不为了同一份数据发两次请求。
    public func connections() async throws -> (connections: [[String: Any]], downloadTotal: Int64) {
        let object = try await getJSON(path: "/connections")
        let list = object["connections"] as? [[String: Any]] ?? []
        return (list, int64(object["downloadTotal"]))
    }

    public func proxies() async throws -> [String: [String: Any]] {
        let object = try await getJSON(path: "/proxies")
        return object["proxies"] as? [String: [String: Any]] ?? [:]
    }

    public func configs() async throws -> [String: Any] {
        try await getJSON(path: "/configs")
    }

    public func setMode(_ mode: String) async throws {
        try await send(path: "/configs", method: "PATCH", body: ["mode": mode])
    }

    /// 热重载：`PUT /configs?force=true` + `{"path": <full.yaml>}`。
    public func reloadConfig(path: String) async throws {
        try await send(path: "/configs?force=true", method: "PUT", body: ["path": path])
    }

    public func selectNode(group: String, name: String) async throws {
        try await send(path: "/proxies/\(escape(group))", method: "PUT", body: ["name": name])
    }

    /// 单节点延迟。失败（超时/不通）不抛错，返回 nil —— 调用方只关心「有没有有效延迟」。
    public func delay(node: String, timeoutMs: Int = 5000) async -> Int? {
        let target = escape("http://www.gstatic.com/generate_204")
        let path = "/proxies/\(escape(node))/delay?timeout=\(timeoutMs)&url=\(target)"
        guard let url = URL(string: "http://\(host):\(port)\(path)") else { return nil }
        var req = URLRequest(url: url)
        applyAuth(&req)
        guard let (data, response) = try? await delaySession.data(for: req),
              let http = response as? HTTPURLResponse, http.statusCode == 200,
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        let value = (object["delay"] as? NSNumber)?.intValue ?? 0
        return value > 0 ? value : nil
    }

    public func clearConnections() async throws {
        try await send(path: "/connections", method: "DELETE", body: nil)
    }

    public func closeConnection(id: String) async throws {
        try await send(path: "/connections/\(escape(id))", method: "DELETE", body: nil)
    }

    /// 经混合端口下载测速文件，返回 (字节数, 耗时毫秒)。
    ///
    /// 计时**从首字节开始**，不含建连+TLS —— 否则慢握手会把速度算低。首字节到达时调用
    /// `onFirstByte`，`ClashService` 用它释放「选组+建连」的串行锁，让下一个节点开始握手；
    /// 此后本次下载已钉死在当前节点上，切组不再影响它，多个下载于是能真正并发。
    ///
    /// 用 `URLSessionDataDelegate` 按**块**收，而不是 `URLSession.bytes` 那个逐字节的
    /// AsyncSequence —— 20 MB 意味着两千万次 await 恢复，那点开销会直接算进被测速度里。
    public func speedProbe(maxBytes: Int64 = 20 * 1024 * 1024,
                           maxMs: Int64 = 3000,
                           onFirstByte: @escaping @Sendable () -> Void) async -> (bytes: Int64, ms: Int64) {
        // Cloudflare 全球 CDN：各地区节点都能就近命中。不会真的下满 100 MB —— 到字节/时间上限就停。
        let url = URL(string: "https://speed.cloudflare.com/__down?bytes=104857600")!
        return await SpeedProbe(host: host, mixedPort: mixedPort,
                                maxBytes: maxBytes, maxMs: maxMs).run(url: url, onFirstByte: onFirstByte)
    }

    // MARK: - 内部

    private func request(path: String) -> URLRequest {
        var req = URLRequest(url: URL(string: "http://\(host):\(port)\(path)")!)
        applyAuth(&req)
        return req
    }

    /// secret 非空时统一加鉴权头。走这一个入口，避免像 C++ 版那样每个接口各写一遍、漏一个就 401。
    private func applyAuth(_ req: inout URLRequest) {
        guard !secret.isEmpty else { return }
        req.setValue("Bearer \(secret)", forHTTPHeaderField: "Authorization")
    }

    private func getJSON(path: String) async throws -> [String: Any] {
        let (data, response) = try await perform(request(path: path))
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            let code = (response as? HTTPURLResponse)?.statusCode ?? -1
            throw APIError.badStatus(code, String(data: data, encoding: .utf8) ?? "")
        }
        return (try? JSONSerialization.jsonObject(with: data) as? [String: Any]) ?? [:]
    }

    private func send(path: String, method: String, body: [String: Any]?) async throws {
        var req = request(path: path)
        req.httpMethod = method
        if let body {
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
            req.httpBody = try JSONSerialization.data(withJSONObject: body)
        }
        let (data, response) = try await perform(req)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            let code = (response as? HTTPURLResponse)?.statusCode ?? -1
            throw APIError.badStatus(code, String(data: data, encoding: .utf8) ?? "")
        }
    }

    /// 把 URLError 归类成「核心没跑」与「其它」两种。
    ///
    /// 这个区分是必需的：C++ 版只在**连接被拒/对端关闭**时才清空 UI 并允许重新自动测延迟，
    /// 超时不算。把超时也当掉线会导致「列表闪空」，还会和自动测延迟形成死循环
    /// （超时→重置→再测→占满池→再超时）。
    private func perform(_ req: URLRequest) async throws -> (Data, URLResponse) {
        do {
            return try await session.data(for: req)
        } catch let error as URLError {
            switch error.code {
            case .cannotConnectToHost, .networkConnectionLost, .cannotFindHost:
                throw APIError.notRunning
            default:
                throw APIError.transport(error.localizedDescription)
            }
        }
    }

    private nonisolated func escape(_ raw: String) -> String {
        raw.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? raw
    }

    private nonisolated func int64(_ value: Any?) -> Int64 {
        (value as? NSNumber)?.int64Value ?? 0
    }
}
