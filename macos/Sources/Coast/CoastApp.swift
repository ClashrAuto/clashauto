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
    }

    /// 点 ✕ **不退出程序**，只隐藏窗口。
    ///
    /// 这是刻意的：代理客户端常驻托盘，✕ 直接退会连带停掉核心与系统代理，用户以为只是关个窗，
    /// 结果整机断代理。真正退出走托盘菜单「退出程序」或 Cmd+Q。
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

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let state else { return .terminateNow }
        // 退出前必须停核心 + 还原系统代理 —— 否则用户退出应用后整机仍指着一个已经没人监听的
        // 端口，表现为「关了 Coast 就上不了网」。用 terminateLater 等清理跑完。
        Task { @MainActor in
            await state.shutdown()
            NSApplication.shared.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
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
        tray.onQuit = { NSApplication.shared.terminate(nil) }
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
        } else {
            // 主窗还没建好（理论上打不开这个窗，留个兜底而不是崩）。
            Color.clear
        }
    }
}
