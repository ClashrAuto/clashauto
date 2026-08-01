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

/// 设备列表的**次序**。照搬 Qt `DeviceListModel::buildTarget()` 的排序键：
/// **在线优先 → 今日流量（MB 档位）降序 → IP 升序 → MAC 兜底**。
///
/// 三点都是 Qt 注释里写明的理由，逐条抄过来：
///
///   • **排序键里绝不能出现实时速率** —— 速率每一拍都在变，一旦拿它排序，
///     只要有设备在跑流量，每次刷新都会重排，列表肉眼可见地抖；
///   • 今日流量**按 MB 取档**再比 —— 压掉「两台用量相近的设备来回互超」那点抖动；
///   • 最后拿 MAC 兜底 —— 保证任何时候次序都是确定的，不随上游容器的遍历顺序漂移。
///
/// 放在 `CoastKit` 而不是设备页里，是为了能单独测：排序错了在界面上只表现为
/// 「行怎么老在动」，很难指认。
public enum DeviceOrdering {

    /// 升序排列即为界面上的从上到下。
    public struct Key: Comparable, Sendable {
        let offline: Bool          // false 排前面 → 在线优先
        let negatedBucket: Int64   // 取负 → 今日流量大的排前面
        let ip: UInt32
        let mac: String

        public static func < (lhs: Key, rhs: Key) -> Bool {
            if lhs.offline != rhs.offline { return !lhs.offline }
            if lhs.negatedBucket != rhs.negatedBucket { return lhs.negatedBucket < rhs.negatedBucket }
            if lhs.ip != rhs.ip { return lhs.ip < rhs.ip }
            return lhs.mac < rhs.mac
        }
    }

    public static func key(online: Bool, ip: String, mac: String, todayBytes: Int64) -> Key {
        // `>> 20` = 按 MB 取档，与 Qt 的 `todayBucket()` 同一个写法。
        Key(offline: !online, negatedBucket: -(max(0, todayBytes) >> 20),
            ip: LanBrowser.ipSortKey(ip), mac: mac)
    }
}
