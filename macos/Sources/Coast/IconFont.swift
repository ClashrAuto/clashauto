import CoreText
import CoastKit
import Foundation

/// Qt 端的两套图标字体，启动时注册进本进程后按 family 名引用。
///
/// 字体文件与 Qt 共用仓库里唯一的一份（`clashauto-c++/assets/*.ttf`，经
/// `Resources.asset` 定位）：`iconfont.ttf` 只有 logo 地球等几个自绘字形，
/// `remixicon.ttf` 是 Remix Icon 通用图标集（Apache-2.0）。侧栏图标必须用它们
/// 而不是 SF Symbols —— SF 各字形的视觉尺寸/字宽不一，同一字号下大小参差，
/// 且形状与 Qt 版完全对不上。
enum IconFont {
    /// `iconfont.ttf` 的 family 名（logo 地球 U+E600，见 `qml/Main.qml` logoBox）。
    static let logo = "iconfont"
    /// `remixicon.ttf` 的 family 名（侧栏导航等 UI 图标，见 `qml/NavButton.qml`）。
    static let remix = "remixicon"

    /// 进程级注册，App 启动时调一次。找不到文件或重复注册都静默放过 ——
    /// 缺字体只会让图标位显示为豆腐块，不该拦着应用启动。
    static func registerAll() {
        for file in ["iconfont.ttf", "remixicon.ttf"] {
            guard let url = Resources.asset(file) else { continue }
            CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
        }
    }
}
