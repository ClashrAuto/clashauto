import Foundation

/// 节点列表的搜索与筛选。
///
/// 抽成纯函数而不是写在视图里：视图里的逻辑只能靠肉眼看，而这段有几个**反直觉的边界**
/// （`delay == 0` 到底算不算不可用、大小写、中英混排），恰恰是需要用例钉住的地方。
public enum NodeFilter {

    /// - Parameters:
    ///   - onlyAvailable: 只保留测得通的。
    ///   - keyword: 搜索词，空串表示不过滤。
    public static func apply(_ nodes: [NodeInfo],
                             keyword: String,
                             onlyAvailable: Bool) -> [NodeInfo] {
        var result = nodes
        if onlyAvailable {
            // ★ `delay == 0` 是「**还没测过**」，不是「不可用」。
            //   一律滤掉的话，刚打开这个开关（此时全部为 0）列表会整个空掉，
            //   看起来像是出了故障 —— 而用户根本不知道要先点一次「测延迟」。
            result = result.filter { $0.delay > 0 }
        }
        let trimmed = keyword.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return result }
        // 不区分大小写：节点名中英混排是常态（`香港01 - HK Airport`），
        // 大小写敏感的话搜 `hk` 一个都搜不到，而用户不会想到是大小写的问题。
        return result.filter { $0.name.localizedCaseInsensitiveContains(trimmed) }
    }
}
