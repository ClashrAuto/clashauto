import AppKit
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

@Suite("菜单栏紧凑速率")
struct CompactRateTests {

    @Test("★ B 不带小数、其余一位——与 Qt speedTextCompact 逐字符相同")
    func matchesQt() {
        #expect(Formatting.compactRate(0) == "0 B/s")
        #expect(Formatting.compactRate(512) == "512 B/s")
        #expect(Formatting.compactRate(1023) == "1023 B/s")
        #expect(Formatting.compactRate(1024) == "1.0 KB/s")
        #expect(Formatting.compactRate(12_595) == "12.3 KB/s")
        #expect(Formatting.compactRate(1_258_291) == "1.2 MB/s")
    }

    @Test("负数按 0 算，不出现「-1.0 KB/s」")
    func clampsNegative() {
        #expect(Formatting.compactRate(-1) == "0 B/s")
    }

    @Test("单位只到 TB —— 再大也不进 PB（菜单栏里画不下，Qt 同）")
    func stopsAtTB() {
        let tb = Int64(1024) * 1024 * 1024 * 1024
        #expect(Formatting.compactRate(tb) == "1.0 TB/s")
        #expect(Formatting.compactRate(tb * 2048).hasSuffix(" TB/s"))
    }
}

@Suite("菜单栏那张手绘图")
@MainActor
struct TrayImageTests {

    /// 菜单栏典型厚度。真机取 `NSStatusBar.system.thickness`，测试里给个定值。
    private let thickness: CGFloat = 22

    @Test("★ 核心没跑：只画图标，宽度收到 iconSide + 4")
    func iconOnlyWhenStopped() {
        let drawn = TrayController.trayImage(thickness: thickness, coreRunning: false,
                                             up: "0 B/s", down: "0 B/s", icon: nil, dark: false)
        #expect(drawn.width == floor(thickness) - 3 + 4)
        #expect(drawn.image.size.height == thickness)
        #expect(drawn.image.size.width == drawn.width)
    }

    @Test("★ 核心在跑：图标 + 定宽文字区，且宽度**不随数字长短变化**")
    func fixedWidthWhileRunning() {
        let narrow = TrayController.trayImage(thickness: thickness, coreRunning: true,
                                              up: "0 B/s", down: "0 B/s", icon: nil, dark: false)
        let wide = TrayController.trayImage(thickness: thickness, coreRunning: true,
                                            up: "888.8 MB/s", down: "888.8 MB/s",
                                            icon: nil, dark: false)
        // 定宽是这块的全部意义：不定宽的话图标每秒被推来推去，菜单栏里看着在抖。
        #expect(narrow.width == wide.width)
        // 比只有图标时宽出一整块文字区
        #expect(narrow.width > floor(thickness) - 3 + 4)
        #expect(narrow.image.size.height == thickness)
    }

    @Test("图标画不出来时不塌成 0 宽（兜底灰块仍占位）")
    func placeholderKeepsWidth() {
        let drawn = TrayController.trayImage(thickness: thickness, coreRunning: true,
                                             up: "1.2 MB/s", down: "3.4 KB/s",
                                             icon: NSImage(), dark: true)
        #expect(drawn.width > 0)
        #expect(drawn.image.size.width == drawn.width)
    }
}
