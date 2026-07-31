import CoastKit
import Observation
import SwiftUI

/// 界面语言。
///
/// 沿用 Qt 版的做法：**用中文源串直接当 key**，翻译表是一张扁平的 `中文 → 目标语` 映射
/// （`assets/i18n/<code>.json`）。这套设计的好处是不需要 lupdate/.ts/.qm 那一整套工具链，
/// 也不需要给每个字符串编 key —— 代码里写的就是最终中文，读代码即读界面。
/// 简体中文是源语言，不加载任何表。
///
/// 12 种语言的表与 Qt 版**共用同一批文件**，不复制一份到 macos/。
@MainActor
@Observable
public final class I18n {
    public static let shared = I18n()

    /// 当前语言码。`zh-CN` = 源语言。
    public private(set) var language = "zh-CN"
    private var table: [String: String] = [:]

    /// 有翻译表的语言码（`zh-CN` 是源语言，不在其中）。
    public static let availableLanguages = [
        "zh-CN", "zh-TW", "en-US", "ja", "ko", "ru", "es", "fr", "de", "pt-BR", "it", "tr", "vi",
    ]

    private init() {}

    /// 译文；表里没有就回落到源串本身（即中文）。
    ///
    /// 回落到源串而不是显示 key 或空白，是这套方案的关键：翻译漏了一条，用户看到的是中文，
    /// 不是 `settings.title` 这种给程序员看的东西，更不是空白。
    public func translate(_ source: String) -> String {
        table[source] ?? source
    }

    public func setLanguage(_ code: String) {
        guard code != language else { return }
        language = code
        table = code == "zh-CN" ? [:] : Self.loadTable(code)
    }

    /// 按 config 决定初始语言：跟随系统则用系统语言，否则用手选的。
    /// `COAST_LANG=<code>` 可覆盖 —— 验各语言的排版（长译文撑不撑破布局）时不必去改用户配置。
    public func applyConfig(_ config: AppConfig) {
        if let forced = ProcessInfo.processInfo.environment["COAST_LANG"], !forced.isEmpty {
            setLanguage(forced)
            return
        }
        setLanguage(config.autoLanguage ? Self.systemLanguage() : config.language)
    }

    private static func loadTable(_ code: String) -> [String: String] {
        guard let url = Resources.asset("i18n/\(code).json"),
              let data = try? Data(contentsOf: url),
              let map = try? JSONSerialization.jsonObject(with: data) as? [String: String]
        else { return [:] }
        return map
    }

    /// 按系统区域推断界面语言。认不出的一律 `en-US` ——
    /// 让一个我们没翻过的语区看到英文，比看到中文更接近「能用」。
    public static func systemLanguage() -> String {
        let preferred = Locale.preferredLanguages.first ?? "en"
        let locale = Locale(identifier: preferred)
        guard let code = locale.language.languageCode?.identifier else { return "en-US" }
        switch code {
        case "zh":
            // 繁简要按脚本分，不能只看地区：香港/台湾/澳门用繁体，其余简体。
            let script = locale.language.script?.identifier
            if script == "Hant" { return "zh-TW" }
            if let region = locale.region?.identifier, ["TW", "HK", "MO"].contains(region) { return "zh-TW" }
            return "zh-CN"
        case "ja": return "ja"
        case "ko": return "ko"
        case "ru": return "ru"
        case "es": return "es"
        case "fr": return "fr"
        case "de": return "de"
        case "pt": return "pt-BR"
        case "it": return "it"
        case "tr": return "tr"
        case "vi": return "vi"
        default: return "en-US"
        }
    }
}

extension String {
    /// 取译文。写法刻意短 —— 它会出现在每一个界面字符串上。
    ///
    /// 走全局单例而不是 `@Environment`：那样每个 View 都得声明一个环境变量才能翻译字符串，
    /// 而 `.help()`、`Button(标题)` 这类地方拿不到环境。语言切换靠给根视图挂
    /// `.id(i18n.language)` 整体重建，等价于 QML 的 `retranslate()`。
    @MainActor
    public var t: String { I18n.shared.translate(self) }
}

extension I18n {
    /// 语言码 → 各语言**自己的**名字（endonym）。
    ///
    /// 刻意不用当前界面语言去翻译语言名：用户切到一门自己看不懂的语言后，要靠这个列表切回来，
    /// 而「用他看不懂的语言写的语言名」帮不上任何忙。列自称是所有语言选择器的通行做法。
    public static func displayName(_ code: String) -> String {
        switch code {
        case "zh-CN": return "简体中文"
        case "zh-TW": return "繁體中文"
        case "en-US": return "English"
        case "ja": return "日本語"
        case "ko": return "한국어"
        case "ru": return "Русский"
        case "es": return "Español"
        case "fr": return "Français"
        case "de": return "Deutsch"
        case "pt-BR": return "Português (Brasil)"
        case "it": return "Italiano"
        case "tr": return "Türkçe"
        case "vi": return "Tiếng Việt"
        default: return code
        }
    }
}
