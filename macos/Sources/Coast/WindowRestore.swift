import AppKit

/// 主窗口的位置与大小持久化。
///
/// ## 为什么要自己接管
///
/// SwiftUI 的 `WindowGroup` 确实会存窗口 frame，但它自动生成的存储键里**带着运行时地址**：
///
/// ```
/// NSWindow Frame SwiftUI.ModifiedContent<Coast.(unknown context at $10028f608).RootView, …>-1-AppWindow-1
/// ```
///
/// `$10028f608` 是那个类型的地址，而 ASLR 保证它**每次启动都不一样**。后果有两个，
/// 都是真实发生的（在开发机上跑了几十次之后，`defaults read com.yuehongsun.coast`
/// 里已经积了 57 个这样的键）：
///
/// 1. **窗口位置从来没有被真正恢复过** —— 每次启动都是一个全新的键，上次存的那份永远读不到。
///    Qt 版专门做了这件事（`Main.qml` 的 `restoreWindowPos`：退出/移动时把 x/y 持久化，
///    下次启动恢复到上次的位置，没有历史位置则落在右下角）。
/// 2. **偏好文件无限膨胀** —— 每启动一次多一个永远不会被读的键。
///
/// 所以这里给窗口挂一个**固定**的 autosave 名，并把历史垃圾键清掉。
enum WindowRestore {

    /// 固定的存储键。改它等于让所有老用户的窗口位置回到默认，别改。
    private static let autosaveName = "CoastMainWindow"

    /// SwiftUI 自动生成的那批键的共同前缀。
    private static let leakedKeyPrefix = "NSWindow Frame SwiftUI."

    /// 认领主窗口：挂固定 autosave 名、恢复上次的位置，并顺手清掉历史垃圾键。
    @MainActor
    static func adopt() {
        purgeLeakedKeys()

        guard let window = NSApplication.shared.windows.first(where: { $0.canBecomeMain }) else {
            return
        }
        // 先设名字再 setFrameUsingName：AppKit 会在设名字的那一刻把当前 frame 写进去，
        // 顺序反了就把「默认位置」当成「上次的位置」存下来了。
        window.setFrameAutosaveName(autosaveName)
        window.setFrameUsingName(autosaveName)
    }

    /// 清掉 SwiftUI 自动生成的那批带地址的键。
    ///
    /// 只删前缀完全匹配的，不碰任何别的偏好（`coast.helper.registeredCDHash` 之类）。
    private static func purgeLeakedKeys() {
        let defaults = UserDefaults.standard
        for key in defaults.dictionaryRepresentation().keys where key.hasPrefix(leakedKeyPrefix) {
            defaults.removeObject(forKey: key)
        }
    }
}
