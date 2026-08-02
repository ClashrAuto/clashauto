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

    /// 菜单栏那两行速率的当前值（紧凑格式）。核心没跑时不画。
    private var upText = "0 B/s"
    private var downText = "0 B/s"
    private var coreRunningForDraw = false

    public init() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        buildMenu()
        // 显式钉住可见性 + 固定 autosaveName。`NSStatusItem.visible` 会按 autosaveName
        // 持久化进 defaults，历史上写进去的 NO 会让整项在菜单栏里根本不出现
        // （Qt 版 `MacSpeedItem.mm` 踩过这个坑，注释在那儿）。
        statusItem.autosaveName = "CoastTray"
        statusItem.isVisible = true
        statusItem.button?.toolTip = "Coast"
        statusItem.menu = menu
        redraw()
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
        // ★ 菜单栏用**紧凑**格式（B 不带小数、其余一位），与 Qt 的 `speedTextCompact`
        //   同一套；菜单里的 UP/DOWN 两行取的也是这同一个串（Qt `macTraySetSpeed`）。
        //   原来这里用的是 `Formatting.rate`（两位小数），比 Qt 长一截。
        let newUp = Formatting.compactRate(up)
        let newDown = Formatting.compactRate(down)
        guard newUp != upText || newDown != downText else { return }
        upText = newUp
        downText = newDown
        upItem.title = "UP: \(newUp)"
        downItem.title = "DOWN: \(newDown)"
        redraw()
    }

    public func updateStatus(coreRunning: Bool, proxyEnabled: Bool, tunEnabled: Bool) {
        if renderedCoreRunning != coreRunning {
            renderedCoreRunning = coreRunning
            coreRunningForDraw = coreRunning
            coreItem.title = coreRunning ? "停止核心".t : "启动核心".t
            statusItem.button?.toolTip = "Coast - \(coreRunning ? "运行中".t : "已停止".t)"
            // 核心开/停要切换「只有图标 ↔ 图标 + 两行速率」两种布局
            redraw()
        }
        if renderedProxyEnabled != proxyEnabled {
            renderedProxyEnabled = proxyEnabled
            proxyItem.title = proxyEnabled ? "关闭网页代理".t : "打开网页代理".t
            proxyItem.state = proxyEnabled ? .on : .off
            redraw()        // 角标字母跟着变（W）
        }
        if renderedTunEnabled != tunEnabled {
            renderedTunEnabled = tunEnabled
            tunItem.title = tunEnabled ? "关闭增强模式".t : "打开增强模式".t
            tunItem.state = tunEnabled ? .on : .off
            redraw()        // 角标字母跟着变（T）
        }
    }

    // MARK: - 菜单栏绘制

    /// 把「图标 + 两行速率」画进一张**恰好等于菜单栏厚度**的图，逐点对齐 Qt 的
    /// `MacSpeedItem.mm::redrawTray()`：
    ///
    ///   • 核心没跑 → 只画图标，状态项宽度收到 `iconSide + 4`；
    ///   • 核心在跑 → 图标在最左（边长 = 厚度 - 3，上下留边），间隔 2pt，
    ///     再一块**定宽右对齐**的两行文字（上行上传、下行下载，没有 ↑↓ 标识）。
    ///
    /// 定宽是关键：宽度按最宽模板值 `888.8 MB/s` 算死，数字长短只在这块区域内右对齐，
    /// 不会把图标推来推去 —— 否则菜单栏里的图标每秒抖一下。
    ///
    /// 手绘图不是 template（图标是彩色的），所以文字颜色得自己按菜单栏明暗取。
    private func redraw() {
        guard let button = statusItem.button else { return }
        let dark = button.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
        let thickness = NSStatusBar.system.thickness
        let drawn = Self.trayImage(thickness: thickness,
                                   coreRunning: coreRunningForDraw,
                                   up: upText, down: downText,
                                   icon: globeIcon(side: floor(thickness) - 3, dark: dark),
                                   dark: dark)
        button.image = drawn.image
        button.imagePosition = .imageOnly
        statusItem.length = drawn.width
    }

    /// 角标字母。优先级与侧栏 logo 上那颗 `StatusBadge` 一致：
    /// 增强 T > 网页 W > 核心开 C > 核心关 N。
    private var badgeLetter: String {
        if renderedTunEnabled == true { return "T" }
        if renderedProxyEnabled == true { return "W" }
        return coreRunningForDraw ? "C" : "N"
    }

    /// 上一次画好的地球（按边长/角标/明暗缓存）。`redraw()` 每秒都会被流量刷一次，
    /// 而这张图只在这三样变了时才需要重画。
    private var cachedGlobe: (side: CGFloat, badge: String, dark: Bool, image: NSImage?)?

    private func globeIcon(side: CGFloat, dark: Bool) -> NSImage? {
        let badge = badgeLetter
        if let c = cachedGlobe, c.side == side, c.badge == badge, c.dark == dark { return c.image }
        let image = Self.globeImage(side: side, badge: badge, dark: dark)
        cachedGlobe = (side, badge, dark, image)
        return image
    }

    /// 托盘图标：**一个地球字形 + 右下角状态角标**，对齐 Qt 的
    /// `TrayController.cpp::renderTrayGlobe()`（mac 那一路）。
    ///
    /// 以前这里画的是 `NSApp.applicationIconImage` —— 整个 app 图标缩到 19pt 塞进菜单栏，
    /// 细节糊成一团，也看不出核心/代理/增强开着没有。Qt 版早就不这么干了。
    ///
    /// 逐项照抄 Qt：字形 U+E600（`iconfont.ttf`，与侧栏那颗 logo 同一个）、字号 = 边长 ×0.94、
    /// **按墨迹居中**（不是按字体度量盒 —— 这个字形的墨迹在盒内偏右约 2%，按盒居中会明显偏）；
    /// 角标是边长一半的圆角方块压在右下，圆角 = 角标边长 ×0.28，字号 ×0.66、加粗
    /// （托盘只有十几 pt，不加粗根本认不出字母）。
    ///
    /// 配色上没照抄 Qt，理由见函数体里那段。
    ///
    /// 字体没注册（`IconFont.registerAll()` 没跑过）时返回 nil，调用方会退回兜底灰块。
    static func globeImage(side: CGFloat, badge: String, dark: Bool) -> NSImage? {
        guard side > 0, let glyphFont = NSFont(name: "iconfont", size: side * 0.94) else { return nil }
        let accent = NSColor(srgbRed: 0x48 / 255, green: 0x98 / 255, blue: 0xF8 / 255, alpha: 1)
        // 地球跟着菜单栏明暗走；角标**恒为品牌蓝底白字**。
        //
        // ★ 这一处没照抄 Qt。Qt 那边 mac 是白地球 + **白**角标 + 蓝字母：放在浅色菜单栏上
        //   白的部分整个看不见；就算在深色栏上，白角标压在白地球上也糊成一坨白，
        //   看不出那是个角标（渲染出来像地球被咬了一口）。蓝底白字在两种菜单栏、
        //   压在黑地球或白地球上都拎得出来。
        let inkColor: NSColor = dark ? .white : .black
        let badgeFill = accent
        let badgeInk = NSColor.white

        return NSImage(size: NSSize(width: side, height: side), flipped: false) { _ in
            let glyph = NSAttributedString(string: "\u{E600}",
                                           attributes: [.font: glyphFont, .foregroundColor: inkColor])
            // `.usesDeviceMetrics` 拿到的是**墨迹**盒（不是行盒），照它居中。
            let ink = glyph.boundingRect(with: .zero, options: [.usesDeviceMetrics])
            glyph.draw(at: NSPoint(x: (side - ink.width) / 2 - ink.minX,
                                   y: (side - ink.height) / 2 - ink.minY))

            guard !badge.isEmpty else { return true }
            let bs = side * 0.5
            let box = NSRect(x: side - bs, y: 0, width: bs, height: bs)   // 右下角
            badgeFill.set()
            NSBezierPath(roundedRect: box, xRadius: bs * 0.28, yRadius: bs * 0.28).fill()

            let letter = NSAttributedString(string: badge, attributes: [
                .font: NSFont.systemFont(ofSize: bs * 0.66, weight: .bold),
                .foregroundColor: badgeInk,
            ])
            let ls = letter.size()
            letter.draw(at: NSPoint(x: box.midX - ls.width / 2, y: box.midY - ls.height / 2))
            return true
        }
    }

    /// 纯函数版，好让测试量得到宽度（`NSStatusItem` 在测试进程里造不出来）。
    /// 返回画好的图和状态项应有的宽度。
    static func trayImage(thickness: CGFloat, coreRunning: Bool,
                          up: String, down: String,
                          icon: NSImage?, dark: Bool) -> (image: NSImage, width: CGFloat) {
        let iconSide = floor(thickness) - 3

        let drawIcon: () -> Void = {
            let rect = NSRect(x: 2, y: (thickness - iconSide) / 2, width: iconSide, height: iconSide)
            if let icon, icon.size.width > 0 {
                icon.draw(in: rect)
            } else {
                // 兜底灰块：这一项永远可见，绝不透明消失。
                NSColor.systemGray.set()
                NSBezierPath(roundedRect: rect, xRadius: 3, yRadius: 3).fill()
            }
        }

        guard coreRunning else {
            let width = iconSide + 4
            let image = NSImage(size: NSSize(width: width, height: thickness),
                                flipped: false) { _ in drawIcon(); return true }
            return (image, width)
        }

        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = .right
        let attributes: [NSAttributedString.Key: Any] = [
            // 8.5pt：两行自然行高约 20pt，塞进 ~22pt 的菜单栏不裁切。
            .font: NSFont.menuBarFont(ofSize: 8.5),
            .paragraphStyle: paragraph,
            .foregroundColor: dark ? NSColor.white : NSColor.black,
        ]
        let gap: CGFloat = 2
        let textWidth = ceil(("888.8 MB/s" as NSString).size(withAttributes: attributes).width)
        let text = NSAttributedString(string: "\(up)\n\(down)", attributes: attributes)
        let total = iconSide + gap + textWidth + 4

        let image = NSImage(size: NSSize(width: total, height: thickness), flipped: false) { _ in
            drawIcon()
            let rect = NSRect(x: iconSide + gap, y: (thickness - text.size().height) / 2,
                              width: textWidth, height: text.size().height)
            text.draw(in: rect)
            return true
        }
        return (image, total)
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

    /// 菜单栏那两行用的紧凑速率："0 B/s"、"12.3 KB/s"、"1.2 MB/s"。
    /// B 不带小数、其余一位 —— 与 Qt `TrayController::speedTextCompact` 逐字符相同。
    /// 单位只到 TB（菜单栏里不可能出现 PB/s）。
    public static func compactRate(_ value: Int64) -> String {
        var n = Double(max(0, value))
        let units = ["B", "KB", "MB", "GB", "TB"]
        var index = 0
        while n >= 1024, index + 1 < units.count {
            n /= 1024
            index += 1
        }
        return String(format: "%.\(index == 0 ? 0 : 1)f %@/s", n, units[index])
    }
}
