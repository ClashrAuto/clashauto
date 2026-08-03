import AppKit
import CoastKit
import SwiftUI

@main
struct CoastApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    init() {
        SelfTests.runIfRequested()
        IconFont.registerAll()
    }

    var body: some Scene {
        Window("Coast", id: MainWindowID.value) {
            RootView()
                .frame(minWidth: 640, minHeight: 430)
                .windowMinSize(width: 640, height: 430)      // Qt: Main.qml 的 minimumWidth/Height
                // 整窗毛玻璃。用 NSVisualEffectView 而不是 `.ultraThinMaterial` ——
                // 后者采样不到窗口后面的桌面，深色主题下就是一块灰底。
                .windowGlass(.sidebar)
        }
        .defaultSize(width: 900, height: 510)
        .windowStyle(.hiddenTitleBar)
        .commands {
            // 默认的「新建窗口」对常驻托盘的单窗应用没有意义，去掉免得用户开出第二个。
            CommandGroup(replacing: .newItem) {}
            // ⌘Q **保留系统默认的「退出 Coast」**。
            //
            // ★ 这里原来把整个 `.appTermination` 组换成了一颗「关闭窗口」——
            //   于是菜单里**根本没有退出这一项**，⌘Q 绑的是收窗口。出发点是
            //   「代理客户端退出 = 整机断代理，⌘Q 太容易顺手按到」，但代价太大：
            //   mac 上 ⌘Q 就是「退出这个程序」，改掉它等于用户唯一的退出手势失灵，
            //   而且**程序自更新会卡住** —— 替换脚本在外面等本进程退出，等不到就永远装不上。
            //   退出时的清理（停核心 + 还原系统代理）本来就在 `applicationShouldTerminate`
            //   里做，所以「退了网就断」并不成立，那正是清理要解决的事。
            //
            //   ✕ 仍然只收窗口不退出（见 `WindowRestore.CloseGuard`）——
            //   那才是真正容易误点的那个。
        }

        // 更新窗是**独立顶层窗**，不是 sheet —— 与 Qt 一致，而且这里有个硬理由：
        // 它 600×560，比主窗默认的 900×**510** 还高。做成 sheet 的话底部那行
        // 「国内代理下载 / 关闭 / 更新」会被主窗边界裁掉，**按钮根本点不到**
        // （截图里就是这样：整条动作行不见了）。独立窗口不受主窗尺寸约束。
        Window("Coast 更新".t, id: UpdateWindowID.value) {
            UpdateWindowRoot()
                .windowMinSize(width: 460, height: 420)      // Qt: UpdateWindow.qml
                .nonRestorableWindow()
        }
        .defaultSize(width: 600, height: 560)
        .keyboardShortcut(nil)
        // 顶栏（版本号 + 三段玻璃组）钉在标题栏那条带子里，所以系统标题栏必须让位 ——
        // 与连接窗同一处理：透明 + 内容铺满整窗（fullSizeContentView）+ 不画标题文字。
        // 带子的**高度**由 `windowGlass(unifiesTitleBar:)` 的空 toolbar 抬到 50。
        .windowStyle(.hiddenTitleBar)

        // 设备详情同理：Qt 是 600×720 的独立窗（最小 420×420），比主窗默认的 510 高得多，
        // 做成 sheet 一样会被裁。显示的永远是 `AppState.selectedDevice`（与 Qt 的
        // `devices.selectedDevice` 同义）：窗口开着时点列表里另一台，内容跟着换。
        Window("设备详情".t, id: DeviceDetailWindowID.value) {
            DeviceDetailWindowRoot()
                .windowMinSize(width: 420, height: 420)      // Qt: DeviceDetailWindow.qml
                .nonRestorableWindow()
        }
        .defaultSize(width: 600, height: 720)

        // 连接窗同理：Qt 是 720×480 的独立窗（最小 480×320）。做成 sheet 的话，
        // 主窗被拖到最小宽（640）时它**横向溢出**—— 实测左边的「Online (0)」直接被切掉半截。
        Window("连接".t, id: ConnectionsWindowID.value) {
            ConnectionsWindowRoot()
                .windowMinSize(width: 480, height: 320)      // Qt: ConnectionsWindow.qml
                .nonRestorableWindow()
        }
        .defaultSize(width: 720, height: 480)
        // 顶栏（Online/Offline + 搜索）钉在标题栏那条带子里，所以系统标题栏必须让位：
        // 这一句给的是「透明 + 内容铺满整窗（fullSizeContentView）+ 不画标题文字」。
        // ★ 从 AppKit 侧补 `fullSizeContentView` **顶不上**它 —— 试过，窗口顶部照样是
        //   一条不透明白带、顶栏被整个盖住。带子的**高度**则由 `unifiedTitleBar()` 抬
        //   （见那边）。红绿灯与拖动都还在，只是和顶栏并成了一条。
        .windowStyle(.hiddenTitleBar)
    }
}

private struct RootView: View {
    @State private var state = AppState()

    @State private var i18n = I18n.shared

    var body: some View {
        MainView()
            .environment(state)
            .environment(state.theme)
            // 切换语言时整体重建视图树 —— 等价于 QML 的 retranslate()。
            // `.t` 走的是全局单例，SwiftUI 看不见它变了，必须靠这个 id 触发重建。
            .id(i18n.language)
            // 毛玻璃只有 `.windowGlass(.sidebar)` 那一层（系统 NSVisualEffectView，
            // 窗口服务器直接采样桌面）。原来这里还垫着一层 `.ultraThinMaterial` ——
            // 它采样不到窗后内容，叠上去等于给真玻璃再蒙一层灰纱，越看越像假的。
            .task {
                I18n.shared.applyConfig(state.config)
                AppState.sharedForWindows = state
                AppDelegate.shared?.attach(state: state)
                state.start()
            }
    }
}

// 整个类标 @MainActor：它碰的东西（NSApplication、NSStatusItem、AppState）全是主线程独占的，
// 逐个方法标反而更啰嗦，也容易漏掉一个。
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    static private(set) var shared: AppDelegate?

    private var tray: TrayController?
    private var state: AppState?
    /// 「点空白让输入框失焦」的鼠标监听。持有着，进程活多久它挂多久。
    private var defocusMonitor: Any?

    override init() {
        super.init()
        AppDelegate.shared = self
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // SPM 直接跑的可执行文件默认是 .prohibited（无 Dock 图标、窗口开不出来）；
        // 打包成 .app 后 Info.plist 接管，但开发期 `swift run` 必须显式提一次。
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        // 窗口这时候可能还没建出来（WindowGroup 是在第一轮 runloop 之后才铺的）。
        DispatchQueue.main.async { WindowRestore.adopt() }

        // 退出自检（`COAST_QUIT_SELFTEST=1`）：起来 3 秒后自己走一遍退出流程。
        // 外面量「进程多久真的没了」就能验两件事：⌘Q/托盘/自更新那条路**没有被拦下**，
        // 以及清理卡住时看门狗兜得住。这条路没法用单测覆盖 —— 它要的正是一个真的
        // NSApplication 生命周期；而它坏掉的表现（退不掉、更新装不上）又足够重。
        if ProcessInfo.processInfo.environment["COAST_QUIT_SELFTEST"] == "1" {
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
                // 顺带验菜单里**有**⌘Q 那一项：这个 bug 的另一半正是它被整组换掉了
                // （`CommandGroup(replacing: .appTermination)`），于是 ⌘Q 绑的是收窗口。
                var quit: NSMenuItem?
                for top in NSApplication.shared.mainMenu?.items ?? [] {
                    for item in top.submenu?.items ?? [] where item.keyEquivalent == "q" {
                        if item.keyEquivalentModifierMask == .command { quit = item }
                    }
                }
                let title = quit?.title ?? "<none>"
                let action = quit?.action.map { NSStringFromSelector($0) } ?? "-"
                print("QUIT-SELFTEST menuItem=\(title) action=\(action)")
                self?.terminateForReal()
            }
        }

        // 点输入框以外的任何地方都让输入框**失去焦点**（对所有窗口生效）。
        // AppKit 的默认行为是焦点一直留在框里，点空白毫无反应 —— 桌面用户的
        // 预期是「点别处 = 收起编辑」。实现：正在编辑时第一响应者是窗口的
        // field editor（isFieldEditor 的 NSTextView），mouseDown 落点不在它
        // 里面就收回第一响应者；事件照常放行 —— 点到的是另一个输入框/按钮时，
        // AppKit 随后自会把焦点交给它，不受影响。
        defocusMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown]) { event in
            if let window = event.window,
               let editor = window.firstResponder as? NSTextView,
               editor.isFieldEditor {
                let location = editor.convert(event.locationInWindow, from: nil)
                if !editor.bounds.contains(location) {
                    window.makeFirstResponder(nil)
                }
            }
            return event
        }
    }

    /// 点 ✕ **不退出程序**，只隐藏窗口。
    ///
    /// 这是刻意的：代理客户端常驻托盘，✕ 直接退会连带停掉核心与系统代理，用户以为只是关个窗，
    /// 结果整机断代理。**⌘Q 不在此列** —— 它是明确的「退出这个程序」，见 scene 的 commands。
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    /// 点 Dock 图标重新打开主窗（窗口被 ✕ 隐藏后回来的方式之一；另一条是托盘的「控制面板」）。
    ///
    /// **返回 false**：这一位的语义是「要不要让 AppKit 再执行它的默认行为」。
    /// 返回 true 的话，SwiftUI 的 `WindowGroup` 会**再建一个**主窗 ——
    /// 实测重开之后屏幕上是两个 Coast 窗，一个在原位、一个在左上角。
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        // ★ 判据是**主窗**可不可见，不是「有没有窗」。原来看 `flag`（AppKit 传的
        //   hasVisibleWindows）：详情窗或连接窗开着时它就是 true，于是点 Dock 图标
        //   什么也不发生 —— 主窗明明是隐藏着的，用户却以为点坏了。
        if !WindowRestore.mainWindowIsVisible { showMainWindow() }
        return false
    }

    /// 退出入口（托盘菜单「退出程序」、程序自更新那条“必须退出”的路）。
    /// 现在与 ⌘Q 走同一条路 —— 留着这个名字是因为调用点写着它，读起来也更清楚。
    func terminateForReal() {
        NSApplication.shared.terminate(nil)
    }

    /// 清理的兜底时限：到点还没跑完就**硬退**。
    ///
    /// 8 秒是按最坏情况给的：清理里有几次 XPC（撤销设备接管、还原系统代理、停提权核心），
    /// helper 不在/没响应时它们各自要等到自己的超时（最长 15 秒）。正常路径（helper 在、
    /// XPC 是本机调用）是毫秒级，走不到这里。
    private static let shutdownDeadline: TimeInterval = 8

    private enum Shutdown { case idle, running, done }
    private var shutdown: Shutdown = .idle

    /// ★ **不能用 `.terminateLater`。**
    ///
    /// 看着最对的写法是「返回 `.terminateLater`，清理跑完再 `reply(toApplicationShouldTerminate:)`」。
    /// 实测是**死等**：`-[NSApplication terminate:]` 会在 `_shouldTerminate` 里跑一个**嵌套事件
    /// 循环**等这个回复，而那个循环不派发主队列的活 —— 清理的 `Task { @MainActor }` 排不上，
    /// 连「到点强制放行」的 `DispatchQueue.main.asyncAfter` 看门狗也排不上，于是回复永远不会来。
    /// 采样抓到的主线程栈就停在 `terminate: → _shouldTerminate → nextEventMatchingMask`，
    /// 进程 5 分钟都没退。**这就是「点了退出退不掉」的根因**：⌘Q、托盘「退出程序」、
    /// 自更新那条「必须退出」的路，全都堵在这儿（自更新的表现是永远卡在「正在退出安装…」，
    /// 替换脚本在外面等一个不会退出的进程）。
    ///
    /// 改成「先 `.terminateCancel` 放掉那个嵌套循环 → 回到正常 runloop 跑清理 → 清理完再
    /// `terminate` 一次」，第二次进来直接 `.terminateNow`。清理卡住时看门狗 `exit(0)` 兜底 ——
    /// 这时主队列是活的，它跑得起来。
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        switch shutdown {
        case .done:
            return .terminateNow
        case .running:
            // 清理在跑，用户又点了一次/托盘又发了一次：别叠第二遍清理，等它自己走完。
            return .terminateCancel
        case .idle:
            guard let state else { return .terminateNow }
            shutdown = .running
            // 退出前停核心 + 还原系统代理 —— 否则用户退出应用后整机仍指着一个已经没人监听的
            // 端口，表现为「关了 Coast 就上不了网」。
            Task { @MainActor in
                await state.shutdown()
                self.shutdown = .done
                NSApplication.shared.terminate(nil)
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + Self.shutdownDeadline) { [weak self] in
                guard self?.shutdown != .done else { return }
                // 清理没在时限内回来（多半卡在某次 XPC 上）。继续等下去就是「退不掉」，
                // 那比少做一步清理糟得多 —— 硬退。
                exit(0)
            }
            return .terminateCancel
        }
    }

    func attach(state: AppState) {
        guard self.state == nil else { return }
        self.state = state
        setupTray(state: state)

        // 「启动静默到托盘」(config.mini):启动即隐藏窗口,只留托盘/Dock。
        // 之后经托盘「控制面板」或点 Dock 图标重新打开(applicationShouldHandleReopen)。
        // 延一拍再隐藏:WindowGroup 的窗口此刻可能还没建出来。
        if state.config.closeToTray {
            DispatchQueue.main.async {
                NSApplication.shared.windows.forEach { $0.orderOut(nil) }
            }
        }
    }

    private func setupTray(state: AppState) {
        let tray = TrayController()
        tray.onShowPanel = { [weak self] in self?.showMainWindow() }
        tray.onToggleCore = { state.toggleCore() }
        tray.onToggleProxy = { state.toggleProxy() }
        tray.onToggleTun = { state.toggleTun() }
        tray.onQuit = { [weak self] in self?.terminateForReal() }
        self.tray = tray

        // 每秒把流量与状态推给托盘。TrayController 内部只在真的变了时才写，不会让图标闪。
        Task { @MainActor in
            while !Task.isCancelled {
                tray.updateTraffic(up: state.clash.up, down: state.clash.down)
                tray.updateStatus(coreRunning: state.controller.isCoreRunning,
                                  proxyEnabled: state.controller.isProxyEnabled,
                                  tunEnabled: state.controller.isTunEnabled)
                try? await Task.sleep(for: .seconds(1))
            }
        }
    }

    /// 拉回主窗。委托给 `WindowRestore` —— 它记着**哪一个**才是主窗；
    /// 原来这里写的是 `windows.first`，那可能是状态栏窗口或更新窗，order front 等于没做。
    private func showMainWindow() { WindowRestore.showMainWindow() }
}


/// 三个附属窗都要跟着**应用主题**走明暗，而不是跟系统外观。
///
/// ★ 少了这一句就是「主窗黑、附属窗白」：`preferredColorScheme` 原来只加在 `MainView`
///   上，附属窗于是沿用系统外观 —— 应用主题设成深色而 macOS 是浅色时（autoTheme 关掉
///   手选深色就是这个组合），系统材质（玻璃、`.regularMaterial`、`.primary` 文字）
///   全按浅色渲染，整窗一片白。Qt 那边不会有这问题：它每个颜色都是自己画的。
private extension View {
    func followsAppTheme(_ theme: Theme) -> some View {
        preferredColorScheme(theme.dark ? .dark : .light)
    }
}

/// 连接窗的 scene id。
enum MainWindowID { static let value = "coast.main" }

enum ConnectionsWindowID { static let value = "coast.connections" }

/// 连接窗的根视图。**不能用 `@State` 接** `sharedForWindows`，理由见 `UpdateWindowRoot`。
private struct ConnectionsWindowRoot: View {
    var body: some View {
        if let state = AppState.sharedForWindows {
            ConnectionsView()
                .environment(state)
                .environment(state.theme)
                .followsAppTheme(state.theme)
        } else {
            Color.clear
        }
    }
}

/// 设备详情窗的 scene id。
enum DeviceDetailWindowID { static let value = "coast.device-detail" }

/// 设备详情窗的根视图。**不能用 `@State` 接** `sharedForWindows`，理由见 `UpdateWindowRoot`。
private struct DeviceDetailWindowRoot: View {
    var body: some View {
        if let state = AppState.sharedForWindows {
            DeviceDetailView()
                .environment(state)
                .environment(state.theme)
                .followsAppTheme(state.theme)
        } else {
            Color.clear
        }
    }
}

/// 更新窗的 scene id。写成常量而不是散在两处的字面量 —— 打开与声明用的必须是同一个串。
enum UpdateWindowID { static let value = "coast.update" }

/// 更新窗的根视图：自己拿 `AppState`（`Window` scene 不在主窗的 environment 链里）。
///
/// ★ **不能用 `@State` 接** —— `@State` 的初值只算一次，而 SwiftUI 可能在主窗
/// 建好（也就是 `sharedForWindows` 被赋值）**之前**就先求值一次这个 scene 的根，
/// 于是它永远抱着那个 nil，窗口开出来是**全空的**（截图里就是一整块空白）。
/// 每次 body 求值时现读那个静态量才对。
private struct UpdateWindowRoot: View {
    var body: some View {
        if let state = AppState.sharedForWindows {
            UpdateView()
                .environment(state)
                .environment(state.theme)
                .followsAppTheme(state.theme)
        } else {
            // 主窗还没建好（理论上打不开这个窗，留个兜底而不是崩）。
            Color.clear
        }
    }
}
