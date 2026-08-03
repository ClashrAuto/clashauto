import AppKit
import SwiftUI

/// 真正的窗口毛玻璃。
///
/// SwiftUI 的 `.background(.ultraThinMaterial)` 在**窗口背景**这一层不等价于系统材质：
/// 它只在自己的图层里做模糊，采样不到窗口后面的桌面，于是深色主题下看起来就是一块灰底。
/// 要露出后面的内容必须用 `NSVisualEffectView` —— 它由窗口服务器直接合成。
///
/// Qt 那边的 `Theme.shell` 注释写着「mac 上透明露玻璃」，说的就是这个效果；
/// 侧栏与页脚透玻璃、中间内容卡是实底，两层对比正是那个界面的观感来源。
struct VisualEffectBackground: NSViewRepresentable {
    var material: NSVisualEffectView.Material = .sidebar
    var blending: NSVisualEffectView.BlendingMode = .behindWindow
    /// 即使窗口失焦也保持材质。默认 `.followsWindowActiveState` 会在切走时整块变灰，
    /// 而这个窗口常驻托盘、大多数时间并不在前台 —— 跟着失活的话它平时就是一块死灰。
    var isEmphasized = false

    func makeNSView(context: Context) -> NSVisualEffectView {
        let view = NSVisualEffectView()
        view.material = material
        view.blendingMode = blending
        view.state = .active          // 恒定生效，不跟随窗口激活状态
        view.isEmphasized = isEmphasized
        return view
    }

    func updateNSView(_ view: NSVisualEffectView, context: Context) {
        view.material = material
        view.blendingMode = blending
        view.state = .active
        view.isEmphasized = isEmphasized
    }
}

/// 把承载它的 `NSWindow` 设成非不透明 + 透明底。
///
/// ★ 这一步不做的话，前面那层 `NSVisualEffectView(.behindWindow)` **等于白做**：
///   窗口本身不透明时，窗口服务器根本不会把它后面的内容合成进来，材质只能退化成
///   一块和背景色差不多的灰。实测：不设的话把窗口挪到屏幕两端，侧栏像素只从
///   #3C4045 变到 #3B3D3C —— 几乎不动，那不是玻璃，是灰底。
struct WindowConfigurator: NSViewRepresentable {
    /// 是否把标题栏抬到与页面顶栏同高（挂空 toolbar）。主窗和连接窗都要 ——
    /// 它们的顶栏就钉在那条带子里。不需要的窗传 false，保持标准 28。
    var unifiesTitleBar = true
    /// 见 `windowGlass(_:unifiesTitleBar:movableByBackground:)`。
    var movableByBackground = false

    final class Coordinator {
        var tokens: [NSObjectProtocol] = []
        deinit { tokens.forEach(NotificationCenter.default.removeObserver) }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async { configure(view.window, context.coordinator) }
        return view
    }

    func updateNSView(_ view: NSView, context: Context) {
        DispatchQueue.main.async { configure(view.window, context.coordinator) }
    }

    private func configure(_ window: NSWindow?, _ coordinator: Coordinator) {
        guard let window else { return }
        window.isOpaque = false
        window.backgroundColor = .clear
        if movableByBackground {
            window.isMovableByWindowBackground = true
        }
        // 阴影要保留：非不透明窗口若不显式开启，macOS 会连投影一起去掉，
        // 窗口边缘会糊在桌面上，看不出这是一个独立的窗。
        window.hasShadow = true

        if #available(macOS 26.0, *) {
            // 26：标题栏加高到与页面顶栏同高。做法是挂一个**空的** NSToolbar ——
            // 这是把标题栏抬到统一工具栏高度、让红绿灯在其中垂直居中的受支持写法；
            // transparent 保证不画任何工具栏底。
            window.titlebarAppearsTransparent = true
            guard unifiesTitleBar else { return }
            if window.toolbar == nil {
                let toolbar = NSToolbar(identifier: "coast.titlebar.spacer")
                toolbar.showsBaselineSeparator = false
                window.toolbar = toolbar
            }
            window.toolbarStyle = .unified
            // ★ 每次都按当前状态摆正，而不是只在通知里翻。通知是「转场开始/结束」的
            //   一次性事件，漏一次（或者被转场本身覆盖掉）带子就回不来了 ——
            //   实测退出全屏后标题栏停在标准 28，而我们的顶栏还是 50，红绿灯中心 14、
            //   顶栏中心 24，差 10 明显错位。`configure` 每轮布局都跑，能自愈。
            window.toolbar?.isVisible = !window.styleMask.contains(.fullScreen)
            observeFullScreen(window, coordinator)
        }
    }

    /// ★ **不收起的话全屏后窗口顶上是一条纯白的空带**。窗口模式下这个 toolbar 是空的没关系
    ///   —— 它只负责把标题栏抬到 50，我们自己的顶栏正好铺在那条带子上，谁也看不见它。
    ///   可全屏时系统把标题栏收走、却**把 toolbar 留下**并单独给它画一条不透明的底：
    ///   于是顶上多出一条白带，里面一个东西都没有（它本来就是空的），
    ///   而我们的顶栏被挤到白带下面。主窗和连接窗都中招。
    ///
    ///   收起 toolbar 之后全屏就没有这条带子了，安全区归零，顶栏直接顶到屏幕上沿 ——
    ///   也正是全屏该有的样子（那 50 本来就是为了让红绿灯有地方待，全屏没有红绿灯）。
    /// ★ 收的时机用 `willEnter`（转场前就藏掉，全屏动画里不会闪一下白带），
    ///   放的时机必须用 **`didExit`** —— `willExit` 太早，转场过程本身会把它又摁回去。
    @available(macOS 26.0, *)
    private func observeFullScreen(_ window: NSWindow, _ coordinator: Coordinator) {
        guard coordinator.tokens.isEmpty else { return }
        let center = NotificationCenter.default
        coordinator.tokens = [
            center.addObserver(forName: NSWindow.willEnterFullScreenNotification,
                               object: window, queue: .main) { note in
                MainActor.assumeIsolated { (note.object as? NSWindow)?.toolbar?.isVisible = false }
            },
            center.addObserver(forName: NSWindow.didExitFullScreenNotification,
                               object: window, queue: .main) { note in
                MainActor.assumeIsolated { (note.object as? NSWindow)?.toolbar?.isVisible = true }
            },
        ]
    }
}

/// 让**附属窗**能进全屏。
///
/// ★ SwiftUI 给次级 `Window` scene 的 collectionBehavior 是
///   `[.auxiliary, .fullScreenAuxiliary]`（实测 131328）。`fullScreenAuxiliary` 的语义是
///   「可以**跟着别的窗**一起待在全屏空间里」，**它自己进不去** —— 于是绿灯点下去只是
///   zoom（铺满可用区），永远进不了全屏。主窗那份是 `[.primary, .fullScreenPrimary]`
///   （65664），所以主窗一直好用，这个坑只在附属窗上。
///
/// ★ **设一次不够**：SwiftUI 会在窗口建好之后把它改回去。实测设完立刻读回是 131200
///   （aux 位已摘、primary 已加），6 秒后又变回 131328。所以挂上 `didUpdateNotification`
///   一直盯着 —— 那个通知在窗口活跃时每轮事件循环都来，而这里只是两次位测试，命中才写。
struct FullScreenCapableWindow: NSViewRepresentable {

    final class Coordinator {
        var token: NSObjectProtocol?
        deinit { if let token { NotificationCenter.default.removeObserver(token) } }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async { attach(view.window, context.coordinator) }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {
        DispatchQueue.main.async { attach(nsView.window, context.coordinator) }
    }

    private func attach(_ window: NSWindow?, _ coordinator: Coordinator) {
        guard let window else { return }
        Self.makePrimary(window)
        guard coordinator.token == nil else { return }
        coordinator.token = NotificationCenter.default.addObserver(
            forName: NSWindow.didUpdateNotification, object: window, queue: .main
        ) { note in
            guard let updated = note.object as? NSWindow else { return }
            MainActor.assumeIsolated { Self.makePrimary(updated) }
        }
    }

    @MainActor
    private static func makePrimary(_ window: NSWindow) {
        let behavior = window.collectionBehavior
        guard !behavior.contains(.fullScreenPrimary) || behavior.contains(.fullScreenAuxiliary)
        else { return }
        window.collectionBehavior.remove(.fullScreenAuxiliary)
        window.collectionBehavior.insert(.fullScreenPrimary)
    }
}

extension View {
    /// 让承载它的窗口能进全屏（附属窗默认不能，见 `FullScreenCapableWindow`）。
    func allowsFullScreen() -> some View {
        background(FullScreenCapableWindow().frame(width: 0, height: 0))
    }
}

extension View {
    /// 给整窗铺一层毛玻璃底（并把窗口本身设为透明，否则材质无效）。
    /// `movableByBackground`：按住窗口的**非交互空白**就能拖动整窗。
    /// 附属窗默认没有这一位（主窗是在 `WindowRestore.adopt` 里单独开的）——
    /// 而标题栏那条带子一旦被顶栏内容占满，它就成了唯一的拖动区，不开这一位窗口拖不动。
    func windowGlass(_ material: NSVisualEffectView.Material = .sidebar,
                     unifiesTitleBar: Bool = true,
                     movableByBackground: Bool = false) -> some View {
        background(VisualEffectBackground(material: material).ignoresSafeArea())
            .background(WindowConfigurator(unifiesTitleBar: unifiesTitleBar,
                                           movableByBackground: movableByBackground)
                .frame(width: 0, height: 0))
    }
}

/// 液态玻璃按钮样式（macOS 26+），旧系统自动回落。
///
/// Apple 在 macOS 26 把控件外观整体换成了 Liquid Glass；`.buttonStyle(.glass)` 是官方入口。
/// 这里包一层是因为工程的部署目标是 macOS 14 —— 直接写 `.glass` 会编译失败，
/// 而用 `#available` 分支又要在每个调用点重复一遍。
struct GlassButtonModifier: ViewModifier {
    var prominent = false

    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            // 全圆角（胶囊）。Liquid Glass 的默认形状是圆角矩形，
            // 而胶囊才是这套材质在系统各处的常见形态 —— 玻璃的高光沿着连续曲率走，
            // 直角处会把高光切断，看起来像贴了一层膜而不是一块玻璃。
            Group {
                if prominent {
                    content.buttonStyle(.glassProminent)
                } else {
                    content.buttonStyle(.glass)
                }
            }
            .buttonBorderShape(.capsule)
        } else {
            // 旧系统回落到 bordered —— 观感最接近，也不会显得突兀。
            content.buttonStyle(.bordered).buttonBorderShape(.capsule)
        }
    }
}

extension View {
    /// 液态玻璃按钮；macOS 26 以下回落 `.bordered`。
    func glassButton(prominent: Bool = false) -> some View {
        modifier(GlassButtonModifier(prominent: prominent))
    }
}

/// 一组互相融合的液态玻璃按钮（macOS 26+；旧系统回落成普通胶囊按钮）。
///
/// `GlassEffectContainer` 会让**间距小于 spacing** 的相邻玻璃互相融合、连成一片 ——
/// 这正是「一组、三选一」与「三颗各自独立的按钮」的区别所在：不靠高亮去暗示，
/// 形状本身就说明了关系。
struct GlassGroup<Content: View>: View {
    var spacing: CGFloat = 4
    @ViewBuilder var content: () -> Content

    var body: some View {
        if #available(macOS 26.0, *) {
            GlassEffectContainer(spacing: spacing) {
                HStack(spacing: spacing) { content() }
            }
        } else {
            HStack(spacing: spacing) { content() }
        }
    }
}

extension View {
    /// 给单个视图上液态玻璃（胶囊形）。`tinted` 时带主题色 —— 用来标出组里的当前项。
    @ViewBuilder
    func glassCapsule(tinted: Color? = nil) -> some View {
        if #available(macOS 26.0, *) {
            if let tinted {
                glassEffect(.regular.tint(tinted), in: .capsule)
            } else {
                glassEffect(.regular, in: .capsule)
            }
        } else {
            // 旧系统：手画一个近似的胶囊底，形状一致、只是没有玻璃折射。
            background(Capsule().fill(tinted ?? Color.gray.opacity(0.25)))
        }
    }
}

extension View {
    /// 26 上用系统默认前景色（玻璃按钮的配色交给系统）；26 以下沿用传入的
    /// 旧配色（Qt 对齐的那套灰/蓝）。
    @ViewBuilder
    func legacyTint(_ color: Color) -> some View {
        if #available(macOS 26.0, *) {
            self
        } else {
            foregroundStyle(color)
        }
    }

    /// 顶栏图标钮的液态玻璃底：26 上定尺寸 + 系统 glassEffect 胶囊；
    /// 26 以下**什么都不加** —— 玻璃是 26 的设计语言，旧系统保持 Qt 的裸图标。
    /// （区别于 `glassCapsule`：那个在旧系统会手画一个灰胶囊底。）
    @ViewBuilder
    func topBarGlass(size: CGFloat = 28, tinted: Color? = nil) -> some View {
        if #available(macOS 26.0, *) {
            frame(width: size, height: size)
                .glassCapsule(tinted: tinted)
        } else {
            self
        }
    }
}

/// 液态玻璃分段控件：整组一层玻璃，段与段紧挨、没有各自的圆角。
///
/// 从页脚的模式切换抽出来 —— 界面上凡是「一组、单选」的地方都该长这样，
/// 各写一套迟早会在圆角、内距、高度上漂移（页脚那处就来回对了好几版）。
struct GlassSegmented<Value: Hashable>: View {
    let items: [(value: Value, title: String)]
    @Binding var selection: Value
    /// 选中段的底色。
    var tint: Color

    var body: some View {
        HStack(spacing: 0) {
            ForEach(items, id: \.value) { item in
                Button {
                    selection = item.value
                } label: {
                    Text(item.title)
                        .font(.system(size: 11))
                        .foregroundStyle(item.value == selection ? Color.white : .secondary)
                        .padding(.horizontal, 10)
                        .frame(height: 22)
                        .fixedSize()
                }
                .buttonStyle(.plain)
                .background {
                    // 压在整组玻璃**里面**，不给整组带来额外圆角
                    if item.value == selection { Capsule().fill(tint) }
                }
            }
        }
        .glassCapsule()
    }
}
