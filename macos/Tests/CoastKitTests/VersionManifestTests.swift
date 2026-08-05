import Foundation
import Testing
@testable import CoastKit

/// 清单 → GitHub `/releases` 形状的适配层。
///
/// 为什么值得单测：翻错了不会崩、不报错，表现是**更新页一片空白**（列表空）或者
/// **挑到另一条产品线的包**（Swift 线的用户装到 Qt 版）。前者用户以为没更新，后者更糟 ——
/// 一键更新会先删掉旧 .app，装完发现是另一个 app，没有退路。两种都不会在编译期暴露。
///
/// 夹具用的是真机上最常见的形态：正式版全套、测试版只有 win-x64（各平台的包是陆续到的）。
@Suite("版本清单适配")
struct VersionManifestTests {

    private var fixture: [String: Any] {
        let raw = """
        {
          "schema": 1,
          "packages": [
            {"type":"release","p":"win","芯片":"x64","kind":"setup","version":"1.0.639",
             "address":"https://x/Coast-1.0.639-windows-x64-setup.exe","size":100,
             "name":"Coast-1.0.639-windows-x64-setup.exe",
             "sha256":"https://x/Coast-1.0.639-windows-x64-setup.exe.sha256"},
            {"type":"release","p":"mac","芯片":"universal","kind":"dmg-qt","version":"1.0.700",
             "address":"https://x/Coast-1.0.700-macos-universal-qt.dmg","size":100,
             "name":"Coast-1.0.700-macos-universal-qt.dmg"},
            {"type":"release","p":"mac","芯片":"arm64","kind":"dmg","version":"1.0.639",
             "address":"https://x/Coast-1.0.639-macos-arm64.dmg","size":100,
             "name":"Coast-1.0.639-macos-arm64.dmg",
             "sha256":"https://x/Coast-1.0.639-macos-arm64.dmg.sha256"},
            {"type":"prerelease","p":"win","芯片":"x64","kind":"setup","version":"1.0.881",
             "address":"https://x/Coast-1.0.881-windows-x64-setup.exe","size":100,
             "name":"Coast-1.0.881-windows-x64-setup.exe"}
          ],
          "releases": {
            "release":    {"tag":"v1.0.639","name":"Coast 1.0.639","version":"1.0.700","notes":"正式说明"},
            "prerelease": {"tag":"v1.0.881-beta.abc1234","name":"beta","version":"1.0.881","notes":"测试说明"}
          },
          "core": {
            "release": {"tag":"v1.10.4393","notes":"内核说明","assets":[
              {"name":"coast-darwin-arm64-v1.10.4393.gz","url":"https://x/m.gz","size":1},
              {"name":"coast-darwin-amd64-compatible-v1.10.4393.gz","url":"https://x/i.gz","size":1}
            ]}
          },
          "geoip": {"published":"2026-08-04T23:33:18Z"}
        }
        """
        return (try? JSONSerialization.jsonObject(with: Data(raw.utf8))) as? [String: Any] ?? [:]
    }

    @Test("两条通道各翻译出一个 release，正式版在前")
    func translatesBothChannels() {
        let list = VersionManifest.appReleases(fixture)
        #expect(list.count == 2)
        #expect(list[0]["tag_name"] as? String == "v1.0.639")
        #expect(list[0]["prerelease"] as? Bool == false)
        #expect(list[1]["prerelease"] as? Bool == true)
        #expect(list[0]["body"] as? String == "正式说明")
    }

    // 清单里校验地址是挂在包上的字段，而 AppUpdater 下载完是按「有没有一个叫
    // <资源名>.sha256 的**资源**」查表校验的。不补的话下载能成、校验被静默跳过。
    @Test("有 sha256 的包会补出一条独立的 .sha256 资源")
    func rebuildsChecksumSidecars() {
        let list = VersionManifest.appReleases(fixture)
        let assets = list[0]["assets"] as? [[String: Any]] ?? []
        let names = assets.compactMap { $0["name"] as? String }
        #expect(names.contains("Coast-1.0.639-macos-arm64.dmg"))
        #expect(names.contains("Coast-1.0.639-macos-arm64.dmg.sha256"))
        // 没有 sha256 字段的包不该凭空造边车：3 个包 + 2 条边车。
        #expect(assets.count == 5)
        #expect(!names.contains("Coast-1.0.700-macos-universal-qt.dmg.sha256"))
    }

    // ★ 整条链路：翻译后的资源交给既有的 macAsset —— 它必须挑到 Swift 线那个包。
    @Test("翻译后 macAsset 挑到的是本线的包，不是 Qt 线的")
    func macAssetPicksThisLine() {
        let list = VersionManifest.appReleases(fixture)
        let assets = (list[0]["assets"] as? [[String: Any]] ?? []).compactMap {
            a -> (name: String, url: String)? in
            guard let n = a["name"] as? String, let u = a["browser_download_url"] as? String
            else { return nil }
            return (n, u)
        }
        let pick = UpdateChecker.macAsset(from: assets)
        #expect(pick?.name == "Coast-1.0.639-macos-arm64.dmg")
    }

    // ★ 这条是整件事的理由：Qt 线的包版本更高（1.0.700），但本线只有 1.0.639。
    //   按 release 的 tag 或按"最高版本"比，都会说「有新版」而实际下不到 —— 角标亮着、
    //   点下去没有本线的包。必须只看本产品线自己的版本。
    @Test("本线版本只看自己那条线，不被 Qt 线的更高版本带跑")
    func versionIgnoresOtherLine() {
        #expect(VersionManifest.versionForThisLine(fixture, channel: "release") == "1.0.639")
    }

    @Test("测试通道里没有本线的包 → nil（不能拿 tag 顶上说有新版）")
    func noPackageMeansNoVersion() {
        #expect(VersionManifest.versionForThisLine(fixture, channel: "prerelease") == nil)
    }

    @Test("内核翻译后能被既有的 CoreDownloader.pick 认出来")
    func coreFeedsExistingPicker() {
        let list = VersionManifest.coreReleases(fixture)
        #expect(list.count == 1)
        let pick = CoreDownloader.pick(releases: list, wantBeta: false)
        #expect(pick != nil)
        #expect(pick?.version == "v1.10.4393")
        #expect(pick?.notes == "内核说明")
    }

    @Test("GeoIP 时间戳取得到；没有就是 nil 而不是空串")
    func geoip() {
        #expect(VersionManifest.geoipPublished(fixture) == "2026-08-04T23:33:18Z")
        #expect(VersionManifest.geoipPublished(["geoip": ["published": ""]]) == nil)
        #expect(VersionManifest.geoipPublished([:]) == nil)
    }

    // 一个包都没有的通道不该被当成「有版本」—— 那会让角标亮着却下不到东西。
    @Test("通道里一个包都没有时，整条通道不出现在结果里")
    func emptyChannelDropped() {
        let root: [String: Any] = [
            "packages": [],
            "releases": ["release": ["tag": "v1.0.1", "notes": "x"]],
        ]
        #expect(VersionManifest.appReleases(root).isEmpty)
    }
}
