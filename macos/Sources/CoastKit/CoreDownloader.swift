import Foundation

/// 从 GitHub Releases 拉 mihomo 内核并装到 `userDir/command/core`。
///
/// 应用**不预装内核** —— 首次运行由用户在「设置 → 系统」点一下下载。对齐 C++
/// `SettingsController::updateCore()` 的资源名规则与安装步骤。
public struct CoreDownloader: Sendable {

    public enum Progress: Sendable {
        case checking
        case downloading(percent: Int)
        case installing
        case done(tag: String)
        case failed(String)
    }

    public enum DownloadError: Error, Sendable {
        case releaseQueryFailed(String)
        case noMatchingAsset(os: String, arch: String)
        case downloadFailed(String)
        case extractFailed
        case installFailed
    }

    /// 国内加速：下载走 ghfast.top 镜像。
    ///
    /// ⚠️ 注意它**只镜像下载，不镜像 GitHub API**（实测 ghfast.top 不代理 api.github.com）。
    /// 也就是说「连不上 GitHub」的用户光靠这个开关仍然装不上内核 —— 版本查询那一步就断了。
    /// 这一点与 Qt 版相同，不是本次移植引入的。缓解手段见下面的 `proxyPort`。
    public var useMirror: Bool

    /// 版本查询与下载走本机代理的端口（通常是核心的混合端口）。nil = 直连。
    ///
    /// 对齐 C++ `SettingsController::applyDownloadProxy`：**核心已经在跑**时，把这些请求
    /// 丢给它自己去出网。这解决的是「更新内核」这个常见场景 —— 首次安装时核心还没有，
    /// 帮不上忙，那种情况只能靠用户自己有办法访问 GitHub。
    public var proxyPort: Int?

    public init(useMirror: Bool = false, proxyPort: Int? = nil) {
        self.useMirror = useMirror
        self.proxyPort = proxyPort
    }

    /// 按 `proxyPort` 造 session 配置。
    private func sessionConfiguration(bypassProxy: Bool) -> URLSessionConfiguration {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 30
        if bypassProxy {
            config.connectionProxyDictionary = [:]
        } else if let proxyPort {
            config.connectionProxyDictionary = [
                kCFNetworkProxiesHTTPEnable as String: 1,
                kCFNetworkProxiesHTTPProxy as String: "127.0.0.1",
                kCFNetworkProxiesHTTPPort as String: proxyPort,
                "HTTPSEnable": 1,
                "HTTPSProxy": "127.0.0.1",
                "HTTPSPort": proxyPort,
            ]
        }
        return config
    }

    /// 查最新版 → 下载 → 解压 → 装到 `AppPaths.coreExecutable`。返回安装的版本号。
    ///
    /// 调用方负责在此前后停/起核心 —— 正在跑的核心文件替换不掉。
    public func install(onProgress: @Sendable @escaping (Progress) -> Void) async throws -> String {
        onProgress(.checking)

        let release = try await fetchLatestRelease()
        let tag = release["tag_name"] as? String ?? ""
        let assets = release["assets"] as? [[String: Any]] ?? []
        guard let asset = Self.pickAsset(assets: assets, tag: tag) else {
            throw DownloadError.noMatchingAsset(os: Self.osName, arch: AppPaths.cpuArch)
        }

        let data = try await download(urlString: asset.url, onProgress: onProgress)

        onProgress(.installing)
        let tmpDir = AppPaths.userDir.appendingPathComponent("core-update")
        try? FileManager.default.removeItem(at: tmpDir)
        try FileManager.default.createDirectory(at: tmpDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tmpDir) }

        let archive = tmpDir.appendingPathComponent(asset.name)
        try data.write(to: archive)

        let binary = try Self.extract(archive: archive, into: tmpDir)
        try Self.install(binary: binary)

        onProgress(.done(tag: tag))
        return tag
    }

    // MARK: - 资源匹配

    static var osName: String { "darwin" }

    struct Asset { let name: String; let url: String }

    /// 资源名规则（与上游发布物一致）：
    ///   • arm64 → `mihomo-darwin-arm64-<tag>.gz`
    ///   • x86   → 先试 `mihomo-darwin-amd64-compatible-<tag>.gz`，再试 `mihomo-darwin-amd64-<tag>.gz`
    ///
    /// Intel 优先 `-compatible` 是因为普通 amd64 构建带较新的指令集基线，老 Mac 上会直接
    /// 非法指令崩掉；compatible 版本才是安全默认。
    ///
    /// 精确名都没命中时按正则兜底 —— 上游偶尔会改 tag 的写法，硬编码全名容易一改就失配。
    static func pickAsset(assets: [[String: Any]], tag: String) -> Asset? {
        let isARM = AppPaths.cpuArch == "arm64"
        let ext = ".gz"
        var wanted: [String] = []
        if !isARM { wanted.append("mihomo-\(osName)-amd64-compatible-\(tag)\(ext)") }
        wanted.append("mihomo-\(osName)-\(AppPaths.cpuArch)-\(tag)\(ext)")

        for want in wanted {
            if let hit = assets.first(where: { $0["name"] as? String == want }),
               let url = hit["browser_download_url"] as? String {
                return Asset(name: want, url: url)
            }
        }

        let pattern = isARM
            ? "^mihomo-\(osName)-arm64-v\\d+\\.\\d"
            : "^mihomo-\(osName)-amd64-compatible-v\\d+\\.\\d"
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return nil }
        for candidate in assets {
            guard let name = candidate["name"] as? String, name.hasSuffix(ext),
                  let url = candidate["browser_download_url"] as? String else { continue }
            let range = NSRange(name.startIndex..., in: name)
            if regex.firstMatch(in: name, range: range) != nil {
                return Asset(name: name, url: url)
            }
        }
        return nil
    }

    // MARK: - 网络

    private func fetchLatestRelease() async throws -> [String: Any] {
        let url = URL(string: "https://api.github.com/repos/MetaCubeX/mihomo/releases/latest")!
        var request = URLRequest(url: url)
        request.setValue("coast-macos", forHTTPHeaderField: "User-Agent")
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.timeoutInterval = 20
        // 核心在跑就让它代出去 —— 这一步直连 api.github.com 在部分网络下是必然失败的，
        // 而镜像帮不上忙（它不代理 API）。
        let session = URLSession(configuration: sessionConfiguration(bypassProxy: false))
        defer { session.finishTasksAndInvalidate() }
        do {
            let (data, _) = try await session.data(for: request)
            guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                throw DownloadError.releaseQueryFailed("返回不是 JSON")
            }
            return object
        } catch let error as DownloadError {
            throw error
        } catch {
            throw DownloadError.releaseQueryFailed(error.localizedDescription)
        }
    }

    private func download(urlString: String, onProgress: @Sendable @escaping (Progress) -> Void) async throws -> Data {
        let finalURL = useMirror ? "https://ghfast.top/" + urlString : urlString
        guard let url = URL(string: finalURL) else { throw DownloadError.downloadFailed("URL 非法") }
        var request = URLRequest(url: url)
        request.setValue("coast-macos", forHTTPHeaderField: "User-Agent")
        request.timeoutInterval = 30

        let downloader = FileDownloader { percent in onProgress(.downloading(percent: percent)) }
        // 走镜像时**不套代理**：镜像本来就是给「连不上 GitHub」的场景用的，再绕回代理没意义。
        switch await downloader.download(request: request,
                                         configuration: sessionConfiguration(bypassProxy: useMirror)) {
        case .success(let data):
            guard !data.isEmpty else { throw DownloadError.downloadFailed("下载内容为空") }
            return data
        case .failure(let error):
            throw DownloadError.downloadFailed(error.localizedDescription)
        }
    }

    // MARK: - 解压与安装

    /// macOS 的资源是**裸 gzip**（不是 tar.gz），解出来直接就是可执行文件。
    /// 用系统 gzip 而不是引第三方库 —— 一个外部依赖换一次 `gzip -dc` 不值。
    static func extract(archive: URL, into directory: URL) throws -> URL {
        let output = directory.appendingPathComponent("mihomo")
        FileManager.default.createFile(atPath: output.path, contents: nil)
        guard let handle = try? FileHandle(forWritingTo: output) else { throw DownloadError.extractFailed }
        defer { try? handle.close() }

        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/gzip")
        task.arguments = ["-dc", archive.path]
        task.standardOutput = handle
        do {
            try task.run()
        } catch {
            throw DownloadError.extractFailed
        }
        task.waitUntilExit()

        // gzip 对损坏的包会返回非 0，但也见过「返回 0 却只写出 0 字节」的情形，故两个都查。
        let size = (try? FileManager.default.attributesOfItem(atPath: output.path))?[.size] as? Int ?? 0
        guard task.terminationStatus == 0, size > 0 else { throw DownloadError.extractFailed }
        return output
    }

    static func install(binary: URL) throws {
        let target = AppPaths.userDir.appendingPathComponent("command/core")
        try FileManager.default.createDirectory(at: target.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: target)
        do {
            try FileManager.default.copyItem(at: binary, to: target)
        } catch {
            throw DownloadError.installFailed
        }
        // 0755：不给执行位的话核心装了也起不来，而报错只是一句含糊的「启动失败」。
        try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: target.path)
    }
}
