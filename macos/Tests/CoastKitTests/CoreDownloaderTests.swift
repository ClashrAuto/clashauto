import Testing
@testable import CoastKit

// 资源名匹配值得单测：上游改一次 tag 写法就可能全部失配，而失败表现只是
// 「内核更新失败: 未找到匹配的 mihomo 资源」，看不出是哪一步错了。
@Suite("内核资源匹配")
struct CoreDownloaderAssetTests {
    private func asset(_ name: String) -> [String: Any] {
        ["name": name, "browser_download_url": "https://example.invalid/\(name)"]
    }

    private var release: [[String: Any]] {
        [
            asset("mihomo-darwin-arm64-v1.19.2.gz"),
            asset("mihomo-darwin-amd64-v1.19.2.gz"),
            asset("mihomo-darwin-amd64-compatible-v1.19.2.gz"),
            asset("mihomo-linux-amd64-v1.19.2.gz"),
            asset("mihomo-windows-amd64-v1.19.2.zip"),
        ]
    }

    @Test("按本机架构命中对应的 darwin 包")
    func picksHostArch() {
        let picked = CoreDownloader.pickAsset(assets: release, tag: "v1.19.2")
        #expect(picked != nil)
        // Intel 上必须优先 -compatible：普通 amd64 构建的指令集基线较新，老 Mac 会非法指令崩掉。
        let expected = AppPaths.cpuArch == "arm64"
            ? "mihomo-darwin-arm64-v1.19.2.gz"
            : "mihomo-darwin-amd64-compatible-v1.19.2.gz"
        #expect(picked?.name == expected)
    }

    @Test("精确名对不上时按正则兜底")
    func regexFallback() {
        // 模拟上游把 tag 写法改了（多了个后缀），精确名全部失配
        let odd = [asset("mihomo-darwin-arm64-v1.19.2-1.gz"),
                   asset("mihomo-darwin-amd64-compatible-v1.19.2-1.gz")]
        let picked = CoreDownloader.pickAsset(assets: odd, tag: "v1.19.2")
        #expect(picked != nil)
        #expect(picked?.name.hasPrefix("mihomo-darwin-") == true)
    }

    @Test("一个 darwin 包都没有时返回 nil，而不是错拿 linux 的")
    func noDarwinAsset() {
        let other = [asset("mihomo-linux-amd64-v1.19.2.gz"),
                     asset("mihomo-windows-amd64-v1.19.2.zip")]
        #expect(CoreDownloader.pickAsset(assets: other, tag: "v1.19.2") == nil)
    }

    @Test("只有 .zip 的 darwin 资源也不要 —— macOS 的是裸 gzip")
    func rejectsWrongExtension() {
        let zipped = [asset("mihomo-darwin-arm64-v1.19.2.zip"),
                      asset("mihomo-darwin-amd64-compatible-v1.19.2.zip")]
        #expect(CoreDownloader.pickAsset(assets: zipped, tag: "v1.19.2") == nil)
    }
}
