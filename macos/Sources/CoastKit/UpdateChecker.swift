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

    /// 走本机代理的端口（核心的混合端口）。nil = 直连。
    /// 与 `CoreDownloader.proxyPort` 同理：直连 api.github.com 在部分网络下必然失败，
    /// 核心在跑时让它代出去。
    public var proxyPort: Int?

    private let repository = "ClashrAuto/clashauto"

    public init(includePrerelease: Bool = false, proxyPort: Int? = nil) {
        self.includePrerelease = includePrerelease
        self.proxyPort = proxyPort
    }

    private var session: URLSession {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 20
        if let proxyPort {
            config.connectionProxyDictionary = [
                kCFNetworkProxiesHTTPEnable as String: 1,
                kCFNetworkProxiesHTTPProxy as String: "127.0.0.1",
                kCFNetworkProxiesHTTPPort as String: proxyPort,
                "HTTPSEnable": 1,
                "HTTPSProxy": "127.0.0.1",
                "HTTPSPort": proxyPort,
            ]
        }
        return URLSession(configuration: config)
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

    /// 一次查更新的完整结果。
    ///
    /// ★ 为什么是两个字段而不是一个 `Release`：`release` 是**展示**用的（版本名、更新说明），
    ///   `lineVersion` 才是**判据**——「本条产品线在对应通道里真的能装到的最高版本」。
    ///   两条 macOS 产品线的 DMG 是由外部仓库签完名再异步挂上来的，同一个 tag 上各平台的包
    ///   陆续到达，所以「release 的 tag 比本地新」根本不等于「有我能装的包」。拿 tag 当判据
    ///   就会出现角标亮着「有新版」、点进更新窗却一个包都挑不出来的死局。
    ///   拿不到 `lineVersion`（清单里这条线还没有包）时判「无更新」——宁可漏报不误报。
    public struct AppStatus: Sendable {
        public let release: Release?
        public let lineVersion: String?
    }

    /// 数据源是 CI 维护的 `version.json`，**不是 GitHub API** —— 理由见 `VersionManifest`。
    /// 清单被翻译回 `/releases` 的形状后交给下面原样的 `parseReleases`：资源过滤、
    /// -qt 排除、边车校验一条都没重写。
    ///
    /// 清单**只取一次**：展示用的 release 和判据用的 lineVersion 都从这一份里算，
    /// 免得两次请求之间清单被 CI 换掉、两个字段来自不同快照。
    public func latestAppStatus() async throws -> AppStatus {
        let root = try await VersionManifest.fetch(session: session)
        let list = VersionManifest.appReleases(root)
        let data = try JSONSerialization.data(withJSONObject: list)
        let release = try Self.parseReleases(data: data, status: 200,
                                             includePrerelease: includePrerelease)
        var best = VersionManifest.versionForThisLine(root, channel: "release")
        if includePrerelease,
           let beta = VersionManifest.versionForThisLine(root, channel: "prerelease"),
           best == nil || Self.isNewer(remote: beta, than: best!) {
            best = beta
        }
        return AppStatus(release: release, lineVersion: best)
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
        try await latestCoreRelease()?.tag
    }

    /// 内核最新版的版本号 + 更新说明。更新窗的「内核」页要展示说明正文，
    /// 而角标只要版本号 —— 两者共用这一次请求，别为了一段说明再打一遍 GitHub
    /// （匿名调用每小时只有 60 次）。
    ///
    /// 发布源与 `CoreDownloader` 同一套规则（对齐 Qt `CoreRelease.h`）：
    /// fork ClashrAuto/clash 的 /releases 全量列表按通道挑，且必须带 darwin 产物才认 ——
    /// 不能用 /releases/latest，fork 从上游继承了一堆零资产空 tag，latest 常落在那上面。
    /// 返回的 tag 是**产物名里嵌的版本号**（beta 的 release tag 是 Prerelease-<分支>，
    /// 拿它和本地内核版本没法比）。
    public func latestCoreRelease() async throws -> (tag: String, notes: String)? {
        let root = try await VersionManifest.fetch(session: session)
        let list = VersionManifest.coreReleases(root)
        guard let pick = CoreDownloader.pick(releases: list, wantBeta: includePrerelease)
        else { return nil }
        return (pick.version, pick.notes)
    }

    /// 内核「有无新版」= 「和本地不一样」，**不是**「更大」（对齐 Qt `CoreRelease::hasUpdate`）。
    /// 两个原因：① 从测试版切回正式版时版本号是变小的，比大小会把用户钉死在测试版上；
    /// ② 用户机器上还留着上游 mihomo（v1.19.x）时它比 fork 的 v1.10.x「大」，
    /// 比大小会判成「已是最新」，永远提示不了换 fork 内核。
    public static func coreHasUpdate(remote: String, local: String) -> Bool {
        !remote.isEmpty && !local.isEmpty && remote != local
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
    ///
    /// ★ 同一个 release 上有**两个** mac 包：本（Swift）版的 `…-macos-arm64.dmg`
    ///   （2026-08 前叫 `…-macos-universal.dmg`，这条线现在只发 arm64）和 Qt 版的
    ///   `…-macos-universal-qt.dmg`。后者部署目标 13.0、是给装不了 macOS 26 的
    ///   机器准备的另一条产品线，把它当成自己的更新包装下去 = 用户被静默换成另一个 app。
    ///   所以先把带 qt 标记的滤掉再挑。
    public static func macAsset(from assets: [(name: String, url: String)]) -> (name: String, url: String)? {
        let mine = assets.filter { !isQtLine($0.name) }
        return mine.first { $0.name.lowercased().hasSuffix(".dmg") }
            ?? mine.first { $0.name.lowercased().contains("macos") }
    }

    /// 资源名是否属于 **Qt 那条线**（`…-qt.dmg` / `…-qt-…` / `…qt-…`）。
    /// 只认带分隔符的 `qt`，不裸匹配子串 —— 否则 "…-quic-…" 这类名字会被误伤。
    public static func isQtLine(_ name: String) -> Bool {
        let lower = name.lowercased()
        return lower.contains("-qt.") || lower.contains("-qt-") || lower.contains("qt-")
            || lower.hasSuffix("-qt")
    }
}
