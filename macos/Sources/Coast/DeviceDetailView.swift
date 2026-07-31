import CoastKit
import SwiftUI

/// 设备详情。**逐元素对齐** `qml/DeviceDetailWindow.qml`：600×720、内距 12、列间距 12，
/// 自上而下 —— 头部（头像 / 名字 / 代理开关）→ 依赖说明 / 不可代理的原因 → 信息网格 →
/// 备注名 / 类型 → 实时流量卡 → 近 7 天 → 策略 → 常用域名 → 该设备的实时连接列表。
///
/// Qt 那边是独立顶层窗；这里沿用本项目既有做法以 sheet 呈现（设备行点开）。
/// 页面那边因此只留列表 —— 与 Qt 一样，备注名/策略都搬进了这里。
struct DeviceDetailView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    let row: DevicesPage.Row
    var rejection: RedirectTargets.Rejection?

    @State private var alias = ""
    @State private var recentDays: [HistoryStore.DayTotal] = []
    @State private var topDomains: [HistoryStore.GroupTotal] = []
    @State private var totals: (up: Int64, down: Int64) = (0, 0)

    private var record: DeviceStore.Device? { state.devices.device(mac: row.id) }
    private var proxyEnabled: Bool { record?.proxyEnabled ?? false }
    /// 已经开着的一律可关（离线 / 跨网段也得能撤销）；关着的只有「可代理且在线」才点得动
    /// —— 离线设备拿不到 IP/ARP，劫持无从下手。
    private var canToggle: Bool { proxyEnabled || (rejection == nil && row.online) }

    private var sample: DeviceTraffic.Sample { state.deviceTraffic.sample(ip: row.discovered.ip) }

    /// 这台设备当前的连接。按发起方 IP 认 —— 透明重定向看不到任何凭据，只有源 IP。
    private var connections: [ConnectionRow] {
        state.connections.filter { $0.sourceIP == row.discovered.ip }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                header
                notices
                infoGrid
                aliasRow
                trafficCard
                recentDaysBlock
                policyRow
                topDomainsBlock
                connectionsBlock
                Color.clear.frame(height: 4)
            }
            .padding(12)
        }
        // Qt 那边是 600×720 的**独立窗**（最小 420×420）。这里仍是 sheet，所以高度写成
        // 「理想 720、但不超过可用高度」——写死 720 的话，主窗默认才 510 高，
        // sheet 会被裁掉一大截（更新窗就是这么发现的：底部整条动作行消失、按钮点不到）。
        // 内容本来就在 ScrollView 里，压矮只是要多滚两下，不会丢东西。
        .frame(width: 600)
        .frame(minHeight: 420, idealHeight: 720, maxHeight: 720)
        .background(theme.card)
        .task { await load() }
    }

    // MARK: 头部

    private var header: some View {
        HStack(spacing: 10) {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(theme.deviceColor(row.discovered.typeKey))
                .frame(width: 48, height: 48)
                .overlay {
                    Image(systemName: theme.deviceSymbol(row.discovered.typeKey))
                        .font(.system(size: 26))
                        .foregroundStyle(.white)
                }

            VStack(alignment: .leading, spacing: 2) {
                Text(displayName)
                    .font(.system(size: 18))
                    .foregroundStyle(theme.textPrimary)
                    .lineLimit(1).truncationMode(.tail)
                Text(Self.typeName(row.discovered.typeKey) + "  ·  "
                     + (row.online ? "在线".t : "离线".t))
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textMuted)
                Spacer(minLength: 0)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            // 不可代理**且**没开着的设备根本不显示这块（与 Qt 的 `proxyBox.visible` 同）。
            if rejection == nil || proxyEnabled {
                VStack(alignment: .trailing, spacing: 2) {
                    BigSwitch(isOn: proxyEnabled, enabled: canToggle) { toggleProxy() }
                    Text("代理网络".t)
                        .font(.system(size: 10))
                        .foregroundStyle(theme.textMuted)
                }
            }
        }
    }

    private var displayName: String {
        let alias = record?.alias ?? ""
        return alias.isEmpty ? row.discovered.displayName : alias
    }

    // MARK: 提示区

    @ViewBuilder
    private var notices: some View {
        // 代理生效中的**依赖说明**：这台设备的默认网关已经指到本机，它的每个包都要本机
        // 转发 —— 用户必须知道「本机不在 = 它上不了网」，以及各种退出方式的实际后果。
        if proxyEnabled {
            noticeBox(background: theme.metricBg, color: theme.textMuted) {
                var text = "该设备的联网由本机转发。退出 / 关机 / 睡眠都会自动把它交还给路由器（约 1 秒内恢复）；但断电或强制结束进程时，它最多可能断网 30 秒左右。".t
                if !state.config.autoStart {
                    text += "\n" + "建议在「设置」里打开「开机自启」，重启后才能自动接着代理。".t
                }
                return text
            }
        }

        // 「为什么不能开代理」—— 本机 / 网关 / 跨网段 / 离线各给一句人话。
        if let rejection {
            noticeBox(background: Color.white.opacity(0.06), color: theme.textMuted) {
                rejection.reason.t
            }
        }
    }

    private func noticeBox(background: Color, color: Color,
                           _ text: () -> String) -> some View {
        Text(text())
            .font(.system(size: 11))
            .lineSpacing(11 * 0.25)
            .foregroundStyle(color)
            .fixedSize(horizontal: false, vertical: true)
            .padding(7)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background {
                RoundedRectangle(cornerRadius: 5, style: .continuous).fill(background)
            }
    }

    // MARK: 信息网格（2 列，列距 24、行距 6，键列宽 64）

    private var infoGrid: some View {
        let items: [(String, String)] = [
            ("IP", row.discovered.ip),
            ("MAC", row.discovered.mac),
            ("厂商".t, row.discovered.vendor),
            ("接口".t, row.discovered.interface),
            ("首次发现".t, record.map { Self.dateText($0.firstSeen) } ?? ""),
            ("主机名".t, row.discovered.hostname),
        ]
        return LazyVGrid(columns: [GridItem(.flexible(), spacing: 24),
                                   GridItem(.flexible(), spacing: 24)],
                         alignment: .leading, spacing: 6) {
            ForEach(items, id: \.0) { key, value in
                HStack(spacing: 8) {
                    Text(key).font(.system(size: 12)).foregroundStyle(theme.textMuted)
                        .frame(width: 64, alignment: .leading)
                    // 空值显示 "-" 而不是留白 —— 留白分不清「没这一项」和「渲染坏了」。
                    Text(value.isEmpty ? "-" : value)
                        .font(.system(size: 12)).foregroundStyle(theme.textSecondary)
                        .lineLimit(1).truncationMode(.tail)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }

    // MARK: 备注名

    private var aliasRow: some View {
        HStack(spacing: 8) {
            Text("备注名".t).font(.system(size: 12)).foregroundStyle(theme.textMuted)
                .frame(width: 64, alignment: .leading)
            ThemedField(text: $alias, placeholder: "为该设备起个名字".t, width: nil)
                .frame(height: 28)
                // 每敲一个字都写库太浪费；与 Qt 的 `onEditingFinished` 一样，离焦才落。
                .onSubmit { state.devices.setAlias(mac: row.id, alias) }
        }
    }

    // MARK: 实时流量卡

    private var trafficCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("实时流量".t).font(.system(size: 14)).foregroundStyle(theme.textPrimary)
                Spacer(minLength: 0)
                Text("↓ " + Formatting.rate(sample.rateDown))
                    .font(.system(size: 13)).foregroundStyle(Color(hex: 0x5B_B4_4B))
                Text("↑ " + Formatting.rate(sample.rateUp))
                    .font(.system(size: 13)).foregroundStyle(Color(hex: 0xB1_4A_4A))
            }

            // 会话 / 今日 / 累计三块**定宽**，窗口一窄要能折行（Qt 用的是 Flow，
            // 放 RowLayout 里窗口一窄就整排溢出到卡片外）。
            HStack(alignment: .top, spacing: 16) {
                totalsColumn("本次会话".t, up: sample.sessionUp, down: sample.sessionDown)
                totalsColumn("今日".t, up: todayUp, down: todayDown)
                totalsColumn("累计".t, up: totals.up, down: totals.down)
                Spacer(minLength: 0)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background {
            RoundedRectangle(cornerRadius: 6, style: .continuous).fill(theme.metricBg)
        }
    }

    private func totalsColumn(_ title: String, up: Int64, down: Int64) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title).font(.system(size: 10)).foregroundStyle(theme.textMuted)
            Text("↓" + Formatting.bytes(down) + "  ↑" + Formatting.bytes(up))
                .font(.system(size: 12)).foregroundStyle(theme.textSecondary)
        }
    }

    private var todayUp: Int64 { recentDays.last?.up ?? 0 }
    private var todayDown: Int64 { recentDays.last?.down ?? 0 }

    // MARK: 近 7 天

    /// 数据来自历史库（跨重启保留）。**一条记录都没有时整块不显示** ——
    /// 新装 / 没开过代理的设备画 7 根空柱子只是噪音。
    @ViewBuilder
    private var recentDaysBlock: some View {
        let total = recentDays.reduce(Int64(0)) { $0 + $1.total }
        if total > 0 {
            let maximum = max(1, recentDays.map(\.total).max() ?? 1)
            VStack(alignment: .leading, spacing: 6) {
                Text("近 7 天".t).font(.system(size: 14)).foregroundStyle(theme.textPrimary)
                HStack(alignment: .bottom, spacing: 6) {
                    ForEach(recentDays) { day in
                        VStack(spacing: 3) {
                            GeometryReader { geo in
                                RoundedRectangle(cornerRadius: 2, style: .continuous)
                                    .fill(theme.accent)
                                    .opacity(0.75)
                                    // 至少 2px：完全没流量的那天也要画得出来，否则
                                    // 横轴上会凭空缺一格，看着像数据丢了。
                                    .frame(height: max(2, geo.size.height
                                                       * Double(day.total) / Double(maximum)))
                                    .frame(maxHeight: .infinity, alignment: .bottom)
                            }
                            Text(String(day.day.dropFirst(5)))   // MM-DD
                                .font(.system(size: 9)).foregroundStyle(theme.textMuted)
                            Text(Formatting.bytes(day.total))
                                .font(.system(size: 9)).foregroundStyle(theme.textSecondary)
                                .lineLimit(1).truncationMode(.tail)
                        }
                        .frame(maxWidth: .infinity)
                    }
                }
                .frame(height: 56)
            }
        }
    }

    // MARK: 策略

    @ViewBuilder
    private var policyRow: some View {
        if rejection == nil, let record {
            HStack(spacing: 8) {
                Text("策略".t).font(.system(size: 12)).foregroundStyle(theme.textMuted)
                    .frame(width: 64, alignment: .leading)
                ThemedCombo(options: DeviceStore.PolicyMode.allCases.map { $0.title.t },
                            selection: Binding(
                                get: { DeviceStore.PolicyMode.allCases.firstIndex(of: record.policyMode) ?? 0 },
                                set: { setPolicy(DeviceStore.PolicyMode.allCases[$0], record.policyTarget) }),
                            width: 130)

                if record.policyMode == .global {
                    // 没选目标时 applyDevicePolicies 会跳过这条规则，给个明确占位而不是空白。
                    let targets = ["（未选）".t] + state.clash.groups
                    ThemedCombo(options: targets,
                                selection: Binding(
                                    get: { max(0, targets.firstIndex(of: record.policyTarget) ?? 0) },
                                    set: { setPolicy(.global, $0 == 0 ? "" : targets[$0]) }),
                                width: nil)
                }
                Spacer(minLength: 0)
            }
        }
    }

    // MARK: 常用域名

    @ViewBuilder
    private var topDomainsBlock: some View {
        if !topDomains.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                Text("常用域名".t).font(.system(size: 14)).foregroundStyle(theme.textPrimary)
                ForEach(topDomains) { entry in
                    HStack(spacing: 8) {
                        Text(entry.key).font(.system(size: 11)).foregroundStyle(theme.textSecondary)
                            .lineLimit(1).truncationMode(.tail)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        Text(Formatting.bytes(entry.bytes))
                            .font(.system(size: 11)).foregroundStyle(theme.textMuted)
                    }
                }
            }
        }
    }

    // MARK: 该设备的连接

    private var connectionsBlock: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Text("连接".t).font(.system(size: 14)).foregroundStyle(theme.textPrimary)
                Text("(\(connections.count))").font(.system(size: 10)).foregroundStyle(theme.textMuted)
                Spacer(minLength: 0)
                if !connections.isEmpty {
                    Button("全部断开".t) {
                        for connection in connections { state.closeConnection(id: connection.id) }
                    }
                    .buttonStyle(.plain)
                    .font(.system(size: 11))
                    .foregroundStyle(Color(hex: 0xFF_6B_6B))
                }
            }
            .padding(.bottom, 4)

            if connections.isEmpty {
                Text("暂无经代理的连接".t)
                    .font(.system(size: 11)).foregroundStyle(theme.textMuted)
                    .frame(maxWidth: .infinity, minHeight: 96)
            } else {
                // **整份铺开，不自带滚动**：嵌在本来就能滚的详情页里再套一层滚动的话，
                // 滚轮落在谁身上全看指针位置，翻连接得先把指针挪进那个小框（Qt 踩过）。
                ForEach(connections) { connection in
                    connectionRow(connection)
                }
            }
        }
    }

    /// 行高 34、半径 4、左右内距 8。
    private func connectionRow(_ connection: ConnectionRow) -> some View {
        HStack(spacing: 8) {
            VStack(alignment: .leading, spacing: 0) {
                Text(connection.host).font(.system(size: 11)).foregroundStyle(theme.textSecondary)
                    .lineLimit(1).truncationMode(.tail)
                Text(connection.type + "  ·  " + connection.chain)
                    .font(.system(size: 9)).foregroundStyle(theme.textMuted)
                    .lineLimit(1).truncationMode(.tail)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            Text("↓" + Formatting.bytes(connection.download))
                .font(.system(size: 10)).foregroundStyle(Color(hex: 0x5B_B4_4B))
            Text("↑" + Formatting.bytes(connection.upload))
                .font(.system(size: 10)).foregroundStyle(Color(hex: 0xB1_4A_4A))

            Button { state.closeConnection(id: connection.id) } label: {
                Image(systemName: "xmark").font(.system(size: 12))
                    .foregroundStyle(theme.textMuted)
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 8)
        .frame(height: 34)
        .background(theme.nodeRowBg)
        .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
    }

    // MARK: 动作

    private func load() async {
        alias = record?.alias ?? ""
        recentDays = state.history.recentDays(mac: row.id)
        topDomains = state.history.topDomains(mac: row.id)
        totals = state.history.total(mac: row.id)
    }

    private func toggleProxy() {
        guard canToggle else { return }
        _ = state.devices.setProxyEnabled(mac: row.id, !proxyEnabled, ip: row.discovered.ip)
        state.refreshHistoryDeviceMap()
        Task { await state.controller.rebuildConfig() }
    }

    private func setPolicy(_ mode: DeviceStore.PolicyMode, _ target: String) {
        guard var record else { return }
        record.policyMode = mode
        record.policyTarget = target
        _ = state.devices.save(record)
        Task { await state.controller.rebuildConfig() }
    }

    // MARK: 文案

    /// 类型 key → 名称。与 `qml/DeviceDetailWindow.qml` 的 `typeName()` 逐条相同。
    static func typeName(_ key: String) -> String {
        switch key {
        case "phone": return "手机".t
        case "tablet": return "平板".t
        case "computer": return "电脑".t
        case "router": return "路由器".t
        case "tvbox": return "电视/盒子".t
        case "speaker": return "音箱".t
        case "printer": return "打印机".t
        case "camera": return "摄像头".t
        case "game": return "游戏机".t
        case "nas": return "存储/NAS".t
        case "iot": return "智能设备".t
        default: return "未知设备".t
        }
    }

    static func dateText(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.dateFormat = "yyyy-MM-dd HH:mm"
        return formatter.string(from: date)
    }
}

/// 详情页头部那颗大开关：52×26 轨道、22 滑块、120ms 过渡。
/// 比设置页那颗（46×24 / 40×20 轨道）大一圈 —— Qt 那边也是两套尺寸。
private struct BigSwitch: View {
    @Environment(Theme.self) private var theme
    let isOn: Bool
    let enabled: Bool
    let action: () -> Void

    var body: some View {
        ZStack(alignment: .leading) {
            RoundedRectangle(cornerRadius: 13, style: .continuous)
                .fill(isOn ? theme.accent : theme.switchTrackOff)
                .frame(width: 52, height: 26)
            Circle()
                .fill(.white)
                .frame(width: 22, height: 22)
                .offset(x: isOn ? 52 - 22 - 2 : 2)
        }
        .frame(width: 52, height: 26)
        .opacity(enabled ? 1 : 0.4)
        .animation(.easeInOut(duration: 0.12), value: isOn)
        .contentShape(Rectangle())
        .onTapGesture { if enabled { action() } }
    }
}
