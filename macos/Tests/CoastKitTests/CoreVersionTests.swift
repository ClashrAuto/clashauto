import Foundation
import Testing

@testable import CoastKit

/// `CoreVersion.parse` 决定侧栏那颗 "core" 角标亮不亮。抠错版本号的后果是角标常亮或常灭，
/// 两种都没有任何报错、也没法从界面上追查，所以正则值得单独钉住。
struct CoreVersionTests {
    /// mihomo 真实的 `-v` 输出形状。
    @Test func parsesRealMihomoBanner() {
        let banner = "Mihomo Meta v1.19.29 darwin arm64 with go1.24.0 Wed Jul  9 12:00:00 UTC 2025"
        #expect(CoreVersion.parse(banner) == "v1.19.29")
    }

    /// ★ 要的是**第一个** `vN…`，而不是输出里任何以 v 开头的东西。
    /// 放宽正则的话 `with go1.24.0` 这类尾巴也会被抠进来。
    @Test func ignoresLaterVersionLikeTokens() {
        #expect(CoreVersion.parse("Mihomo Meta v1.18.0 linux amd64 with go1.22.5") == "v1.18.0")
        #expect(CoreVersion.parse("built with go1.24.0") == nil)
    }

    /// 预发布 / 带后缀的 tag 要完整取回来 —— 截断成 `v1.19.0` 会把
    /// 「本地是 alpha、上游是正式版」判成同一版。
    @Test func keepsPrereleaseSuffix() {
        #expect(CoreVersion.parse("Mihomo Meta v1.19.0-alpha.3 darwin arm64") == "v1.19.0-alpha.3")
    }

    /// 空输出 / 没有版本形状的输出 → nil，调用方据此**不置角标**（宁可漏报不误报）。
    @Test func returnsNilWhenNoVersionShape() {
        #expect(CoreVersion.parse("") == nil)
        #expect(CoreVersion.parse("permission denied") == nil)
    }

    /// 核心不存在时 `local()` 必须直接 nil，而不是去起一个不存在的进程。
    @Test func localIsNilWhenExecutableMissing() {
        #expect(CoreVersion.local(executable: URL(fileURLWithPath: "/nonexistent/mihomo")) == nil)
    }
}