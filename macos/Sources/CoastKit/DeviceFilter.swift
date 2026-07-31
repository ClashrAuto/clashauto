import Foundation

/// 设备列表的搜索。对齐 Qt 设备页的 `搜索设备 / IP / 厂商`。
///
/// 抽成纯函数的理由和 `NodeFilter` 一样：判据里有几个反直觉的地方（MAC 的分隔符、
/// 大小写、别名与自动识别名哪个优先），写在视图里就只能靠肉眼看。
public enum DeviceFilter {

    /// 一台设备里**所有可搜的文本**。
    ///
    /// 别名放在最前：用户给设备起了名字，就是打算按这个名字找它。
    public static func haystack(alias: String, hostname: String, vendor: String,
                                ip: String, mac: String) -> [String] {
        [alias, hostname, vendor, ip, mac,
         // MAC 去掉分隔符也要能搜到：用户从路由器后台复制过来的常常是
         // `a4:83:e7:…`、`a4-83-e7-…` 或 `a483e7…` 三种写法之一，
         // 只按原样比对的话，三种里只有一种搜得到。
         mac.replacingOccurrences(of: ":", with: "")
            .replacingOccurrences(of: "-", with: "")]
    }

    /// 关键词是否命中。空关键词一律命中（等于不过滤）。
    public static func matches(keyword: String, fields: [String]) -> Bool {
        let trimmed = keyword.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return true }
        // MAC 用户可能带也可能不带分隔符，所以关键词也去一次分隔符再比。
        let bare = trimmed.replacingOccurrences(of: ":", with: "")
                          .replacingOccurrences(of: "-", with: "")
        return fields.contains {
            guard !$0.isEmpty else { return false }
            return $0.localizedCaseInsensitiveContains(trimmed)
                || (bare.count >= 2 && $0.localizedCaseInsensitiveContains(bare))
        }
    }
}
