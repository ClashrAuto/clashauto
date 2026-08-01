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
    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async { configure(view.window) }
        return view
    }

    func updateNSView(_ view: NSView, context: Context) {
        DispatchQueue.main.async { configure(view.window) }
    }

    private func configure(_ window: NSWindow?) {
        guard let window else { return }
        window.isOpaque = false
        window.backgroundColor = .clear
        // 阴影要保留：非不透明窗口若不显式开启，macOS 会连投影一起去掉，
        // 窗口边缘会糊在桌面上，看不出这是一个独立的窗。
        window.hasShadow = true

        if #available(macOS 26.0, *) {
            // 26：标题栏加高到与页面顶部导航栏一带同高（≈ 顶距 10 + 栏高 28 那一带）。
            // 做法是挂一个**空的** NSToolbar —— 这是把标题栏抬到统一工具栏高度、
            // 让红绿灯在其中垂直居中的受支持写法；transparent 保证不画任何工具栏底。
            window.titlebarAppearsTransparent = true
            if window.toolbar == nil {
                let toolbar = NSToolbar(identifier: "coast.titlebar.spacer")
                toolbar.showsBaselineSeparator = false
                window.toolbar = toolbar
            }
            window.toolbarStyle = .unified
        }
    }
}

extension View {
    /// 给整窗铺一层毛玻璃底（并把窗口本身设为透明，否则材质无效）。
    func windowGlass(_ material: NSVisualEffectView.Material = .sidebar) -> some View {
        background(VisualEffectBackground(material: material).ignoresSafeArea())
            .background(WindowConfigurator().frame(width: 0, height: 0))
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
