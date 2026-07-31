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

    var body: some View {
        ZStack {
            // 背景：线色极淡的底。
            if !minimal {
                Rectangle().fill(lineColor.opacity(0.03))
            }

            GeometryReader { geo in
                ZStack(alignment: .topLeading) {
                    if !minimal {
                        grid(in: geo.size)
                    }
                    // 底纹模式下线下有一层淡填充；独立成图时**只有线**。
                    if minimal {
                        areaPath(in: geo.size)
                            .fill(LinearGradient(colors: [lineColor.opacity(0.22), .clear],
                                                 startPoint: .top, endPoint: .bottom))
                    }
                    linePath(in: geo.size)
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
        }
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

    private func points(in size: CGSize) -> [CGPoint] {
        guard samples.count > 1 else { return [] }
        let usable = size.height * headroom
        let dx = size.width / CGFloat(samples.count - 1)
        return samples.enumerated().map { index, value in
            CGPoint(x: CGFloat(index) * dx,
                    y: size.height - usable * CGFloat(min(1, value / scale)))
        }
    }

    private func linePath(in size: CGSize) -> Path {
        var path = Path()
        let pts = points(in: size)
        guard let first = pts.first else { return path }
        path.move(to: first)
        for point in pts.dropFirst() { path.addLine(to: point) }
        return path
    }

    private func areaPath(in size: CGSize) -> Path {
        var path = linePath(in: size)
        guard !path.isEmpty else { return path }
        path.addLine(to: CGPoint(x: size.width, y: size.height))
        path.addLine(to: CGPoint(x: 0, y: size.height))
        path.closeSubpath()
        return path
    }
}
