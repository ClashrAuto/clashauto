import Foundation

/// `full.yaml` 生成用的文本手术工具。
///
/// 与 `YAMLText`（读 config.yaml 那套）分开：那边只需要读写单个标量，这边要整块替换、
/// 定位 `proxies:` / `proxy-groups:` / `rules:` 三个锚点、还要在列表里前插。口径对齐
/// C++ `ConfigBuilder` 的同名私有方法。
///
/// 再强调一次为什么不上 YAML 库：`default.yaml` 是 8 万行、带大量注释和特定缩进的模板，
/// 用库往返一次会把注释和格式全抹掉，生成的 full.yaml 与现在完全不同。
enum YAMLSurgery {

    // MARK: - 标量

    /// 顶层 `key: value`。键不存在时**前置**到文件开头（对齐 C++ 的 `prepend`）——
    /// 追加到末尾会掉进最后一个块的缩进范围里，变成那个块的子键。
    static func setScalar(_ yaml: String, key: String, value: String) -> String {
        let pattern = "(?m)^\(NSRegularExpression.escapedPattern(for: key)):\\s*.*$"
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return yaml }
        let range = NSRange(yaml.startIndex..., in: yaml)
        if regex.firstMatch(in: yaml, range: range) != nil {
            let template = NSRegularExpression.escapedTemplate(for: "\(key): \(value)")
            return regex.stringByReplacingMatches(in: yaml, range: range, withTemplate: template)
        }
        return "\(key): \(value)\n" + yaml
    }

    /// `section:` 块下缩进两格的 `key: value`；块或键不存在时补出来。
    static func setNestedScalar(_ yaml: String, section: String, key: String, value: String) -> String {
        guard let blockRange = blockRange(yaml, key: section) else {
            return yaml + "\n\(section):\n  \(key): \(value)\n"
        }
        var block = String(yaml[blockRange])
        let pattern = "(?m)^  \(NSRegularExpression.escapedPattern(for: key)):\\s*.*$"
        if let regex = try? NSRegularExpression(pattern: pattern) {
            let range = NSRange(block.startIndex..., in: block)
            if regex.firstMatch(in: block, range: range) != nil {
                let template = NSRegularExpression.escapedTemplate(for: "  \(key): \(value)")
                block = regex.stringByReplacingMatches(in: block, range: range, withTemplate: template)
            } else {
                if !block.hasSuffix("\n") { block += "\n" }
                block += "  \(key): \(value)\n"
            }
        }
        return yaml.replacingCharacters(in: blockRange, with: block)
    }

    // MARK: - 块

    /// 顶层键 `key:` 及其后所有缩进行的区间。
    static func blockRange(_ yaml: String, key: String) -> Range<String.Index>? {
        let pattern = "(?m)^\(NSRegularExpression.escapedPattern(for: key)):\\n(?:(?:  |\\t)[^\\n]*(?:\\n|$))*"
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(in: yaml, range: NSRange(yaml.startIndex..., in: yaml)),
              let range = Range(match.range, in: yaml) else { return nil }
        return range
    }

    /// 整块替换；没有就追加到文件末尾。`sniffer:` / `profile:` 这类「我们全权生成」的块用它。
    static func replaceOrAppendBlock(_ yaml: String, key: String, with block: String) -> String {
        if let range = blockRange(yaml, key: key) {
            return yaml.replacingCharacters(in: range, with: block)
        }
        var out = yaml
        if !out.hasSuffix("\n") { out += "\n" }
        return out + "\n" + block
    }

    // MARK: - 锚点

    /// `\nrules:` 那一行结束后的插入点。前插规则都插在这里。
    static func rulesInsertionPoint(_ yaml: String) -> String.Index? {
        guard let rules = yaml.range(of: "\nrules:") else { return nil }
        guard let lineEnd = yaml[rules.lowerBound...].dropFirst().firstIndex(of: "\n") else { return nil }
        return yaml.index(after: lineEnd)
    }

    /// `\nrules:` 那一行的起始（用于在 rules 之前插入 proxy-group 定义）。
    static func rulesAnchor(_ yaml: String) -> String.Index? {
        guard let rules = yaml.range(of: "\nrules:") else { return nil }
        return yaml.index(after: rules.lowerBound)
    }

    /// 顶层 `proxies:` 到 `proxy-groups:` 之间的整块（含 `proxies:` 行）。
    static func topLevelProxiesRange(_ yaml: String) -> Range<String.Index>? {
        guard let start = yaml.range(of: "(?m)^proxies:", options: .regularExpression),
              let end = yaml.range(of: "\nproxy-groups:", range: start.lowerBound..<yaml.endIndex)
        else { return nil }
        return start.lowerBound..<yaml.index(after: end.lowerBound)
    }

    /// 从 `proxy-groups:` 开始，第 n 个 `    proxies:` 行的**行首**位置。
    /// 策略组的成员列表都挂在这些行下面。
    static func groupProxiesLine(_ yaml: String, occurrence: Int) -> String.Index? {
        guard let groups = yaml.range(of: "\nproxy-groups:") else { return nil }
        var cursor = groups.lowerBound
        for _ in 0...occurrence {
            guard let hit = yaml.range(of: "\n    proxies:", range: yaml.index(after: cursor)..<yaml.endIndex)
            else { return nil }
            cursor = hit.lowerBound
        }
        return yaml.index(after: cursor)
    }

    /// 读某个 `    proxies:` 行下面的成员名（`      - xxx`）。
    static func groupMembers(_ yaml: String, at proxiesLine: String.Index) -> [String] {
        var members: [String] = []
        guard var cursor = yaml[proxiesLine...].firstIndex(of: "\n") else { return members }
        cursor = yaml.index(after: cursor)
        while cursor < yaml.endIndex {
            let lineEnd = yaml[cursor...].firstIndex(of: "\n") ?? yaml.endIndex
            let line = String(yaml[cursor..<lineEnd])
            guard line.hasPrefix("      - ") else { break }
            members.append(YAMLScalar.unquote(String(line.dropFirst(8))))
            if lineEnd == yaml.endIndex { break }
            cursor = yaml.index(after: lineEnd)
        }
        return members
    }

    /// 用给定成员整体替换某个 `    proxies:` 行及其下面的列表。
    static func replaceGroupMembers(_ yaml: String, at proxiesLine: String.Index, with values: [String]) -> String {
        guard var end = yaml[proxiesLine...].firstIndex(of: "\n") else { return yaml }
        end = yaml.index(after: end)
        while end < yaml.endIndex {
            let lineEnd = yaml[end...].firstIndex(of: "\n") ?? yaml.endIndex
            guard yaml[end..<lineEnd].hasPrefix("      - ") else { break }
            end = lineEnd == yaml.endIndex ? yaml.endIndex : yaml.index(after: lineEnd)
        }
        var replacement = "    proxies:\n"
        for value in values {
            replacement += "      - \(quote(value))\n"
        }
        return yaml.replacingCharacters(in: proxiesLine..<end, with: replacement)
    }

    // MARK: - 引号

    /// 生成侧的加引号。默认单引号（内部 `'` 翻倍）。
    ///
    /// 带 `\u`/`\U` 的值改用**双引号**并保留那个转义序列 —— 有些机场的节点名里是
    /// 字面量 `\uXXXX`，单引号 YAML 不解转义，写出去核心读到的会是那六个字符本身。
    static func quote(_ value: String) -> String {
        if value.isEmpty { return "''" }
        if value.contains("\\U") || value.contains("\\u") {
            var escaped = value.replacingOccurrences(of: "\\", with: "\\\\")
            escaped = escaped.replacingOccurrences(of: "\"", with: "\\\"")
            escaped = escaped.replacingOccurrences(of: "\\\\U", with: "\\U")
            escaped = escaped.replacingOccurrences(of: "\\\\u", with: "\\u")
            return "\"\(escaped)\""
        }
        return "'" + value.replacingOccurrences(of: "'", with: "''") + "'"
    }

    /// 抓 `^  - name: xxx` 形式的全部名字（proxies 条目、proxy-group 定义都用这个形状）。
    static func listItemNames(in block: String) -> [String] {
        guard let regex = try? NSRegularExpression(pattern: "(?m)^  - name:\\s*(.+)$") else { return [] }
        let range = NSRange(block.startIndex..., in: block)
        return regex.matches(in: block, range: range).compactMap { match in
            guard let captured = Range(match.range(at: 1), in: block) else { return nil }
            let name = YAMLScalar.unquote(String(block[captured]))
            return name.isEmpty ? nil : name
        }
    }
}
