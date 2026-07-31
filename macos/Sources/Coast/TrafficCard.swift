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

    /// 刻度：取当前窗口峰值向上取整到 32 KB/s 的整数倍，至少 128 KB/s。
    ///
    /// 固定刻度会让小流量时曲线贴着底边看不出形状，纯自适应又会让刻度数字每秒跳；
    /// 按 32 KB/s 台阶取整是两者的折中 —— 与 Qt 卡上那组 32/64/96/128 的观感一致。
    private var scaleMax: Double {
        let peak = samples.max() ?? 0
        let step = 32.0 * 1024
        let steps = max(4.0, (peak / step).rounded(.up))
        return steps * step
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

            HStack(alignment: .top, spacing: 8) {
                Image(systemName: symbol)
                    .font(.system(size: 18))
                    .foregroundStyle(theme.textSecondary)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.system(size: 12)).foregroundStyle(accent)
                    Text(value).font(.system(size: 22, weight: .medium)).foregroundStyle(accent)
                }
                Spacer()
            }
            .padding(12)
        }
        .frame(height: 170)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
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
