import CoastKit
import SwiftUI

/// 上传 / 下载卡。严格对齐 Qt 的 `MetricCard`（`showChart: true` 那一路）。
///
/// 关键是**曲线画在卡片背景里**，不是另起一张图：Qt 的注释写得很清楚 ——
/// 「曲线以前是页面下方两张独立的图，现在各自并进对应的卡里 —— 同一个数字的『此刻』
/// 和『最近 40 秒』挨在一起看，也省下了整整一屏的竖向空间」。
///
/// 曲线用 `headroom = 0.62` 压在下半部，免得冲到标题和数值上。
///
/// ★ 刻度这件事以前记反了：Qt 的 `minimal` **属性注释**写着「右侧刻度、左上标题全是噪音，
///   minimal 时一概不画」，但它的**代码**在 minimal 分支里另画了一套 —— 四条按曲线实际高度
///   （而不是整高四等分）定位的淡线 + 9px 的速率标注。注释与代码不一致时以代码为准：
///   那段代码旁边还专门写了「等分线会全部对不上曲线，刻度就成了骗人的」。
struct TrafficCard: View {
    @Environment(Theme.self) private var theme

    /// Remix 码点（`IconFont.remix`，对齐 Qt `MetricCard.glyph`）。
    let glyph: String
    let title: String
    /// 标题与数值的颜色（Qt 的 `accentColor`）。
    let accent: Color
    /// 背景那条曲线的颜色（Qt 的 `chartColor`，比文字亮一档）。
    let lineColor: Color
    /// 最近若干拍的速率（字节/秒），越靠后越新。
    /// 采样节拍，透传给曲线做连续左滑（见 `BandwidthChart.tick`）。
    /// 上行卡还是下行卡。**决定两个叶子子视图去读 AppState 的哪一半**。
    ///
    /// ★ 这里**故意不接收** `value` / `samples` / `tick` 这三个每拍都变的参数。
    ///   真机实测（macOS 26.5.2 / arm64，n=18）：卡片结构一个字不改，只要这三个参数
    ///   每拍在变，SwiftUI 就判定整棵卡片子树失效、连 `frame`/`background`/`clipShape`
    ///   一起重走 layout pass —— 中位 CPU 1.80%；把它们换成常量后立刻掉到 0.60%
    ///   （空循环下限 0.50%）。而"卡片内部清空、参数照旧变"仍是 1.85%，
    ///   说明**成本来自失效范围，不是画了什么**。
    ///   所以变化的读取必须下沉到叶子子视图（见文件末尾 `RateText` / `CurveLayer`），
    ///   让失效只落在那一层，卡片外壳保持稳定。
    let isUp: Bool

    var body: some View {
        ZStack(alignment: .topLeading) {
            // 背景那条曲线走 `BandwidthChart` 的**底纹模式**：网格、右侧刻度、标题一概不画
            // —— 它们压在卡片的数字底下只会打架（Qt 的 `minimal` 就是这个意思）。
            //
            // 原来这里自己画了一份，顺带也就没有那条「整条连续左滑」的滚动。现在共用同一个组件。
            CurveLayer(isUp: isUp, lineColor: lineColor)

            // 图标 + 标题/数值。带背景折线时**只占卡片顶部一条 64 高的带子**（内容在带子里
            // 居中）：卡片会随窗口长高，若还按整卡居中，图和字就会在卡片中央撞在一起，
            // 且窗口越高字越往下漂。尺寸逐项照抄 `qml/MetricCard.qml`：
            // 左内距 14 / 右 10 / 间距 12、图标 28、标题 13、数值 24，标题与数值同为品牌色。
            HStack(spacing: 12) {
                Text(glyph)
                    .font(.custom(IconFont.remix, size: 28))
                    .foregroundStyle(theme.cardIcon)
                VStack(alignment: .leading, spacing: 3) {
                    Text(title).font(.system(size: 13)).foregroundStyle(accent)
                        .lineLimit(1).truncationMode(.tail)
                    RateText(isUp: isUp, accent: accent)
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

/// 速率数字。**单独成一个 View 是关键**：只有它的 body 读 `state.upText/downText`，
/// 于是每拍失效的只有这一层，卡片外壳（frame/background/clipShape）不参与。
/// 放回父视图里读会让整张卡片失效 —— 那正是这次优化要消除的东西。
private struct RateText: View {
    @Environment(AppState.self) private var state
    let isUp: Bool
    let accent: Color
    var body: some View {
        Text(isUp ? state.upText : state.downText)
            .font(.system(size: 24))
            .foregroundStyle(accent)
            .lineLimit(1)
            .truncationMode(.tail)
    }
}

/// 背景曲线。同理单独成层：`samples`/`tick` 只在这一层被读到。
private struct CurveLayer: View {
    @Environment(AppState.self) private var state
    let isUp: Bool
    let lineColor: Color
    var body: some View {
        BandwidthChart(samples: isUp ? state.bandwidthUp : state.bandwidthDown,
                       lineColor: lineColor,
                       tick: state.pollTick,
                       minimal: true,
                       headroom: 0.62)
    }
}
