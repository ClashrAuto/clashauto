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
            // `delay <= 0` = 未测或超时，两者都不显示（与设置项的字面意思一致）。
            //
            // ★ 注意这与 **Qt 的实装**不同：Qt 的 QML 版 `NodeListModel::reconcile()`
            //   **只按搜索词过滤**，`nodeOnlyAvailable` 这个设置在它那儿是**读了、存了、
            //   界面上有开关，但没有任何地方用**（真正会过滤的是已经不编译的 Widgets 版
            //   `MainWindow`）。这里是**刻意不跟** —— 一个点了没反应的开关比行为差异更糟。
            //
            //   代价：核心刚起来、还没测过延迟的那一两秒，列表会是空的（`ClashService`
            //   拿到节点后会自动测一次，随即填上）。这一点和上面那句注释原来是矛盾的：
            //   注释说「一律滤掉会让列表空掉，所以不能滤」，代码却正是这么滤的，
            //   测试也断言了滤掉。三者里错的是注释，已改。
            result = result.filter { $0.delay > 0 }
        }
        let trimmed = keyword.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return result }
        // 不区分大小写：节点名中英混排是常态（`香港01 - HK Airport`），
        // 大小写敏感的话搜 `hk` 一个都搜不到，而用户不会想到是大小写的问题。
        return result.filter { $0.name.localizedCaseInsensitiveContains(trimmed) }
    }
}
