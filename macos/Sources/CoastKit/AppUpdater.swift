import CryptoKit
import Foundation

/// 程序自身的一键更新：下载发布产物（dmg / zip）→ 校验 sha256 → 写一个分离运行的
/// 替换脚本 → 调用方退出进程，脚本等本进程死透后**覆盖 .app 并重启**。
///
/// 流程对齐 Qt 的 `UpdateController::launchSilentUpdateAndRestartMac()`，外加 zip 支持
/// （Qt 的 mac 产物只有 dmg；万一哪天发布物换成 zip，这里两种都认）。
///
/// ## 为什么替换要交给脚本，而不是进程内做
///
/// 要被替换的正是**正在运行的自己**。进程内 rm/ditto 自己的 bundle，代码页还映射着
/// 旧文件，行为未定义；先退出再替换就没有这个问题。脚本单飞（`Process` detach 后
/// 本进程退出不影响它），等 PID 消失才动手，普通权限失败时升管理员再试一次，
/// 再不行就 `open` 安装包交给用户 —— 每一步都有下一级兜底，与 Qt 逐级一致。
///
/// ## sha256
///
/// CI 给每个产物都发了 `<name>.sha256`。有镜像（ghfast.top）这一环，校验不是洁癖：
/// 镜像是第三方，内容对不上就得当被篡改处理，宁可失败也不装。
/// 找不到 .sha256 资产时跳过校验（老版本的发布没有它）。
public struct AppUpdater: Sendable {

    /// 走镜像下载（`https://ghfast.top/` 前缀，与 `CoreDownloader` 同一个）。
    public var useMirror: Bool
    /// 走本机代理的端口。直连 github 释出的 CDN 在部分网络下极慢，核心在跑时让它代出去。
    public var proxyPort: Int?

    public init(useMirror: Bool = false, proxyPort: Int? = nil) {
        self.useMirror = useMirror
        self.proxyPort = proxyPort
    }

    public enum Progress: Sendable {
        case downloading(percent: Int, received: Int64, total: Int64)
        case verifying
        case installing
    }

    public enum UpdateError: Error, LocalizedError, Sendable {
        /// 不是从 .app 包里跑起来的（`swift run` / `.build/debug/Coast`）——
        /// 没有可替换的目标，调用方应回退到打开发布页。
        case notInAppBundle
        /// 这个 release 里没有 macOS 产物。
        case noMacAsset
        case downloadFailed(String)
        /// 校验失败 = 内容和发布时不一致，当被篡改处理。
        case checksumMismatch
        case scriptLaunchFailed

        public var errorDescription: String? {
            switch self {
            case .notInAppBundle: return "当前不是从 .app 运行，无法自动替换"
            case .noMacAsset: return "该版本没有 macOS 安装包"
            case .downloadFailed(let detail): return "下载失败：\(detail)"
            case .checksumMismatch: return "安装包校验失败，可能被篡改，已取消"
            case .scriptLaunchFailed: return "更新脚本启动失败"
            }
        }
    }

    /// 一切就绪、只差退出进程的状态。`relaunchScript` 已经在跑、正等着本进程死透。
    public struct Staged: Sendable {
        public let assetName: String
        public let bundlePath: String
    }

    /// 下载 + 校验 + 放出替换脚本。成功返回后调用方**必须尽快退出进程**
    /// （退出路径上该做的清理照做 —— 脚本最多等 60 秒）。
    ///
    /// `bundleURL` 参数只为测试注入，真调用留 nil 取 `Bundle.main`。
    public func stage(release: UpdateChecker.Release,
                      bundleURL: URL? = nil,
                      onProgress: @Sendable @escaping (Progress) -> Void) async throws -> Staged {
        // 先验运行形态，不行就别浪费几十 MB 流量。
        let bundle = bundleURL ?? Bundle.main.bundleURL
        guard bundle.pathExtension == "app" else { throw UpdateError.notInAppBundle }
        guard let asset = Self.pickAsset(from: release.assets) else { throw UpdateError.noMacAsset }

        let workDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("coast-app-update", isDirectory: true)
        try? FileManager.default.removeItem(at: workDir)
        try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)

        let payload = try await download(urlString: asset.url) { percent, received, total in
            onProgress(.downloading(percent: percent, received: received, total: total))
        }

        // 校验：产物旁边的 <name>.sha256（内容是「hex 文件名」或裸 hex，取第一个词）。
        if let checksumAsset = release.assets.first(where: { $0.name == asset.name + ".sha256" }) {
            onProgress(.verifying)
            let expectedRaw = try await download(urlString: checksumAsset.url, onProgress: { _, _, _ in })
            let expected = String(data: expectedRaw, encoding: .utf8)?
                .split(whereSeparator: \.isWhitespace).first.map(String.init) ?? ""
            let actual = SHA256.hash(data: payload).map { String(format: "%02x", $0) }.joined()
            guard !expected.isEmpty, expected.lowercased() == actual else {
                throw UpdateError.checksumMismatch
            }
        }

        onProgress(.installing)
        let package = workDir.appendingPathComponent(asset.name)
        try payload.write(to: package)

        let script = Self.relaunchScript(pid: ProcessInfo.processInfo.processIdentifier,
                                         package: package.path,
                                         appBundle: bundle.path)
        let scriptPath = workDir.appendingPathComponent("coast-update.sh")
        try script.write(to: scriptPath, atomically: true, encoding: .utf8)

        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/bin/sh")
        task.arguments = [scriptPath.path]
        do {
            try task.run()
        } catch {
            throw UpdateError.scriptLaunchFailed
        }
        return Staged(assetName: asset.name, bundlePath: bundle.path)
    }

    /// 挑 macOS 该装的产物：优先 zip（解压比挂载快、无提权触点）→ dmg。
    /// 现在的发布物只有 dmg，zip 分支是给将来留的。
    /// **按扩展名 + 关键词挑，绝不按文件名前缀**（品牌串改过一次名，见 `UpdateChecker.macAsset`）。
    /// 同样要把 Qt 那条线的 `…-macos-universal-qt.dmg` 排除掉 —— 它是另一个 app，
    /// 下下来会把用户从 Swift 版静默换成 Qt 版。理由见 `UpdateChecker.isQtLine`。
    static func pickAsset(from assets: [(name: String, url: String)]) -> (name: String, url: String)? {
        let mac = assets.filter {
            $0.name.lowercased().contains("macos")
                && !$0.name.hasSuffix(".sha256")
                && !UpdateChecker.isQtLine($0.name)
        }
        return mac.first { $0.name.lowercased().hasSuffix(".zip") }
            ?? mac.first { $0.name.lowercased().hasSuffix(".dmg") }
    }

    /// 替换脚本，逐行对齐 Qt 的 `launchSilentUpdateAndRestartMac`，多一个 zip 分支：
    /// 等 PID 死透（最多 60s）→ 解压/挂载 → rm 旧 .app + ditto 新的 → 失败升管理员重试
    /// → 再失败 open 安装包交给用户 → 成功则去隔离并重启。
    static func relaunchScript(pid: Int32, package: String, appBundle: String) -> String {
        let shq = { (s: String) in "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }
        return """
        #!/bin/sh
        PID=\(pid)
        PKG=\(shq(package))
        APP=\(shq(appBundle))
        do_replace() {
          case "$PKG" in
            *.zip)
              EX=$(mktemp -d /tmp/coast-upd.XXXXXX) || return 2
              ditto -xk "$PKG" "$EX" 2>/dev/null || { rm -rf "$EX"; return 2; }
              SRC=$(find "$EX" -maxdepth 2 -name '*.app' 2>/dev/null | head -n1)
              RC=1
              if [ -n "$SRC" ] && rm -rf "$APP" 2>/dev/null && ditto "$SRC" "$APP" 2>/dev/null; then RC=0; fi
              rm -rf "$EX"
              return $RC ;;
            *.dmg)
              MNT=$(mktemp -d /tmp/coast-upd.XXXXXX) || return 2
              hdiutil attach "$PKG" -nobrowse -noverify -mountpoint "$MNT" >/dev/null 2>&1 || return 2
              SRC=$(ls -d "$MNT"/*.app 2>/dev/null | head -n1)
              RC=1
              if [ -n "$SRC" ] && rm -rf "$APP" 2>/dev/null && ditto "$SRC" "$APP" 2>/dev/null; then RC=0; fi
              hdiutil detach "$MNT" >/dev/null 2>&1
              return $RC ;;
          esac
          return 2
        }
        if [ "$1" = replace ]; then do_replace; exit $?; fi
        i=0; while kill -0 "$PID" 2>/dev/null; do sleep 0.5; i=$((i+1)); [ $i -ge 120 ] && break; done
        if ! do_replace; then
          osascript -e "do shell script \\"/bin/sh '$0' replace\\" with administrator privileges" || { open "$PKG"; exit 1; }
        fi
        xattr -dr com.apple.quarantine "$APP" 2>/dev/null
        open -n "$APP"
        rm -f "$0"
        """
    }

    private func download(urlString: String,
                          onProgress: @Sendable @escaping (Int, Int64, Int64) -> Void) async throws -> Data {
        let finalURL = useMirror ? "https://ghfast.top/" + urlString : urlString
        guard let url = URL(string: finalURL) else { throw UpdateError.downloadFailed("URL 非法") }
        var request = URLRequest(url: url)
        request.setValue("coast-macos", forHTTPHeaderField: "User-Agent")
        request.timeoutInterval = 60

        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 60
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
        let downloader = FileDownloader(onProgress: onProgress)
        switch await downloader.download(request: request, configuration: config) {
        case .success(let data):
            guard !data.isEmpty else { throw UpdateError.downloadFailed("下载内容为空") }
            return data
        case .failure(let error):
            throw UpdateError.downloadFailed(error.localizedDescription)
        }
    }
}
