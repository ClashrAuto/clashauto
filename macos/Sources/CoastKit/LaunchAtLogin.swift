import Foundation
import ServiceManagement

/// 开机自启。
///
/// 用 `SMAppService.mainApp` 而不是往 `~/Library/LaunchAgents` 写 plist：后者在
/// 签名+公证的应用上会被系统当成「未托管的登录项」，用户在「系统设置 → 登录项」里
/// 关掉之后我们也无从知晓，两边状态会长期不一致。`SMAppService` 的状态就是系统的真值。
public enum LaunchAtLogin {

    public static var isEnabled: Bool {
        SMAppService.mainApp.status == .enabled
    }

    /// 用户在「系统设置」里手动关掉过。这个状态要单独认出来 —— 此时再调 `register()`
    /// 不会生效，只能引导用户自己去开。
    public static var requiresApproval: Bool {
        SMAppService.mainApp.status == .requiresApproval
    }

    @discardableResult
    public static func setEnabled(_ enabled: Bool) -> Result<Void, Error> {
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            return .success(())
        } catch {
            return .failure(error)
        }
    }

    public static func openLoginItemsSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }
}
