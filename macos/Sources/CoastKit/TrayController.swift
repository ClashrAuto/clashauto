import AppKit
import Foundation
import UserNotifications

/// 菜单栏图标与菜单 —— C++ `TrayController` 的对应物，改用 `NSStatusItem`。
///
/// 菜单项与 Qt 版一一对应：控制面板 / UP·DOWN 速率 / 启停核心 / 网页代理 / 增强模式 / 退出。
@MainActor
public final class TrayController {

    public var onShowPanel: (() -> Void)?
    public var onToggleCore: (() -> Void)?
    public var onToggleProxy: (() -> Void)?
    public var onToggleTun: (() -> Void)?
    public var onQuit: (() -> Void)?

    private let statusItem: NSStatusItem
    private let menu = NSMenu()

    private let upItem = NSMenuItem(title: "UP: 0 B/s", action: nil, keyEquivalent: "")
    private let downItem = NSMenuItem(title: "DOWN: 0 B/s", action: nil, keyEquivalent: "")
    private let coreItem = NSMenuItem(title: "启动核心".t, action: nil, keyEquivalent: "")
    private let proxyItem = NSMenuItem(title: "打开网页代理".t, action: nil, keyEquivalent: "")
    private let tunItem = NSMenuItem(title: "打开增强模式".t, action: nil, keyEquivalent: "")

    /// 上一次已渲染的状态。`NSStatusItem` 的图标/标题赋值是到 window server 的往返，
    /// 每秒无脑重设会让图标肉眼可见地闪 —— 只在真的变了时才写。
    private var renderedCoreRunning: Bool?
    private var renderedProxyEnabled: Bool?
    private var renderedTunEnabled: Bool?

    public init() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        buildMenu()
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "shippingbox", accessibilityDescription: "Coast")
            button.image?.isTemplate = true   // 跟随菜单栏明暗，别自己上色
            button.toolTip = "Coast"
        }
        statusItem.menu = menu
    }

    private func buildMenu() {
        let panel = NSMenuItem(title: "控制面板".t, action: #selector(showPanel), keyEquivalent: "")
        panel.target = self
        menu.addItem(panel)
        menu.addItem(.separator())

        // 速率两行只做显示，不可点
        upItem.isEnabled = false
        downItem.isEnabled = false
        menu.addItem(upItem)
        menu.addItem(downItem)
        menu.addItem(.separator())

        coreItem.action = #selector(toggleCore); coreItem.target = self
        proxyItem.action = #selector(toggleProxy); proxyItem.target = self
        tunItem.action = #selector(toggleTun); tunItem.target = self
        menu.addItem(coreItem)
        menu.addItem(proxyItem)
        menu.addItem(tunItem)
        menu.addItem(.separator())

        let quit = NSMenuItem(title: "退出程序".t, action: #selector(quit), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
    }

    // MARK: - 更新

    public func updateTraffic(up: Int64, down: Int64) {
        upItem.title = "UP: \(Formatting.rate(up))"
        downItem.title = "DOWN: \(Formatting.rate(down))"
    }

    public func updateStatus(coreRunning: Bool, proxyEnabled: Bool, tunEnabled: Bool) {
        if renderedCoreRunning != coreRunning {
            renderedCoreRunning = coreRunning
            coreItem.title = coreRunning ? "停止核心".t : "启动核心".t
            statusItem.button?.toolTip = "Coast - \(coreRunning ? "运行中".t : "已停止".t)"
        }
        if renderedProxyEnabled != proxyEnabled {
            renderedProxyEnabled = proxyEnabled
            proxyItem.title = proxyEnabled ? "关闭网页代理".t : "打开网页代理".t
            proxyItem.state = proxyEnabled ? .on : .off
        }
        if renderedTunEnabled != tunEnabled {
            renderedTunEnabled = tunEnabled
            tunItem.title = tunEnabled ? "关闭增强模式".t : "打开增强模式".t
            tunItem.state = tunEnabled ? .on : .off
        }
    }

    /// 系统通知。首次调用会请求授权 —— 被拒绝就静默跳过，通知不是关键路径。
    public func notify(title: String, message: String) {
        Notifier.post(title: title, body: message)
    }

    // MARK: - 动作

    @objc private func showPanel() { onShowPanel?() }
    @objc private func toggleCore() { onToggleCore?() }
    @objc private func toggleProxy() { onToggleProxy?() }
    @objc private func toggleTun() { onToggleTun?() }
    @objc private func quit() { onQuit?() }
}

/// 人类可读的字节/速率。对齐 `qml/Theme.qml` 的 `fmtBytes` / `fmtRate`，
/// 好让菜单栏与界面里的数字口径一致。
public enum Formatting {
    public static func bytes(_ value: Int64) -> String {
        var n = Double(value)
        if n < 1024 { return String(format: "%.0f B", n) }
        let units = ["KB", "MB", "GB", "TB", "PB"]
        var index = -1
        repeat {
            n /= 1024
            index += 1
        } while n >= 1024 && index < units.count - 1
        return String(format: "%.2f %@", n, units[index])
    }

    public static func rate(_ value: Int64) -> String { bytes(value) + "/s" }
}
