import Foundation

/// 程序自身与内核的新版检查。对齐 C++ `AboutController` / `UpdateController`。
public struct UpdateChecker: Sendable {

    public struct Release: Sendable, Equatable {
        public let tag: String
        public let name: String
        public let notes: String
        public let isPrerelease: Bool
        public let assets: [(name: String, url: String)]

        public static func == (lhs: Release, rhs: Release) -> Bool {
            lhs.tag == rhs.tag && lhs.isPrerelease == rhs.isPrerelease
        }
    }

    /// 是否接收测试版。
    ///
    /// 注意 GitHub 的 `/releases/latest` **按定义就排除 prerelease**，所以不能靠它做频道切换 ——
    /// 必须拉完整的 `/releases` 列表自己筛。C++ 版踩过这个坑。
    public var includePrerelease: Bool

    private let repository = "ClashrAuto/clashauto"

    public init(includePrerelease: Bool = false) {
        self.includePrerelease = includePrerelease
    }

    public enum CheckError: Error, Sendable, LocalizedError {
        /// 服务端回了东西，但不是我们要的发布列表 —— 最常见的是 GitHub 限流
        /// （匿名调用每小时 60 次），它回的是一个带 message 的**对象**而不是数组。
        case unexpectedResponse(status: Int, detail: String)

        public var errorDescription: String? {
            switch self {
            case .unexpectedResponse(let status, let detail):
                return "HTTP \(status)：\(detail)"
            }
        }
    }

    public func latestAppRelease() async throws -> Release? {
        let url = URL(string: "https://api.github.com/repos/\(repository)/releases?per_page=20")!
        var request = URLRequest(url: url)
        request.setValue("coast-macos", forHTTPHeaderField: "User-Agent")
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.timeoutInterval = 20

        let (data, response) = try await URLSession.shared.data(for: request)
        return try Self.parseReleases(data: data,
                                      status: (response as? HTTPURLResponse)?.statusCode ?? -1,
                                      includePrerelease: includePrerelease)
    }

    /// 从 `/releases` 的响应里挑出该用的那一条。单独抽出来是为了能脱离网络单测 ——
    /// 限流那条路径正是**只在真实网络上才会走到**的，不测就永远不知道它坏没坏。
    static func parseReleases(data: Data, status: Int, includePrerelease: Bool) throws -> Release? {
        // ★ 这里必须**抛错**而不是返回 nil。返回 nil 会被调用方当成「没有更新」渲染成
        //   「已是最新版本」—— 检查其实失败了，用户却以为自己是最新的。GitHub 匿名调用每小时
        //   只有 60 次，限流时回的是带 message 的**对象**而不是数组，非常容易中招。
        let parsed = try? JSONSerialization.jsonObject(with: data)
        guard let list = parsed as? [[String: Any]] else {
            let detail = (parsed as? [String: Any])?["message"] as? String
                ?? String(data: data.prefix(200), encoding: .utf8)
                ?? "响应无法解析"
            throw CheckError.unexpectedResponse(status: status, detail: detail)
        }

        for entry in list {
            let isPrerelease = (entry["prerelease"] as? Bool) ?? false
            if isPrerelease, !includePrerelease { continue }
            if (entry["draft"] as? Bool) == true { continue }
            let assets = (entry["assets"] as? [[String: Any]] ?? []).compactMap { asset -> (String, String)? in
                guard let name = asset["name"] as? String,
                      let url = asset["browser_download_url"] as? String else { return nil }
                return (name, url)
            }
            return Release(tag: entry["tag_name"] as? String ?? "",
                           name: entry["name"] as? String ?? "",
                           notes: entry["body"] as? String ?? "",
                           isPrerelease: isPrerelease,
                           assets: assets)
        }
        return nil
    }

    /// 内核最新版号（只查版本，不下载 —— 下载在 `CoreDownloader`）。
    public func latestCoreTag() async throws -> String? {
        let url = URL(string: "https://api.github.com/repos/MetaCubeX/mihomo/releases/latest")!
        var request = URLRequest(url: url)
        request.setValue("coast-macos", forHTTPHeaderField: "User-Agent")
        request.timeoutInterval = 20
        let (data, _) = try await URLSession.shared.data(for: request)
        let object = try JSONSerialization.jsonObject(with: data) as? [String: Any]
        return object?["tag_name"] as? String
    }

    /// 语义化版本比较：`remote` 是否比 `local` 新。
    ///
    /// 逐段比数字，缺的段按 0 补。**不能用字符串比较** —— `"1.10.0" < "1.9.0"` 在字典序下成立，
    /// 于是用户会一直收不到 1.10 的更新提示，而这种版本号迟早会出现。
    /// 忽略前导 `v` 与 `-beta.xxx` 之类的后缀。
    public static func isNewer(remote: String, than local: String) -> Bool {
        let a = versionComponents(remote)
        let b = versionComponents(local)
        for index in 0..<max(a.count, b.count) {
            let left = index < a.count ? a[index] : 0
            let right = index < b.count ? b[index] : 0
            if left != right { return left > right }
        }
        return false
    }

    static func versionComponents(_ raw: String) -> [Int] {
        var text = raw.trimmingCharacters(in: .whitespaces)
        if text.hasPrefix("v") || text.hasPrefix("V") { text.removeFirst() }
        // 砍掉 -beta.<sha> 这类后缀：它不参与大小比较，带上会让 Int() 解析失败而整段变 0
        if let dash = text.firstIndex(of: "-") { text = String(text[text.startIndex..<dash]) }
        return text.split(separator: ".").map { Int($0.prefix(while: \.isNumber)) ?? 0 }
    }

    /// 从一组资源里挑 macOS 该下的那个。
    ///
    /// **按扩展名 + 关键词挑，绝不按文件名前缀** —— 前缀是品牌串，改过一次名（ClashAuto → Coast）
    /// 就会让所有老版本的一键更新失效。C++ 版正是靠这条才敢改资源名。
    public static func macAsset(from assets: [(name: String, url: String)]) -> (name: String, url: String)? {
        assets.first { $0.name.lowercased().hasSuffix(".dmg") }
            ?? assets.first { $0.name.lowercased().contains("macos") }
    }
}
