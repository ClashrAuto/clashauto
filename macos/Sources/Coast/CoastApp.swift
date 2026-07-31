import AppKit
import CoastKit
import SwiftUI

@main
struct CoastApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    init() {
        SelfTests.runIfRequested()
    }

    var body: some Scene {
        WindowGroup("Coast") {
            RootView()
                .frame(minWidth: 640, minHeight: 430)
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
        }
        .defaultSize(width: 600, height: 560)
        .keyboardShortcut(nil)
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
            // mac 上整窗毛玻璃，内容卡浮在上面 —— 与 Qt 版 applyMacGlass 的观感一致。
            .background(.ultraThinMaterial)
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

    /// 点 Dock 图标重新打开主窗（窗口被 ✕ 隐藏后唯一的回来方式之一）。
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag { showMainWindow() }
        return true
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

    private func showMainWindow() {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        NSApplication.shared.windows.first?.makeKeyAndOrderFront(nil)
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
