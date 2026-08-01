import Foundation
import UserNotifications

/// 系统通知的唯一入口。抽出来是因为切节点(AppState)和托盘(TrayController)都要发,
/// 免得那段 UNUserNotificationCenter 的授权+投递样板抄两遍。
///
/// 首次调用请求授权;被拒就静默跳过 —— 通知不是关键路径,不该因为它失败而打断任何流程。
public enum Notifier {
    public static func post(title: String, body: String) {
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
