import Foundation
import Testing

@testable import CoastKit

/// `Notifier.post` 在**没有 app 包身份**的进程里必须静默跳过。
///
/// 这条钉的是一次真实崩溃(2026-08-02):`UNUserNotificationCenter.current()` 在
/// 没有 bundle id 的进程里直接抛 NSException,整个进程 SIGABRT。而
/// `swift run Coast` / `.build/debug/Coast` —— README 里写的那条开发流程 —— 恰恰
/// 就没有 bundle id,于是核心一崩(`onCoreUnexpectedlyExited` → `Notifier.post`)
/// 整个 app 跟着一起死。
///
/// **这条用例本身就是探针**:修复前它会把整个 `swift test` 进程一起带走
/// (测试宿主同样没有 bundle id,见下面的 guard),不是「失败一条」而是全量中断。
struct NotifierTests {

    /// ★ 调用不崩 = 通过。没有 `#expect` 是故意的 —— 要验的就是「这一行能返回」。
    @Test("★ 没有 bundle id 时 post 必须直接返回,不能抛 NSException 崩掉进程")
    func postIsSilentWithoutBundleIdentity() {
        // 宿主要是**有**包身份就跳过:那时 post 会真的走到通知中心去请求授权,
        // 跑测试不该弹系统授权框。(SwiftPM 的测试宿主目前是 nil,这条会真的跑。)
        guard Bundle.main.bundleIdentifier == nil else {
            print("⏭  跳过:测试宿主有 bundle id,再跑下去会弹系统授权框")
            return
        }
        Notifier.post(title: "t", body: "b")
    }
}
