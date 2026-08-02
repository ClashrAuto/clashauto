import AppKit
import SwiftUI

/// 实时曲线（带宽图 / 设备行底纹）里**会动的那一层**：折线 + 线下填充，整条匀速左滑。
///
/// ★ **为什么不是 SwiftUI 的 `TimelineView` + `Path`。** 观感要求是曲线**连续左滑**
///   （不是每来一个采样整条跳一格），原来的做法是 20fps 的 `TimelineView` 每 50ms 按
///   相位重算一遍路径。问题不在于重算路径本身，而在于 SwiftUI 的每一帧更新都要走一遍
///   `NSHostingView.layout()` —— **整个窗口的视图树跟着每帧重排**，开销正比于当前页上有
///   多少视图，与这条曲线的复杂度无关。
///
///   实测（core 不启动、零流量、稳态 CPU，状态页）：20fps 时 12.0%，把它降到 1fps 是
///   3.5% —— 一条曲线要 8.5 个点，而它每帧的变化**只是整条平移**。
///
///   平移正是 CoreAnimation 最擅长、且完全跑在渲染服务进程里的事：路径只在**新采样到达时**
///   （1Hz）重建一次，中间那一秒由 CA 自己插值。app 进程每帧零工作，SwiftUI 视图图不参与。
///   相位口径与原来逐字相同：一拍之内正好左移一格 `dx`，到位后**停住**等下一拍
///   （对应原来的 `phase = min(1, 距上次入点的秒数)` —— 采样卡住时曲线不该继续往左走）。
///
/// 静态的那些（网格、刻度、刻度文字、标题）仍留在 SwiftUI 里：它们只在量程变化时才动，
/// 一秒最多一次，不值得也不该搬进来。
struct SlidingCurve: NSViewRepresentable {

    /// 线下填充的画法。三种用法各不相同，逐个对齐原来的 SwiftUI 写法。
    enum Fill: Equatable {
        /// 不填充（带宽图的「独立成图」模式）。
        case none
        /// 实色淡填充（带宽图的卡片底纹模式：线色 α0.16）。
        case solid(opacity: Double)
        /// 上深下浅的竖直渐变（设备行底纹：α0.22 → α0.02）。
        case gradient(top: Double, bottom: Double)
    }

    /// 采样序列，**已补齐到定长**，越靠后越新。
    var samples: [Double]
    /// 纵轴满量程（字节/秒）。
    var scale: Double
    /// 曲线最高只占控件高度的这个比例。
    var headroom: Double
    var lineColor: Color
    var lineWidth: CGFloat
    var lineOpacity: Double
    var fill: Fill
    /// 采样节拍。**只认它的变化**来重启滑动 —— `updateNSView` 因为别的原因（换主题、
    /// 改尺寸）也会被调到，那些不该让曲线从头滑一遍。
    var tick: UInt64
    /// 一拍的时长（秒）。采样是 1Hz，所以左滑一格也用 1 秒。
    var slideInterval: Double = 1

    func makeNSView(context: Context) -> CurveView {
        let view = CurveView()
        view.apply(self, restartSlide: true)
        return view
    }

    func updateNSView(_ view: CurveView, context: Context) {
        view.apply(self, restartSlide: view.lastTick != tick)
    }

    /// 承载三层：渐变（带遮罩）/ 实色填充 / 折线。三层一起挂在一个容器层上，
    /// **平移动画只加在容器上** —— 各层分别动的话，填充与线之间会错开半个像素。
    final class CurveView: NSView {
        private(set) var lastTick: UInt64 = .max
        private let container = CALayer()
        private let fillShape = CAShapeLayer()
        private let gradient = CAGradientLayer()
        private let line = CAShapeLayer()
        private var model: SlidingCurve?

        override init(frame: NSRect) {
            super.init(frame: frame)
            wantsLayer = true
            // 滑出左边缘的那一小段不能画到控件外面去（原来是 SwiftUI 的 `.clipped()`）。
            layer?.masksToBounds = true
            fillShape.fillColor = nil
            fillShape.strokeColor = nil
            line.fillColor = nil
            line.lineCap = .round
            line.lineJoin = .round
            gradient.startPoint = CGPoint(x: 0.5, y: 1)   // 上
            gradient.endPoint = CGPoint(x: 0.5, y: 0)     // 下
            container.addSublayer(gradient)
            container.addSublayer(fillShape)
            container.addSublayer(line)
            layer?.addSublayer(container)
        }

        @available(*, unavailable)
        required init?(coder: NSCoder) { fatalError("init(coder:) is not implemented") }

        override func layout() {
            super.layout()
            if let model { apply(model, restartSlide: true) }   // 尺寸变了，路径要重画
        }

        func apply(_ model: SlidingCurve, restartSlide: Bool) {
            self.model = model
            lastTick = model.tick
            let size = bounds.size
            guard size.width > 0, size.height > 0 else { return }

            // 建层与改色都不要隐式动画：CA 默认给每个属性变化补 0.25s 过渡，
            // 那会让换主题时曲线颜色慢慢淡过去，也会让新路径「变形」而不是直接就位。
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            defer { CATransaction.commit() }

            container.frame = CGRect(origin: .zero, size: size)
            for sublayer in [gradient, fillShape, line] as [CALayer] {
                sublayer.frame = container.bounds
            }

            let stroke = Self.linePath(model, in: size)
            line.path = stroke
            line.lineWidth = model.lineWidth
            line.strokeColor = NSColor(model.lineColor.opacity(model.lineOpacity)).cgColor

            switch model.fill {
            case .none:
                fillShape.path = nil
                gradient.isHidden = true
            case .solid(let opacity):
                gradient.isHidden = true
                fillShape.path = Self.areaPath(from: stroke, in: size)
                fillShape.fillColor = NSColor(model.lineColor.opacity(opacity)).cgColor
            case .gradient(let top, let bottom):
                fillShape.path = nil
                gradient.isHidden = false
                gradient.colors = [NSColor(model.lineColor.opacity(top)).cgColor,
                                   NSColor(model.lineColor.opacity(bottom)).cgColor]
                // 渐变本身是整块的，靠面积路径当遮罩裁出曲线下方那一块。
                let mask = gradient.mask as? CAShapeLayer ?? CAShapeLayer()
                mask.frame = gradient.bounds
                mask.path = Self.areaPath(from: stroke, in: size)
                mask.fillColor = NSColor.black.cgColor
                gradient.mask = mask
            }

            if restartSlide { startSlide(model, in: size) }
        }

        /// 一拍之内左移一格 `dx`，到位后**停住**（`fillMode: .forwards` + 不移除）——
        /// 与原来 `phase = min(1, elapsed)` 的封顶行为一致：采样断了，曲线就该停在那儿。
        private func startSlide(_ model: SlidingCurve, in size: CGSize) {
            let slide = CABasicAnimation(keyPath: "transform.translation.x")
            slide.fromValue = 0
            slide.toValue = -Self.dx(model, in: size)
            slide.duration = model.slideInterval
            slide.timingFunction = CAMediaTimingFunction(name: .linear)
            slide.fillMode = .forwards
            slide.isRemovedOnCompletion = false
            container.removeAnimation(forKey: "slide")
            container.add(slide, forKey: "slide")
        }

        /// 相邻两点的横向间距。**多留一格**给「正在滑进来的那一点」，
        /// 否则最后一点滑到位时右边会空出一条缝（口径照搬原实现）。
        private static func dx(_ model: SlidingCurve, in size: CGSize) -> CGFloat {
            let count = model.samples.count
            return size.width / CGFloat(count - 2 > 0 ? count - 2 : 1)
        }

        /// 折线。**画在相位 0 上**（不含左移），那一格由 CA 的平移动画补。
        ///
        /// 纵坐标按 CA 的坐标系（原点在**左下**）算：`y = 可用高度 × 归一化值`。
        /// SwiftUI 那版是 `height - usable × 值`（原点在左上），两者是同一条曲线。
        private static func linePath(_ model: SlidingCurve, in size: CGSize) -> CGPath? {
            let values = model.samples
            guard values.count > 1, model.scale > 0 else { return nil }
            let usable = size.height * model.headroom
            let step = dx(model, in: size)
            let path = CGMutablePath()
            for (index, value) in values.enumerated() {
                let point = CGPoint(x: CGFloat(index) * step,
                                    y: usable * CGFloat(min(1, value / model.scale)))
                index == 0 ? path.move(to: point) : path.addLine(to: point)
            }
            return path
        }

        /// 线下填充：折线 + 右下角 + 左下角闭合。收口用的是**控件右边缘**而不是最后一点
        /// （最后一点在 `width + dx` 处，右边那一小段本来就滑在框外），与原实现一致。
        private static func areaPath(from stroke: CGPath?, in size: CGSize) -> CGPath? {
            guard let stroke else { return nil }
            let path = CGMutablePath()
            path.addPath(stroke)
            path.addLine(to: CGPoint(x: size.width, y: 0))
            path.addLine(to: CGPoint(x: 0, y: 0))
            path.closeSubpath()
            return path
        }
    }
}
