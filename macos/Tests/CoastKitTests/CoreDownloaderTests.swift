import Testing
@testable import CoastKit

// 资源匹配值得单测:发布规则改一次就可能全部失配,而失败表现只是
// 「内核更新失败: 未找到匹配资源」,看不出是哪一步错了。
// 规则对齐 Qt `CoreRelease.h`:fork ClashrAuto/clash 的产物 `coast-darwin-*`,
// 版本号嵌在产物名里,/releases 全量列表按通道挑、必须带本平台产物才认。
@Suite("内核资源匹配")
struct CoreDownloaderAssetTests {
    private func asset(_ name: String) -> [String: Any] {
        ["name": name, "browser_download_url": "https://example.invalid/\(name)"]
    }

    private func release(prerelease: Bool = false, draft: Bool = false,
                         notes: String = "", assets: [[String: Any]]) -> [String: Any] {
        ["prerelease": prerelease, "draft": draft, "body": notes, "assets": assets]
    }

    /// 正式版 release 的完整资产表(CI 全平台产物)。
    private var stableAssets: [[String: Any]] {
        [
            asset("coast-darwin-arm64-v1.10.3951.gz"),
            asset("coast-darwin-amd64-compatible-v1.10.3951.gz"),
            asset("coast-darwin-amd64-v1-v1.10.3951.gz"),
            asset("coast-linux-amd64-v1.10.3951.gz"),
            asset("coast-windows-amd64-v1.10.3951.zip"),
        ]
    }

    /// 测试版:tag 是 Prerelease-<分支>,版本号只在产物名里。
    private var betaAssets: [[String: Any]] {
        [
            asset("coast-darwin-arm64-v1.10.3950-beta.d6e0fef9.gz"),
            asset("coast-darwin-amd64-compatible-v1.10.3950-beta.d6e0fef9.gz"),
        ]
    }

    @Test("按本机架构命中对应的 darwin 包,版本号从产物名剥出")
    func picksHostArch() {
        let picked = CoreDownloader.pickAsset(assets: stableAssets)
        #expect(picked != nil)
        // Intel 上必须优先 -compatible:不带后缀的 amd64 产物是 GOAMD64=v3,老 Mac 会非法指令崩掉。
        let expected = AppPaths.cpuArch == "arm64"
            ? "coast-darwin-arm64-v1.10.3951.gz"
            : "coast-darwin-amd64-compatible-v1.10.3951.gz"
        #expect(picked?.name == expected)
        #expect(picked?.version == "v1.10.3951")
    }

    @Test("一个 darwin 包都没有时返回 nil,而不是错拿 linux 的")
    func noDarwinAsset() {
        let other = [asset("coast-linux-amd64-v1.10.3951.gz"),
                     asset("coast-windows-amd64-v1.10.3951.zip")]
        #expect(CoreDownloader.pickAsset(assets: other) == nil)
    }

    @Test("只有 .zip 的 darwin 资源也不要 —— macOS 的是裸 gzip")
    func rejectsWrongExtension() {
        let zipped = [asset("coast-darwin-arm64-v1.10.3951.zip"),
                      asset("coast-darwin-amd64-compatible-v1.10.3951.zip")]
        #expect(CoreDownloader.pickAsset(assets: zipped) == nil)
    }

    @Test("上游品牌的 mihomo-* 产物不认 —— 发布源已换到 fork,别把上游包错装进来")
    func rejectsUpstreamNaming() {
        let upstream = [asset("mihomo-darwin-arm64-v1.19.2.gz"),
                        asset("mihomo-darwin-amd64-compatible-v1.19.2.gz")]
        #expect(CoreDownloader.pickAsset(assets: upstream) == nil)
    }

    @Test("零资产的空 tag 要跳过 —— fork 从上游继承了一堆,latest 常落在那上面")
    func skipsEmptyReleases() {
        let releases = [release(assets: []),
                        release(notes: "changelog", assets: stableAssets)]
        let picked = CoreDownloader.pick(releases: releases, wantBeta: false)
        #expect(picked?.version == "v1.10.3951")
        #expect(picked?.notes == "changelog")
    }

    @Test("通道各挑各的:正式版跳过 prerelease,测试版只认 prerelease")
    func channelFilter() {
        let releases = [release(prerelease: true, assets: betaAssets),
                        release(assets: stableAssets)]
        #expect(CoreDownloader.pick(releases: releases, wantBeta: false)?.version == "v1.10.3951")
        let beta = CoreDownloader.pick(releases: releases, wantBeta: true)
        #expect(beta?.version == "v1.10.3950-beta.d6e0fef9")
        #expect(beta?.beta == true)
    }

    @Test("draft 一律不认")
    func skipsDrafts() {
        let releases = [release(draft: true, assets: stableAssets)]
        #expect(CoreDownloader.pick(releases: releases, wantBeta: false) == nil)
    }
}

// 「有无新版」的判定单独测:它是「不一样」而不是「更大」,写错的表现是
// 角标永远不亮(装着上游 v1.19.x 时)或用户被钉死在测试版上,两种都极难排查。
@Suite("内核更新判定")
struct CoreHasUpdateTests {
    @Test("和本地不一样即可更新 —— 包括远端版本号更小(测试版切回正式版)")
    func differentMeansUpdate() {
        #expect(UpdateChecker.coreHasUpdate(remote: "v1.10.3951", local: "v1.10.3950"))
        #expect(UpdateChecker.coreHasUpdate(remote: "v1.10.3950", local: "v1.10.3951-beta.d6e0fef9"))
        // 本地还是上游 mihomo(版本号比 fork 的大)也要提示换
        #expect(UpdateChecker.coreHasUpdate(remote: "v1.10.3951", local: "v1.19.29"))
        #expect(!UpdateChecker.coreHasUpdate(remote: "v1.10.3951", local: "v1.10.3951"))
    }

    @Test("任一边拿不到版本时不提示 —— 宁可漏报也不误报一颗常亮的角标")
    func emptyNeverUpdates() {
        #expect(!UpdateChecker.coreHasUpdate(remote: "", local: "v1.10.3951"))
        #expect(!UpdateChecker.coreHasUpdate(remote: "v1.10.3951", local: ""))
    }
}
