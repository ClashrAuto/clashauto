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

extension View {
    /// 给整窗铺一层毛玻璃底。
    func windowGlass(_ material: NSVisualEffectView.Material = .sidebar) -> some View {
        background(VisualEffectBackground(material: material).ignoresSafeArea())
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
            if prominent {
                content.buttonStyle(.glassProminent)
            } else {
                content.buttonStyle(.glass)
            }
        } else {
            // 旧系统回落到 bordered —— 观感最接近，也不会显得突兀。
            content.buttonStyle(.bordered)
        }
    }
}

extension View {
    /// 液态玻璃按钮；macOS 26 以下回落 `.bordered`。
    func glassButton(prominent: Bool = false) -> some View {
        modifier(GlassButtonModifier(prominent: prominent))
    }
}
