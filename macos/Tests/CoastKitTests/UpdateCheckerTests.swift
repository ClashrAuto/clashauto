import Foundation
import Testing
@testable import CoastKit

// 版本比较是个经典坑：字典序下 "1.10.0" < "1.9.0"，一旦真出到 1.10 用户就永远收不到更新提示，
// 而且这种 bug 要等半年后才暴露。这里把口径钉死。
@Suite("版本比较")
struct VersionCompareTests {

    @Test("逐段按数字比，不是字符串比")
    func numericNotLexicographic() {
        #expect(UpdateChecker.isNewer(remote: "1.10.0", than: "1.9.0"))
        #expect(UpdateChecker.isNewer(remote: "1.9.0", than: "1.10.0") == false)
        #expect(UpdateChecker.isNewer(remote: "2.0.0", than: "1.99.99"))
    }

    @Test("相同版本不算新")
    func equalIsNotNewer() {
        #expect(UpdateChecker.isNewer(remote: "1.2.3", than: "1.2.3") == false)
        #expect(UpdateChecker.isNewer(remote: "v1.2.3", than: "1.2.3") == false)
    }

    @Test("缺的段按 0 补")
    func missingComponentsAreZero() {
        #expect(UpdateChecker.isNewer(remote: "1.2.1", than: "1.2"))
        #expect(UpdateChecker.isNewer(remote: "1.2", than: "1.2.0") == false)
        #expect(UpdateChecker.isNewer(remote: "1.3", than: "1.2.9"))
    }

    @Test("前导 v 与 -beta 后缀都要忽略")
    func stripsPrefixAndSuffix() {
        #expect(UpdateChecker.versionComponents("v1.2.3") == [1, 2, 3])
        #expect(UpdateChecker.versionComponents("1.2.3-beta.abc1234") == [1, 2, 3])
        // 带后缀的 beta 与同号正式版比，数字部分相同 → 不算新
        #expect(UpdateChecker.isNewer(remote: "v1.2.3-beta.deadbee", than: "1.2.3") == false)
    }

    @Test("非法输入不崩，按 0 处理")
    func garbageIsSafe() {
        #expect(UpdateChecker.versionComponents("") == [])
        #expect(UpdateChecker.versionComponents("abc") == [0])
        #expect(UpdateChecker.isNewer(remote: "abc", than: "1.0.0") == false)
    }
}

@Suite("更新资源挑选")
struct UpdateAssetTests {

    @Test("按扩展名挑 dmg，绝不按文件名前缀")
    func picksByExtensionNotPrefix() {
        // 品牌改过名（ClashAuto → Coast）；按前缀挑会让所有老版本的一键更新失效
        let assets = [("Coast-1.2.3-windows-x64.zip", "u1"),
                      ("SomeOtherName-1.2.3-macos-universal.dmg", "u2"),
                      ("Coast-1.2.3-linux-x64.tar.gz", "u3")]
        #expect(UpdateChecker.macAsset(from: assets)?.url == "u2")
    }

    @Test("没有 dmg 时退而找名字里带 macos 的")
    func fallsBackToKeyword() {
        let assets = [("Coast-1.2.3-macos-universal.zip", "u1"),
                      ("Coast-1.2.3-linux.tar.gz", "u2")]
        #expect(UpdateChecker.macAsset(from: assets)?.url == "u1")
    }

    @Test("一个都不匹配时返回 nil，不错拿 windows 的")
    func noMatch() {
        let assets = [("Coast-1.2.3-windows-x64.zip", "u1")]
        #expect(UpdateChecker.macAsset(from: assets) == nil)
    }

    @Test("★ 排除 Qt 那条线：-qt 的 mac 包是另一个 app，不是自己的更新")
    func skipsQtLine() {
        // Qt 包排在前面 —— 「取第一个 .dmg」的老写法在这里正好挑错
        let assets = [("Coast-1.2.3-macos-universal-qt.dmg", "qt"),
                      ("Coast-1.2.3-macos-universal.dmg", "swift")]
        #expect(UpdateChecker.macAsset(from: assets)?.url == "swift")

        // 只有 Qt 包 → 宁可报「没有可用更新」，也不能把用户换成另一条产品线
        #expect(UpdateChecker.macAsset(from: [("Coast-1.2.3-macos-universal-qt.dmg", "qt")]) == nil)
    }

    @Test("qt 的判定要带分隔符，别把名字里恰好有 qt 字母的资源误伤")
    func qtDetectionIsNotASubstringMatch() {
        #expect(UpdateChecker.isQtLine("Coast-1.2.3-macos-universal-qt.dmg"))
        #expect(UpdateChecker.isQtLine("Coast-macos-qt-1.2.3-universal.dmg"))
        #expect(UpdateChecker.isQtLine("Coast-1.2.3-macos-universal.dmg") == false)
        // 「quic」「qtest」这种含 qt 字母的名字不该被当成 Qt 线
        #expect(UpdateChecker.isQtLine("Coast-1.2.3-macos-universal-quic.dmg") == false)
    }
}

@Suite("发布列表解析")
struct ReleaseParsingTests {
    private func data(_ json: String) -> Data { json.data(using: .utf8)! }

    @Test("★ 限流响应必须抛错，不能被当成「已是最新版本」")
    func rateLimitThrows() {
        // GitHub 匿名调用每小时 60 次，超了回的是这个对象（不是数组）。
        // 早先版本在这里返回 nil，界面就显示「已是最新版本」—— 检查其实压根没成功。
        let body = #"{"message":"API rate limit exceeded for 1.2.3.4.","documentation_url":"https://..."}"#
        #expect(throws: UpdateChecker.CheckError.self) {
            try UpdateChecker.parseReleases(data: data(body), status: 403, includePrerelease: false)
        }
    }

    @Test("响应根本不是 JSON 时也抛错")
    func garbageThrows() {
        #expect(throws: UpdateChecker.CheckError.self) {
            try UpdateChecker.parseReleases(data: data("<html>502</html>"), status: 502, includePrerelease: false)
        }
    }

    @Test("空数组是合法的「没有发布」，返回 nil 而不是抛错")
    func emptyListIsNil() throws {
        #expect(try UpdateChecker.parseReleases(data: data("[]"), status: 200, includePrerelease: false) == nil)
    }

    @Test("关掉测试版时跳过 prerelease，开启时取到")
    func prereleaseChannel() throws {
        let body = """
        [{"tag_name":"v1.3.0-beta.abc","prerelease":true,"draft":false,"assets":[]},
         {"tag_name":"v1.2.0","prerelease":false,"draft":false,"assets":[]}]
        """
        let stable = try UpdateChecker.parseReleases(data: data(body), status: 200, includePrerelease: false)
        #expect(stable?.tag == "v1.2.0")
        let beta = try UpdateChecker.parseReleases(data: data(body), status: 200, includePrerelease: true)
        #expect(beta?.tag == "v1.3.0-beta.abc")
    }

    @Test("draft 永远跳过")
    func skipsDrafts() throws {
        let body = """
        [{"tag_name":"v9.9.9","prerelease":false,"draft":true,"assets":[]},
         {"tag_name":"v1.2.0","prerelease":false,"draft":false,"assets":[]}]
        """
        let release = try UpdateChecker.parseReleases(data: data(body), status: 200, includePrerelease: false)
        #expect(release?.tag == "v1.2.0")
    }
}
