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
@MainActor
enum WindowRestore {

    /// 固定的存储键。改它等于让所有老用户的窗口位置回到默认，别改。
    private static let autosaveName = "CoastMainWindow"

    /// 主窗口。`showMainWindow()` 也用它 —— **不能用 `windows.first`**：
    /// 那可能是状态栏窗口或更新窗，把它 order front 等于什么都没做。
    static weak var mainWindow: NSWindow?

    /// ✕ 的拦截器。必须**强引用**：`NSWindow.delegate` 是 weak 的，
    /// 挂完就被释放的话拦截器等于没装。
    private static var closeGuard: CloseGuard?

    /// SwiftUI 自动生成的那批键的共同前缀。
    private static let leakedKeyPrefix = "NSWindow Frame SwiftUI."

    /// 认领主窗口：挂固定 autosave 名、恢复上次的位置，并顺手清掉历史垃圾键。
    @MainActor
    static func adopt() {
        purgeLeakedKeys()

        guard let window = NSApplication.shared.windows.first(where: { $0.canBecomeMain }) else {
            return
        }
        mainWindow = window

        // 先设名字再 setFrameUsingName：AppKit 会在设名字的那一刻把当前 frame 写进去，
        // 顺序反了就把「默认位置」当成「上次的位置」存下来了。
        let hadSavedFrame = UserDefaults.standard.object(forKey: "NSWindow Frame " + autosaveName) != nil
        window.setFrameAutosaveName(autosaveName)
        if hadSavedFrame {
            window.setFrameUsingName(autosaveName)
        } else {
            placeBottomRight(window)
        }

        // ✕ **不销毁窗口，只隐藏**（与 Qt 的 `onClosing: close.accepted = false; hide()` 一致）。
        //
        // ★ 不拦的话是个死局：SwiftUI 的 `WindowGroup` 窗口一关就没了，而
        //   `applicationShouldTerminateAfterLastWindowClosed` 又是 false（进程照样活着）——
        //   于是点一下红点，界面**再也回不来**（实测：关掉之后窗口数恒为 0，
        //   重新激活 app 也开不出来，只剩一个够不着的托盘进程）。
        // 按住窗口任意「非交互」空白/文字/卡片背景即可拖动整窗 —— 对齐 `Main.qml` 里
        // 那个铺满窗口、z:-1 的 `DragHandler`。列表 / 输入框 / 按钮会先吃掉按下事件，
        // 所以在它们身上按住拖不会移动窗口，只有空白处才触发（AppKit 的判据与 Qt 那套
        // 「不夺取的 grabPermissions」是同一个效果）。
        window.isMovableByWindowBackground = true

        let guardian = CloseGuard()
        closeGuard = guardian
        window.delegate = guardian
    }

    /// 把窗口摆到当前屏**可用区**的右下角（已扣除菜单栏与程序坞）。
    /// 与 Qt 的 `restoreWindowPos` 在「没有历史位置」时的落点一致。
    private static func placeBottomRight(_ window: NSWindow) {
        guard let visible = (window.screen ?? NSScreen.main)?.visibleFrame else { return }
        let size = window.frame.size
        window.setFrameOrigin(CGPoint(x: visible.maxX - size.width,
                                      y: visible.minY))
    }

    /// 把主窗拉回前台。窗口只是被隐藏过，所以这里总能拿到它。
    static func showMainWindow() {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        mainWindow?.makeKeyAndOrderFront(nil)
        // 兜底：万一还是多出来一个，延后一拍收掉（主路径靠
        // `applicationShouldHandleReopen` 返回 false 拦住）。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { discardDuplicateMainWindows() }
    }

    /// ✕ 之后 SwiftUI 的 `WindowGroup` 会在「重新打开」时**再建一个**主窗
    /// （实测重开之后窗口数是 2：一个在原位、一个在屏幕左上角）。
    /// 这里把多出来的那些收掉，保证「只有一个主窗」这个不变式。
    private static func discardDuplicateMainWindows() {
        for window in NSApplication.shared.windows
        where window !== mainWindow
            && window.canBecomeMain
            && window.frameAutosaveName.isEmpty
            && window.title == mainWindow?.title {
            window.orderOut(nil)
        }
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

/// ✕ 的拦截器：只隐藏、不销毁，并同时收掉 Dock 图标。
///
/// 收 Dock 图标是照 Qt 来的 —— `Main.qml` 的 `onVisibleChanged` 里
/// `bridge.setMacDockVisible(visible)`：窗口一藏，Dock 上就不留图标，
/// 回来的路只有托盘（或菜单栏）那一条。顺带也避开了「Dock 重开会再建一个窗」那个坑。
private final class CloseGuard: NSObject, NSWindowDelegate {
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        sender.orderOut(nil)
        NSApplication.shared.setActivationPolicy(.accessory)
        return false
    }
}
