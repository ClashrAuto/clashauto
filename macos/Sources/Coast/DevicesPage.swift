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
    /// 今日每台设备的累计字节。只用来**排序**，不显示 —— 跟着扫描那一拍刷新即可。
    @State private var todayByDevice: [String: Int64] = [:]
    @State private var scanning = false
    /// 打开设备详情窗用。窗口显示的是 `AppState.selectedDevice`，
    /// 所以「点开某一行」= 先写选中、再开窗（与 Qt 的 `openFor(mac)` 完全一致：
    /// `devices.select(mac)` 然后 show + raise）。
    @Environment(\.openWindow) private var openWindow
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
    ///
    /// 排序照搬 Qt `DeviceListModel::buildTarget()`：**在线优先 → 今日流量（MB 档位）降序
    /// → IP 升序 → MAC 兜底**。三点说明，都是 Qt 注释里写明的理由：
    ///
    ///   • **排序键里绝不能出现实时速率** —— 速率每拍都在变，跑流量的设备会一直换位置；
    ///   • 今日流量按 **MB 取档**再比，压掉「两台用量相近的设备来回互超」的抖动；
    ///   • 最后拿 MAC 兜底，保证次序**任何时候都是确定的**。
    ///     原来这里是「发现顺序 + 离线设备按字典遍历顺序追加」——
    ///     Swift 的 `Dictionary` 没有顺序保证，离线那几行每次刷新都可能换个位置。
    private var allRows: [Row] {
        var result = discovered.map { Row(discovered: $0, record: ledger[$0.mac]) }
        let seen = Set(discovered.map(\.mac))
        for (mac, record) in ledger where !seen.contains(mac) {
            var offline = LanBrowser.Device(mac: mac, ip: "", interface: "")
            offline.hostname = record.alias
            result.append(Row(discovered: offline, record: record, online: false))
        }
        return Self.ordered(result, todayBytes: todayByDevice)
    }

    /// 见 `allRows` 的说明。比较键在 `CoastKit.DeviceOrdering`（那边有测试）。
    static func ordered(_ rows: [Row], todayBytes: [String: Int64]) -> [Row] {
        rows.sorted {
            DeviceOrdering.key(online: $0.online, ip: $0.discovered.ip, mac: $0.discovered.mac,
                               todayBytes: todayBytes[$0.discovered.mac] ?? 0)
                < DeviceOrdering.key(online: $1.online, ip: $1.discovered.ip, mac: $1.discovered.mac,
                                     todayBytes: todayBytes[$1.discovered.mac] ?? 0)
        }
    }

    struct Row: Identifiable {
        var discovered: LanBrowser.Device
        var record: DeviceStore.Device?
        var online = true
        var id: String { discovered.mac }
        var proxyEnabled: Bool { record?.proxyEnabled ?? false }
        /// 生效类型 = 用户手动指定优先，否则自动识别（对齐 Qt `DeviceStore::effectiveType()`）。
        /// 详情窗改完类型，列表里的色块图标要立刻跟着变。
        var typeKey: String {
            let override = record?.typeOverride ?? ""
            return override.isEmpty ? discovered.typeKey : override
        }
    }

    var body: some View {
        // 右下角浮动提示压在整页之上（Qt 的 `noticeBar`）。
        ZStack(alignment: .bottomTrailing) {
        // Qt 的设备页没有任何分隔线：概览条 / 搜索行 / 列表靠 10 的行距分开。
        VStack(spacing: 10) {
            // 安全告警横幅在**最上面**（Qt 的顺序：告警 → 概览条 → 搜索行 → 列表）——
            // 有人正在冒充网关时，那条「已接管 N 台」远没它要紧。
            securityBanner
            header
            if rows.isEmpty { emptyState } else { list }
            proxyBanner
        }
        .task { await scan() }
        // 详情窗改完台账后立刻重读（否则列表要等下一轮扫描才更新图标/名字）
        .onChange(of: state.ledgerRevision) { _, _ in reloadLedger() }

            noticeBar
        }
    }

    /// 右下角浮动提示（导出成功 / 出错），自动消失。对齐 `qml/DevicesPage.qml` 的 `noticeBar`：
    /// 距右下各 12、半径 5、黑底 78%、白色 12px 正文。
    ///
    /// **必须限宽 + 换行**：网关那类报错可以很长（Qt 注释举的例子是 Npcap 权限那条），
    /// 单行的话会一路撑到窗口左边界外，长文案直接看不全。
    ///
    /// 停留 **6 秒**而不是三秒半 —— 两三行的报错三秒半读不完（Qt 的注释原话）。
    @ViewBuilder
    private var noticeBar: some View {
        if !exportMessage.isEmpty {
            Text(exportMessage)
                .font(.system(size: 12))
                .foregroundStyle(.white)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: 320, alignment: .leading)
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background {
                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                        .fill(Color.black.opacity(0.78))
                }
                .padding(12)
                .transition(.opacity)
                .task(id: exportMessage) {
                    try? await Task.sleep(for: .seconds(6))
                    exportMessage = ""
                }
        }
    }

    /// 概览条。**逐元素对齐** `qml/DevicesPage.qml` 顶部那一行：
    /// 设备(18) | 在线 N/M | 代理中 N | 今日 ↓x ↑y | ——— | 新设备提醒 + 34×18 开关 | 导出 | 网关 X。
    /// 项间距 16；「在线 / 代理中 / 今日」都是**标签在前、数值在后**，两者都是 12px
    /// —— 原来是「大号数值 + 小标签」，那是另一种读法（像仪表盘），Qt 是一行紧凑的状语。
    private var header: some View {
        VStack(alignment: .leading, spacing: 10) {
            // 窄了就按优先级把次要项收起来（收起顺序：今日 → 网关 → 提醒文字 → 导出）。
            //
            // ★ 断点**不写死像素**，用 `ViewThatFits` 让它按真实排版挑第一个装得下的变体。
            //   Qt 那边同样不写死，是拿各项自己的 `implicitWidth` 现算的，注释里讲了理由：
            //   同一句话在 12 种语言里宽度能差一倍（德语的「新设备提醒」比中文长一大截），
            //   写死的数字必然在某个语言上翻车。
            //
            //   不做自适应的后果是实测出来的：窗口拖到最小（640）时，「今日 ↓42.52 KB」
            //   被压成三行竖排的碎字，「新设备提醒」更是**一个字一行**竖着排下来。
            ViewThatFits(in: .horizontal) {
                overviewBar(today: true, gateway: true, alertLabel: true, export: true)
                overviewBar(today: false, gateway: true, alertLabel: true, export: true)
                overviewBar(today: false, gateway: false, alertLabel: true, export: true)
                overviewBar(today: false, gateway: false, alertLabel: false, export: true)
                overviewBar(today: false, gateway: false, alertLabel: false, export: false)
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

        }
        // 右内距**不写在这里**：设备列表要一直铺到页面最右缘，它的滚动条才是贴着窗口右侧的。
        // 上面几行各自补 10 回来（与 Qt 的做法完全一致）。
        .padding(.leading, 10)
        .padding(.top, 10)
    }

    /// 选中并打开详情窗。窗口已经开着时只是换内容（`selectedDevice` 一变它就跟着重画）。
    private func openDetail(_ row: Row) {
        state.selectedDevice = AppState.SelectedDevice(discovered: row.discovered,
                                                       online: row.online,
                                                       rejection: rejection(for: row))
        openWindow(id: DeviceDetailWindowID.value)
    }

    /// 正被别的机器争抢的设备 IP。取自 ArpWatch 的告警（`deviceContended` 那一类）——
    /// 横幅说「有这回事」，行上的红标说「是哪一台」，两处缺一不可。
    private var contendedIPs: Set<String> {
        Set(state.securityAlerts.filter { $0.kind == .deviceContended }.map(\.subjectIP))
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

    /// 概览条的一个变体。**逐元素对齐** `qml/DevicesPage.qml` 顶部那一行：
    /// 设备(18) | 在线 N/M | 代理中 N | 今日 ↓x ↑y | ——— | 新设备提醒 + 34×18 开关 | 导出 | 网关 X，
    /// 项间距 16；「在线 / 代理中 / 今日」都是**标签在前、数值在后**，两者都是 12px。
    ///
    /// 中间那个 `Spacer` 要给 `minLength` —— `ViewThatFits` 比的是各变体的**理想宽**，
    /// 而 `Spacer()` 的理想宽是 0、怎么都「装得下」，第一个变体就会被无脑选中。
    private func overviewBar(today: Bool, gateway: Bool,
                             alertLabel: Bool, export: Bool) -> some View {
        HStack(spacing: 16) {
            Text("设备".t)
                .lineLimit(1)
                .font(.system(size: 18))
                .foregroundStyle(theme.textPrimary)
            stat("在线".t, "\(allRows.filter(\.online).count)/\(allRows.count)")
            stat("代理中".t, "\(enabledCount)", valueColor: theme.accent)

            // 全部设备**今日**累计上/下行。实时总速率状态页已经有一份（而且更完整 ——
            // 那是核心的全局速率，不受「能不能归属到某台设备」影响）；这里该回答的是
            // 「今天这个网络一共用了多少」。
            if today {
                HStack(spacing: 8) {
                    Text("今日".t).font(.system(size: 12)).foregroundStyle(theme.textMuted).lineLimit(1)
                    Text("↓ " + Formatting.bytes(todayTotals.down))
                        .font(.system(size: 12)).foregroundStyle(Color(hex: 0x5B_B4_4B))
                    Text("↑ " + Formatting.bytes(todayTotals.up))
                        .font(.system(size: 12)).foregroundStyle(Color(hex: 0xB1_4A_4A))
                }
                .fixedSize()
            }

            Spacer(minLength: 16)

            HStack(spacing: 5) {
                if alertLabel {
                    Text("新设备提醒".t).font(.system(size: 11))
                        .foregroundStyle(theme.textMuted).fixedSize()
                }
                // 34×18 的小开关（比设置页那颗 46×24 小一圈）—— Qt 这里就是两套尺寸。
                SmallSwitch(isOn: state.config.newDeviceAlert) {
                    state.setNewDeviceAlert(!state.config.newDeviceAlert)
                }
            }

            // 「导出」在 Qt 里是一段**品牌色文字**，不是按钮 —— 它是个低频动作，
            // 做成按钮会和旁边的开关抢注意力。
            if export {
                Button { exportCSV() } label: {
                    Text("导出".t).font(.system(size: 11)).foregroundStyle(theme.accent)
                }
                .buttonStyle(.plain)
                .fixedSize()
            }

            if gateway {
                Text("网关 ".t + (gatewayIP.isEmpty ? "-" : gatewayIP))
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .fixedSize()
            }
        }
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
                          tick: state.pollTick,
                          contended: contendedIPs.contains(row.discovered.ip),
                          onToggleProxy: { enabled in setProxy(row: row, enabled: enabled) },
                          onOpenDetail: { openDetail(row) })
            }
            .listRowBackground(Color.clear)
            .listRowSeparator(.hidden)
            // Qt: 列表 `spacing: 4`，行宽 = 列表宽 - 10（行右端与上面几行对齐，
            // 滚动条正好悬在那条 10px 的空隙上）。
            .listRowInsets(EdgeInsets(top: 2, leading: 0, bottom: 2, trailing: 10))
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
    }

    /// 安全告警横幅。**逐元素对齐** `qml/DevicesPage.qml` 顶部那一块：
    /// 半径 6、红色淡底（12%）+ 1px 红边（50%）、内距 10、20px 警告图标顶对齐、
    /// **加粗 13px 的标题**「检测到局域网内有异常代理行为」+ 每条威胁一行 11px 说明、
    /// 右上角一颗「知道了」小胶囊（半径 4、同色淡底、悬停加深）。
    ///
    /// 加粗是有理由的：全 UI 只有**两处**加粗（另一处是 logo 上那个 26px 角标里的字母），
    /// 这两处的共同点是「不加粗就看不清 / 压不住」。别顺手给别处也加。
    @ViewBuilder
    private var securityBanner: some View {
        let alerts = state.securityAlerts.filter { !dismissedAlerts.contains($0.id) }
        if !alerts.isEmpty {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .font(.system(size: 20))
                    .foregroundStyle(Color(hex: 0xE0_53_3D))

                VStack(alignment: .leading, spacing: 3) {
                    Text("检测到局域网内有异常代理行为".t)
                        .font(.system(size: 13, weight: .bold))
                        .foregroundStyle(theme.textPrimary)
                        .lineLimit(1)
                    ForEach(alerts) { alert in
                        Text(alert.kind == .gatewaySpoofed
                             ? String(format: "%@ 正在冒充网关，可能在监听或代理你的流量".t, alert.offenderMAC)
                             : String(format: "%@ 也在劫持你代理的设备 %@".t, alert.offenderMAC, alert.subjectIP))
                            .font(.system(size: 11))
                            .foregroundStyle(theme.textSecondary)
                            .fixedSize(horizontal: false, vertical: true)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }

                DismissChip { alerts.forEach { dismissedAlerts.insert($0.id) } }
            }
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background {
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(Color(red: 224 / 255, green: 83 / 255, blue: 61 / 255, opacity: 0.12))
            }
            .overlay {
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .stroke(Color(red: 224 / 255, green: 83 / 255, blue: 61 / 255, opacity: 0.5),
                            lineWidth: 1)
            }
            .padding(.trailing, 10)
        }
    }

    /// 底部横幅：说明当前状态。设备端零配置，所以这里没有任何要用户抄的东西。
    @ViewBuilder
    private var proxyBanner: some View {
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
        todayByDevice = state.history.todayByDevice()
    }

    private func reloadLedger() {
        ledger = Dictionary(uniqueKeysWithValues: state.devices.all().map { ($0.mac, $0) })
    }

    private func setProxy(row: Row, enabled: Bool) {
        _ = state.devices.setProxyEnabled(mac: row.discovered.mac, enabled, ip: row.discovered.ip)
        // 走统一信号：本页重读快照的同时，历史库的 IP→MAC 映射也跟着更新
        // （原来这里只重读了本页，映射要等下一轮扫描才对得上）。
        state.ledgerDidChange()
        if enabled { openDetail(row) }   // 刚开启就把详情打开（策略在里面选）
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
    /// 采样节拍，透传给背景那张流量图做连续左滑。
    var tick: UInt64 = 0
    /// 这台设备正被**别的机器**也投毒争抢（ArpWatch 检测到）。
    /// 名字旁打红标 + 整行描一圈红框 —— 与 Qt 一致，一眼看出是哪一台。
    var contended = false
    let onToggleProxy: (Bool) -> Void
    let onOpenDetail: () -> Void

    @State private var hovering = false

    /// 已经开着的一律可关（离线/跨网段也得能撤销）；关着的只有「可代理且在线」才点得动。
    private var canToggle: Bool { row.proxyEnabled || (rejection == nil && row.online) }

    /// 本行的实际宽度。用来决定要不要收起速率列（Qt 的 `compact`）。
    @State private var width: CGFloat = 999

    private var rowContent: some View {
        HStack(spacing: 8) {
            avatar

            // 名称 + 副标题 + 最后访问：**吃掉右侧剩下的全部宽度**，放不下就省略号。
            // 右侧那几列都是刚性定宽的，所以「该被挤的是名字」——不这样的话，
            // 副标题长的行会把右侧几列压窄，同一列在不同行落在不同的 x 上。
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Text(row.discovered.displayName)
                        .font(.system(size: 13))
                        .foregroundStyle(theme.textPrimary)
                        .lineLimit(1).truncationMode(.tail)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    if contended {
                        // 半径 3、淡红底、9px 红字；**靠右不挤名字**（与 Qt 一致）。
                        Text("被争抢".t)
                            .font(.system(size: 9))
                            .foregroundStyle(Color(hex: 0xE0_53_3D))
                            .lineLimit(1)
                            .padding(.horizontal, 5)
                            .padding(.vertical, 1.5)
                            .background {
                                RoundedRectangle(cornerRadius: 3, style: .continuous)
                                    .fill(Color(red: 224 / 255, green: 83 / 255,
                                                blue: 61 / 255, opacity: 0.18))
                            }
                            .help("另一台设备也在把这台设备的流量劫走".t)
                    }
                }

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

            // 行窄到放不下速率两列时（窗口拖到很小），把它收起来只留头像/名字/开关 ——
            // 右侧那几列是定宽的，不收就只能溢出到行外面去。判据与 Qt 相同：行宽 < 250。
            if width >= 250 { speedColumn }

            // 最右一列：**开关和徽章共用同一个 38 宽的槽位**（一台设备要么能开代理、
            // 要么有一个「为什么不能开」的理由，两者互斥）。槽位宽度写死 = 开关宽度，
            // 所以整列的左右边缘在每一行都一样齐。
            ZStack {
                if rejection == nil || row.proxyEnabled {
                    proxySwitch
                } else if showsReasonBadge {
                    reasonBadge
                }
            }
            .frame(width: 38, height: 20)
        }
        .padding(.horizontal, 8)
        .frame(height: 60)                     // Qt: 60（多了「最后访问」一行；**所有行等高**）
        .background { GeometryReader { geo in Color.clear.onAppear { width = geo.size.width }
            .onChange(of: geo.size.width) { _, new in width = new } } }
        .background {
            ZStack {
                (row.proxyEnabled ? theme.nodeRowBg : (hovering ? theme.hover : theme.nodeRowBg))
                // 背景实时流量图：**只有被代理的设备才画**。其余设备的流量不经核心，
                // 画出来永远是一条贴底的 0 线。速率是 0 也照画 —— 那正是
                // 「已接管、此刻闲着」的样子，不是「没数据」。
                if row.proxyEnabled {
                    DeviceTrafficBg(up: sample.upHistory, down: sample.downHistory, tick: tick)
                }
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 5, style: .continuous))
        // 被争抢的设备：整行描一圈红框，配合名字旁的红标（一眼看出是哪一台）。
        .overlay {
            if contended {
                RoundedRectangle(cornerRadius: 5, style: .continuous)
                    .stroke(Color(hex: 0xE0_53_3D), lineWidth: 1)
            }
        }
        .opacity(row.online ? 1 : 0.5)         // 离线行整体淡化
        .onHover { hovering = $0 }
    }

    /// 整行可点（打开详情窗）。
    ///
    /// ★ 必须是**真的 Button**，不能只挂 `.onTapGesture` —— 行在 `List` 里，
    /// 光挂手势点下去没有任何反应（实测点了好几次，详情窗一个都没开出来，
    /// 而且没有任何报错）。Qt 那边同样是给整行单独铺了一个 `MouseArea`，
    /// 它的注释写的理由也是「TapHandler 抢不到」。
    ///
    /// 行里那颗代理开关自己带手势，嵌在里面**优先吃掉**落在它上面的点击 ——
    /// 与 Qt「开关的 MouseArea 压在整行的 MouseArea 之上」是同一个效果。
    var body: some View {
        Button(action: onOpenDetail) { rowContent }
            .buttonStyle(.plain)
    }

    /// 类型头像 34×34 圆角 8 + 图标 18 + 右下角 11 的在线小圆点（带 2px 描边）。
    /// 头像是这一行里**最先被看到**的东西 —— 一屏十几台设备时，先认出的是色块。
    private var avatar: some View {
        RoundedRectangle(cornerRadius: 8, style: .continuous)
            .fill(theme.deviceColor(row.typeKey))
            .frame(width: 34, height: 34)
            .overlay {
                Image(systemName: theme.deviceSymbol(row.typeKey))
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
        .allowsHitTesting(canToggle)
        .help(rejection?.reason.t ?? "代理网络".t)
    }

    /// 要不要显示原因徽章。
    ///
    /// ★ 「其它网络」这个结论**只在设备在线时才敢下** —— 判据依赖当轮扫描拿到的地址，
    ///   而从台账加载出来、还没被本轮扫描确认的设备一律拿不到地址。不加这个条件的话，
    ///   刚进页面那一两秒里**每台离线设备都会被扣上「其它网络」的帽子**（Qt 的注释
    ///   专门写了这一条）。本机与网关两种是恒定事实，离线也照说。
    private var showsReasonBadge: Bool {
        switch rejection {
        case .isLocalMachine, .isGateway: return true
        default: return row.online
        }
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

    /// 副标题：`IP · 厂商`。
    ///
    /// ★ **名字就是厂商时不再重复一遍**。`displayName` 的回退链是
    /// 主机名 → 厂商 → MAC，所以查不到主机名的设备（局域网里很常见）名字本身就是厂商，
    /// 原样拼的话整行会变成「Beijing Xiaomi Mobile Software Co. / 192.168.31.1 ·
    /// Beijing Xiaomi Mobile Software Co.」—— 同一串字占掉两行，而副标题本该补充信息。
    /// 截图看出来的。
    private var subtitle: String {
        var parts: [String] = []
        if !row.discovered.ip.isEmpty { parts.append(row.discovered.ip) }
        let vendor = row.discovered.vendor
        if !vendor.isEmpty, vendor != row.discovered.displayName { parts.append(vendor) }
        return parts.joined(separator: "  ·  ")
    }
}

/// 设备行的**背景**实时流量图：下行(绿) / 上行(红) 两条面积曲线叠在一起，共用同一量程。
/// 对齐 `qml/DeviceTrafficBg.qml`：曲线最高只占行高的 0.75，量程下限 128KB/s ——
/// 闲着时几百字节的抖动不会被放大成满屏山峰。
struct DeviceTrafficBg: View {
    let up: [Double]
    let down: [Double]
    /// 采样节拍。与 `BandwidthChart` 同理：曲线**连续左滑**，
    /// 而不是每来一个样本整条跳一格（QML 那份注释专门讲了这件事）。
    var tick: UInt64 = 0

    @State private var lastPush = Date()

    /// 量程下限 128 KB/s。
    private static let floorScale = 131_072.0
    private static let headroom = 0.75

    private var scale: Double {
        max(Self.floorScale, (up + down).max() ?? 0)
    }

    var body: some View {
        // 50ms ≈ 20fps，与 QML 那个 `Timer { interval: 50 }` 同一档；只在行可见时才有开销
        // （非代理行根本不实例化这个视图）。
        TimelineView(.periodic(from: .now, by: 0.05)) { context in
            let phase = min(1, max(0, context.date.timeIntervalSince(lastPush)))
            ZStack {
                area(down, color: Color(hex: 0x5B_B4_4B), phase: phase)
                area(up, color: Color(hex: 0xB1_4A_4A), phase: phase)
            }
        }
        .clipped()
        .allowsHitTesting(false)
        .onChange(of: tick) { _, _ in lastPush = Date() }
    }

    private func area(_ samples: [Double], color: Color, phase: Double) -> some View {
        GeometryReader { geo in
            let path = areaPath(samples, in: geo.size, phase: phase)
            ZStack {
                path.fill(LinearGradient(colors: [color.opacity(0.22), color.opacity(0.02)],
                                         startPoint: .top, endPoint: .bottom))
                strokePath(samples, in: geo.size, phase: phase).stroke(color.opacity(0.55), lineWidth: 1)
            }
        }
    }

    private func points(_ samples: [Double], in size: CGSize, phase: Double) -> [CGPoint] {
        guard samples.count > 1 else { return [] }
        let usable = size.height * Self.headroom
        // 多留一格给「滑进来的那一点」，否则最后一点滑到位时右边会空出一条缝。
        let dx = size.width / CGFloat(samples.count - 2 > 0 ? samples.count - 2 : 1)
        let shift = dx * CGFloat(phase)
        return samples.enumerated().map { index, value in
            CGPoint(x: CGFloat(index) * dx - shift,
                    y: size.height - usable * CGFloat(min(1, value / scale)))
        }
    }

    private func strokePath(_ samples: [Double], in size: CGSize, phase: Double) -> Path {
        var path = Path()
        let pts = points(samples, in: size, phase: phase)
        guard let first = pts.first else { return path }
        path.move(to: first)
        for point in pts.dropFirst() { path.addLine(to: point) }
        return path
    }

    private func areaPath(_ samples: [Double], in size: CGSize, phase: Double) -> Path {
        var path = strokePath(samples, in: size, phase: phase)
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

/// 告警横幅右上角那颗「知道了」。半径 4、同色淡底，悬停加深 —— 对齐 QML 的 `dismissHover`。
private struct DismissChip: View {
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Text("知道了".t)
                .font(.system(size: 11))
                .foregroundStyle(Color(hex: 0xE0_53_3D))
                .lineLimit(1)
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background {
                    RoundedRectangle(cornerRadius: 4, style: .continuous)
                        .fill(Color(red: 224 / 255, green: 83 / 255, blue: 61 / 255,
                                    opacity: hovering ? 0.22 : 0.12))
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
    }
}
