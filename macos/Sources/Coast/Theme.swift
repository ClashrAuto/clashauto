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
/// （`inputBg` / `inputBorder` 后来又加了回来 —— 设置页按 Qt 重做成手画控件之后，
/// 它们重新有了消费者。删掉的仍是 `shell` / `scrollHandle` / `navSelected` 这三个。）
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

    /// 上传红 / 下载绿。取自 Qt 状态页两张卡的配色 —— 上下行用固定的红绿区分，
    /// 换成主题色的话两张卡会长得一模一样，一眼看不出哪张是哪张。
    ///
    /// ★ Qt 的这两张卡**文字与曲线不同色**：文字 `accentColor`、曲线 `chartColor`
    /// （上传 `#a84343` / `#b14a4a`，下载 `#4da13e` / `#5bb44b`）—— 曲线比文字亮一档，
    /// 压在卡底上才不至于糊掉。原来这里只有一个色、还把上传写成了 Qt 里根本没有的
    /// `#e05a5a`（自己编的），两处一起改。
    public var uploadAccent: Color { Color(hex: 0xA8_43_43) }
    public var uploadLine: Color { Color(hex: 0xB1_4A_4A) }
    public var downloadAccent: Color { Color(hex: 0x4D_A1_3E) }
    public var downloadLine: Color { Color(hex: 0x5B_B4_4B) }

    /// 今日流量卡的紫色。Qt `StatusPage.qml` 里这张卡通篇是 `#8a72c6` ——
    /// 标题、大数字、小时柱、Top5 的占比条全用它。原来 Swift 侧整张卡走的是 `accent`(蓝)，
    /// 于是它和「连接」卡撞色，六张卡里少了一张能一眼认出来的。
    public var todayAccent: Color { Color(hex: 0x8A_72_C6) }

    public var directDot: Color { Color(hex: 0x48A5A7) }

    /// 状态页四张卡左侧那个大图标的颜色。**中性灰，不是卡片的主色，也不是 `textMuted`**
    /// —— Qt 四张卡统一 `dark ? "#aaaaaa" : "#888888"`。图标只是个提示符号，
    /// 跟着主色走会和标题/大数字抢注意力，而那两样才是这张卡要说的话。
    public var cardIcon: Color { dark ? Color(hex: 0xAA_AA_AA) : Color(hex: 0x88_88_88) }

    public var accentStrong: Color { dark ? Color(hex: 0x83BDFF) : Color(hex: 0x1F6FD2) }
    public let danger = Color(hex: 0xFF4D4F)

    // MARK: 结构面
    /// 主内容卡 / 页脚开关底
    public var card: Color { dark ? Color(hex: 0x000000) : Color(hex: 0xFFFFFF) }
    /// 小卡底色。整窗玻璃化后带一点通透 —— 完全实底的话，六张卡会像六块贴纸糊在
    /// 玻璃上，玻璃只在卡与卡的缝隙里露出来，反而更显脏。
    public var metricBg: Color {
        (dark ? Color(hex: 0x2A2A2A) : Color(hex: 0xEEEEEE)).opacity(0.82)
    }
    public var nodeRowBg: Color { dark ? Color(hex: 0x252525) : Color(hex: 0xEEEEEE) }
    public let nodeRowActive = Color(red: 72 / 255, green: 151 / 255, blue: 248 / 255, opacity: 0.69)

    // MARK: 文本
    public var textPrimary: Color { dark ? Color(hex: 0xEEEEEE) : Color(hex: 0x111111) }
    public var textSecondary: Color { dark ? Color(hex: 0xCCCCCC) : Color(hex: 0x333333) }
    public var textMuted: Color { dark ? Color(hex: 0x888888) : Color(hex: 0x999999) }
    public let versionColor = Color(hex: 0x666666)

    // MARK: 控件
    /// 输入类控件的底与描边。**手画的控件才需要它们** —— 设置页把 Qt 的
    /// `ThemedField` / `ThemedCombo` / `ThemedSpin` / `PillButton` 逐个复刻了一遍，
    /// 那些控件的底色与描边就是这两个令牌。（早先按「无人引用」删过一次，
    /// 那时设置页还在用系统原生控件。）取值与 `qml/Theme.qml` 逐条相同。
    public var inputBg: Color { dark ? Color(hex: 0x444444) : Color(hex: 0xEAEAEA) }
    public var inputBorder: Color { dark ? Color(hex: 0x333333) : Color(hex: 0xCCCCCC) }

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
