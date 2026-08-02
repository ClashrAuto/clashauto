import Foundation

/// 一条日志的**来源**与**分量**。决定它进哪条时间线、进不进页脚。
///
/// 以前只有一个布尔 `isCore`，而且判据是「这条消息从哪个回调过来的」——
/// `CoastController.onLog` 那条流里**既有**核心进程的 stdout 原文、**也有**程序自己的
/// 编排消息（设置系统代理、接管设备、装 helper …），于是两边都错：
/// 「主日志」里混着核心的 debug 刷屏，「Clash 内核」里混着程序自己的动作。
/// 分量得在**产生消息的地方**标出来，收集端猜不出来。
///
/// `routine` 与 `notice` 都是程序侧的，日志页一样收；区别只在页脚那一行 ——
/// 它只有一行，被每次开关都会刷的例行回执占着，真正要看的错误就永远露不出来。
public enum LogKind: Sendable, Equatable {
    /// 程序侧、值得让用户看见的一条。错误和警告也走这里（红/黄由 `LogSeverity` 判）。
    case notice
    /// 程序侧的**例行回执**：每次开关/每轮任务都会刷一条，本身不含用户要处理的信息
    /// （`Start sysproxy ok!`、`Delay test finished.` 之类）。日志页照收，页脚不显示。
    case routine
    /// mihomo 进程吐出来的**原文**。只进「Clash 内核」那条时间线。
    case core
}
