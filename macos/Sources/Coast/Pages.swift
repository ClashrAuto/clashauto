import CoastKit
import SwiftUI

// MARK: - 状态页

/// 流量指标 + 实时带宽图。对齐 `qml/StatusPage.qml` 的上半部分；
/// 「今日流量」「连接速览」两块依赖历史库，随阶段 6 补。
struct StatusPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @State private var showingConnections = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 10) {
                    MetricCard(symbol: "arrow.up", title: "上传".t, value: state.upText)
                    MetricCard(symbol: "arrow.down", title: "下载".t, value: state.downText)
                    // 连接卡可点 —— 打开实时连接查看器(对齐 Qt 的 ConnectionsWindow)
                    Button { showingConnections = true } label: {
                        MetricCard(symbol: "link", title: "连接".t, value: String(state.connectionsCount))
                    }
                    .buttonStyle(.plain)
                    .help("查看全部连接".t)
                    MetricCard(symbol: "tray.and.arrow.down", title: "累计下载".t, value: state.totalDownText)
                }

                BandwidthChart(samples: state.bandwidthSamples)
                    .frame(height: 160)
                    .background(theme.metricBg)
                    .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))

                TodayTrafficCard()

                HStack(spacing: 8) {
                    StatusDot(label: "核心".t, on: state.controller.isCoreRunning)
                    StatusDot(label: "网页代理".t, on: state.controller.isProxyEnabled)
                    StatusDot(label: "增强(TUN)".t, on: state.controller.isTunEnabled)
                    if state.controller.isTunEnabled, !state.controller.isPrivileged {
                        // 这个组合是「增强灯亮着却不全局」的根因，必须当场说清楚，
                        // 否则用户完全无从查起。
                        Label("核心非 root 启动，TUN 不会生效".t, systemImage: "exclamationmark.triangle")
                            .font(.system(size: 11))
                            .foregroundStyle(theme.danger)
                    }
                    Spacer()
                }
            }
            .padding(14)
        }
        .sheet(isPresented: $showingConnections) {
            ConnectionsView().environment(state).environment(theme)
        }
    }
}

struct MetricCard: View {
    @Environment(Theme.self) private var theme
    let symbol: String
    let title: String
    let value: String

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 5) {
                Image(systemName: symbol).font(.system(size: 11))
                Text(title).font(.system(size: 11))
            }
            .foregroundStyle(theme.textMuted)

            Text(value)
                .font(.system(size: 18))
                .foregroundStyle(theme.textPrimary)
                .lineLimit(1)
                .minimumScaleFactor(0.6)
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
    }
}

struct StatusDot: View {
    @Environment(Theme.self) private var theme
    let label: String
    let on: Bool

    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(on ? theme.accent : theme.textMuted).frame(width: 7, height: 7)
            Text(label).font(.system(size: 11)).foregroundStyle(theme.textSecondary)
        }
        .padding(.horizontal, 8)
        .frame(height: 22)
        .background(theme.metricBg)
        .clipShape(Capsule())
    }
}

/// 实时带宽折线。对齐 `qml/BandwidthChart.qml`（那边是 Canvas，这边用 `Path`）。
struct BandwidthChart: View {
    @Environment(Theme.self) private var theme
    let samples: [(up: Double, down: Double)]

    var body: some View {
        GeometryReader { geometry in
            // 纵轴按窗口内峰值自适应。给个下限，否则空闲时噪声会被放大成满屏抖动。
            let peak = max(samples.map { max($0.up, $0.down) }.max() ?? 0, 1024)
            ZStack {
                line(samples.map(\.down), in: geometry.size, peak: peak)
                    .stroke(theme.accent, lineWidth: 1.5)
                line(samples.map(\.up), in: geometry.size, peak: peak)
                    .stroke(theme.accentStrong.opacity(0.7), lineWidth: 1.5)
            }
            .overlay(alignment: .topLeading) {
                Text("峰值 \(Formatting.rate(Int64(peak)))")
                    .font(.system(size: 10))
                    .foregroundStyle(theme.textMuted)
                    .padding(6)
            }
        }
    }

    private func line(_ values: [Double], in size: CGSize, peak: Double) -> Path {
        Path { path in
            guard values.count > 1 else { return }
            let step = size.width / CGFloat(values.count - 1)
            for (index, value) in values.enumerated() {
                let x = CGFloat(index) * step
                let y = size.height - CGFloat(value / peak) * size.height
                if index == 0 { path.move(to: CGPoint(x: x, y: y)) }
                else { path.addLine(to: CGPoint(x: x, y: y)) }
            }
        }
    }
}

// MARK: - 节点页

struct NodesPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                if !state.clash.groups.isEmpty {
                    Picker("", selection: Binding(
                        get: { state.clash.selectedGroup },
                        set: { state.clash.setSelectedGroup($0) }
                    )) {
                        ForEach(state.clash.groups, id: \.self) { Text($0).tag($0) }
                    }
                    .labelsHidden()
                    .frame(maxWidth: 240)
                }
                Spacer()
                Button("测延迟".t) { Task { await state.clash.testDelays() } }
                Button(state.clash.speedTesting ? "测速中…".t : "测速".t) {
                    state.clash.startSpeedTestForValidNodes()
                }
                .disabled(state.clash.speedTesting)
            }
            .padding(10)

            Divider().overlay(theme.divider)

            if state.clash.nodes.isEmpty {
                VStack(spacing: 6) {
                    Text("暂无节点".t).foregroundStyle(theme.textMuted)
                    Text(state.controller.isCoreRunning ? "等待核心返回代理列表".t : "核心未运行".t)
                        .font(.system(size: 11))
                        .foregroundStyle(theme.textMuted)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(state.clash.nodes) { node in
                    NodeRow(node: node) { state.selectNode(node.name) }
                        .listRowBackground(Color.clear)
                        .listRowSeparator(.hidden)
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
            }
        }
    }
}

struct NodeRow: View {
    @Environment(Theme.self) private var theme
    let node: NodeInfo
    let onSelect: () -> Void

    var body: some View {
        Button(action: onSelect) {
            HStack(spacing: 10) {
                Circle()
                    .fill(node.active ? theme.accent : theme.textMuted.opacity(0.4))
                    .frame(width: 8, height: 8)

                VStack(alignment: .leading, spacing: 2) {
                    Text(node.name)
                        .font(.system(size: 13))
                        .foregroundStyle(theme.textPrimary)
                        .lineLimit(1)
                    if !node.now.isEmpty {
                        Text("→ \(node.now)")
                            .font(.system(size: 10))
                            .foregroundStyle(theme.textMuted)
                            .lineLimit(1)
                    }
                }

                Spacer(minLength: 8)

                if node.speed > 0 {
                    Text(Formatting.rate(node.speed))
                        .font(.system(size: 11))
                        .foregroundStyle(theme.accentStrong)
                }
                Text(node.delay > 0 ? "\(node.delay) ms" : "—")
                    .font(.system(size: 11))
                    .foregroundStyle(delayColor)
                    .frame(width: 56, alignment: .trailing)
            }
            .padding(.horizontal, 10)
            .frame(height: 40)
            .background(node.active ? theme.nodeRowActive : theme.nodeRowBg)
            .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    /// 延迟配色：越低越绿，超时用中性灰而不是红 —— 红色在这个列表里留给真正的错误。
    private var delayColor: Color {
        switch node.delay {
        case 1..<200: return Color(hex: 0x4DA13E)
        case 200..<500: return Color(hex: 0xC69A54)
        case 500...: return Color(hex: 0xA84343)
        default: return theme.textMuted
        }
    }
}

// MARK: - 日志页

struct LogsPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    var body: some View {
        ScrollViewReader { proxy in
            List(state.logs) { entry in
                HStack(alignment: .top, spacing: 8) {
                    Text(entry.time, format: .dateTime.hour().minute().second())
                        .font(.system(size: 10).monospacedDigit())
                        .foregroundStyle(theme.textMuted)
                    Text(entry.message)
                        .font(.system(size: 11))
                        .foregroundStyle(theme.textSecondary)
                        .textSelection(.enabled)
                }
                .listRowBackground(Color.clear)
                .listRowSeparator(.hidden)
                .id(entry.id)
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .onChange(of: state.logs.count) {
                // 跟到底：日志页多数时候是拿来看最新一条的
                if let last = state.logs.last { proxy.scrollTo(last.id, anchor: .bottom) }
            }
        }
    }
}


/// 今日流量卡：24 小时柱状 + Top N。数据来自历史库，**跨重启保留** ——
/// 核心的 /connections 只有当前活动连接，断了就没了，这张卡不落盘就没法存在。
struct TodayTrafficCard: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    var body: some View {
        @Bindable var bindable = state
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text("今日流量".t).font(.system(size: 12)).foregroundStyle(theme.textMuted)
                Text(Formatting.bytes(state.todayTotal))
                    .font(.system(size: 13)).foregroundStyle(theme.textPrimary)
                Spacer()
                Toggle("只算代理".t, isOn: $bindable.trafficProxyOnly)
                    .toggleStyle(.checkbox).font(.system(size: 11))
                Picker("", selection: $bindable.trafficDimension) {
                    Text("进程".t).tag(HistoryStore.Dimension.process)
                    Text("域名".t).tag(HistoryStore.Dimension.host)
                }
                .labelsHidden().pickerStyle(.segmented).frame(width: 120)
            }

            hourlyBars

            if state.todayTop.isEmpty {
                Text(state.history.isOpen ? "今天还没有已结束的连接".t : "历史库不可用".t)
                    .font(.system(size: 11)).foregroundStyle(theme.textMuted)
            } else {
                ForEach(state.todayTop) { item in
                    HStack(spacing: 6) {
                        Text(item.key)
                            .font(.system(size: 11)).foregroundStyle(theme.textSecondary)
                            .lineLimit(1).truncationMode(.middle)
                        Spacer(minLength: 8)
                        Text(Formatting.bytes(item.bytes))
                            .font(.system(size: 11).monospacedDigit())
                            .foregroundStyle(theme.textMuted)
                    }
                }
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
    }

    private var hourlyBars: some View {
        // 纵轴按当日峰值自适应；全 0 时给 1 兜底，免得除零。
        let peak = max(state.todayHourly.max() ?? 0, 1)
        return HStack(alignment: .bottom, spacing: 2) {
            ForEach(Array(state.todayHourly.enumerated()), id: \.offset) { hour, bytes in
                RoundedRectangle(cornerRadius: 1)
                    .fill(bytes > 0 ? theme.accent : theme.textMuted.opacity(0.15))
                    .frame(height: max(2, CGFloat(bytes) / CGFloat(peak) * 44))
                    .help("\(hour) 点：\(Formatting.bytes(bytes))")
            }
        }
        .frame(height: 44)
    }
}
