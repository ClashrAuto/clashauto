import Foundation

/// version.json —— 查更新的唯一数据源，用来绕开 GitHub API 的限流。
///
/// **为什么不直接打 api.github.com。** 未登录的 GitHub API 是 **60 次/小时、按出口 IP 算**。
/// 用户经机场出网时几百人共用一个出口，限流几乎必中；GitHub 这时回的是一个带
/// `API rate limit exceeded for <ip>` 的**对象**（不是数组），在界面上表现成「检查失败」，
/// 读起来完全像网络故障 —— 用户只会觉得更新坏了，没人会想到是配额。
///
/// 清单由 CI 维护（`.github/scripts/build_version_manifest.py`），作为**固定 tag `version`
/// 的 release 资源**发布。下载它走的是 release 资源的 CDN，不是 API，没有配额。
///
/// ★ **本类型只做「适配」，不做「挑选」。** 清单被翻译回 GitHub `/releases` 的 JSON 形状，
///   再交给原有的解析器（`UpdateChecker.parseReleases`、`CoreDownloader.pick` 那套按 CPU
///   架构挑产物的规则、`macAsset` 的 -qt 排除……）。那些规则一条都没重写 —— 重写一遍就
///   意味着两套挑包逻辑要长期保持一致，而它们只要漂移一次，用户就会装到错的包上。
///   Qt 线的 `VersionManifest.h/.cpp` 是同一套做法。
public enum VersionManifest {

    /// 清单直链。固定 tag `version` 上的资源 —— 走 CDN，不是 API。
    public static let url =
        "https://github.com/ClashrAuto/clashauto/releases/download/version/version.json"

    /// 取清单原文。失败抛错 —— **不能吞成 nil**：调用方会把 nil 渲染成「已是最新版本」，
    /// 于是检查其实失败了，用户却以为自己是最新的。
    public static func fetch(session: URLSession) async throws -> [String: Any] {
        var request = URLRequest(url: URL(string: url)!)
        request.setValue("coast-macos", forHTTPHeaderField: "User-Agent")
        request.timeoutInterval = 20
        let (data, response) = try await session.data(for: request)
        let status = (response as? HTTPURLResponse)?.statusCode ?? -1
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw UpdateChecker.CheckError.unexpectedResponse(
                status: status,
                detail: String(data: data.prefix(200), encoding: .utf8) ?? "版本清单无法解析")
        }
        return root
    }

    /// 程序包 → GitHub `/releases` 形状的数组（正式版在前）。
    ///
    /// ★ **`.sha256` 边车要作为独立资源补回去**：清单里校验地址是挂在包上的字段，而
    ///   `AppUpdater` 下载完是按「有没有一个叫 `<资源名>.sha256` 的**资源**」查表校验的。
    ///   不补的话下载能成、校验被静默跳过 —— 而那正是校验最该起作用的场合。
    public static func appReleases(_ root: [String: Any]) -> [[String: Any]] {
        let packages = root["packages"] as? [[String: Any]] ?? []
        let heads = root["releases"] as? [String: Any] ?? [:]
        var out: [[String: Any]] = []
        for channel in ["release", "prerelease"] {
            guard let head = heads[channel] as? [String: Any] else { continue }
            var assets: [[String: Any]] = []
            for p in packages where (p["type"] as? String) == channel {
                guard let name = p["name"] as? String, let address = p["address"] as? String,
                      !name.isEmpty, !address.isEmpty else { continue }
                assets.append(["name": name, "browser_download_url": address,
                               "size": p["size"] ?? 0])
                if let sha = p["sha256"] as? String, !sha.isEmpty {
                    assets.append(["name": name + ".sha256", "browser_download_url": sha,
                                   "size": 0])
                }
            }
            // 一个包都没有的通道不当成「有版本」—— 那会让角标亮着却下不到东西。
            guard !assets.isEmpty else { continue }
            out.append([
                "tag_name": head["tag"] ?? "",
                "name": head["name"] ?? "",
                "body": head["notes"] ?? "",
                "prerelease": channel == "prerelease",
                "draft": false,
                "assets": assets,
            ])
        }
        return out
    }

    /// 内核 → GitHub `/releases` 形状的数组，直接喂 `CoreDownloader.pick`。
    public static func coreReleases(_ root: [String: Any]) -> [[String: Any]] {
        let core = root["core"] as? [String: Any] ?? [:]
        var out: [[String: Any]] = []
        for channel in ["release", "prerelease"] {
            guard let c = core[channel] as? [String: Any],
                  let raw = c["assets"] as? [[String: Any]], !raw.isEmpty else { continue }
            let assets: [[String: Any]] = raw.compactMap { a in
                guard let name = a["name"] as? String, let url = a["url"] as? String
                else { return nil }
                return ["name": name, "browser_download_url": url, "size": a["size"] ?? 0]
            }
            guard !assets.isEmpty else { continue }
            out.append([
                "tag_name": c["tag"] ?? "",
                "name": c["name"] ?? "",
                "body": c["notes"] ?? "",
                "prerelease": channel == "prerelease",
                "draft": false,
                "assets": assets,
            ])
        }
        return out
    }

    /// **本机这条产品线**在该通道里能装到的最高版本，拿不到返回 nil。
    ///
    /// ★ 以前「有没有新版」比的是 release 的 **tag**，而同一个 tag 上各平台的包是**陆续
    ///   到的**（本线的 DMG 由外部签名仓库 schat.build 异步追加，比 Windows/Linux 晚几分钟
    ///   到几小时都正常）。按 tag 比会让用户看到「有新版」却下不到包 —— 比不提示更糟。
    ///
    /// ★ mac 上架构不是判据，**产品线**才是：同一个 release 上并排放着两条线的 DMG，
    ///   本二进制是 Swift 线（`kind == "dmg"`，macOS 26+、arm64）。认成 Qt 线那个包等于
    ///   把用户换成另一个 app，而一键更新会先删掉旧 .app，没有退路。
    public static func versionForThisLine(_ root: [String: Any], channel: String) -> String? {
        let packages = root["packages"] as? [[String: Any]] ?? []
        var best: String?
        for p in packages {
            guard (p["type"] as? String) == channel,
                  (p["p"] as? String) == "mac",
                  (p["kind"] as? String) == "dmg",
                  let v = p["version"] as? String, !v.isEmpty
            else { continue }
            if best == nil || UpdateChecker.isNewer(remote: v, than: best!) {
                best = v
            }
        }
        return best
    }

    /// GeoIP 上游的最新发布时间戳，拿不到返回 nil。
    public static func geoipPublished(_ root: [String: Any]) -> String? {
        let g = root["geoip"] as? [String: Any]
        let s = g?["published"] as? String
        return (s?.isEmpty ?? true) ? nil : s
    }
}
