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
    /// 打开独立的连接窗（Qt 那边也是独立顶层窗）。
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
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
                    // ★ 这里必须是**速率**。Qt 用的是 `bridge.upText`，那是
                    //   `speedText(up)`（带 `/s`）。原来写的是 `Formatting.bytes(...)`，
                    //   于是把「1.2 MB/s」显示成了「1.2 MB」—— 一个把速率读成总量的错。
                    TrafficCard(symbol: "arrow.up.square", title: "上传".t,
                                value: state.upText,
                                accent: theme.uploadAccent,
                                samples: state.bandwidthSamples.map(\.up),
                                tick: state.pollTick)
                    TrafficCard(symbol: "arrow.down.square", title: "下载".t,
                                value: state.downText,
                                accent: theme.downloadAccent,
                                samples: state.bandwidthSamples.map(\.down),
                                tick: state.pollTick)
                }
                HStack(spacing: 10) {
                    ConnectionsCard(recent: recentRows,
                                    onOpenAll: { openWindow(id: ConnectionsWindowID.value) },
                                    onClearAll: { state.closeAllConnections() },
                                    deviceName: deviceName(for:))
                        .frame(height: 268)
                    LatencyCard(monitor: state.latency)
                        .frame(height: 268)
                }
                HStack(spacing: 10) {
                    CompositionCard().frame(height: 268)
                    TodayTrafficCard().frame(height: 268)
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
            // 页面内距对齐 Qt：左/上/下 10，**右边不留** —— 滚动条要贴页面右缘，
            // 而不是悬在离边 10 的空中；内容列自己收窄 10 补回来。
            .padding(.leading, 10)
            .padding(.vertical, 10)
            .padding(.trailing, 10)
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


    /// 按搜索词与「仅可用」筛过的节点。
    ///
    /// 搜索**不区分大小写**：节点名里中英文混排是常态（`香港01 - HK Airport`），
    /// 大小写敏感的话用户搜 `hk` 会一个都搜不到，而他不会想到是大小写的问题。
    private var visibleNodes: [NodeInfo] {
        // 「仅可用」跟随**设置页**那一项（Qt 就是这么放的），页面上不再单独放一个勾选框。
        NodeFilter.apply(state.clash.nodes, keyword: search,
                         onlyAvailable: state.config.nodeOnlyAvailable)
    }

    var body: some View {
        VStack(spacing: 8) {
            // 顶栏固定 30 高（搜索框 28，展开时不撑高整行）、间距 6 —— 与 Qt 逐项一致。
            HStack(spacing: 6) {
                Text("节点".t)
                    .font(.system(size: 18))
                    .foregroundStyle(theme.textPrimary)
                Text("(\(visibleNodes.count))")
                    .font(.system(size: 9))
                    .foregroundStyle(theme.textMuted)

                // 搜索：默认只有一个放大镜，点开才出输入框（右侧 ✕ 清空并收起）。
                // 常驻输入框会把「节点 (N)」和右边的动作图标挤到一起。
                if searchShown {
                    ZStack(alignment: .trailing) {
                        TextField("搜索节点".t, text: $search)
                            .textFieldStyle(.plain)
                            .font(.system(size: 12))
                            .foregroundStyle(theme.textPrimary)
                            .padding(.leading, 8)
                            .padding(.trailing, 24)
                            .frame(height: 28)
                            .background {
                                RoundedRectangle(cornerRadius: 3, style: .continuous)
                                    .fill(theme.inputBg)
                            }
                            .overlay {
                                RoundedRectangle(cornerRadius: 3, style: .continuous)
                                    .stroke(theme.inputBorder, lineWidth: 1)
                            }
                        Button {
                            search = ""
                            searchShown = false
                        } label: {
                            Image(systemName: "xmark").font(.system(size: 14))
                                .foregroundStyle(theme.textMuted)
                        }
                        .buttonStyle(.plain)
                        .padding(.trailing, 7)
                    }
                    .frame(maxWidth: .infinity)
                } else {
                    Button { searchShown = true } label: {
                        Image(systemName: "magnifyingglass").font(.system(size: 16))
                            .foregroundStyle(theme.textMuted)
                    }
                    .buttonStyle(.plain)
                    .padding(.leading, 4)
                    Spacer(minLength: 0)
                }

                // 测速：空闲 refresh-line、测速中持续旋转（Qt 换的是 loader-4-line 字形，
                // 这里换 SF Symbol 的对应物）。
                Button { state.clash.startSpeedTestForValidNodes() } label: {
                    Image(systemName: state.clash.speedTesting
                          ? "arrow.triangle.2.circlepath" : "arrow.clockwise")
                        .font(.system(size: 19))
                        .foregroundStyle(theme.accent)
                        .rotationEffect(.degrees(state.clash.speedTesting ? 360 : 0))
                        .animation(state.clash.speedTesting
                                   ? .linear(duration: 0.9).repeatForever(autoreverses: false)
                                   : .default,
                                   value: state.clash.speedTesting)
                }
                .buttonStyle(.plain)
                .disabled(state.clash.speedTesting)
                .help("测速".t)

                // 帮助：打开在线文档。Qt 有这颗按钮，Swift 侧一直没有。
                Button {
                    if let url = URL(string: "https://clashr-auto.gitbook.io/") {
                        NSWorkspace.shared.open(url)
                    }
                } label: {
                    Image(systemName: "questionmark.circle.fill")
                        .font(.system(size: 18))
                        .foregroundStyle(theme.textMuted)
                }
                .buttonStyle(.plain)
                .padding(.trailing, 5)
                .help("帮助".t)
            }
            .frame(height: 30)

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
                // Qt: ListView spacing 1 —— 行距紧凑,行占满列表宽
                List(visibleNodes) { node in
                    NodeRow(node: node,
                            switching: state.clash.switchingTo != nil,
                            isTarget: state.clash.switchingTo == node.name,
                            onApply: { state.selectNode(node.name) },
                            onDisable: { state.disableCurrentNode(node) })
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
                    .listRowInsets(EdgeInsets(top: 0.5, leading: 0, bottom: 0.5, trailing: 0))
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
            }
        }
        // Qt: `anchors.margins: 10`，各页自管内距（StackLayout 那边是 0）。
        .padding(10)
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

    @State private var hovering = false

    /// 一行显示的文案。与 Qt `NodeListModel` 的 `DisplayRole` 拼法完全一致。
    private var displayText: String {
        guard !node.now.isEmpty, node.now != node.name else { return node.name }
        return "\(node.name) → \(node.now)"
    }

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
        HStack(spacing: 8) {
            // **一行**，不是两行 —— Qt 的 `DisplayRole` 就是把两段拼成
            // `名字 → 它此刻走到的叶子` 一个字符串（组行才有后半段）。
            //
            // 拆成两行会让「有叶子」和「没叶子」的行**高度不一样**：一屏里行高参差，
            // 而且切换节点导致某一行多出/少掉一行时，它下面所有行的位置都会跳一格。
            Text(displayText)
                .font(.system(size: 12))
                .foregroundStyle(theme.textSecondary)
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: .infinity, alignment: .leading)

            // 药丸：半径 5、宽 = 文字 + 10、高 = 文字 + 6、**深色字压在实色底上**
            // （Qt 写死 `#222222`）—— 底色本身就是延迟档位的颜色，字再用同色就看不清了。
            Text(badgeText)
                .font(.system(size: 12))
                .foregroundStyle(Color(hex: 0x22_22_22))
                .padding(.horizontal, 5)
                .padding(.vertical, 3)
                .background {
                    RoundedRectangle(cornerRadius: 5, style: .continuous).fill(badgeColor)
                }

            // 单个按钮：非活动行「应用」= 切换到该节点；活动（正在使用）行「禁用」=
            // 把它从订阅池摘除并重建配置。宽 82、**撑满行高**、悬停才有底色。
            //
            // 切换在途：目标行显示转圈，其余行文字压到 0.35 透明表示不可点 ——
            // 只禁目标行的话，用户会在等待期间连点好几个节点，排出一串切换请求，
            // 最后停在哪个全看运气。
            Button(action: node.active ? onDisable : onApply) {
                Group {
                    if isTarget {
                        ProgressView().controlSize(.small)
                    } else {
                        Text(node.active ? "禁用".t : "应用".t)
                            .font(.system(size: 12))
                            .foregroundStyle(theme.textSecondary)
                            .lineLimit(1)
                            .truncationMode(.tail)
                            .opacity(switching && !isTarget ? 0.35 : 1)
                    }
                }
                .frame(width: 82)
                .frame(maxHeight: .infinity)
                .background(hovering && !switching ? theme.hover : .clear)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(switching)
            .onHover { hovering = $0 }
        }
        .padding(.leading, 8)
        .frame(height: 40)                       // Qt: NodeRow height: 40
        .background(node.active ? theme.nodeRowActive : theme.nodeRowBg)
        .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))   // Qt: radius 4
    }
}

// MARK: - 日志页

/// 日志页。对齐 `qml/LogsPage.qml`：两个标签「主日志 / Clash 内核」，各一条滚动时间线。
///
/// 尺寸逐项照抄 QML：标签栏上/左/右内距 10、标签间距 6、标签高 30、左右内距 16、字号 13；
/// 标签栏与时间线之间 8；时间线卡**贴到页面左/右/下边缘**（QML 的 `anchors.margins: 0`）。
struct LogsPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    /// 0 = 主日志，1 = Clash 内核。与 QML 的 `TabBar.currentIndex` 同义。
    @State private var tab = 0

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 6) {
                LogTab(title: "主日志".t, isCurrent: tab == 0) { tab = 0 }
                LogTab(title: "Clash 内核".t, isCurrent: tab == 1) { tab = 1 }
                Spacer(minLength: 0)
            }
            .padding(.top, 10)
            .padding(.horizontal, 10)

            LogTimeline(entries: tab == 0 ? state.logs : state.coreLogs)
        }
    }
}

/// 日志页的标签按钮。对齐 QML 里那个 `component LogTab: TabButton`：
/// 选中态是「卡底 + 强调色文字 + 底部一条 2px 强调条」，未选中悬停给一层浅底。
private struct LogTab: View {
    @Environment(Theme.self) private var theme
    let title: String
    let isCurrent: Bool
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 13))
                .foregroundStyle(isCurrent ? theme.accentStrong : theme.textSecondary)
                .padding(.horizontal, 16)
                .frame(height: 30)
                .background {
                    RoundedRectangle(cornerRadius: theme.radius, style: .continuous)
                        .fill(isCurrent ? theme.card : (hovering ? theme.hover : .clear))
                }
                // 底部强调条：QML 里宽度是 `parent.width - 20`，即左右各让 10。
                .overlay(alignment: .bottom) {
                    if isCurrent {
                        RoundedRectangle(cornerRadius: 1, style: .continuous)
                            .fill(theme.accent)
                            .frame(height: 2)
                            .padding(.horizontal, 10)
                    }
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
    }
}

/// 日志时间线。对齐 `qml/LogTimeline.qml`：不透明卡内一个滚动列表，
/// 左侧一颗按严重级别上色的圆点（红=失败/错误、橙=警告、绿=成功/完成、蓝=信息），
/// 右侧时间戳 + 正文。**最新在顶部**，新条目到达自动滚回顶部。
struct LogTimeline: View {
    @Environment(Theme.self) private var theme
    let entries: [AppState.LogEntry]

    var body: some View {
        Card {
            ScrollViewReader { proxy in
                ScrollView {
                    // 行间距 0：行与行的间隔由每行自己的下内距 10 提供（与 QML 一致）。
                    LazyVStack(spacing: 0) {
                        ForEach(entries) { entry in
                            row(entry).id(entry.id)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.top, 6)
                    .padding(.bottom, 10)
                }
                // 新日志插在第 0 行 → 把视图拉回顶部（QML 的 `positionViewAtBeginning()`）。
                .onChange(of: entries.count) {
                    if let newest = entries.first { proxy.scrollTo(newest.id, anchor: .top) }
                }
            }
            .overlay {
                if entries.isEmpty {
                    Text("暂无日志".t)
                        .font(.system(size: 13))
                        .foregroundStyle(theme.textMuted)
                }
            }
        }
    }

    private func row(_ entry: AppState.LogEntry) -> some View {
        HStack(alignment: .top, spacing: 8) {
            // 圆点槽：宽 14，圆点 8×8 落在 (3, 5) —— 圆心 y≈9，与时间戳那行对齐。
            Color.clear
                .frame(width: 14, height: 1)
                .overlay(alignment: .topLeading) {
                    Circle()
                        .fill(Color(hex: entry.severity.colorHex))
                        .frame(width: 8, height: 8)
                        .offset(x: 3, y: 5)
                }

            VStack(alignment: .leading, spacing: 2) {
                Text(entry.time)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                Text(entry.message)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.top, 4)
            .padding(.bottom, 10)
        }
        .padding(.horizontal, 10)
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
            // 头部照 Qt：图标 + 标题 + 大数 | 右侧「全部 / 仅代理」分段。
            // 口径用分段而不是勾选框 —— 「全部」和「仅代理」是两个并列的视角，
            // 勾选框把「全部」变成了「没勾上的那个」，读起来是另一回事。
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: "chart.bar")
                    .font(.system(size: 16)).foregroundStyle(theme.accent)
                VStack(alignment: .leading, spacing: 2) {
                    Text("今日流量".t).font(.system(size: 13)).foregroundStyle(theme.accent)
                    Text(Formatting.bytes(state.todayTotal))
                        .font(.system(size: 22, weight: .medium)).foregroundStyle(theme.accent)
                }
                Spacer()
                GlassSegmented(items: [(false, "全部".t), (true, "仅代理".t)],
                               selection: Binding(get: { state.trafficProxyOnly },
                                                  set: { state.trafficProxyOnly = $0 }),
                               tint: theme.accent.opacity(0.45))
            }

            hourlyBars

            // 维度 tab：进程 / 设备 / 域名。「设备」这一维的数据(conn.mac)一直在写，
            // 却从来没有查询用它 —— 于是「这些流量是哪台设备跑的」在界面上无从回答。
            GlassSegmented(items: [(HistoryStore.Dimension.process, "进程".t),
                                   (HistoryStore.Dimension.device, "设备".t),
                                   (HistoryStore.Dimension.host, "域名".t)],
                           selection: $bindable.trafficDimension,
                           tint: theme.accent.opacity(0.45))

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
        // Qt 这几张卡：内距 12、`radius: 4`（比 Theme.radius(5) 小一档）。
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
    }

    private var hourlyBars: some View {
        // 纵轴按当日峰值自适应；全 0 时给 1 兜底，免得除零。
        let peak = max(state.todayHourly.max() ?? 0, 1)
        return HStack(alignment: .bottom, spacing: 2) {
            ForEach(Array(state.todayHourly.enumerated()), id: \.offset) { hour, bytes in
                RoundedRectangle(cornerRadius: 1)
                    .fill(bytes > 0 ? theme.accent : theme.textMuted.opacity(0.15))
                    .frame(height: max(1, CGFloat(bytes) / CGFloat(peak) * 34))
                    .help(String(format: "%d 点：%@".t, hour, Formatting.bytes(bytes)))
            }
        }
        .frame(height: 34)
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
        // Qt 这几张卡：内距 12、`radius: 4`（比 Theme.radius(5) 小一档）。
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
    }

    private func legend(_ color: Color, _ label: String, _ bytes: Int64) -> some View {
        HStack(spacing: 5) {
            Circle().fill(color).frame(width: 7, height: 7)
            Text(label).font(.system(size: 10)).foregroundStyle(theme.textMuted)
            Text(Formatting.bytes(bytes)).font(.system(size: 10)).foregroundStyle(theme.textSecondary)
        }
    }
}
