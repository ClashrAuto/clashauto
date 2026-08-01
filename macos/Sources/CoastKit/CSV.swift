import Foundation

/// 极小的 CSV 生成。对齐 Qt `DevicesController::exportCsv` 的转义规则。
///
/// 不引第三方库：只需要「写出去」，而 CSV 的写入侧规则很短 ——
/// 含逗号 / 引号 / 换行就整段加引号，内部的引号翻倍。
/// 但这几条**必须有测试**：转义漏了不会报错，只会让导出的表格在别人电脑上错位，
/// 而错位往往到很久以后才被发现。
public enum CSV {

    /// 单个字段的转义。
    public static func escape(_ field: String) -> String {
        guard field.contains(",") || field.contains("\"") || field.contains("\n")
                || field.contains("\r") else {
            return field
        }
        return "\"" + field.replacingOccurrences(of: "\"", with: "\"\"") + "\""
    }

    /// 一整张表。第一行是表头。
    public static func render(header: [String], rows: [[String]]) -> String {
        var out = header.map(escape).joined(separator: ",") + "\n"
        for row in rows {
            out += row.map(escape).joined(separator: ",") + "\n"
        }
        return out
    }
}
