import CoastKit
import SwiftUI

/// 状态页两张卡（「最近连接」「用量最多」）共用的一行。
///
/// 严格对齐 Qt 的 `StatusPage.qml` 里的 `component ConnLine`：
/// 色点(6pt) · 主机名(占满、右截断) · 设备名(定宽 96、右对齐) · 字节数(定宽 72、右对齐)。
///
/// 两处定宽不是随手写的 —— Qt 的注释讲明了理由：设备名不定宽的话，长设备名会把右边的
/// 字节数挤走，几行之间对不齐。这里用 `.frame(width:alignment:)` 复刻同样的效果。
struct ConnLine: View {
    @Environment(Theme.self) private var theme

    let host: String
    let device: String
    let trailing: String
    let direct: Bool

    var body: some View {
        HStack(spacing: 8) {
            // 直连/代理的色点：一眼看出这条走没走代理，比再加一列文字省地方
            Circle()
                .fill(direct ? theme.directDot : theme.accent)
                .frame(width: 6, height: 6)
            Text(host)
                .font(.system(size: 11))
                .foregroundStyle(theme.textSecondary)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)
            Text(device)
                .font(.system(size: 10))
                .foregroundStyle(theme.textMuted)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(width: 96, alignment: .trailing)
            Text(trailing)
                .font(.system(size: 10))
                .foregroundStyle(theme.textMuted)
                .frame(width: 72, alignment: .trailing)
        }
    }
}

/// 固定 `rows` 行的连接列表；不足的补空行。
///
/// **行数固定**是 Qt 那边写死的行为，理由在它的注释里：条数随流量增减的话，卡片高度会跟着
/// 一秒一变，整页跟着抖。补空行而不是让卡片收缩。
struct ConnLineList: View {
    let items: [ConnectionRow]
    let rows: Int
    let deviceName: (ConnectionRow) -> String

    var body: some View {
        VStack(spacing: 3) {
            ForEach(0..<rows, id: \.self) { index in
                if index < items.count {
                    let row = items[index]
                    ConnLine(host: row.host.isEmpty ? "-" : row.host,
                             device: deviceName(row),
                             trailing: Formatting.bytes(row.totalBytes),
                             direct: !row.isProxied)
                } else {
                    // 占位空行：撑住高度，不显示任何内容
                    ConnLine(host: "", device: "", trailing: "", direct: true)
                        .hidden()
                }
            }
        }
    }
}

/// 状态页的「连接」卡。对齐 Qt `StatusPage.qml` 的同名卡。
struct ConnectionsCard: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    /// ★ **不再从父视图接收 `recent`**（原来是 `ConnectionRow.recent(state.connections, 5)`
    ///   在状态页自己的 body 里算好再传进来）。那样一来负载下连接列表每拍都变，
    ///   **整张状态页的 body 跟着失效**，两张流量卡、延迟卡、页面外壳全被拉进 layout pass。
    ///   与 `TrafficCard` 那次同因同解：变化的读取必须落在**用得到它的那一层**，
    ///   `@Observable` 的依赖是按 View 的 body 记录的。
    ///   真机实测（负载下 n=20）：改前 Swift 端均值 6.15%、p90 高达 19.20%，
    ///   而 Qt 端 p90 只有 2.50% —— 尖峰就来自这条整页失效链。
    private var recent: [ConnectionRow] { ConnectionRow.recent(state.connections, limit: 5) }
    let onOpenAll: () -> Void
    let onClearAll: () -> Void
    let deviceName: (ConnectionRow) -> String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .center, spacing: 12) {
                Text("\u{EEB8}") // pulse（对齐 Qt，沿用原卡图标）
                    .font(.custom(IconFont.remix, size: 28))
                    .foregroundStyle(theme.cardIcon)
                VStack(alignment: .leading, spacing: 2) {
                    // ★ **必须 `lineLimit(1)`**。SwiftUI 的 `Text` 默认会换行，而
                    //   QML 的 `Text` 默认**不换行**（没写 `wrapMode` 就是单行）。
                    //   德文的「Verbindungen」在 640 宽下被折成了「Verbindun / gen」，
                    //   把下面那个大号数字整体顶下去，与右边的延迟卡错开一行 ——
                    //   截图才看得出来。其余几张卡的标题同理。
                    Text("连接".t)
                        .lineLimit(1)
                        .font(.system(size: 13))
                        .foregroundStyle(theme.accent)
                    Text(String(state.connectionsCount))
                        .font(.system(size: 24, weight: .regular))
                        .foregroundStyle(theme.accent)
                }
                Spacer()
            }
            // 按钮组压在卡片**右上角**（Qt：`Layout.alignment: AlignTop` 的 52×22 深色小条）。
            // 原来它内嵌在标题行里、还各套一层玻璃胶囊 —— 把标题行撑高，
            // 整张卡的头部比右边的延迟卡矮半截，两张卡顶不齐。
            .overlay(alignment: .topTrailing) {
                HStack(spacing: 0) {
                    Button(action: onOpenAll) {
                        Text("\u{ECB5}") // eye-line
                            .font(.custom(IconFont.remix, size: 14))
                            .foregroundStyle(.white)
                            .frame(width: 26, height: 22)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .help("查看全部连接".t)
                    Rectangle().fill(.white.opacity(0.15)).frame(width: 1, height: 22)
                    Button(action: onClearAll) {
                        // 垃圾桶用危险色 —— 这是不可撤销的批量断开，
                        // 和旁边的「查看」不该长得一样。
                        Text("\u{EC2A}") // delete-bin-line
                            .font(.custom(IconFont.remix, size: 14))
                            .foregroundStyle(Color(hex: 0xFF_6B_6B))
                            .frame(width: 25, height: 22)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .help("全部断开".t)
                }
                .background(RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .fill(.black.opacity(0.32)))
            }

            Divider().overlay(theme.divider)

            Text("最近连接".t)
                .lineLimit(1)
                .font(.system(size: 11))
                .foregroundStyle(theme.textMuted)

            if recent.isEmpty {
                Text("暂无连接".t)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .padding(.vertical, 12)
            } else {
                ConnLineList(items: recent, rows: 5, deviceName: deviceName)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))   // Qt: radius 4
    }
}
