import CoastKit
import SwiftUI

/// 实时带宽折线。对齐 `qml/BandwidthChart.qml`（那边是 Canvas，这边用 `Path`）。
///
/// 两种用法，与 QML 的 `minimal` 开关一一对应：
/// - **独立成图**（设备详情窗的「实时流量」卡）：四分网格 + 右侧速度刻度（max/¾/½/¼）
///   + 左上标题，**只画折线、不填充**，背景是线色极淡的底（α0.03）；
/// - **卡片底纹**（上传/下载卡）：不画背景底与左上标题，改成「一条更细更淡的线 +
///   线下一层实色淡填充」，另配四条按**曲线实际高度**定位的刻度（不是整高四等分 ——
///   `headroom` 压着曲线，等分线会全对不上）。
struct BandwidthChart: View {
    @Environment(Theme.self) private var theme

    /// 最近若干拍的速率（字节/秒），越靠后越新。
    let samples: [Double]

    /// 图上画多少个点。**40 个可见 + 2 个富余**（两端各一个，滑动时右侧不留缺口）——
    /// 与 QML 的 `maxPointer: 42` 同值，横轴也就是「最近 40 秒」。
    ///
    /// ★ 这个数决定横轴的时间跨度。状态页原来喂 60 拍进来，同样的宽度里塞了 60 秒，
    ///   一次流量尖峰看起来比 Qt 窄一截；而点数不足 42 时（刚启动那几十秒）
    ///   Qt 是**预先填满**的（`Component.onCompleted` 塞 42 个 1.0），曲线一上来就贴着底边
    ///   铺满整宽，Swift 这边则是从左边一小截慢慢长出来。两处都在这儿统一。
    static let pointCount = 42

    /// 补齐到 `pointCount`：不足的在**左边**补（旧的一侧），与 Qt 的预填充同效。
    private var padded: [Double] {
        let tail = samples.suffix(Self.pointCount)
        guard tail.count < Self.pointCount else { return Array(tail) }
        return Array(repeating: 1, count: Self.pointCount - tail.count) + tail
    }
    var title = ""
    var lineColor: Color = .accentColor
    /// 采样节拍（`AppState.pollTick`）。每 +1 表示「刚进来一个新点」，
    /// 曲线据此开始新一轮左滑。
    var tick: UInt64 = 0
    /// 卡片底纹模式。
    var minimal = false
    /// 曲线最高只占本控件高度的这个比例。底纹模式下要给卡片的标题/数值让位。
    var headroom: Double = 1.0

    /// 量程：**128KB 基准 / 2MB 步进**，与 QML 的 `currentMax()` 逐字相同。
    ///
    /// 固定量程会让小流量时曲线贴着底边看不出形状，纯自适应又会让刻度数字每秒乱跳；
    /// 「先垫一个 128KB 的底、超了再按 2MB 一档一档往上跳」是两者的折中。
    private var scale: Double {
        let base = 131_072.0        // 128 KB
        let step = 2_097_152.0      // 2 MB
        let peak = padded.max() ?? 0
        return peak > base ? (peak / step).rounded(.up) * step : base
    }

    /// 上一拍进点的时刻。
    @State private var lastPush = Date()

    var body: some View {
        ZStack {
            // 背景：线色极淡的底。
            if !minimal {
                Rectangle().fill(lineColor.opacity(0.03))
            }

            GeometryReader { geo in
                // 50ms ≈ 20fps，与 QML 那个 `Timer { interval: 50 }` 同一档。
                // **按「距上次入点的真实经过时间」算相位**，而不是每帧固定推进一点点 ——
                // 采样节拍会飘（轮询、节流都会让它偏离 1s），固定步长滑到位的时刻就对不上
                // 下一拍，曲线仍旧一顿一顿。
                TimelineView(.periodic(from: .now, by: 0.05)) { context in
                    let phase = min(1, max(0, context.date.timeIntervalSince(lastPush)))
                ZStack(alignment: .topLeading) {
                    if !minimal {
                        grid(in: geo.size)
                    }
                    // 底纹模式下**先画刻度**（压在填充之下），再填充、再画线，
                    // 最后把刻度文字画在最上面 —— 与 Qt 的绘制顺序一致，
                    // 否则文字会被那层填充糊掉。
                    if minimal {
                        minimalTicks(in: geo.size)
                        // 平铺一层淡填充（Qt 是 `rgba(lc, 0.16)` 的**实色**，不是渐变）：
                        // 只有一条细线的话，在卡片底纹这个尺度上几乎看不见。
                        areaPath(in: geo.size, phase: phase).fill(lineColor.opacity(0.16))
                    }
                    linePath(in: geo.size, phase: phase)
                        // 线宽与透明度**两种模式不同**（Qt：`minimal ? 2.0 : 3.0`、
                        // `minimal ? 0.55 : 0.70`）—— 底纹要压得住卡片上的数字，
                        // 原来两处都按独立成图那一档画，线又粗又实，抢了标题的注意力。
                        .stroke(lineColor.opacity(minimal ? 0.55 : 0.70),
                                style: StrokeStyle(lineWidth: minimal ? 2 : 3,
                                                   lineCap: .round, lineJoin: .round))

                    if minimal {
                        minimalTickLabels(in: geo.size)
                    }

                    if !minimal, !title.isEmpty {
                        // Qt：`11px`、线色 70%、`fillText(title, 10, 18)`（18 是**基线**）。
                        Text(title)
                            .font(.system(size: 11))
                            .foregroundStyle(lineColor.opacity(0.70))
                            .offset(x: 10, y: Self.topForBaseline(18, size: 11))
                    }
                }
                }
                .clipped()   // 左滑出去的那一小段别画到控件外面
            }
        }
        .onChange(of: tick) { _, _ in lastPush = Date() }
    }

    /// 底纹模式的刻度：**必须按曲线的实际高度定位**，不能沿用「整高四等分」——
    /// `headroom` 只让曲线占下面一截，等分线会全部对不上曲线，刻度就成了骗人的
    /// （Qt 在这处专门写了这句）。顶到卡片标题那一带（y < 18）的那一档不画。
    private func minimalTickY(in size: CGSize) -> [(y: CGFloat, value: Double)] {
        [1.0, 0.75, 0.5, 0.25].compactMap { (fraction: Double) -> (y: CGFloat, value: Double)? in
            let y = CGFloat((Double(size.height) - Double(size.height) * headroom * fraction)
                .rounded()) + 0.5
            return y < 18 ? nil : (y: y, value: scale * fraction)
        }
    }

    private func minimalTicks(in size: CGSize) -> some View {
        ForEach(minimalTickY(in: size), id: \.y) { tick in
            Path { path in
                path.move(to: CGPoint(x: 0, y: tick.y))
                path.addLine(to: CGPoint(x: size.width, y: tick.y))
            }
            .stroke(lineColor.opacity(0.13), lineWidth: 1)
        }
    }

    /// 刻度文字最后画：压在曲线之上才不会被填充盖掉。贴右边缘 6、坐在刻度线上方 3。
    private func minimalTickLabels(in size: CGSize) -> some View {
        ForEach(minimalTickY(in: size), id: \.y) { tick in
            Text(Formatting.rate(Int64(tick.value)))
                .font(.system(size: 9))
                .foregroundStyle(theme.dark ? Color(hex: 0x9A_9A_9A) : Color(hex: 0x7A_7A_7A))
                .frame(width: size.width - 6, alignment: .trailing)
                .offset(y: Self.topForBaseline(tick.y - 3, size: 9))
        }
    }

    /// Canvas 的 `fillText` 给的是**基线**，SwiftUI 的 `offset` 定的是**顶边**。
    /// 系统字体的 ascent 约等于字号，拿字号当近似即可 —— 差一两个点在这个尺度上看不出来，
    /// 而按基线原样写会整体偏低一整行。
    static func topForBaseline(_ baseline: CGFloat, size: CGFloat) -> CGFloat {
        baseline - size
    }

    /// 四分网格 + 右侧速度刻度（max / ¾ / ½ / ¼）。逐项照 Qt 的非 minimal 分支：
    /// **五条**线（`gi = 0…4`，含顶边与底边）、线色 `rgba(线色, 0.10)`、
    /// 刻度文字 10px `#969696`、贴右边缘 6、基线在 `H/4*li + 12`。
    private func grid(in size: CGSize) -> some View {
        ZStack(alignment: .topLeading) {
            ForEach(0...4, id: \.self) { step in
                let y = (size.height / 4 * CGFloat(step)).rounded() + 0.5
                Path { path in
                    path.move(to: CGPoint(x: 0, y: y))
                    path.addLine(to: CGPoint(x: size.width, y: y))
                }
                .stroke(lineColor.opacity(0.10), lineWidth: 1)
            }
            ForEach(0..<4, id: \.self) { index in
                Text(Formatting.rate(Int64(scale * (1 - Double(index) / 4))))
                    .font(.system(size: 10))
                    .foregroundStyle(Color(hex: 0x96_96_96))
                    .frame(width: size.width - 6, alignment: .trailing)
                    .offset(y: Self.topForBaseline(size.height / 4 * CGFloat(index) + 12, size: 10))
            }
        }
    }

    /// 折线的顶点。整条按 `dx * phase` 左移 —— 新点从右边缘外进入、匀速滑到位，
    /// 相位归零时正好接上下一拍。
    private func points(in size: CGSize, phase: Double) -> [CGPoint] {
        let values = padded
        guard values.count > 1 else { return [] }
        let usable = size.height * headroom
        // 多留一格宽度给「滑进来的那一点」，否则最后一点滑到位时右边会空出一条缝。
        let dx = size.width / CGFloat(values.count - 2 > 0 ? values.count - 2 : 1)
        let shift = dx * CGFloat(phase)
        return values.enumerated().map { index, value in
            CGPoint(x: CGFloat(index) * dx - shift,
                    y: size.height - usable * CGFloat(min(1, value / scale)))
        }
    }

    private func linePath(in size: CGSize, phase: Double) -> Path {
        var path = Path()
        let pts = points(in: size, phase: phase)
        guard let first = pts.first else { return path }
        path.move(to: first)
        for point in pts.dropFirst() { path.addLine(to: point) }
        return path
    }

    private func areaPath(in size: CGSize, phase: Double) -> Path {
        var path = linePath(in: size, phase: phase)
        guard !path.isEmpty else { return path }
        path.addLine(to: CGPoint(x: size.width, y: size.height))
        path.addLine(to: CGPoint(x: 0, y: size.height))
        path.closeSubpath()
        return path
    }
}
