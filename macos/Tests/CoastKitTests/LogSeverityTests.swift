import Testing

@testable import CoastKit

/// `LogSeverity.of` 是 Qt `LogModel::severityFor` 的移植。这组用例钉住的是
/// **判定表与优先级顺序**——顺序错了不会编译失败、也不会崩，只会让「更新失败」显示成绿点，
/// 而绿点的含义正好相反。
struct LogSeverityTests {
    @Test func englishKeywordsMatchCaseInsensitively() {
        #expect(LogSeverity.of("Download FAILED") == .error)
        #expect(LogSeverity.of("connection Error: refused") == .error)
        #expect(LogSeverity.of("WARN: config not found") == .warn)
        #expect(LogSeverity.of("update finished") == .success)
        #expect(LogSeverity.of("Success") == .success)
        #expect(LogSeverity.of("core started ok") == .success)
        #expect(LogSeverity.of("polling /proxies") == .info)
    }

    @Test func chineseKeywordsMatchTheOriginalString() {
        #expect(LogSeverity.of("订阅更新失败") == .error)
        #expect(LogSeverity.of("已启动核心") == .success)
        #expect(LogSeverity.of("配置生成完成") == .success)
        #expect(LogSeverity.of("正在拉取订阅") == .info)
    }

    /// ★ 优先级是这个函数的全部难点。「更新失败」同时含「失败」和……不含「已」；
    /// 但「已下载失败的那份」这类串两边都命中 —— 必须判红。同理 warn 要压过 success。
    @Test func earlierRulesWin() {
        #expect(LogSeverity.of("已更新失败") == .error)
        #expect(LogSeverity.of("error: 已完成一半") == .error)
        #expect(LogSeverity.of("warn: 已跳过") == .warn)
    }

    /// `" ok"` **带前导空格**是 Qt 侧的原样写法，不是笔误：没有它，
    /// 任何含 "ok" 子串的词（token、lookup、broken）都会被判成成功。
    @Test func okRequiresALeadingSpace() {
        #expect(LogSeverity.of("token refreshed") == .info)
        #expect(LogSeverity.of("lookup done") == .info)
        #expect(LogSeverity.of("check ok") == .success)
    }

    /// 圆点色必须与 `qml/LogTimeline.qml` 的 `dotColor()` 逐条相同。
    @Test func dotColorsMatchQt() {
        #expect(LogSeverity.error.colorHex == 0xF5_6C_6C)
        #expect(LogSeverity.warn.colorHex == 0xE6_A2_3C)
        #expect(LogSeverity.success.colorHex == 0x67_C2_3A)
        #expect(LogSeverity.info.colorHex == 0x48_98_F8)
    }
}
