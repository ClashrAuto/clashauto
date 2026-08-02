import Foundation
import UserNotifications

/// 系统通知的唯一入口。抽出来是因为切节点(AppState)和托盘(TrayController)都要发,
/// 免得那段 UNUserNotificationCenter 的授权+投递样板抄两遍。
///
/// 首次调用请求授权;被拒就静默跳过 —— 通知不是关键路径,不该因为它失败而打断任何流程。
/// 没有 app 包身份时同样静默跳过,见 `hasBundleIdentity`。
public enum Notifier {

    /// 本进程有没有 app 包身份(bundle id)。
    ///
    /// ★ **这不是洁癖检查,是崩溃防线。** `UNUserNotificationCenter.current()` 在没有
    ///   bundle id 的进程里不是返回 nil、也不是抛 Swift 错误 —— 它内部对
    ///   `bundleProxyForCurrentProcess` 下断言,直接抛 NSException,整个进程 SIGABRT。
    ///   而 Swift **接不住 NSException**(`try?`/`do-catch` 一概无效),所以只能在
    ///   调用**之前**挡住,没有第二种写法。
    ///
    ///   谁会没有 bundle id:`swift run Coast` / `.build/debug/Coast` —— 正是 README 里
    ///   写的那条开发流程。实测 2026-08-02:跑 dev 二进制、把核心 kill 掉,
    ///   `handleUnexpectedCoreExit()` → `onCoreUnexpectedlyExited` → 这里 → abort,
    ///   栈顶就是 `+[UNUserNotificationCenter currentNotificationCenter]`。
    ///   也就是说上面那句「不该因为它失败而打断任何流程」当时是不成立的:
    ///   崩溃发生在授权被问到之前。打好包的 `Coast.app` 有 bundle id,从来不受影响。
    ///
    /// `static let` 只求值一次 —— 进程的包身份运行期不会变。
    private static let hasBundleIdentity = Bundle.main.bundleIdentifier != nil

    public static func post(title: String, body: String) {
        guard hasBundleIdentity else { return }
        let center = UNUserNotificationCenter.current()
        center.requestAuthorization(options: [.alert]) { granted, _ in
            guard granted else { return }
            let content = UNMutableNotificationContent()
            content.title = title
            content.body = body
            center.add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
        }
    }
}
