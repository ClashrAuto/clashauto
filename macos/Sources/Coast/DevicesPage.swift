import AppKit
import CoastKit
import SwiftUI
import UniformTypeIdentifiers

/// 局域网设备页。
///
/// 两份数据按 MAC 合并：
///   • **发现**（`LanBrowser`，读系统邻居表）—— 这个网络上现在有谁；
///   • **台账**（`DeviceStore`，SQLite）—— 我们给谁签发过代理凭据、用什么策略。
///
/// 代理是**零配置**的：用户在这里点一下开关，设备端什么都不用改。
/// 底下发生的是 ARP 欺骗 + 内核转发 + PF 重定向到 mihomo 的 redir-port（见 `DeviceStore` 的说明）。
///
/// 正因为设备端不配置，这一页就**不该出现任何要用户去抄的东西** ——
/// 一个开关、一个策略选择，就是全部。
struct DevicesPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    @State private var discovered: [LanBrowser.Device] = []
    @State private var ledger: [String: DeviceStore.Device] = [:]
    @State private var scanning = false
    /// 点开的设备详情（Qt 那边是独立窗口，这里是 sheet）。页面本身只留列表 ——
    /// 备注名 / 策略 / 流量都搬进详情里了，与 Qt 一致。
    @State private var detail: Row?
    @State private var search = ""
    /// 已被「知道了」消掉的告警 id。
    ///
    /// 只记 id 不删数据：告警来自每 30 秒一轮的巡检，删掉下一轮又会冒出来。
    /// 消掉的语义是「这条我看过了」，威胁本身还在（列表里仍然能通过设备状态看出来）。
    @State private var dismissedAlerts: Set<String> = []
    @State private var exportMessage = ""

    /// CSV 导出。用原生保存面板，落盘位置由用户定 —— 直接写到某个固定目录的话，
    /// 用户既找不到，也可能根本没有那个目录的写权限。
    private func exportCSV() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "devices.csv"
        panel.allowedContentTypes = [.commaSeparatedText]
        panel.title = "导出设备列表".t
        guard panel.runModal() == .OK, let url = panel.url else { return }

        // 只导出**我们真的有**的列。Qt 那边还有 type/model/totalDown/totalUp，
        // 但 Swift 的设备台账里没有这些字段 —— 补几列空值出来只会让人以为数据丢了。
        let text = CSV.render(
            header: ["name", "ip", "mac", "vendor", "online", "proxied", "firstSeen"],
            rows: allRows.map { row in
                let record = row.record
                return [
                    record?.alias.isEmpty == false ? record!.alias : row.discovered.displayName,
                    row.discovered.ip,
                    row.discovered.mac,
                    row.discovered.vendor,
                    row.online ? "1" : "0",
                    row.proxyEnabled ? "1" : "0",
                    record.map { ISO8601DateFormatter().string(from: $0.firstSeen) } ?? "",
                ]
            })
        do {
            try text.write(to: url, atomically: true, encoding: .utf8)
            exportMessage = String(format: "已导出到 %@".t, url.path)
        } catch {
            exportMessage = String(format: "导出失败：%@".t, error.localizedDescription)
        }
    }

    private let browser = LanBrowser()

    /// 当前网络的网关（概览头显示 + 判定哪些设备不可接管）。
    /// 只在 `rows` 变化时重算，不在每行里各查一次。
    @State private var gateway: LanTopology.Gateway?
    @State private var localMACs: Set<String> = []
    private var gatewayIP: String { gateway?.ip ?? "" }

    private func rejection(for row: Row) -> RedirectTargets.Rejection? {
        RedirectTargets.rejection(ip: row.discovered.ip, mac: row.discovered.mac,
                                  gatewayIP: gateway?.ip ?? "", gatewayMAC: gateway?.mac ?? "",
                                  localMACs: localMACs)
    }
    /// 代理中的台数。
    ///
    /// 用 `allRows` 而不是筛过的 `rows`：这是「这个网络里有几台在被代理」的统计，
    /// 跟用户此刻在搜什么无关。用 `rows` 的话，一搜索这个数就跳，看着像状态突变。
    private var enabledCount: Int { allRows.filter(\.proxyEnabled).count }

    /// 经搜索与「仅在线」筛过的行。
    private var rows: [Row] {
        allRows.filter { row in
            guard !onlineOnly || row.online else { return false }
            return DeviceFilter.matches(keyword: search, fields: DeviceFilter.haystack(
                alias: row.record?.alias ?? "",
                hostname: row.discovered.hostname,
                vendor: row.discovered.vendor,
                ip: row.discovered.ip,
                mac: row.discovered.mac))
        }
    }

    /// 合并后的行：发现到的 + 只在台账里的（设备离线了但凭据还在，不能让它从界面上消失）。
    private var allRows: [Row] {
        var result = discovered.map { Row(discovered: $0, record: ledger[$0.mac]) }
        let seen = Set(discovered.map(\.mac))
        for (mac, record) in ledger where !seen.contains(mac) {
            var offline = LanBrowser.Device(mac: mac, ip: "", interface: "")
            offline.hostname = record.alias
            result.append(Row(discovered: offline, record: record, online: false))
        }
        return result
    }

    struct Row: Identifiable {
        var discovered: LanBrowser.Device
        var record: DeviceStore.Device?
        var online = true
        var id: String { discovered.mac }
        var proxyEnabled: Bool { record?.proxyEnabled ?? false }
    }

    var body: some View {
        // Qt 的设备页没有任何分隔线：概览条 / 搜索行 / 列表靠 10 的行距分开。
        VStack(spacing: 10) {
            header
            if rows.isEmpty { emptyState } else { list }
            proxyBanner
        }
        .task { await scan() }
    }

    /// 概览条。**逐元素对齐** `qml/DevicesPage.qml` 顶部那一行：
    /// 设备(18) | 在线 N/M | 代理中 N | 今日 ↓x ↑y | ——— | 新设备提醒 + 34×18 开关 | 导出 | 网关 X。
    /// 项间距 16；「在线 / 代理中 / 今日」都是**标签在前、数值在后**，两者都是 12px
    /// —— 原来是「大号数值 + 小标签」，那是另一种读法（像仪表盘），Qt 是一行紧凑的状语。
    private var header: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 16) {
                Text("设备".t)
                    .font(.system(size: 18))
                    .foregroundStyle(theme.textPrimary)
                stat("在线".t, "\(allRows.filter(\.online).count)/\(allRows.count)")
                stat("代理中".t, "\(enabledCount)", valueColor: theme.accent)

                // 全部设备**今日**累计上/下行。实时总速率状态页已经有一份（而且更完整 ——
                // 那是核心的全局速率，不受「能不能归属到某台设备」影响）；这里该回答的是
                // 「今天这个网络一共用了多少」。
                HStack(spacing: 8) {
                    Text("今日".t).font(.system(size: 12)).foregroundStyle(theme.textMuted)
                    Text("↓ " + Formatting.bytes(todayTotals.down))
                        .font(.system(size: 12)).foregroundStyle(Color(hex: 0x5B_B4_4B))
                    Text("↑ " + Formatting.bytes(todayTotals.up))
                        .font(.system(size: 12)).foregroundStyle(Color(hex: 0xB1_4A_4A))
                }

                Spacer(minLength: 0)

                HStack(spacing: 5) {
                    Text("新设备提醒".t).font(.system(size: 11)).foregroundStyle(theme.textMuted)
                    // 34×18 的小开关（比设置页那颗 46×24 小一圈）—— Qt 这里就是两套尺寸。
                    SmallSwitch(isOn: state.config.newDeviceAlert) {
                        state.setNewDeviceAlert(!state.config.newDeviceAlert)
                    }
                }

                // 「导出」在 Qt 里是一段**品牌色文字**，不是按钮 —— 它是个低频动作，
                // 做成按钮会和旁边的开关抢注意力。
                Button { exportCSV() } label: {
                    Text("导出".t).font(.system(size: 11)).foregroundStyle(theme.accent)
                }
                .buttonStyle(.plain)

                Text("网关 ".t + (gatewayIP.isEmpty ? "-" : gatewayIP))
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
            }
            .padding(.trailing, 10)

            // 搜索 / 仅在线 / 重扫：一行三件，两个方钮都是 28×28。
            HStack(spacing: 6) {
                TextField("搜索设备 / IP / 厂商".t, text: $search)
                    .textFieldStyle(.plain)
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textPrimary)
                    .padding(.horizontal, 8)
                    .frame(height: 28)
                    .background {
                        RoundedRectangle(cornerRadius: 3, style: .continuous).fill(theme.inputBg)
                    }
                    .overlay {
                        RoundedRectangle(cornerRadius: 3, style: .continuous)
                            .stroke(theme.inputBorder, lineWidth: 1)
                    }

                SquareToggle(symbol: "circle.fill", on: onlineOnly) { onlineOnly.toggle() }
                SquareToggle(symbol: "arrow.clockwise", on: false, accent: true, spinning: scanning) {
                    Task { await scan() }
                }
                .disabled(scanning)
            }
            .padding(.trailing, 10)

            if !exportMessage.isEmpty {
                Text(exportMessage)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textSecondary)
                    .lineLimit(1).truncationMode(.middle)
            }
        }
        // 右内距**不写在这里**：设备列表要一直铺到页面最右缘，它的滚动条才是贴着窗口右侧的。
        // 上面几行各自补 10 回来（与 Qt 的做法完全一致）。
        .padding(.leading, 10)
        .padding(.top, 10)
    }

    /// 这台设备最近新建的那条连接的目标。没开代理的设备流量不经核心，这里永远是空 ——
    /// 行里那一行也就整条收起来，不占位。
    private func lastHost(for row: Row) -> String {
        guard !row.discovered.ip.isEmpty else { return "" }
        return state.connections
            .filter { $0.sourceIP == row.discovered.ip && !$0.host.isEmpty }
            .max { $0.start < $1.start }?
            .host ?? ""
    }

    /// 今日全网上/下行。几条 SUM 聚合，不必每帧算 —— 跟着扫描那一拍刷新即可。
    @State private var todayTotals: (up: Int64, down: Int64) = (0, 0)

    /// 只看在线设备。
    @State private var onlineOnly = false

    private func stat(_ label: String, _ value: String,
                      valueColor: Color? = nil) -> some View {
        HStack(spacing: 4) {
            Text(label)
                .font(.system(size: 12))
                .foregroundStyle(theme.textMuted)
            Text(value)
                .font(.system(size: 12))
                .foregroundStyle(valueColor ?? theme.textSecondary)
        }
    }

    private var emptyState: some View {
        VStack(spacing: 6) {
            Image(systemName: "wifi.slash").font(.system(size: 26)).foregroundStyle(theme.textMuted)
            // 「一台都没发现」「正在扫描」「被搜索筛没了」是三种情况，提示必须分开 ——
            // 混成一句的话，用户搜错一个字就以为设备全不见了。
            if scanning {
                Text("正在扫描局域网…".t).foregroundStyle(theme.textMuted)
            } else if !search.trimmingCharacters(in: .whitespaces).isEmpty {
                Text("没有匹配的设备".t).foregroundStyle(theme.textMuted)
                Text(String(format: "共 %d 台被筛掉".t, allRows.count))
                    .font(.system(size: 11)).foregroundStyle(theme.textMuted)
            } else {
                Text("未发现设备".t).foregroundStyle(theme.textMuted)
                Text("只读取系统已有的邻居表，所以只看得到最近通信过的设备".t)
                    .font(.system(size: 11)).foregroundStyle(theme.textMuted)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var list: some View {
        List(rows) { row in
            VStack(spacing: 0) {
                DeviceRow(row: row,
                          rejection: rejection(for: row),
                          sample: state.deviceTraffic.sample(ip: row.discovered.ip),
                          lastHost: lastHost(for: row),
                          onToggleProxy: { enabled in setProxy(row: row, enabled: enabled) },
                          onOpenDetail: { detail = row })
            }
            .listRowBackground(Color.clear)
            .listRowSeparator(.hidden)
            // Qt: 列表 `spacing: 4`，行宽 = 列表宽 - 10（行右端与上面几行对齐，
            // 滚动条正好悬在那条 10px 的空隙上）。
            .listRowInsets(EdgeInsets(top: 2, leading: 0, bottom: 2, trailing: 10))
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .sheet(item: $detail) { row in
            DeviceDetailView(row: row, rejection: rejection(for: row))
                .environment(state).environment(theme)
        }
    }

    /// 底部横幅：说明当前状态。设备端零配置，所以这里没有任何要用户抄的东西。
    @ViewBuilder
    private var proxyBanner: some View {
        // 安全告警排在提示条之前 —— 有人正在冒充网关时，那条「已接管 N 台」远没它要紧。
        ForEach(state.securityAlerts.filter { !dismissedAlerts.contains($0.id) }) { alert in
            HStack(spacing: 6) {
                Image(systemName: "exclamationmark.shield.fill")
                    .font(.system(size: 11)).foregroundStyle(theme.danger)
                Text(alert.kind == .gatewaySpoofed
                     ? String(format: "%@ 正在冒充网关，可能在监听或代理你的流量".t, alert.offenderMAC)
                     : String(format: "%@ 也在劫持你代理的设备 %@".t, alert.offenderMAC, alert.subjectIP))
                    .font(.system(size: 11)).foregroundStyle(theme.danger)
                Spacer()
                Button("知道了".t) { dismissedAlerts.insert(alert.id) }
                    .buttonStyle(.plain)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
            }
        }

        HStack(spacing: 6) {
            Image(systemName: enabledCount > 0 ? "network" : "info.circle")
                .font(.system(size: 10)).foregroundStyle(theme.textMuted)
            if enabledCount > 0 {
                Text(String(format: "%d 台设备已接管 · 设备端无需任何配置".t, enabledCount))
                    .font(.system(size: 10)).foregroundStyle(theme.textSecondary)
            } else {
                Text("打开开关即可接管该设备的流量，设备端无需任何配置".t)
                    .font(.system(size: 10)).foregroundStyle(theme.textMuted)
            }
            Spacer()
        }
        .padding(.horizontal, 10)
        .frame(height: 26)
    }

    // MARK: 动作

    private func scan() async {
        scanning = true
        defer { scanning = false }
        gateway = LanTopology.defaultGateway()
        localMACs = LanTopology.localMACs()
        discovered = await browser.scan()
        // 每轮扫描后交给 AppState 判断有没有没见过的设备（首轮只记基线、不提醒）
        state.noticeDevices(discovered.map(\.mac))
        reloadLedger()
        // 今日全网上/下行是几条 SUM 聚合，跟着扫描那一拍刷新即可，不必每帧算。
        todayTotals = state.history.todayUpDown(scope: .all)
    }

    private func reloadLedger() {
        ledger = Dictionary(uniqueKeysWithValues: state.devices.all().map { ($0.mac, $0) })
    }

    private func setProxy(row: Row, enabled: Bool) {
        _ = state.devices.setProxyEnabled(mac: row.discovered.mac, enabled, ip: row.discovered.ip)
        reloadLedger()
        if enabled { detail = row }   // 刚开启就把详情打开（策略在里面选）
        Task { await state.controller.rebuildConfig() }
    }

}

private struct DeviceRow: View {
    @Environment(Theme.self) private var theme
    let row: DevicesPage.Row
    /// 这台设备不可接管的原因（nil = 可以）。由页面算好传进来 ——
    /// 每行自己去查网关和本机 MAC 的话，一次渲染会重复读几十次系统表。
    var rejection: RedirectTargets.Rejection?
    /// 实时速率与最近若干拍的历史（背景那张流量图的数据源）。
    var sample: DeviceTraffic.Sample = .empty
    /// 最后访问的地址（域名，没嗅探到就是目标 IP）。
    var lastHost = ""
    let onToggleProxy: (Bool) -> Void
    let onOpenDetail: () -> Void

    @State private var hovering = false

    /// 已经开着的一律可关（离线/跨网段也得能撤销）；关着的只有「可代理且在线」才点得动。
    private var canToggle: Bool { row.proxyEnabled || (rejection == nil && row.online) }

    var body: some View {
        HStack(spacing: 8) {
            avatar

            // 名称 + 副标题 + 最后访问：**吃掉右侧剩下的全部宽度**，放不下就省略号。
            // 右侧那几列都是刚性定宽的，所以「该被挤的是名字」——不这样的话，
            // 副标题长的行会把右侧几列压窄，同一列在不同行落在不同的 x 上。
            VStack(alignment: .leading, spacing: 1) {
                Text(row.discovered.displayName)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.textPrimary)
                    .lineLimit(1).truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)

                // 副标题是 **IP · 厂商**，不含 MAC（Qt 同）——MAC 是详情页的内容，
                // 放进这一行只会把厂商挤没，而厂商才是「这是台什么设备」的线索。
                Text(subtitle)
                    .font(.system(size: 10))
                    .foregroundStyle(theme.textMuted)
                    .lineLimit(1).truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)

                // 最后访问：没有就整行收起（不占位）—— 大多数设备没开代理，
                // 流量不经核心，这里永远没有值。
                if !lastHost.isEmpty {
                    Text("→ " + lastHost)
                        .font(.system(size: 10))
                        .foregroundStyle(theme.textSecondary)
                        .lineLimit(1).truncationMode(.tail)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            speedColumn

            // 最右一列：**开关和徽章共用同一个 38 宽的槽位**（一台设备要么能开代理、
            // 要么有一个「为什么不能开」的理由，两者互斥）。槽位宽度写死 = 开关宽度，
            // 所以整列的左右边缘在每一行都一样齐。
            ZStack {
                if rejection == nil || row.proxyEnabled {
                    proxySwitch
                } else {
                    reasonBadge
                }
            }
            .frame(width: 38, height: 20)
        }
        .padding(.horizontal, 8)
        .frame(height: 60)                     // Qt: 60（多了「最后访问」一行；**所有行等高**）
        .background {
            ZStack {
                (row.proxyEnabled ? theme.nodeRowBg : (hovering ? theme.hover : theme.nodeRowBg))
                // 背景实时流量图：**只有被代理的设备才画**。其余设备的流量不经核心，
                // 画出来永远是一条贴底的 0 线。速率是 0 也照画 —— 那正是
                // 「已接管、此刻闲着」的样子，不是「没数据」。
                if row.proxyEnabled {
                    DeviceTrafficBg(up: sample.upHistory, down: sample.downHistory)
                }
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 5, style: .continuous))
        .opacity(row.online ? 1 : 0.5)         // 离线行整体淡化
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture(perform: onOpenDetail)
    }

    /// 类型头像 34×34 圆角 8 + 图标 18 + 右下角 11 的在线小圆点（带 2px 描边）。
    /// 头像是这一行里**最先被看到**的东西 —— 一屏十几台设备时，先认出的是色块。
    private var avatar: some View {
        RoundedRectangle(cornerRadius: 8, style: .continuous)
            .fill(theme.deviceColor(row.discovered.typeKey))
            .frame(width: 34, height: 34)
            .overlay {
                Image(systemName: theme.deviceSymbol(row.discovered.typeKey))
                    .font(.system(size: 18)).foregroundStyle(.white)
            }
            .overlay(alignment: .bottomTrailing) {
                Circle()
                    .fill(row.online ? Color(hex: 0x4D_A1_3E) : Color(hex: 0x88_88_88))
                    .frame(width: 11, height: 11)
                    // 描边与卡底同色，把小圆点和色块分开 —— 不描边的话绿点会和
                    // 绿色的手机底色糊在一起。
                    .overlay(Circle().stroke(theme.card, lineWidth: 2))
                    .offset(x: 2, y: 2)
            }
    }

    /// 实时速率：**常驻显示，0 也显示**（`↓ 0 B/s`）。宽度写死 76 ——
    /// 速率文字每一拍都在变宽变窄（`↓ 9.77 KB/s` ↔ `↓ 1.20 MB/s`），
    /// 列宽跟着变整行就在抖。闲着时只是淡下去，位置和占位都不变。
    private var speedColumn: some View {
        VStack(alignment: .trailing, spacing: 0) {
            Text("↓ " + Formatting.rate(sample.rateDown))
                .foregroundStyle(Color(hex: 0x5B_B4_4B))
            Text("↑ " + Formatting.rate(sample.rateUp))
                .foregroundStyle(Color(hex: 0xB1_4A_4A))
        }
        .font(.system(size: 10))
        .lineLimit(1)
        .frame(width: 76, alignment: .trailing)
        .opacity(sample.rateDown > 0 || sample.rateUp > 0 ? 1 : 0.45)
        .animation(.easeInOut(duration: 0.12), value: sample.rateDown > 0 || sample.rateUp > 0)
    }

    /// 手画的代理开关：38×20、半径 10、滑块 16、120ms 过渡。
    private var proxySwitch: some View {
        ZStack(alignment: .leading) {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(row.proxyEnabled ? theme.accent : theme.switchTrackOff)
                .frame(width: 38, height: 20)
            Circle()
                .fill(.white)
                .frame(width: 16, height: 16)
                .offset(x: row.proxyEnabled ? 38 - 16 - 2 : 2)
        }
        .frame(width: 38, height: 20)
        .opacity(canToggle ? 1 : 0.4)
        .animation(.easeInOut(duration: 0.12), value: row.proxyEnabled)
        .contentShape(Rectangle())
        .onTapGesture { if canToggle { onToggleProxy(!row.proxyEnabled) } }
        .help(rejection?.reason.t ?? "代理网络".t)
    }

    /// 不可代理的原因徽章，**占开关的位置**（同一个 38 宽的槽）。
    /// 槽位是死的 38 宽而文案有 12 种语言，塞不下就缩到最小可读字号再省略，
    /// 完整文案挂在悬停提示上（详情窗里另有一整句解释）。
    private var reasonBadge: some View {
        Text(reasonLabel)
            .font(.system(size: 9))
            .foregroundStyle(theme.textSecondary)
            .lineLimit(1).truncationMode(.tail)
            .minimumScaleFactor(7.0 / 9.0)
            .padding(.horizontal, 2)
            .frame(width: 38, height: 20)
            .background {
                RoundedRectangle(cornerRadius: 3, style: .continuous)
                    .fill(Color.black.opacity(0.25))
            }
            .help(reasonLabel)
    }

    private var reasonLabel: String {
        switch rejection {
        case .isLocalMachine: return "本机".t
        case .isGateway: return "网关".t
        // `noAddress` 是「拿不到 IP/ARP」。Qt 那颗徽章在这一档写的是「其它网络」，
        // 完整解释挂在悬停提示和详情窗里。
        default: return "其它网络".t
        }
    }

    private var subtitle: String {
        var parts: [String] = []
        if !row.discovered.ip.isEmpty { parts.append(row.discovered.ip) }
        if !row.discovered.vendor.isEmpty { parts.append(row.discovered.vendor) }
        return parts.joined(separator: "  ·  ")
    }
}

/// 设备行的**背景**实时流量图：下行(绿) / 上行(红) 两条面积曲线叠在一起，共用同一量程。
/// 对齐 `qml/DeviceTrafficBg.qml`：曲线最高只占行高的 0.75，量程下限 128KB/s ——
/// 闲着时几百字节的抖动不会被放大成满屏山峰。
struct DeviceTrafficBg: View {
    let up: [Double]
    let down: [Double]

    /// 量程下限 128 KB/s。
    private static let floorScale = 131_072.0
    private static let headroom = 0.75

    private var scale: Double {
        max(Self.floorScale, (up + down).max() ?? 0)
    }

    var body: some View {
        ZStack {
            area(down, color: Color(hex: 0x5B_B4_4B))
            area(up, color: Color(hex: 0xB1_4A_4A))
        }
        .allowsHitTesting(false)
    }

    private func area(_ samples: [Double], color: Color) -> some View {
        GeometryReader { geo in
            let path = areaPath(samples, in: geo.size)
            ZStack {
                path.fill(LinearGradient(colors: [color.opacity(0.22), color.opacity(0.02)],
                                         startPoint: .top, endPoint: .bottom))
                strokePath(samples, in: geo.size).stroke(color.opacity(0.55), lineWidth: 1)
            }
        }
    }

    private func points(_ samples: [Double], in size: CGSize) -> [CGPoint] {
        guard samples.count > 1 else { return [] }
        let usable = size.height * Self.headroom
        let dx = size.width / CGFloat(samples.count - 1)
        return samples.enumerated().map { index, value in
            CGPoint(x: CGFloat(index) * dx,
                    y: size.height - usable * CGFloat(min(1, value / scale)))
        }
    }

    private func strokePath(_ samples: [Double], in size: CGSize) -> Path {
        var path = Path()
        let pts = points(samples, in: size)
        guard let first = pts.first else { return path }
        path.move(to: first)
        for point in pts.dropFirst() { path.addLine(to: point) }
        return path
    }

    private func areaPath(_ samples: [Double], in size: CGSize) -> Path {
        var path = strokePath(samples, in: size)
        guard !path.isEmpty else { return path }
        path.addLine(to: CGPoint(x: size.width, y: size.height))
        path.addLine(to: CGPoint(x: 0, y: size.height))
        path.closeSubpath()
        return path
    }
}

/// 概览条上那颗 34×18 的小开关（比设置页那颗 46×24 小一圈 —— Qt 这里就是两套尺寸）。
struct SmallSwitch: View {
    @Environment(Theme.self) private var theme
    let isOn: Bool
    let action: () -> Void

    var body: some View {
        ZStack(alignment: .leading) {
            RoundedRectangle(cornerRadius: 9, style: .continuous)
                .fill(isOn ? theme.accent : theme.switchTrackOff)
                .frame(width: 34, height: 18)
            Circle()
                .fill(.white)
                .frame(width: 14, height: 14)
                .offset(x: isOn ? 34 - 14 - 2 : 2)
        }
        .frame(width: 34, height: 18)
        .animation(.easeInOut(duration: 0.12), value: isOn)
        .contentShape(Rectangle())
        .onTapGesture(perform: action)
    }
}

/// 搜索行右侧那两颗 28×28 的方钮（仅在线 / 重扫）。
struct SquareToggle: View {
    @Environment(Theme.self) private var theme
    let symbol: String
    let on: Bool
    var accent = false
    var spinning = false
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: accent ? 15 : 12))
                .foregroundStyle(on ? .white : (accent ? theme.accent : theme.textMuted))
                .rotationEffect(.degrees(spinning ? 360 : 0))
                .animation(spinning ? .linear(duration: 0.9).repeatForever(autoreverses: false)
                           : .default, value: spinning)
                .frame(width: 28, height: 28)
                .background {
                    RoundedRectangle(cornerRadius: 3, style: .continuous)
                        .fill(on ? theme.accent : (hovering ? theme.hover : theme.inputBg))
                }
                .overlay {
                    RoundedRectangle(cornerRadius: 3, style: .continuous)
                        .stroke(on ? theme.accent : theme.inputBorder, lineWidth: 1)
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
    }
}
