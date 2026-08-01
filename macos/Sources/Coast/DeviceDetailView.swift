import CoastKit
import SwiftUI

/// 设备详情。**逐元素对齐** `qml/DeviceDetailWindow.qml`：600×720、内距 12、列间距 12，
/// 自上而下 —— 头部（头像 / 名字 / 代理开关）→ 依赖说明 / 不可代理的原因 → 信息网格 →
/// 备注名 / 类型 → 实时流量卡 → 近 7 天 → 策略 → 常用域名 → 该设备的实时连接列表。
///
/// **独立顶层窗**，与 Qt 一致（做成 sheet 会被 510 高的主窗裁掉一大截）。
/// 显示的永远是 `AppState.selectedDevice` —— 窗口开着时点列表里另一台，内容跟着换，
/// 不用来回开关窗口。页面那边因此只留列表：备注名/策略都搬进了这里。
struct DeviceDetailView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    /// 当前选中的设备。没有选中时给一句空态，而不是整窗空白。
    private var selected: AppState.SelectedDevice? { state.selectedDevice }
    private var discovered: LanBrowser.Device { selected?.discovered ?? LanBrowser.Device(mac: "", ip: "", interface: "") }
    /// 生效的类型：用户手动指定过就用它，否则用自动识别的结果。
    private var typeKey: String {
        let override = record?.typeOverride ?? ""
        return override.isEmpty ? discovered.typeKey : override
    }
    private var online: Bool { selected?.online ?? false }
    private var rejection: RedirectTargets.Rejection? { selected?.rejection }
    private var mac: String { discovered.mac }

    @State private var alias = ""
    @State private var recentDays: [HistoryStore.DayTotal] = []
    @State private var topDomains: [HistoryStore.GroupTotal] = []
    @State private var totals: (up: Int64, down: Int64) = (0, 0)

    private var record: DeviceStore.Device? { state.devices.device(mac: mac) }
    private var proxyEnabled: Bool { record?.proxyEnabled ?? false }
    /// 已经开着的一律可关（离线 / 跨网段也得能撤销）；关着的只有「可代理且在线」才点得动
    /// —— 离线设备拿不到 IP/ARP，劫持无从下手。
    private var canToggle: Bool { proxyEnabled || (rejection == nil && online) }

    private var sample: DeviceTraffic.Sample { state.deviceTraffic.sample(ip: discovered.ip) }

    /// 这台设备当前的连接。按发起方 IP 认 —— 透明重定向看不到任何凭据，只有源 IP。
    /// 这台设备此刻在跟谁说话。**新的在最上面**（start 倒序，同刻按 id 兜底）。
    ///
    /// ★ 原来是直接拿 `/connections` 的原始顺序 —— 那是 Go map 的快照，**顺序是随机的**，
    ///   于是这张表每一拍都在重排，看着像在抽风（Qt 的注释专门讲了这件事，
    ///   它那边还额外做了 reconcile：存活的行原地不动，新行插到最上面）。
    private var connections: [ConnectionRow] {
        state.connections
            .filter { $0.sourceIP == discovered.ip }
            .sorted { $0.start == $1.start ? $0.id > $1.id : $0.start > $1.start }
    }

    var body: some View {
        Group {
            if selected == nil {
                Text("未选择设备".t)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.textMuted)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                content
            }
        }
        .frame(minWidth: 420, minHeight: 420)
        .background(theme.card)
        .task(id: mac) { await load() }
    }

    private var content: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                header
                notices
                infoGrid
                aliasRow
                typeRow
                trafficCard
                recentDaysBlock
                policyRow
                topDomainsBlock
                connectionsBlock
                Color.clear.frame(height: 4)
            }
            .padding(12)
        }
    }

    // MARK: 头部

    private var header: some View {
        HStack(spacing: 10) {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(theme.deviceColor(typeKey))
                .frame(width: 48, height: 48)
                .overlay {
                    Image(systemName: theme.deviceSymbol(typeKey))
                        .font(.system(size: 26))
                        .foregroundStyle(.white)
                }

            VStack(alignment: .leading, spacing: 2) {
                Text(displayName)
                    .font(.system(size: 18))
                    .foregroundStyle(theme.textPrimary)
                    .lineLimit(1).truncationMode(.tail)
                Text(Self.typeName(typeKey) + "  ·  "
                     + (online ? "在线".t : "离线".t))
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
                        .lineLimit(1)
                        .font(.system(size: 10))
                        .foregroundStyle(theme.textMuted)
                }
            }
        }
    }

    private var displayName: String {
        let alias = record?.alias ?? ""
        return alias.isEmpty ? discovered.displayName : alias
    }

    // MARK: 提示区

    /// 透明接管是否真的能生效。
    ///
    /// macOS 上 ARP 欺骗与 PF 重定向全在**特权 helper**里跑（见 `Redirector`），
    /// 没装 helper 就只是在台账里记了一笔「要代理这台」，流量根本不会拐过来 ——
    /// 与 Qt 的 `devices.gatewayReady`（网关模块可用性）是同一个意思。
    private var gatewayReady: Bool { state.controller.helperStatus == .enabled }

    @ViewBuilder
    private var notices: some View {
        // ★ 开着代理但网关还没就绪：**如实说明它现在还没生效**（Qt 的琥珀色提示）。
        //   原来这里不分状态、一律显示下面那条「联网由本机转发」—— 而在没有 helper 时
        //   那句话是**假的**：什么都没被接管。两种状态两句话，不能混。
        if proxyEnabled, !gatewayReady {
            noticeBox(background: Color(red: 200 / 255, green: 154 / 255,
                                        blue: 84 / 255, opacity: 0.15),
                      color: Color(hex: 0xC6_9A_54)) {
                "已标记代理此设备；透明网关模块启用后将自动接管其流量。".t
            }
        }

        // 代理生效中的**依赖说明**：这台设备的默认网关已经指到本机，它的每个包都要本机
        // 转发 —— 用户必须知道「本机不在 = 它上不了网」，以及各种退出方式的实际后果。
        if proxyEnabled, gatewayReady {
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
            ("IP", discovered.ip),
            ("MAC", discovered.mac),
            ("厂商".t, discovered.vendor),
            // Qt 的第四格就是「型号」（`DeviceDetailWindow.qml:264`），位置照搬。
            ("型号".t, discovered.model),
            ("首次发现".t, record.map { Self.dateText($0.firstSeen) } ?? ""),
            ("主机名".t, discovered.hostname),
            // 第七格是 macOS 独有的：Qt 那边没有「接口」这一项，但本机可能同时挂着
            // Wi-Fi、有线和雷雳网桥，设备是从哪张网卡看到的直接决定 ARP 欺骗往哪儿发。
            // 放在 Qt 六项**之后**，前六格的位置与 Qt 逐格对齐。
            ("接口".t, discovered.interface),
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
            Text("备注名".t).font(.system(size: 12)).foregroundStyle(theme.textMuted).lineLimit(1)
                .frame(width: 64, alignment: .leading)
            ThemedField(text: $alias, placeholder: "为该设备起个名字".t, width: nil)
                .frame(height: 28)
                // 每敲一个字都写库太浪费；与 Qt 的 `onEditingFinished` 一样，离焦才落。
                .onSubmit {
                    state.devices.setAlias(mac: mac, alias)
                    state.ledgerDidChange()
                }
        }
    }

    // MARK: 类型（手动覆盖自动识别）

    /// 类型覆盖下拉。**Qt 有、Swift 一直没有** —— 自动识别是拿主机名与厂商猜的，
    /// 猜错很常见（一屋子设备的厂商都是 "Apple"），认错了却改不了只能一直看着错图标。
    /// `unknown` = 自动识别，其余为手动指定；选完立刻落库。
    private var typeRow: some View {
        HStack(spacing: 8) {
            Text("类型".t).font(.system(size: 12)).foregroundStyle(theme.textMuted)
                .lineLimit(1)
                .frame(width: 64, alignment: .leading)
            ThemedCombo(options: Self.typeKeys.map(Self.typeOverrideName),
                        selection: Binding(
                            get: {
                                let key = record?.typeOverride ?? ""
                                return Self.typeKeys.firstIndex(of: key.isEmpty ? "unknown" : key) ?? 0
                            },
                            set: { setTypeOverride(Self.typeKeys[$0]) }),
                        width: 150)
            Spacer(minLength: 0)
        }
    }

    /// 顺序与 `qml/DeviceDetailWindow.qml` 的 `typeKeys` 完全一致。
    static let typeKeys = ["unknown", "phone", "tablet", "computer", "router",
                           "tvbox", "speaker", "printer", "camera", "game", "nas", "iot"]

    @MainActor
    static func typeOverrideName(_ key: String) -> String {
        key == "unknown" ? "自动识别".t : typeName(key)
    }

    private func setTypeOverride(_ key: String) {
        var record = recordOrDefault
        // `unknown` 存空串 —— 台账里「空」就是「跟着自动识别走」，不占一个魔法值。
        record.typeOverride = key == "unknown" ? "" : key
        _ = state.devices.save(record)
        state.ledgerDidChange()
    }

    // MARK: 实时流量卡

    private var trafficCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("实时流量".t).font(.system(size: 14)).foregroundStyle(theme.textPrimary).lineLimit(1)
                Spacer(minLength: 0)
                Text("↓ " + Formatting.rate(sample.rateDown))
                    .font(.system(size: 13)).foregroundStyle(Color(hex: 0x5B_B4_4B))
                Text("↑ " + Formatting.rate(sample.rateUp))
                    .font(.system(size: 13)).foregroundStyle(Color(hex: 0xB1_4A_4A))
            }

            // 90 高的实时带宽折线。Qt 那边喂的是这台设备的**下行**速率，
            // 固定 1s 一拍（它专门加了个定时器，因为挂在「选中设备变了」上会让入点节奏乱掉，
            // 曲线一顿一顿）。这里的数据源 `downHistory` 本来就是每拍推一个点，节奏天然是稳的。
            BandwidthChart(samples: sample.downHistory,
                           title: "下载".t,
                           lineColor: Color(hex: 0x5B_B4_4B),
                           tick: state.pollTick)
                .frame(height: 90)

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
                Text("近 7 天".t).font(.system(size: 14)).foregroundStyle(theme.textPrimary).lineLimit(1)
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

    /// 台账里还没有这台设备时用的默认记录。
    ///
    /// ★ Qt 显示策略行的条件只有 `proxyable === true` —— 与「台账里有没有这条记录」无关。
    /// 而这里原来写的是 `let record`，于是**从没被开过代理的设备根本看不到策略选择**
    /// （截图里就是这样：一台可代理的在线设备，策略那一行整个不见了）。
    /// 台账记录是「用户动过它」才产生的，不该反过来决定用户能不能动它。
    private var recordOrDefault: DeviceStore.Device {
        record ?? DeviceStore.Device(mac: mac)
    }

    @ViewBuilder
    private var policyRow: some View {
        if rejection == nil {
            let record = recordOrDefault
            HStack(spacing: 8) {
                Text("策略".t).font(.system(size: 12)).foregroundStyle(theme.textMuted).lineLimit(1)
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
                Text("连接".t).font(.system(size: 14)).foregroundStyle(theme.textPrimary).lineLimit(1)
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
        recentDays = state.history.recentDays(mac: mac)
        topDomains = state.history.topDomains(mac: mac)
        totals = state.history.total(mac: mac)
    }

    private func toggleProxy() {
        guard canToggle else { return }
        _ = state.devices.setProxyEnabled(mac: mac, !proxyEnabled, ip: discovered.ip)
        state.ledgerDidChange()
        Task { await state.controller.rebuildConfig() }
    }

    private func setPolicy(_ mode: DeviceStore.PolicyMode, _ target: String) {
        var record = recordOrDefault
        record.policyMode = mode
        record.policyTarget = target
        // 台账里还没有就顺手建一条 —— 用户选了策略，那条记录本来就该存在了。
        _ = state.devices.save(record)
        state.ledgerDidChange()
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
