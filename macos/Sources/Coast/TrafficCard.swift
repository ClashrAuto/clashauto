import CoastKit
import SwiftUI

/// 上传 / 下载卡。严格对齐 Qt 的 `MetricCard`（`showChart: true` 那一路）。
///
/// 关键是**曲线画在卡片背景里**，不是另起一张图：Qt 的注释写得很清楚 ——
/// 「曲线以前是页面下方两张独立的图，现在各自并进对应的卡里 —— 同一个数字的『此刻』
/// 和『最近 40 秒』挨在一起看，也省下了整整一屏的竖向空间」。
///
/// 曲线用 `headroom = 0.62` 压在下半部，免得冲到标题和数值上；右侧标注 4 条刻度线。
struct TrafficCard: View {
    @Environment(Theme.self) private var theme

    let symbol: String
    let title: String
    let value: String
    let accent: Color
    /// 最近若干拍的速率（字节/秒），越靠后越新。
    let samples: [Double]

    /// 量程：**128KB 基准 / 2MB 步进**，与 `qml/BandwidthChart.qml` 的 `currentMax()`
    /// 逐字相同（原来这里是「32KB 台阶」，是我自己编的，两张图的刻度因此对不上）。
    ///
    /// 固定量程会让小流量时曲线贴着底边看不出形状，纯自适应又会让刻度数字每秒乱跳；
    /// 「先垫一个 128KB 的底、超了再按 2MB 一档一档往上跳」是两者的折中。
    private var scaleMax: Double {
        let base = 131_072.0        // 128 KB
        let step = 2_097_152.0      // 2 MB
        let peak = samples.max() ?? 0
        return peak > base ? (peak / step).rounded(.up) * step : base
    }

    private var gridLines: [Double] {
        let top = scaleMax
        return [top, top * 0.75, top * 0.5, top * 0.25]
    }

    var body: some View {
        ZStack(alignment: .topLeading) {
            // 背景:刻度线 + 曲线。声明在前 → 压在标题/数值之下。
            GeometryReader { geo in
                ZStack(alignment: .topTrailing) {
                    VStack(spacing: 0) {
                        ForEach(Array(gridLines.enumerated()), id: \.offset) { _, level in
                            HStack {
                                Spacer()
                                Text(Formatting.rate(Int64(level)))
                                    .font(.system(size: 9))
                                    .foregroundStyle(theme.textMuted.opacity(0.7))
                            }
                            .frame(height: geo.size.height / 4, alignment: .bottom)
                            .overlay(alignment: .bottom) {
                                Rectangle()
                                    .fill(accent.opacity(0.12))
                                    .frame(height: 1)
                            }
                        }
                    }
                    SparkLine(samples: samples, maximum: scaleMax, headroom: 0.62)
                        .stroke(accent, lineWidth: 1.2)
                    SparkLine(samples: samples, maximum: scaleMax, headroom: 0.62, closed: true)
                        .fill(LinearGradient(colors: [accent.opacity(0.22), .clear],
                                             startPoint: .top, endPoint: .bottom))
                }
            }

            // 图标 + 标题/数值。带背景折线时**只占卡片顶部一条 64 高的带子**（内容在带子里
            // 居中）：卡片会随窗口长高，若还按整卡居中，图和字就会在卡片中央撞在一起，
            // 且窗口越高字越往下漂。尺寸逐项照抄 `qml/MetricCard.qml`：
            // 左内距 14 / 右 10 / 间距 12、图标 28、标题 13、数值 24，标题与数值同为品牌色。
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .font(.system(size: 28))
                    .foregroundStyle(theme.dark ? Color(hex: 0xAA_AA_AA) : Color(hex: 0x88_88_88))
                VStack(alignment: .leading, spacing: 3) {
                    Text(title).font(.system(size: 13)).foregroundStyle(accent)
                        .lineLimit(1).truncationMode(.tail)
                    Text(value).font(.system(size: 24)).foregroundStyle(accent)
                        .lineLimit(1).truncationMode(.tail)
                }
                Spacer(minLength: 0)
            }
            .padding(.leading, 14)
            .padding(.trailing, 10)
            .frame(height: 64)
        }
        .frame(height: 170)
        .background(theme.metricBg)
        // Qt 这几张卡是 `radius: 4`，比 `Theme.radius`(5) 小一档 —— 照抄。
        .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
    }
}

/// 折线本体。`headroom` 表示曲线最高只能到卡片高度的这个比例处（0.62 = 下 62%）。
private struct SparkLine: Shape {
    let samples: [Double]
    let maximum: Double
    let headroom: Double
    var closed = false

    func path(in rect: CGRect) -> Path {
        var path = Path()
        guard samples.count > 1, maximum > 0 else { return path }
        let top = rect.height * (1 - headroom)
        let usable = rect.height - top
        let dx = rect.width / CGFloat(samples.count - 1)
        func point(_ index: Int) -> CGPoint {
            let ratio = min(1, samples[index] / maximum)
            return CGPoint(x: CGFloat(index) * dx, y: rect.height - usable * ratio)
        }
        path.move(to: point(0))
        for index in 1..<samples.count { path.addLine(to: point(index)) }
        if closed {
            path.addLine(to: CGPoint(x: rect.width, y: rect.height))
            path.addLine(to: CGPoint(x: 0, y: rect.height))
            path.closeSubpath()
        }
        return path
    }
}
