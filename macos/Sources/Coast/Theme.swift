import Observation
import SwiftUI

/// 全局设计令牌 —— `qml/Theme.qml` 的对应物，取值**逐条对齐**，不是「重新设计一套」。
///
/// 翻 `dark` 即整窗换肤：所有颜色都是依赖它的计算属性，`@Observable` 保证读到它们的视图
/// 自动重算。
@Observable
/// 设计令牌。
///
/// **不为原生控件准备令牌**（输入框底色/描边、滚动条把手、窗口壳层等）：
/// `MainView` 上设了 `.preferredColorScheme(theme.dark ? .dark : .light)`，
/// 系统控件与所有 sheet 都会跟着 app 主题走 —— 手写一组硬编码色反而会和系统外观打架。
/// 早先从 Qt 直译过来的那几个（shell / inputBg / inputBorder / scrollHandle）确认无人引用，已移除；
/// Qt 那边必须手动画一切，SwiftUI 不必。
public final class Theme {
    /// 真值来源。启动时按配置/系统外观初始化。
    public var dark: Bool = true

    public init(dark: Bool = true) { self.dark = dark }

    // MARK: 品牌色
    public let accent = Color(hex: 0x4898F8)
    /// 「直连」的色点。取值与 Qt 的 `StatusPage.qml` 一致（`#48a5a7`）——
    /// 状态页两张连接列表靠它和 `accent` 区分这条走没走代理。
    /// 不跟随明暗切换：它要和 `accent` 形成固定对比，换一套值就失去了「一眼分辨」的作用。
    /// 延迟 → 颜色：绿(快) / 黄(一般) / 红(慢)。取值与阈值都照搬 Qt 的 `StatusPage.qml`。
    ///
    /// 阈值按「体感」定，不是什么标准：100ms 以内基本无感，300ms 以上开网页就明显在等了。
    public func latencyColor(_ ms: Int) -> Color {
        if ms < 0 { return textMuted }
        if ms < 100 { return Color(hex: 0x4DA13E) }
        if ms < 300 { return Color(hex: 0xC69A54) }
        return Color(hex: 0xA84343)
    }

    public var directDot: Color { Color(hex: 0x48A5A7) }

    public var accentStrong: Color { dark ? Color(hex: 0x83BDFF) : Color(hex: 0x1F6FD2) }
    public let danger = Color(hex: 0xFF4D4F)

    // MARK: 结构面
    /// 主内容卡 / 页脚开关底
    public var card: Color { dark ? Color(hex: 0x000000) : Color(hex: 0xFFFFFF) }
    public var metricBg: Color { dark ? Color(hex: 0x2A2A2A) : Color(hex: 0xEEEEEE) }
    public var nodeRowBg: Color { dark ? Color(hex: 0x252525) : Color(hex: 0xEEEEEE) }
    public let nodeRowActive = Color(red: 72 / 255, green: 151 / 255, blue: 248 / 255, opacity: 0.69)

    // MARK: 文本
    public var textPrimary: Color { dark ? Color(hex: 0xEEEEEE) : Color(hex: 0x111111) }
    public var textSecondary: Color { dark ? Color(hex: 0xCCCCCC) : Color(hex: 0x333333) }
    public var textMuted: Color { dark ? Color(hex: 0x888888) : Color(hex: 0x999999) }
    public let versionColor = Color(hex: 0x666666)

    // MARK: 控件
    public var footerComboBg: Color { dark ? Color(hex: 0x111111) : Color(hex: 0xFFFFFF) }
    public var switchTrackOff: Color { dark ? Color(hex: 0x666666) : Color(hex: 0xFFFFFF) }
    public var hover: Color { dark ? Color(hex: 0x3E3E3E) : Color(hex: 0xD2D2D2) }
    public var divider: Color { dark ? Color(hex: 0x333333) : Color(hex: 0xCCCCCC) }

    // MARK: 度量
    public let radius: CGFloat = 5
    public let sidebarWidth: CGFloat = 170
    public let footerHeight: CGFloat = 38
    /// mac 上主内容离窗顶/右 5px，透出玻璃。
    public let inset: CGFloat = 5

    // MARK: 设备类型 → 颜色
    /// 纯色区分设备类型，不依赖图标字体。
    public func deviceColor(_ typeKey: String) -> Color {
        switch typeKey {
        case "phone": return Color(hex: 0x4DA13E)
        case "tablet": return Color(hex: 0x3E8FA1)
        case "computer": return Color(hex: 0x466EA8)
        case "router": return Color(hex: 0xA86E43)
        case "tvbox": return Color(hex: 0x8A54C6)
        case "speaker": return Color(hex: 0xC65492)
        case "printer": return Color(hex: 0x5A6470)
        case "camera": return Color(hex: 0xA84343)
        case "game": return Color(hex: 0x43A897)
        case "nas": return Color(hex: 0x7A7A3E)
        case "iot": return Color(hex: 0xC69A54)
        default: return Color(hex: 0x888888)
        }
    }

    /// 设备类型 → SF Symbol。
    ///
    /// Qt 版用的是 Remix Icon 私用区码点（要额外带一个 300KB 的字体文件）。macOS 上换成
    /// SF Symbols：系统自带、自动跟随字重与明暗、无版权与打包负担。
    public func deviceSymbol(_ typeKey: String) -> String {
        switch typeKey {
        case "phone": return "iphone"
        case "tablet": return "ipad"
        case "computer": return "desktopcomputer"
        case "router": return "wifi.router"
        case "tvbox": return "tv"
        case "speaker": return "hifispeaker"
        case "printer": return "printer"
        case "camera": return "camera"
        case "game": return "gamecontroller"
        case "nas": return "externaldrive"
        case "iot": return "homekit"
        default: return "questionmark.circle"
        }
    }
}

extension Color {
    /// `0xRRGGBB` 字面量。写 `Color(hex: 0x4898F8)` 比逐个算 `red:green:blue:` 更容易和
    /// Theme.qml 的十六进制值逐条核对。
    init(hex: UInt32) {
        self.init(
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255
        )
    }
}
