import Foundation

/// 从 GitHub Releases 拉 coast 内核并装到 `userDir/command/core`。
///
/// 应用**不预装内核** —— 首次运行由用户在「设置 → 系统」点一下下载。发布源与
/// 资源名规则对齐 Qt 端 `CoreRelease.h`(那边是三处共用的单一事实源):内核来自
/// 我们自己的 fork **ClashrAuto/clash**(产物名 `coast-*`),两条通道 ——
/// 正式版 = 最新的正式 release,测试版 = 最新的 prerelease。
///
/// ⚠️ 不能用 `/releases/latest`:一是它按 GitHub 定义排除 prerelease,测试版通道
/// 根本拿不到;二是这个 fork 从上游继承了一堆**零资产**的空 tag(v2025.11.25-* 之类),
/// latest 很可能正好落在那上面 —— 表现为「找不到匹配资源」。所以走 /releases 全量列表,
/// 且**必须校验该 release 真的带 darwin 产物**才认。
public struct CoreDownloader: Sendable {

    public enum Progress: Sendable {
        case checking
        case downloading(percent: Int, received: Int64, total: Int64)
        case installing
        case done(version: String)
        case failed(String)
    }

    public enum DownloadError: Error, Sendable {
        case releaseQueryFailed(String)
        case noMatchingAsset(os: String, arch: String)
        case downloadFailed(String)
        case extractFailed
        case installFailed
    }

    /// 国内加速:下载走 ghfast.top 镜像。
    ///
    /// ⚠️ 注意它**只镜像下载,不镜像 GitHub API**(实测 ghfast.top 不代理 api.github.com)。
    /// 内核下载**不做任何显式代理**(这是产品决定:出不出得去网由系统环境说了算,
    /// 应用只提供这一个镜像开关);系统级代理(若开着)会被 URLSession 自然沿用。
    public var useMirror: Bool

    /// 是否走测试版通道。对齐 Qt:读的是 config.yaml 的 beta 开关(接收测试版)。
    /// 测试版是 fork 的滚动 prerelease,tag 形如 `Prerelease-<分支>` —— 版本号
    /// 不在 tag 里而在产物名里,所以 `Downloaded.version` 从产物名剥。
    public var includePrerelease: Bool

    public init(useMirror: Bool = false, includePrerelease: Bool = false) {
        self.useMirror = useMirror
        self.includePrerelease = includePrerelease
    }

    private func sessionConfiguration() -> URLSessionConfiguration {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 30
        return config
    }

    /// 下载并解压好的内核(还躺在临时目录里)。装完或放弃后调 `cleanup()`。
    public struct Downloaded: Sendable {
        public let version: String
        public let binary: URL
        let tmpDir: URL
        public func cleanup() { try? FileManager.default.removeItem(at: tmpDir) }
    }

    /// 查最新版 → 下载 → 解压。**不动**已装的内核。
    ///
    /// ★ 与安装(`finishInstall`)拆成两步是刻意的:下载/解压期间**不该动核心**
    ///   (用户网络还靠它跑着;系统代理开着时这些请求也会自然经它出网)——
    ///   调用方在拿到产物后才停核、替换、重启。原来是一步式的 `install()`,
    ///   调用方「先停核再下载」,一次更新失败网就断了。
    public func fetchAndExtract(onProgress: @Sendable @escaping (Progress) -> Void) async throws -> Downloaded {
        onProgress(.checking)

        let releases = try await fetchReleases()
        guard let pick = Self.pick(releases: releases, wantBeta: includePrerelease) else {
            throw DownloadError.noMatchingAsset(os: Self.osName, arch: AppPaths.cpuArch)
        }

        let data = try await download(urlString: pick.url, onProgress: onProgress)

        let tmpDir = AppPaths.userDir.appendingPathComponent("core-update")
        try? FileManager.default.removeItem(at: tmpDir)
        do {
            try FileManager.default.createDirectory(at: tmpDir, withIntermediateDirectories: true)
            let archive = tmpDir.appendingPathComponent(pick.name)
            try data.write(to: archive)
            let binary = try Self.extract(archive: archive, into: tmpDir)
            return Downloaded(version: pick.version, binary: binary, tmpDir: tmpDir)
        } catch {
            try? FileManager.default.removeItem(at: tmpDir)
            throw error
        }
    }

    /// 把解压好的内核装到 `command/core`。调用方此刻应已停掉核心(装完再起,
    /// 新文件才会被用上)。无论成败都清掉临时目录。
    public static func finishInstall(_ downloaded: Downloaded) throws {
        defer { downloaded.cleanup() }
        try install(binary: downloaded.binary)
    }

    // MARK: - 资源匹配(对齐 Qt `CoreRelease.h`,别分头演化)

    static var osName: String { "darwin" }

    /// 发布列表接口。正式版和测试版都在里面,按通道各挑各的。
    /// `UpdateChecker` 的内核角标查询也用它 —— 下载与角标必须同一套规则,
    /// 否则角标提示的版本和实际装到的版本会各说各话。
    static let releasesURL = "https://api.github.com/repos/ClashrAuto/clash/releases?per_page=20"

    /// 一次选中的产物。版本号嵌在产物名里 —— beta 的 release tag 是
    /// `Prerelease-<分支>`,从 tag 拿不到版本。
    struct Pick {
        let name: String
        let url: String
        let version: String
        let notes: String
        let beta: Bool
    }

    /// 本平台可接受的产物名前缀,按优先级排列。
    /// x86-64 优先 `-compatible`:CI 里不带后缀的 amd64 产物是 GOAMD64=v3,要较新 CPU;
    /// compatible 才是 v1 基线,老 Mac 上普通 amd64 构建会直接非法指令崩掉。
    static var assetPrefixes: [String] {
        if AppPaths.cpuArch == "arm64" { return ["coast-\(osName)-arm64-"] }
        return ["coast-\(osName)-amd64-compatible-", "coast-\(osName)-amd64-v1-"]
    }

    /// 从产物名剥出版本:`coast-darwin-arm64-v1.10.3951.gz` → `v1.10.3951`。
    /// 前缀或扩展名不符时返回 nil —— macOS 的产物是裸 gzip,`.zip` 一律不认。
    static func versionFromAsset(_ name: String, prefix: String) -> String? {
        let ext = ".gz"
        guard name.hasPrefix(prefix), name.hasSuffix(ext),
              name.count > prefix.count + ext.count else { return nil }
        return String(name.dropFirst(prefix.count).dropLast(ext.count))
    }

    /// 在一个 release 的 assets 里找本平台产物。
    static func pickAsset(assets: [[String: Any]]) -> (name: String, url: String, version: String)? {
        for prefix in assetPrefixes {
            for candidate in assets {
                guard let name = candidate["name"] as? String,
                      let url = candidate["browser_download_url"] as? String,
                      let version = versionFromAsset(name, prefix: prefix) else { continue }
                return (name, url, version)
            }
        }
        return nil
    }

    /// 从 /releases 列表里挑本通道的产物。列表是新→旧排序。
    /// wantBeta=true 取最新的 prerelease,false 取最新的正式版。
    /// **必须带本平台产物才认** —— 见文件头的零资产空 tag 问题。
    static func pick(releases: [[String: Any]], wantBeta: Bool) -> Pick? {
        for release in releases {
            if (release["draft"] as? Bool) == true { continue }
            if ((release["prerelease"] as? Bool) ?? false) != wantBeta { continue }
            guard let asset = pickAsset(assets: release["assets"] as? [[String: Any]] ?? []) else { continue }
            return Pick(name: asset.name,
                        url: asset.url,
                        version: asset.version,
                        notes: release["body"] as? String ?? "",
                        beta: wantBeta)
        }
        return nil
    }

    // MARK: - 网络

    /// 发布列表。**取自 CI 维护的 version.json，不打 GitHub API** —— 匿名 API 是 60 次/
    /// 小时按出口 IP 算，机场出口后面几百人共用，限流几乎必中（见 VersionManifest）。
    /// 清单翻译回 /releases 的形状后，下面挑产物那套规则一行都没改。
    private func fetchReleases() async throws -> [[String: Any]] {
        let url = URL(string: VersionManifest.url)!
        var request = URLRequest(url: url)
        request.setValue("coast-macos", forHTTPHeaderField: "User-Agent")
        request.timeoutInterval = 20
        let session = URLSession(configuration: sessionConfiguration())
        defer { session.finishTasksAndInvalidate() }
        do {
            let (data, _) = try await session.data(for: request)
            let parsed = try? JSONSerialization.jsonObject(with: data)
            guard let root = parsed as? [String: Any] else {
                throw DownloadError.releaseQueryFailed("版本清单无法解析")
            }
            let list = VersionManifest.coreReleases(root)
            guard !list.isEmpty else {
                // 清单在、但内核那段是空的。**必须报错而不是返回空数组** —— 空数组会一路
                // 变成「该通道没有本平台产物」，读起来像内核发布出了问题，而实际是清单没写全。
                throw DownloadError.releaseQueryFailed("版本清单里没有内核发布信息")
            }
            return list
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

        let downloader = FileDownloader { percent, received, total in
            onProgress(.downloading(percent: percent, received: received, total: total))
        }
        switch await downloader.download(request: request,
                                         configuration: sessionConfiguration()) {
        case .success(let data):
            guard !data.isEmpty else { throw DownloadError.downloadFailed("下载内容为空") }
            return data
        case .failure(let error):
            throw DownloadError.downloadFailed(error.localizedDescription)
        }
    }

    // MARK: - 解压与安装

    /// macOS 的资源是**裸 gzip**(不是 tar.gz),解出来直接就是可执行文件。
    /// 用系统 gzip 而不是引第三方库 —— 一个外部依赖换一次 `gzip -dc` 不值。
    static func extract(archive: URL, into directory: URL) throws -> URL {
        let output = directory.appendingPathComponent("core")
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

        // gzip 对损坏的包会返回非 0,但也见过「返回 0 却只写出 0 字节」的情形,故两个都查。
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
        // 0755:不给执行位的话核心装了也起不来,而报错只是一句含糊的「启动失败」。
        try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: target.path)
    }
}
