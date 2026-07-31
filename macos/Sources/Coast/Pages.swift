import CoastKit
import SwiftUI

// MARK: - 状态页

/// 流量指标 + 实时带宽图。对齐 `qml/StatusPage.qml` 的上半部分；
/// 「今日流量」「连接速览」两块依赖历史库，随阶段 6 补。
struct StatusPage: View {
    /// 最近建立的 5 条。
    private var recentRows: [ConnectionRow] { ConnectionRow.recent(state.connections, limit: 5) }
    /// 跑量最多的 5 条。
    private var topRows: [ConnectionRow] { ConnectionRow.top(state.connections, limit: 5) }

    /// 发起方的设备名。判定逻辑在 `ConnectionRow.deviceLabel` —— 状态页两张卡共用一份。
    private func deviceName(for row: ConnectionRow) -> String {
        ConnectionRow.deviceLabel(for: row, proxied: state.proxiedDeviceLabels)
    }

    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @State private var showingConnections = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                // ★ 双列网格,与 Qt 的 `GridLayout { columns: 2; rowSpacing/columnSpacing: 10 }`
                //   一致;卡高也照搬(上传/下载 170,其余 268)。
                //
                //   之前是单列竖排 + 上传/下载做成小卡再配一张独立的带宽图 —— 那是照着 QML
                //   的元素清单猜的。真跑起来截图才看清:曲线是**画在上传/下载卡背景里**的,
                //   Qt 注释也讲了理由:同一个数字的「此刻」和「最近 40 秒」挨在一起看,
                //   还省下整整一屏的竖向空间。
                // 双列布局。**不用 `Grid`** —— 它按行等分可用高度，一旦某行内容更高，
                // 其余行之间就会被撑出不均匀的空隙（实测第二、三行之间空了近 100pt）。
                // 两个 HStack 各自定高，间距完全可控，也更贴近 Qt 的 GridLayout 行为
                // （那边每张卡都写死 Layout.preferredHeight）。
                HStack(spacing: 10) {
                    TrafficCard(symbol: "arrow.up.square", title: "上传".t,
                                value: Formatting.bytes(state.clash.up),
                                accent: theme.uploadAccent,
                                samples: state.bandwidthSamples.map(\.up))
                    TrafficCard(symbol: "arrow.down.square", title: "下载".t,
                                value: Formatting.bytes(state.clash.down),
                                accent: theme.downloadAccent,
                                samples: state.bandwidthSamples.map(\.down))
                }
                HStack(spacing: 10) {
                    ConnectionsCard(recent: recentRows,
                                    onOpenAll: { showingConnections = true },
                                    onClearAll: { state.closeAllConnections() },
                                    deviceName: deviceName(for:))
                        .frame(height: 230)
                    LatencyCard(monitor: state.latency)
                        .frame(height: 230)
                }
                HStack(spacing: 10) {
                    CompositionCard().frame(height: 230)
                    TodayTrafficCard().frame(height: 230)
                }

                // 三盏状态灯已移除:页脚本来就有「核心 / 网页 / 增强」三个开关，
                // 状态页再放一排只读的灯是重复,Qt 那边也没有。
                // 两条警示保留 —— 它们是别处看不到的信息。
                if state.controller.isCoreRunning, state.clash.coreUnresponsive {
                    Label("核心无响应(进程在,但 API 不通)".t, systemImage: "exclamationmark.triangle")
                        .font(.system(size: 11)).foregroundStyle(theme.danger)
                }
                if state.controller.isTunEnabled, !state.controller.isPrivileged {
                    Label("核心非 root 启动，TUN 不会生效".t, systemImage: "exclamationmark.triangle")
                        .font(.system(size: 11)).foregroundStyle(theme.danger)
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
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
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
                Text(String(format: "峰值 %@".t, Formatting.rate(Int64(peak))))
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

    @State private var search = ""
    /// 搜索框是否展开。Qt 默认只显示一个放大镜，点开才出输入框（右侧 ✕ 清空并收起）——
    /// 节点页顶栏本来就挤，常驻一个输入框会把「节点 (N)」和右边的动作图标挤到一起。
    @State private var searchShown = false
    /// 只看测得通的节点。对应配置项 `nodeOnlyAvailable`（`node:`）——
    /// 那个配置一直存在，却从来没有界面开关，等于用户改不了。
    @State private var onlyAvailable = false

    /// 按搜索词与「仅可用」筛过的节点。
    ///
    /// 搜索**不区分大小写**：节点名里中英文混排是常态（`香港01 - HK Airport`），
    /// 大小写敏感的话用户搜 `hk` 会一个都搜不到，而他不会想到是大小写的问题。
    private var visibleNodes: [NodeInfo] {
        NodeFilter.apply(state.clash.nodes, keyword: search, onlyAvailable: onlyAvailable)
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text("节点".t)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(theme.textPrimary)
                Text("(\(visibleNodes.count))")
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textMuted)

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
                if searchShown {
                    HStack(spacing: 4) {
                        TextField("搜索节点".t, text: $search)
                            .textFieldStyle(.plain)
                            .frame(width: 150)
                        Button {
                            search = ""
                            searchShown = false
                        } label: {
                            Image(systemName: "xmark.circle.fill").font(.system(size: 11))
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(theme.textMuted)
                    }
                    .padding(.horizontal, 6)
                    .padding(.vertical, 3)
                    .background(theme.metricBg)
                    .clipShape(RoundedRectangle(cornerRadius: 5, style: .continuous))
                } else {
                    Button { searchShown = true } label: {
                        Image(systemName: "magnifyingglass").font(.system(size: 12))
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(theme.textMuted)
                    .help("搜索节点".t)
                }

                Toggle("仅可用节点".t, isOn: $onlyAvailable)
                    .toggleStyle(.checkbox)
                    .font(.system(size: 11))
                Spacer()

                Button { Task { await state.clash.testDelays() } } label: {
                    Image(systemName: "bolt").font(.system(size: 12))
                }
                .buttonStyle(.plain)
                .foregroundStyle(theme.textMuted)
                .help("测延迟".t)

                // 测速：空闲显示刷新图标，测速中持续旋转（对齐 Qt 的 refresh-line / loader-4-line）
                Button { state.clash.startSpeedTestForValidNodes() } label: {
                    Image(systemName: state.clash.speedTesting
                          ? "arrow.triangle.2.circlepath" : "arrow.clockwise")
                        .font(.system(size: 12))
                        .rotationEffect(.degrees(state.clash.speedTesting ? 360 : 0))
                        .animation(state.clash.speedTesting
                                   ? .linear(duration: 0.9).repeatForever(autoreverses: false)
                                   : .default,
                                   value: state.clash.speedTesting)
                }
                .buttonStyle(.plain)
                .foregroundStyle(theme.textMuted)
                .disabled(state.clash.speedTesting)
                .help(state.clash.speedTesting ? "测速中…".t : "测速".t)
            }
            .padding(10)

            Divider().overlay(theme.divider)

            if visibleNodes.isEmpty {
                VStack(spacing: 6) {
                    // 「一个都没有」与「筛没了」是两回事，提示必须分开 ——
                    // 否则用户搜错一个字就以为节点全丢了。
                    if state.clash.nodes.isEmpty {
                        Text("暂无节点".t).foregroundStyle(theme.textMuted)
                        Text(state.controller.isCoreRunning ? "等待核心返回代理列表".t : "核心未运行".t)
                            .font(.system(size: 11))
                            .foregroundStyle(theme.textMuted)
                    } else {
                        Text("没有匹配的节点".t).foregroundStyle(theme.textMuted)
                        Text(String(format: "共 %d 个节点被筛掉".t, state.clash.nodes.count))
                            .font(.system(size: 11))
                            .foregroundStyle(theme.textMuted)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(visibleNodes) { node in
                    NodeRow(node: node,
                            switching: state.clash.switchingTo != nil,
                            isTarget: state.clash.switchingTo == node.name,
                            onApply: { state.selectNode(node.name) },
                            onDisable: { state.disableCurrentNode(node) })
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
            }
        }
    }
}

/// 节点列表行。严格对齐 Qt 的 `NodeRow.qml`：
/// 名称（省略号）+ 延迟/速度**药丸** + **单个按钮**（非活动行「应用」/ 活动行「禁用」）。
///
/// 关键行为也照搬：
/// - 「禁用」只出现在**正在使用**的那一行 —— 它把该节点从订阅池摘除并重建配置，
///   是个破坏性动作，放在任意一行上太容易误触；
/// - 切换在途时，**目标行转圈、其余行全部不可点**。只禁目标行的话，用户会在等待期间
///   连点好几个节点，排出一串切换请求，最后停在哪个全看运气。
struct NodeRow: View {
    @Environment(Theme.self) private var theme

    let node: NodeInfo
    /// 是否有切换/禁用在途（此时所有行的按钮都禁用）。
    var switching: Bool = false
    /// 本行是否为切换目标（是则按钮显示转圈帧）。
    var isTarget: Bool = false
    let onApply: () -> Void
    let onDisable: () -> Void

    /// 药丸文案：优先显示实测速度，没有则显示延迟。
    private var badgeText: String {
        if node.speed > 0 { return Formatting.rate(node.speed) }
        if node.delay > 0 { return "\(node.delay) ms" }
        return "-"
    }

    private var badgeColor: Color {
        if node.speed > 0 { return theme.accent }
        return theme.latencyColor(node.delay > 0 ? node.delay : -1)
    }

    var body: some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                Text(node.name)
                    .font(.system(size: 12))
                    .foregroundStyle(node.active ? theme.textPrimary : theme.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                if !node.now.isEmpty {
                    // 组行：显示它此刻实际走到的叶子。禁用时禁的也是这个叶子。
                    Text("→ \(node.now)")
                        .font(.system(size: 10))
                        .foregroundStyle(theme.textMuted)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
            }
            Spacer(minLength: 8)

            Text(badgeText)
                .font(.system(size: 10))
                .foregroundStyle(badgeColor)
                .padding(.horizontal, 5)
                .padding(.vertical, 3)
                .background(badgeColor.opacity(0.15))
                .clipShape(Capsule())

            Button(action: node.active ? onDisable : onApply) {
                Group {
                    if isTarget {
                        ProgressView().controlSize(.small)
                    } else {
                        Text(node.active ? "禁用".t : "应用".t)
                            .font(.system(size: 11))
                    }
                }
                .frame(width: 44)
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
            .disabled(switching)
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 8)
        .background(node.active ? theme.accent.opacity(0.12) : Color.clear)
        .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
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
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
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
                    .help(String(format: "%d 点：%@".t, hour, Formatting.bytes(bytes)))
            }
        }
        .frame(height: 44)
    }
}

/// 本次会话的流量构成:直连 vs 代理,一根占比条 + 两个数值。对齐 Qt 的 directBytes/proxyBytes。
struct CompositionCard: View {
    /// 跑量最多的 5 条。与「最近连接」是两个不同的榜 —— 刚建立的连接往往还没跑量。
    private var topRows: [ConnectionRow] { ConnectionRow.top(state.connections, limit: 5) }

    /// 发起方的设备名。判定逻辑在 `ConnectionRow.deviceLabel` —— 状态页两张卡共用一份。
    private func deviceName(for row: ConnectionRow) -> String {
        ConnectionRow.deviceLabel(for: row, proxied: state.proxiedDeviceLabels)
    }

    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    var body: some View {
        let comp = state.composition
        let total = max(comp.totalBytes, 1)
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: "arrow.left.arrow.right.circle")
                    .font(.system(size: 16)).foregroundStyle(theme.accent)
                VStack(alignment: .leading, spacing: 2) {
                    Text("总流量".t).font(.system(size: 13)).foregroundStyle(theme.accent)
                    Text(Formatting.bytes(comp.totalBytes))
                        .font(.system(size: 22, weight: .medium)).foregroundStyle(theme.accent)
                }
                Spacer()
                Spacer()
            }
            // 占比条:代理(品牌色)+ 直连(灰)
            GeometryReader { geo in
                HStack(spacing: 0) {
                    Rectangle().fill(theme.accent)
                        .frame(width: geo.size.width * CGFloat(comp.proxyBytes) / CGFloat(total))
                    Rectangle().fill(theme.textMuted.opacity(0.4))
                }
            }
            .frame(height: 8)
            .clipShape(RoundedRectangle(cornerRadius: 4))
            HStack(spacing: 14) {
                legend(theme.accent, "代理".t, comp.proxyBytes)
                legend(theme.textMuted.opacity(0.4), "直连".t, comp.directBytes)
                Spacer()
            }

            // 「用量最多」—— Qt 把它放在同一张「总流量」卡里，而不是单开一张：
            // 占比条回答「代理/直连各占多少」，这份列表回答「那些量到底是谁跑的」，
            // 两个问题连着看才有意义。
            Divider().overlay(theme.divider).padding(.vertical, 2)
            Text("用量最多".t)
                .font(.system(size: 11))
                .foregroundStyle(theme.textMuted)
            if topRows.isEmpty {
                Text("暂无流量".t)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .padding(.vertical, 12)
            } else {
                ConnLineList(items: topRows, rows: 5, deviceName: deviceName)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
    }

    private func legend(_ color: Color, _ label: String, _ bytes: Int64) -> some View {
        HStack(spacing: 5) {
            Circle().fill(color).frame(width: 7, height: 7)
            Text(label).font(.system(size: 10)).foregroundStyle(theme.textMuted)
            Text(Formatting.bytes(bytes)).font(.system(size: 10)).foregroundStyle(theme.textSecondary)
        }
    }
}
