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
        closeRestoredAuxiliaryWindows()

        guard let window = findMainWindow() else {
            openMainWindow()
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

    /// 三个按需打开的附属窗（更新 / 设备详情 / 连接）的 scene id 前缀。
    /// SwiftUI 会把 `Window(id:)` 的 id 原样写进 `NSWindow.identifier`。
    private static let auxiliaryPrefix = "coast."

    /// 找出主窗。**按 scene id 认**（SwiftUI 把 `Window(id:)` 原样写进 `NSWindow.identifier`），
    /// 认不出来时才退回原来的「能当主窗且不是附属窗」这条启发式。
    ///
    /// ★ 只有启发式那一条是不够的：`canBecomeMain` 在窗口被 `orderOut` 之后是 **false**，
    ///   于是「启动静默到托盘」(`mini`) 这条路会在这里认不出主窗 → 走兜底 `openMainWindow()`
    ///   → **把刚收起来的窗口又开了出来**。实测就是这样：勾了静默启动，窗口照样弹在脸上。
    ///   同一个坑还让 `mainWindow` 停在 nil，`anyWindowVisible` 只好一直 fail-open，
    ///   于是「收进托盘就别算界面数据」那套门控整个不生效。
    private static func findMainWindow() -> NSWindow? {
        let byIdentifier = NSApplication.shared.windows
            .first { $0.identifier?.rawValue == MainWindowID.value }
        return byIdentifier ?? NSApplication.shared.windows
            .first { $0.canBecomeMain && !isAuxiliary($0) }
    }

    static func isAuxiliary(_ window: NSWindow) -> Bool {
        guard let id = window.identifier?.rawValue else { return false }
        return id.hasPrefix(auxiliaryPrefix) && id != MainWindowID.value
    }

    /// 启动时把系统「恢复」出来的附属窗关掉。
    ///
    /// 这三个窗是**点出来的**：详情窗要有选中的设备、更新窗要有检查结果。macOS 的窗口恢复
    /// 不管这些，进程被杀之后下次启动照样把它们摆出来 —— 于是开机先看到一个**空白的
    /// 「设备详情」**。更糟的是 `adopt()` 原来按 `canBecomeMain` 认主窗，第一个恰好是它，
    /// 主窗的 autosave 名和位置就都记到了这个空壳上（实测：启动后**只剩**一个 420×420
    /// 的空详情窗，主界面根本不存在）。
    /// 关掉 + `isRestorable = false`（见 `NonRestorableWindow`）两头堵。
    private static func closeRestoredAuxiliaryWindows() {
        for window in NSApplication.shared.windows where isAuxiliary(window) {
            window.close()
        }
    }

    /// 把窗口摆到当前屏**可用区**的右下角（已扣除菜单栏与程序坞）。
    /// 与 Qt 的 `restoreWindowPos` 在「没有历史位置」时的落点一致。
    private static func placeBottomRight(_ window: NSWindow) {
        guard let visible = (window.screen ?? NSScreen.main)?.visibleFrame else { return }
        let size = window.frame.size
        window.setFrameOrigin(CGPoint(x: visible.maxX - size.width,
                                      y: visible.minY))
    }

    /// 主窗此刻是不是看得见。点 Dock 图标该不该把它拉回来，看的是这个。
    static var mainWindowIsVisible: Bool { mainWindow?.isVisible ?? false }

    /// 自家窗口里还有没有一个**正被人看着**的（主窗，或更新/详情/连接三个附属窗）。
    ///
    /// 用来决定「只喂界面的那些计算还要不要继续做」：点 ✕ 是 `orderOut`（收进托盘）而不是
    /// 销毁，视图树仍然活着 —— 于是每秒的流量/连接/节点更新照旧把整棵隐藏的树重算一遍，
    /// 算完没有任何人看。实测托盘态下 `NSHostingView.layout()` 仍占 1.1%，外加一堆
    /// AttributeGraph 的更新。
    ///
    /// 附属窗要算进来：主窗收起来了、连接窗还开着的时候，那份数据仍然有人在看。
    ///
    /// **认领到主窗之前一律算「可见」** —— 宁可多算一秒，也不能因为判定不出来就把界面
    /// 数据停在那儿（那会变成「打开窗口一片空白」这种没法排查的毛病）。
    static var anyWindowVisible: Bool {
        guard mainWindow != nil else { return true }
        return NSApplication.shared.windows.contains { window in
            window.isVisible && !window.isMiniaturized
                && (window === mainWindow || isAuxiliary(window))
        }
    }

    /// 兜底：主窗一个都没有时，把它**开出来**。
    ///
    /// ★ 实测启动后可能一个主窗都没有（只剩一个空白的「设备详情」）。这时托盘的
    ///   「控制面板」和点 Dock 图标都救不回来 —— 它们走的是
    ///   `mainWindow?.makeKeyAndOrderFront`，而 `mainWindow` 是 nil，等于什么都没做，
    ///   **界面再也打不开**，只剩一个够不着的托盘进程。
    ///
    ///   `openWindow(id:)` 只在 View 里拿得到，而这里恰恰一个 View 都没有。所以走
    ///   AppKit 侧唯一的入口：主窗改成 `Window(id:)` 之后，SwiftUI 会在「窗口」菜单里
    ///   放一条打开它的菜单项（`Window` scene 才有，`WindowGroup` 没有 —— 这也是把主窗
    ///   从 `WindowGroup` 换成 `Window` 的原因之一，顺带也就没有「开出第二个主窗」的问题了）。
    private static func openMainWindow() {
        guard !openingMainWindow else { return }
        guard let item = mainWindowMenuItem(), let action = item.action else { return }
        openingMainWindow = true
        NSApplication.shared.sendAction(action, to: item.target, from: item)
        // 窗口下一拍才建出来，再认领一次（autosave 名、✕ 拦截器都还没挂上）。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            openingMainWindow = false
            adopt()
        }
    }

    /// 「窗口」菜单里那条打开主窗的项。标题就是 scene 的标题（`Window("Coast", id:)`），
    /// 不随语言变 —— 这也是主窗标题**不**用 `.t` 的原因。
    private static func mainWindowMenuItem() -> NSMenuItem? {
        guard let menu = NSApplication.shared.windowsMenu else { return nil }
        return menu.items.first { $0.title == "Coast" && $0.action != nil }
    }

    private static var openingMainWindow = false

    /// 把主窗拉回前台。窗口只是被隐藏过，所以这里总能拿到它。
    static func showMainWindow() {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        // 隐藏过的窗口直接拉回来；一个都没有（见 `openMainWindow` 的说明）就现开一个。
        if mainWindow == nil { adopt() }
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
