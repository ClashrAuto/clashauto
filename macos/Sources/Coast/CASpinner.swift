import AppKit
import SwiftUI

/// 无限旋转的小图标载体（延迟卡刷新 / 节点测速 / 设备重扫共用）。
///
/// ★ **旋转必须由 CoreAnimation 驱动，不能用 SwiftUI 的 `.animation(.repeatForever)`。**
///   后者是一条永不结束的**视图图**动画：只要在转，整个窗口的视图树就每帧走一遍
///   `NSHostingView.layout()`，开销正比于当前页的视图数，与这枚 15pt 的字形无关
///   （页脚呼吸点当初也是同一个坑，见 `PulsingRing`）。实测（状态页、接真核心、25s 窗口）：
///   SwiftUI 版转圈把 app 从 ~9% 顶到 20%，CA 版转圈与不转没有可测差别。
///   CA 动画跑在渲染服务进程里，app 进程每帧零工作。
///
/// 实现是「`ImageRenderer` 把内容按当前屏幕倍率栅格化 → 贴进一个自管的 `CALayer` →
/// 对这个图层做 CA 旋转」。**不是**嵌一层 `NSHostingView`：那条路先在设备页
/// 稳定崩掉（嵌套 hosting view 在约束更新过程中再触发约束失效，AppKit 抛
/// `_postWindowNeedsUpdateConstraints` 异常），绕开约束后又量不出尺寸。自管图层
/// AppKit 完全不碰，anchorPoint 保持默认的中心，旋转天然绕中心。
///
/// 用法约束：
/// - `content` 在**栅格化那一刻**求值，外层的 Environment 传不进来。颜色最好在调用点
///   解析成具体值（`theme.accent` 这类）再写进 `content`；**没有显式颜色的内容必须靠
///   `scheme` 把明暗告诉栅格化器** —— 不给的话它按默认（浅色）渲，深色界面上那枚字形
///   会渲成黑的、直接看不见。★ 这不是假设：设备页重扫那颗图标就是这么丢的，
///   改前/改后截图逐像素比对里「改后那一块一个亮像素都没有」（状态页那颗有显式颜色，
///   同一次比对里最大通道差只有 1/255，纯抗锯齿噪声）。
///   内容变了（换色/换主题）`updateNSView` 会重新栅格化，一次就是画一枚小字形的钱。
/// - 事件全部放行（`hitTest` 返 nil）：这层只管转，点击仍由外层的 SwiftUI `Button` 接。
/// - 停转时图形**立即回正**（CA 动画移除即回相位 0）。原 SwiftUI 写法停转时是一段
///   `.default` 的回摆 —— 只在「停下的那一瞬」有亚秒级差别，转动中逐帧一致。
/// ★ **不转的时候不走栅格化。** 静止态直接摆原来的 SwiftUI 内容 —— 逐像素与改造前完全一致；
///   只有真在转时才换成 CA 图层。这样「栅格化复现不出实时的 vibrancy」这个差别只存在于
///   转动的那几秒里（而且那时它正在转），静止态一点风险都没有。
///   这不是洁癖：设备页那颗重扫图标没有显式颜色，实时渲染是 (197,197,198)、栅格化出来是
///   (180,180,181) —— 逐像素比对量得到，肉眼盯着看也能觉出暗了一档。
///   顺带也省掉了「每次 SwiftUI 更新都重新栅格化一次」的常态开销（那颗按钮所在的页面
///   每秒都在更新）。
struct CASpinner<Content: View>: View {
    var active: Bool
    /// 栅格化时用的明暗。内容没写死颜色时，靠它把系统前景色解析对。
    var scheme: ColorScheme
    @ViewBuilder var content: Content

    var body: some View {
        if active {
            SpinLayer(scheme: scheme) { content }
        } else {
            content
        }
    }
}

private struct SpinLayer<Content: View>: NSViewRepresentable {
    /// 栅格化时用的明暗。内容没写死颜色时，靠它把系统前景色解析对。
    var scheme: ColorScheme
    @ViewBuilder var content: Content
    /// 0.9 秒一圈、匀速、顺时针 —— 与原
    /// `.linear(duration: 0.9).repeatForever(autoreverses: false)` 逐值相同。

    func makeNSView(context: Context) -> SpinView {
        let view = SpinView()
        view.render(content, scheme: scheme)
        view.setSpinning(true)   // 这层只在转的时候才存在
        return view
    }

    func updateNSView(_ view: SpinView, context: Context) {
        view.render(content, scheme: scheme)
        view.setSpinning(true)
    }

    /// 尺寸 = 内容的理想尺寸（这里装的都是定字号的图标字形）。
    func sizeThatFits(_ proposal: ProposedViewSize, nsView: SpinView, context: Context) -> CGSize? {
        nsView.contentSize
    }

    final class SpinView: NSView {
        private let glyph = CALayer()
        private(set) var contentSize = CGSize.zero
        private var spinning = false

        init() {
            super.init(frame: .zero)
            wantsLayer = true
            glyph.contentsGravity = .resizeAspect
            layer?.addSublayer(glyph)
        }

        @available(*, unavailable)
        required init?(coder: NSCoder) { fatalError("init(coder:) is not implemented") }

        /// 点击穿透：外层的 SwiftUI `Button` 才是事件的主人。不放行的话，
        /// 这个 NSView 会把落在字形上的点击吃掉，按钮点了没反应。
        override func hitTest(_ point: NSPoint) -> NSView? { nil }

        @MainActor
        func render<V: View>(_ content: V, scheme: ColorScheme) {
            let renderer = ImageRenderer(content: content.environment(\.colorScheme, scheme))
            let scale = window?.backingScaleFactor ?? (NSScreen.main?.backingScaleFactor ?? 2)
            renderer.scale = scale
            guard let image = renderer.cgImage else { return }
            contentSize = CGSize(width: CGFloat(image.width) / scale,
                                 height: CGFloat(image.height) / scale)
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            glyph.contents = image
            glyph.contentsScale = scale
            CATransaction.commit()
            needsLayout = true
        }

        /// 首次挂窗时倍率可能从兜底的 2 变成真实值（外接屏 1x）—— 重新栅格化一次。
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            needsLayout = true
        }

        override func layout() {
            super.layout()
            // 自管图层：frame 由这里手排。CALayer 的 anchorPoint 默认就是中心，
            // AppKit 不管这个图层，旋转天然绕中心，不需要任何锚点补偿。
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            glyph.frame = CGRect(x: ((bounds.width - contentSize.width) / 2).rounded(),
                                 y: ((bounds.height - contentSize.height) / 2).rounded(),
                                 width: contentSize.width, height: contentSize.height)
            CATransaction.commit()
        }

        func setSpinning(_ on: Bool) {
            guard on != spinning else { return }
            spinning = on
            if on {
                let spin = CABasicAnimation(keyPath: "transform.rotation.z")
                spin.fromValue = 0
                // 负角：macOS 图层坐标 y 朝上，正角是逆时针；原 SwiftUI 的 0→360° 是顺时针。
                spin.toValue = -2 * Double.pi
                spin.duration = 0.9
                spin.timingFunction = CAMediaTimingFunction(name: .linear)
                spin.repeatCount = .infinity
                // 窗口失焦/最小化后 CA 会暂停图层动画，恢复时要接着转而不是停格。
                spin.isRemovedOnCompletion = false
                glyph.add(spin, forKey: "spin")
            } else {
                glyph.removeAnimation(forKey: "spin")
            }
        }
    }
}
