import CoastKit
import SwiftUI

/// 实时连接查看器。对齐 Qt 的 `ConnectionsWindow`:每条活动连接的 host/进程/出口链/上下行,
/// 可逐条关闭、可全部关闭。以 sheet 呈现(状态页连接卡点开)。
struct ConnectionsView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text("连接".t).font(.system(size: 14, weight: .medium)).foregroundStyle(theme.textPrimary)
                Text("\(state.connections.count)").font(.system(size: 12)).foregroundStyle(theme.textMuted)
                Spacer()
                Button("全部关闭".t) { state.closeAllConnections() }
                    .disabled(state.connections.isEmpty)
                Button("关闭".t) { dismiss() }
            }
            .padding(10)
            Divider().overlay(theme.divider)

            if state.connections.isEmpty {
                VStack(spacing: 6) {
                    Image(systemName: "link").font(.system(size: 26)).foregroundStyle(theme.textMuted)
                    Text("当前没有活动连接".t).foregroundStyle(theme.textMuted)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(state.connections) { row in
                    ConnRow(row: row) { state.closeConnection(id: row.id) }
                        .listRowBackground(Color.clear)
                        .listRowSeparator(.hidden)
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
            }
        }
        .frame(width: 620, height: 460)
    }
}

private struct ConnRow: View {
    @Environment(Theme.self) private var theme
    let row: ConnectionRow
    let onClose: () -> Void

    var body: some View {
        HStack(spacing: 8) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 5) {
                    Text(row.host).font(.system(size: 12)).foregroundStyle(theme.textPrimary)
                        .lineLimit(1).truncationMode(.middle)
                    if !row.process.isEmpty {
                        Text(row.process).font(.system(size: 9)).foregroundStyle(theme.textMuted)
                    }
                }
                HStack(spacing: 4) {
                    badge(row.chain.isEmpty ? row.network : row.chain,
                          row.isProxied ? theme.accent : theme.textMuted)
                    if row.download > 0 { badge("↓ \(Formatting.bytes(row.download))", theme.accentStrong) }
                    if row.upload > 0 { badge("↑ \(Formatting.bytes(row.upload))", theme.danger) }
                }
            }
            Spacer(minLength: 8)
            Button(action: onClose) { Image(systemName: "xmark.circle").font(.system(size: 12)) }
                .buttonStyle(.borderless)
                .help("关闭这条连接".t)
        }
        .padding(.horizontal, 10)
        .frame(height: 44)
        .background(theme.nodeRowBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
        .textSelection(.enabled)
    }

    private func badge(_ text: String, _ color: Color) -> some View {
        Text(text).font(.system(size: 9)).foregroundStyle(color)
            .padding(.horizontal, 5).padding(.vertical, 1)
            .background(RoundedRectangle(cornerRadius: 3).fill(color.opacity(0.15)))
    }
}
