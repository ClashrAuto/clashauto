import CoastKit
import SwiftUI

/// 上传 / 下载卡。严格对齐 Qt 的 `MetricCard`（`showChart: true` 那一路）。
///
/// 关键是**曲线画在卡片背景里**，不是另起一张图：Qt 的注释写得很清楚 ——
/// 「曲线以前是页面下方两张独立的图，现在各自并进对应的卡里 —— 同一个数字的『此刻』
/// 和『最近 40 秒』挨在一起看，也省下了整整一屏的竖向空间」。
///
/// 曲线用 `headroom = 0.62` 压在下半部，免得冲到标题和数值上。
/// **不画网格与刻度** —— Qt 的 `minimal` 模式明确说那是噪音（见 `BandwidthChart`）。
struct TrafficCard: View {
    @Environment(Theme.self) private var theme

    let symbol: String
    let title: String
    let value: String
    /// 标题与数值的颜色（Qt 的 `accentColor`）。
    let accent: Color
    /// 背景那条曲线的颜色（Qt 的 `chartColor`，比文字亮一档）。
    let lineColor: Color
    /// 最近若干拍的速率（字节/秒），越靠后越新。
    let samples: [Double]
    /// 采样节拍，透传给曲线做连续左滑（见 `BandwidthChart.tick`）。
    var tick: UInt64 = 0

    var body: some View {
        ZStack(alignment: .topLeading) {
            // 背景那条曲线走 `BandwidthChart` 的**底纹模式**：网格、右侧刻度、标题一概不画
            // —— 它们压在卡片的数字底下只会打架（Qt 的 `minimal` 就是这个意思）。
            //
            // 原来这里自己画了一份，还多画了四条带速率标注的网格线 —— 那是 Qt 明确说
            // **不要**的东西；顺带也就没有那条「整条连续左滑」的滚动。现在共用同一个组件。
            BandwidthChart(samples: samples,
                           lineColor: lineColor,
                           tick: tick,
                           minimal: true,
                           headroom: 0.62)

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

