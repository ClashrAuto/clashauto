import Foundation

/// 把 YAML 当**文本**读写的工具集。
///
/// 这不是偷懒 —— Qt 端就是这么做的（`AppConfig` / `ConfigBuilder` / `SubscriptionStore` 全部
/// 用 `QRegularExpression` + 字符串手术），而 `default.yaml` 是一份 8 万行、带大量注释和特定
/// 缩进的模板；用真正的 YAML 库往返一次会把注释和格式全部抹掉，生成出来的 full.yaml 与现在
/// 完全不同，风险远大于收益。所以此处刻意**不引 YAML 库**，逐条对齐 C++ 侧的正则语义。
///
/// 对应关系：
///   `value(_:key:)`       ↔ `AppConfigLoader::valueFromYaml`
///   `bool(_:key:)`        ↔ `AppConfigLoader::boolFromYaml`
///   `int(_:key:)`         ↔ `AppConfigLoader::intFromYaml`
///   `nestedValue(...)`    ↔ `AppConfigLoader::nestedValueFromYaml`
///   `nestedBool(...)`     ↔ `AppConfigLoader::nestedBoolFromYaml`
public enum YAMLText {

    // MARK: - 读

    /// 顶层标量：`^key: value`（去掉可选引号，截断行内 `#` 注释）。
    public static func value(_ text: String, key: String, default fallback: String? = nil) -> String? {
        let pattern = "(?m)^\(NSRegularExpression.escapedPattern(for: key)):[ \\t]*['\"]?([^'\"\\r\\n#]+)"
        guard let captured = firstCapture(pattern, in: text) else { return fallback }
        return captured.trimmingCharacters(in: .whitespaces)
    }

    public static func bool(_ text: String, key: String, default fallback: Bool) -> Bool {
        parseBool(value(text, key: key), default: fallback)
    }

    public static func int(_ text: String, key: String, default fallback: Int) -> Int {
        guard let raw = value(text, key: key), let parsed = Int(raw) else { return fallback }
        return parsed
    }

    /// 两级标量：`section:` 下缩进两格的 `key:`。
    public static func nestedValue(_ text: String, section: String, key: String,
                                   default fallback: String? = nil) -> String? {
        let sectionPattern = "(?m)^\(NSRegularExpression.escapedPattern(for: section)):\\n((?:  [^\\n]*\\n?)*)"
        guard let block = firstCapture(sectionPattern, in: text) else { return fallback }
        let keyPattern = "(?m)^  \(NSRegularExpression.escapedPattern(for: key)):[ \\t]*['\"]?([^'\"\\r\\n#]*)"
        guard let captured = firstCapture(keyPattern, in: block) else { return fallback }
        return captured.trimmingCharacters(in: .whitespaces)
    }

    public static func nestedBool(_ text: String, section: String, key: String, default fallback: Bool) -> Bool {
        parseBool(nestedValue(text, section: section, key: key), default: fallback)
    }

    // MARK: - 写

    /// 就地改写顶层标量；键不存在则**追加**到末尾。
    ///
    /// 只动那一行，其余字节原样保留 —— 用户手写的注释和键序不能因为改了一个开关就重排。
    public static func setValue(_ text: String, key: String, value: String) -> String {
        let escaped = NSRegularExpression.escapedPattern(for: key)
        let line = "\(key): \(value)"
        let pattern = "(?m)^[ \\t]*\(escaped)[ \\t]*:.*$"
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return text }
        let range = NSRange(text.startIndex..., in: text)
        if regex.firstMatch(in: text, range: range) != nil {
            let template = NSRegularExpression.escapedTemplate(for: line)
            return regex.stringByReplacingMatches(in: text, range: range, withTemplate: template)
        }
        let separator = (text.isEmpty || text.hasSuffix("\n")) ? "" : "\n"
        return text + separator + line + "\n"
    }

    public static func setBool(_ text: String, key: String, value: Bool) -> String {
        setValue(text, key: key, value: value ? "true" : "false")
    }

    public static func setInt(_ text: String, key: String, value: Int) -> String {
        setValue(text, key: key, value: String(value))
    }

    /// 需要引号的标量（含空格、`:`、`#`，或空串）。写订阅名/规则这类自由文本时用。
    public static func quoted(_ raw: String) -> String {
        if raw.isEmpty { return "\"\"" }
        let needsQuote = raw.rangeOfCharacter(from: CharacterSet(charactersIn: " :#\"'{}[],&*?|<>=!%@`")) != nil
        guard needsQuote else { return raw }
        let escaped = raw.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return "\"\(escaped)\""
    }

    // MARK: - 内部

    private static func parseBool(_ raw: String?, default fallback: Bool) -> Bool {
        switch raw?.lowercased() {
        case "true", "yes", "1": return true
        case "false", "no", "0": return false
        default: return fallback
        }
    }

    private static func firstCapture(_ pattern: String, in text: String) -> String? {
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return nil }
        let range = NSRange(text.startIndex..., in: text)
        guard let match = regex.firstMatch(in: text, range: range), match.numberOfRanges > 1,
              let captured = Range(match.range(at: 1), in: text) else { return nil }
        return String(text[captured])
    }
}
