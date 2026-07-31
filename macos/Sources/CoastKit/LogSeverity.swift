import Foundation

/// 日志条目的严重级别 —— 决定时间线左侧那颗圆点的颜色。
///
/// 判定规则**逐字对齐** Qt 的 `LogModel::severityFor`（它本身又是 Widgets 版
/// `MainWindow::appendTimeline` 的复刻）。优先级顺序是有意义的：错误 > 警告 > 成功 > 信息 ——
/// 一条「更新失败」既含「失败」也含「已」，必须判成红色而不是绿色，所以顺序不能调。
///
/// 写成**纯函数 + 独立类型**放在 CoastKit 而不是塞进视图里：这是唯一能对着 Qt 的
/// 判定表逐条测的形态，塞进 SwiftUI 视图就只能靠眼睛看颜色对不对。
public enum LogSeverity: String, Sendable, CaseIterable {
    case error
    case warn
    case success
    case info

    /// 由日志正文派生级别。大小写不敏感的英文关键字 + 原文匹配的中文关键字，
    /// 与 Qt 侧同一套（英文查小写副本、中文查原串）。
    public static func of(_ message: String) -> LogSeverity {
        let low = message.lowercased()
        if low.contains("fail") || low.contains("error") || message.contains("失败") {
            return .error
        }
        if low.contains("warn") {
            return .warn
        }
        if low.contains("finished") || low.contains("success") || low.contains(" ok")
            || message.contains("已") || message.contains("完成") {
            return .success
        }
        return .info
    }

    /// 圆点色的十六进制值。与 `qml/LogTimeline.qml` 的 `dotColor()` 一一对应 ——
    /// 这四个是固定品牌色，**不跟随深浅主题**（Qt 那边也是硬编码），
    /// 换一套值就失去了「红=出事了」这种跨版本的肌肉记忆。
    public var colorHex: UInt32 {
        switch self {
        case .error: return 0xF5_6C_6C
        case .warn: return 0xE6_A2_3C
        case .success: return 0x67_C2_3A
        case .info: return 0x48_98_F8
        }
    }
}
