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

    let recent: [ConnectionRow]
    let onOpenAll: () -> Void
    let onClearAll: () -> Void
    let deviceName: (ConnectionRow) -> String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .center, spacing: 10) {
                Image(systemName: "link")
                    .font(.system(size: 26))
                    .foregroundStyle(theme.textMuted)
                VStack(alignment: .leading, spacing: 2) {
                    // 按钮组**紧贴标题右侧**，不是甩到卡片最右 —— 对齐 Qt。
                    // 甩到最右的话，标题和它之间隔着一整片空白，读起来像两件不相干的东西。
                    HStack(spacing: 6) {
                        Text("连接".t)
                            .font(.system(size: 13))
                            .foregroundStyle(theme.accent)
                        // 两个动作是一组，用融合玻璃：形状上连成一片，
                        // 比原来「手画一个底 + 中间画一条线」更贴系统。
                        GlassGroup(spacing: 2) {
                            Button(action: onOpenAll) {
                                Image(systemName: "eye").font(.system(size: 12))
                                    .foregroundStyle(theme.textPrimary)
                                    .padding(.horizontal, 7).padding(.vertical, 3)
                            }
                            .buttonStyle(.plain)
                            .glassCapsule()
                            .help("查看全部连接".t)
                            Button(action: onClearAll) {
                                // 垃圾桶用危险色 —— 这是不可撤销的批量断开，
                                // 和旁边的「查看」不该长得一样。
                                Image(systemName: "trash").font(.system(size: 12))
                                    .foregroundStyle(theme.danger)
                                    .padding(.horizontal, 7).padding(.vertical, 3)
                            }
                            .buttonStyle(.plain)
                            .glassCapsule()
                            .help("全部断开".t)
                        }
                    }
                    Text(String(state.connectionsCount))
                        .font(.system(size: 24, weight: .regular))
                        .foregroundStyle(theme.accent)
                }
                Spacer()
            }

            Divider().overlay(theme.divider)

            Text("最近连接".t)
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
