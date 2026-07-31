import CoastKit
import SwiftUI

/// 实时带宽折线。对齐 `qml/BandwidthChart.qml`（那边是 Canvas，这边用 `Path`）。
///
/// 两种用法，与 QML 的 `minimal` 开关一一对应：
/// - **独立成图**（设备详情窗的「实时流量」卡）：四分网格 + 右侧速度刻度（max/¾/½/¼）
///   + 左上标题，**只画折线、不填充**，背景是线色极淡的底（α0.03）；
/// - **卡片底纹**（上传/下载卡）：网格、刻度、标题全是噪音，压在卡片的数字底下只会打架，
///   所以一概不画，改成「一条线 + 线下的淡填充」—— 底纹要的是趋势的形状，不是能读数的图表。
struct BandwidthChart: View {
    @Environment(Theme.self) private var theme

    /// 最近若干拍的速率（字节/秒），越靠后越新。
    let samples: [Double]
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
        let peak = samples.max() ?? 0
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
                    // 底纹模式下线下有一层淡填充；独立成图时**只有线**。
                    if minimal {
                        areaPath(in: geo.size, phase: phase)
                            .fill(LinearGradient(colors: [lineColor.opacity(0.22), .clear],
                                                 startPoint: .top, endPoint: .bottom))
                    }
                    linePath(in: geo.size, phase: phase)
                        .stroke(lineColor.opacity(0.70),
                                style: StrokeStyle(lineWidth: 3, lineCap: .round, lineJoin: .round))

                    if !minimal, !title.isEmpty {
                        Text(title)
                            .font(.system(size: 10))
                            .foregroundStyle(theme.textMuted)
                            .padding(4)
                    }
                }
                }
                .clipped()   // 左滑出去的那一小段别画到控件外面
            }
        }
        .onChange(of: tick) { _, _ in lastPush = Date() }
    }

    /// 四分网格 + 右侧速度刻度（max / ¾ / ½ / ¼）。
    private func grid(in size: CGSize) -> some View {
        ForEach(1...4, id: \.self) { step in
            let ratio = Double(step) / 4
            let y = size.height * (1 - ratio)
            ZStack(alignment: .topTrailing) {
                Path { path in
                    path.move(to: CGPoint(x: 0, y: y))
                    path.addLine(to: CGPoint(x: size.width, y: y))
                }
                .stroke(theme.divider.opacity(0.5), lineWidth: 1)

                Text(Formatting.rate(Int64(scale * ratio)))
                    .font(.system(size: 9))
                    .foregroundStyle(theme.textMuted.opacity(0.7))
                    .offset(y: y + 1)
            }
        }
    }

    /// 折线的顶点。整条按 `dx * phase` 左移 —— 新点从右边缘外进入、匀速滑到位，
    /// 相位归零时正好接上下一拍。
    private func points(in size: CGSize, phase: Double) -> [CGPoint] {
        guard samples.count > 1 else { return [] }
        let usable = size.height * headroom
        // 多留一格宽度给「滑进来的那一点」，否则最后一点滑到位时右边会空出一条缝。
        let dx = size.width / CGFloat(samples.count - 2 > 0 ? samples.count - 2 : 1)
        let shift = dx * CGFloat(phase)
        return samples.enumerated().map { index, value in
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
