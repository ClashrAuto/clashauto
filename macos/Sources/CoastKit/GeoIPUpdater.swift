import Foundation

/// GeoIP 数据库（`Country.mmdb`）的下载与更新。
///
/// Qt 版有这个功能而 Swift 版一直没有 —— `CoreProcess.seedGeoIP` 的注释里就写着
/// 「随「更新 GeoIP」功能一起在阶段 5 补 —— 现在还没有下载入口」。后果是内置库只会越来越旧：
/// `GEOIP,CN` 依赖 IP 归属，库过期就会把国内 IP 判成境外（或反过来），分流全错。
///
/// 下载来的字节**一律先过 `MmdbFile.validate`**，且只写到暂存文件，绝不原地覆盖线上库 ——
/// 理由见 `MmdbFile` 的说明（Qt 版真机上为此坏过一次，且核心完全不报错）。
public struct GeoIPUpdater: Sendable {

    public enum Progress: Sendable {
        case downloading(percent: Int, received: Int64, total: Int64)
        case validating
    }

    public enum UpdateError: LocalizedError {
        case network(String)
        case invalid(String)

        public var errorDescription: String? {
            switch self {
            case .network(let why): return "下载失败：\(why)"
            case .invalid(let why): return "下载到的库校验不通过，已保留原有数据库 —— \(why)"
            }
        }
    }

    private static let releaseURL =
        "https://github.com/MetaCubeX/meta-rules-dat/releases/latest/download/country.mmdb"
    /// 走本机代理下载（核心在跑时）。GitHub 直连不通的网络里这是唯一能成的路径 ——
    /// 原来还有个 ghfast.top 镜像开关做备选，已整条撤掉。
    public var proxyPort: Int?

    public init(proxyPort: Int? = nil) {
        self.proxyPort = proxyPort
    }

    /// 下载 → 校验 → 暂存。**不动线上库**；真正换上去发生在下次起核心前
    /// （`MmdbFile.applyStaged`）—— 核心把 mmdb mmap 着且只加载一次，原地换根本不会生效。
    public func update(target: URL = AppPaths.userDir.appendingPathComponent("Country.mmdb"),
                       onProgress: @Sendable @escaping (Progress) -> Void = { _ in })
        async throws -> String {
        guard let url = URL(string: Self.releaseURL) else { throw UpdateError.network("URL 不合法") }

        let configuration = URLSessionConfiguration.ephemeral
        if let port = proxyPort {
            configuration.connectionProxyDictionary = [
                kCFNetworkProxiesHTTPEnable: 1, kCFNetworkProxiesHTTPProxy: "127.0.0.1",
                kCFNetworkProxiesHTTPPort: port,
                "HTTPSEnable": 1, "HTTPSProxy": "127.0.0.1", "HTTPSPort": port,
            ]
        }
        configuration.timeoutIntervalForResource = 300
        let session = URLSession(configuration: configuration)
        defer { session.invalidateAndCancel() }

        onProgress(.downloading(percent: 0, received: 0, total: 0))
        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await session.data(from: url)
        } catch {
            throw UpdateError.network(error.localizedDescription)
        }
        if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            throw UpdateError.network("HTTP \(http.statusCode)")
        }
        onProgress(.downloading(percent: 100, received: Int64(data.count), total: Int64(data.count)))
        onProgress(.validating)

        let result = MmdbFile.stage(data, target: target)
        guard result.ok else { throw UpdateError.invalid(result.why) }
        return "已下载 \(data.count / 1024 / 1024) MB，将在下次启动核心时生效"
    }
}
