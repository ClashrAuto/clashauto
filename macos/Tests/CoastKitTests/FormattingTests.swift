import Testing
@testable import CoastKit

// 口径必须和 qml/Theme.qml 的 fmtBytes/fmtRate 一致 —— 菜单栏和界面里显示的是同一个数，
// 两处算得不一样用户一眼就会发现。
@Suite("字节/速率格式化")
struct FormattingTests {

    @Test("1024 以下按整数字节，不带小数")
    func bytesUnderOneKB() {
        #expect(Formatting.bytes(0) == "0 B")
        #expect(Formatting.bytes(999) == "999 B")
        #expect(Formatting.bytes(1023) == "1023 B")
    }

    @Test("逐级进位，保留两位小数")
    func scalesUp() {
        #expect(Formatting.bytes(1024) == "1.00 KB")
        #expect(Formatting.bytes(1536) == "1.50 KB")
        #expect(Formatting.bytes(1024 * 1024) == "1.00 MB")
        #expect(Formatting.bytes(1024 * 1024 * 1024) == "1.00 GB")
    }

    @Test("速率就是字节加 /s")
    func rate() {
        #expect(Formatting.rate(1024 * 1024) == "1.00 MB/s")
        #expect(Formatting.rate(0) == "0 B/s")
    }

    @Test("大到超出单位表时停在 PB，不会越界")
    func clampsAtLargestUnit() {
        let huge: Int64 = 1024 * 1024 * 1024 * 1024 * 1024 * 4   // 4 PB
        #expect(Formatting.bytes(huge).hasSuffix(" PB"))
    }
}
