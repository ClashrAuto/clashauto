import Foundation

/// `subscribe.yaml` 的**按行**文档模型。
///
/// 这个文件的结构是固定的两层缩进，C++ 侧每个操作都各自写一遍「扫描 + 记住行号」的循环
/// （`setSubscriptionEnabled`、`replaceSubscriptionList`、`existingNodeYamlByEndpoint`… 各来一遍）。
/// 这里把「切块」抽出来做一次，各操作都在块上动 —— 输出与 C++ 逐字节一致，但少了七八处
/// 手写扫描各自跑偏的机会。
///
/// 格式：
/// ```yaml
/// - name: 'Sub1'          ← 订阅块起始（顶格 "- "）
///   url: 'https://…'      ← 订阅字段（2 空格）
///   use: true
///   list:                 ← 节点列表起始
///     - name: 'HK-1'      ← 节点起始（4 空格 "- "）
///       server: '1.2.3.4' ← 节点字段（6 空格）
///       use: true
/// ```
struct SubscribeDocument {
    var lines: [String]

    init(text: String) {
        // 逐行去掉 CR 与 BOM：订阅文件被各种编辑器碰过，这两个脏字符在 C++ 侧也是每个循环都清一遍。
        lines = text.components(separatedBy: "\n").map { line in
            var cleaned = line.replacingOccurrences(of: "\r", with: "")
            if cleaned.first == "\u{FEFF}" { cleaned.removeFirst() }
            return cleaned
        }
    }

    var text: String { lines.joined(separator: "\n") }

    /// 每个订阅块占的行区间（含起始的 `- ` 行）。
    func blockRanges() -> [Range<Int>] {
        var starts: [Int] = []
        for (index, line) in lines.enumerated() where line.hasPrefix("- ") {
            starts.append(index)
        }
        guard !starts.isEmpty else { return [] }
        var ranges: [Range<Int>] = []
        for (position, start) in starts.enumerated() {
            let end = position + 1 < starts.count ? starts[position + 1] : lines.count
            ranges.append(start..<end)
        }
        return ranges
    }

    func blockRange(at index: Int) -> Range<Int>? {
        let ranges = blockRanges()
        guard index >= 0, index < ranges.count else { return nil }
        return ranges[index]
    }

    /// 订阅块内 `  list:` 那一行的行号。
    func listLine(in block: Range<Int>) -> Int? {
        for index in block where isSubscriptionField(lines[index])
            && lines[index].trimmingCharacters(in: .whitespaces).hasPrefix("list:") {
            return index
        }
        return nil
    }

    /// 订阅块内每个节点占的行区间。
    func nodeRanges(in block: Range<Int>) -> [Range<Int>] {
        guard let listStart = listLine(in: block) else { return [] }
        var starts: [Int] = []
        for index in (listStart + 1)..<block.upperBound where lines[index].hasPrefix("    - ") {
            starts.append(index)
        }
        guard !starts.isEmpty else { return [] }
        var ranges: [Range<Int>] = []
        for (position, start) in starts.enumerated() {
            let end = position + 1 < starts.count ? starts[position + 1] : block.upperBound
            // 末尾可能跟着空行，收窄到最后一个非空行
            var trimmedEnd = end
            while trimmedEnd > start + 1,
                  lines[trimmedEnd - 1].trimmingCharacters(in: .whitespaces).isEmpty {
                trimmedEnd -= 1
            }
            ranges.append(start..<trimmedEnd)
        }
        return ranges
    }

    // MARK: - 字段读写

    /// 订阅级字段（2 空格缩进，且不是 4 空格的节点内容）。
    func isSubscriptionField(_ line: String) -> Bool {
        line.hasPrefix("  ") && !line.hasPrefix("    ")
    }

    /// 读订阅块里某个字段的标量值。起始的 `- name: x` 那一行也算 name 字段。
    func subscriptionValue(_ key: String, in block: Range<Int>) -> String? {
        for index in block {
            let line = lines[index]
            if index == block.lowerBound, line.hasPrefix("- ") {
                let rest = String(line.dropFirst(2)).trimmingCharacters(in: .whitespaces)
                if rest.hasPrefix("\(key):") {
                    return YAMLScalar.unquote(String(rest.dropFirst(key.count + 1)))
                }
                continue
            }
            guard isSubscriptionField(line) else { continue }
            let field = line.trimmingCharacters(in: .whitespaces)
            if field.hasPrefix("\(key):") {
                return YAMLScalar.unquote(String(field.dropFirst(key.count + 1)))
            }
        }
        return nil
    }

    /// 写订阅级字段；不存在则插到块内**首个 `list:` 之前**（或块末），保持 list 永远在最后。
    mutating func setSubscriptionValue(_ key: String, raw value: String, in blockIndex: Int) -> Bool {
        guard let block = blockRange(at: blockIndex) else { return false }
        for index in block {
            let line = lines[index]
            if index == block.lowerBound, line.hasPrefix("- ") {
                let rest = String(line.dropFirst(2)).trimmingCharacters(in: .whitespaces)
                if rest.hasPrefix("\(key):") {
                    lines[index] = "- \(key): \(value)"
                    return true
                }
                continue
            }
            guard isSubscriptionField(line) else { continue }
            if line.trimmingCharacters(in: .whitespaces).hasPrefix("\(key):") {
                lines[index] = "  \(key): \(value)"
                return true
            }
        }
        let insertAt = listLine(in: block) ?? block.upperBound
        lines.insert("  \(key): \(value)", at: insertAt)
        return true
    }

    /// 读节点区间里某字段。
    func nodeValue(_ key: String, in node: Range<Int>) -> String? {
        for index in node {
            let line = lines[index]
            let field: String
            if index == node.lowerBound, line.hasPrefix("    - ") {
                field = String(line.dropFirst(6)).trimmingCharacters(in: .whitespaces)
            } else if line.hasPrefix("      ") {
                field = line.trimmingCharacters(in: .whitespaces)
            } else {
                continue
            }
            if field.hasPrefix("\(key):") {
                return YAMLScalar.unquote(String(field.dropFirst(key.count + 1)))
            }
        }
        return nil
    }

    /// 写节点的 `use:`；没有该键就补一行到节点末尾。
    mutating func setNodeUse(_ enabled: Bool, in node: Range<Int>) {
        let value = enabled ? "true" : "false"
        for index in node {
            let line = lines[index]
            guard line.hasPrefix("      ") else { continue }
            if line.trimmingCharacters(in: .whitespaces).hasPrefix("use:") {
                lines[index] = "      use: \(value)"
                return
            }
        }
        lines.insert("      use: \(value)", at: node.upperBound)
    }
}

/// YAML 标量的加/去引号。与 C++ `SubscriptionStore::scalar()` / `quote()` 同口径。
enum YAMLScalar {
    /// 去掉行内注释与包裹引号。注意判注释用的是 **" #"**（前面必须有空格）——
    /// 节点名里带 `#` 很常见（`香港#1`），按裸 `#` 切会把名字截断。
    static func unquote(_ raw: String) -> String {
        var out = raw.trimmingCharacters(in: .whitespaces)
        if let comment = out.range(of: " #") {
            out = String(out[out.startIndex..<comment.lowerBound]).trimmingCharacters(in: .whitespaces)
        }
        if out.count >= 2 {
            let first = out.first!, last = out.last!
            if (first == "\"" && last == "\"") || (first == "'" && last == "'") {
                out = String(out.dropFirst().dropLast())
            }
        }
        return out.replacingOccurrences(of: "''", with: "'").trimmingCharacters(in: .whitespaces)
    }

    /// 单引号包裹，内部 `'` 翻倍。用单引号是因为节点名里反斜杠很常见，
    /// 单引号 YAML 标量不做转义处理，写进去是什么就是什么。
    static func quote(_ raw: String) -> String {
        "'" + raw.replacingOccurrences(of: "'", with: "''") + "'"
    }
}
