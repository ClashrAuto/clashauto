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
    @State private var expanded: String?
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
    /// 代理中的台数。
    ///
    /// 用 `allRows` 而不是筛过的 `rows`：这是「这个网络里有几台在被代理」的统计，
    /// 跟用户此刻在搜什么无关。用 `rows` 的话，一搜索这个数就跳，看着像状态突变。
    private var enabledCount: Int { allRows.filter(\.proxyEnabled).count }

    /// 经搜索筛过的行。
    private var rows: [Row] {
        allRows.filter { row in
            DeviceFilter.matches(keyword: search, fields: DeviceFilter.haystack(
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
        VStack(spacing: 0) {
            header
            Divider().overlay(theme.divider)
            if rows.isEmpty { emptyState } else { list }
            Divider().overlay(theme.divider)
            proxyBanner
        }
        .task { await scan() }
    }

    /// 概览头。对齐 Qt 设备页顶部那一块：标题 + 在线/代理中两个计数 + 网关。
    ///
    /// 计数用**在线**而不是「发现的总数」：邻居表里会留着早就搬走的设备，
    /// 拿总数当「我家有几台设备」会一直偏大，越用越离谱。
    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline, spacing: 12) {
                Text("设备".t)
                    .font(.system(size: 18, weight: .medium))
                    .foregroundStyle(theme.textPrimary)
                stat("在线".t, "\(allRows.filter(\.online).count)")
                stat("代理中".t, "\(enabledCount)")
                Spacer()
                Toggle("新设备提醒".t, isOn: Binding(
                    get: { state.config.newDeviceAlert },
                    set: { state.setNewDeviceAlert($0) }))
                    .toggleStyle(.switch)
                    .controlSize(.mini)
                    .font(.system(size: 11))
                Button {
                    exportCSV()
                } label: {
                    Text("导出".t).font(.system(size: 12))
                        .padding(.horizontal, 10).frame(height: 24)
                }
                .buttonStyle(.plain)
                .glassCapsule()
                Button { Task { await scan() } } label: {
                    Label(scanning ? "扫描中…".t : "重新扫描".t, systemImage: "arrow.clockwise")
                        .font(.system(size: 12))
                        .padding(.horizontal, 10).frame(height: 24)
                }
                .buttonStyle(.plain)
                .glassCapsule()
                .disabled(scanning)
            }
            HStack(spacing: 8) {
                Text("网关 ".t + (gatewayIP.isEmpty ? "-" : gatewayIP))
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                if !exportMessage.isEmpty {
                    Text(exportMessage)
                        .font(.system(size: 11))
                        .foregroundStyle(theme.textSecondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                Spacer()
                TextField("搜索设备 / IP / 厂商".t, text: $search)
                    .textFieldStyle(.roundedBorder)
                    .frame(maxWidth: 220)
            }
        }
        .padding(10)
    }

    private func stat(_ label: String, _ value: String) -> some View {
        HStack(spacing: 4) {
            Text(value)
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(theme.textPrimary)
            Text(label)
                .font(.system(size: 12))
                .foregroundStyle(theme.textMuted)
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
                          expanded: expanded == row.id,
                          onToggleProxy: { enabled in setProxy(row: row, enabled: enabled) },
                          onToggleExpand: { expanded = expanded == row.id ? nil : row.id })
                // 展开区不再只在「已代理」时出现 —— 备注名与「为什么不能代理」这两件事,
                // 恰恰是在还没开代理时最需要看到的。
                if expanded == row.id, let record = row.record {
                    PolicyBox(record: record,
                              targets: state.clash.groups,
                              rejection: rejection(for: row),
                              onRename: { state.devices.setAlias(mac: row.id, $0); reloadLedger() },
                              onPolicyChange: { mode, target in
                                  setPolicy(row: row, mode: mode, target: target)
                              })
                }
            }
            .listRowBackground(Color.clear)
            .listRowSeparator(.hidden)
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
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
    }

    private func reloadLedger() {
        ledger = Dictionary(uniqueKeysWithValues: state.devices.all().map { ($0.mac, $0) })
    }

    private func setProxy(row: Row, enabled: Bool) {
        _ = state.devices.setProxyEnabled(mac: row.discovered.mac, enabled, ip: row.discovered.ip)
        reloadLedger()
        if enabled { expanded = row.id }   // 刚开启就把策略选择摊开
        Task { await state.controller.rebuildConfig() }
    }

    private func setPolicy(row: Row, mode: DeviceStore.PolicyMode, target: String) {
        guard var record = state.devices.device(mac: row.discovered.mac) else { return }
        record.policyMode = mode
        record.policyTarget = target
        _ = state.devices.save(record)
        reloadLedger()
        Task { await state.controller.rebuildConfig() }
    }
}

private struct DeviceRow: View {
    @Environment(Theme.self) private var theme
    let row: DevicesPage.Row
    /// 这台设备不可接管的原因（nil = 可以）。由页面算好传进来 ——
    /// 每行自己去查网关和本机 MAC 的话，一次渲染会重复读几十次系统表。
    var rejection: RedirectTargets.Rejection?
    let expanded: Bool
    let onToggleProxy: (Bool) -> Void
    let onToggleExpand: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            // 类型头像 + 在线角标。尺寸取自 Qt DeviceRow：34×34、圆角 8、图标 18、
            // 角标 11。头像是这一行里**最先被看到**的东西 —— 一屏十几台设备时，
            // 先认出的是色块，名字要看第二眼，所以它的尺寸不能随手改小。
            ZStack(alignment: .bottomTrailing) {
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(theme.deviceColor(row.discovered.typeKey))
                    .frame(width: 34, height: 34)
                    .overlay {
                        Image(systemName: theme.deviceSymbol(row.discovered.typeKey))
                            .font(.system(size: 18)).foregroundStyle(.white)
                    }
                    .opacity(row.online ? 1 : 0.45)

                // 在线角标：压在头像右下角，带一圈与背景同色的描边把它和色块分开，
                // 不描边的话绿点会和绿色的手机底色糊在一起。
                Circle()
                    .fill(row.online ? theme.deviceColor("phone") : theme.textMuted)
                    .frame(width: 11, height: 11)
                    .overlay(Circle().stroke(theme.metricBg, lineWidth: 2))
                    .offset(x: 3, y: 3)
            }

            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 5) {
                    Text(row.discovered.displayName)
                        .font(.system(size: 13)).foregroundStyle(theme.textPrimary).lineLimit(1)
                    if row.discovered.isGateway { tag("网关".t, theme.accent) }
                    if !row.online { tag("离线".t, theme.textMuted) }
                }
                Text(subtitle)
                    .font(.system(size: 10).monospacedDigit())
                    .foregroundStyle(theme.textMuted).lineLimit(1)
            }

            Spacer(minLength: 8)

            if row.proxyEnabled {
                Button(action: onToggleExpand) {
                    Image(systemName: expanded ? "chevron.up" : "chevron.down")
                        .font(.system(size: 10))
                }
                .buttonStyle(.borderless)
            }

            Toggle("", isOn: Binding(get: { row.proxyEnabled }, set: onToggleProxy))
                .labelsHidden().toggleStyle(.switch).controlSize(.mini)
                .disabled(rejection != nil)
                .help(rejection?.reason.t ?? "代理网络".t)
        }
        .padding(.horizontal, 10)
        .frame(height: 46)
        .background(theme.nodeRowBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
    }

    private var subtitle: String {
        var parts: [String] = []
        if !row.discovered.ip.isEmpty { parts.append(row.discovered.ip) }
        parts.append(row.discovered.mac)
        if !row.discovered.vendor.isEmpty { parts.append(row.discovered.vendor) }
        return parts.joined(separator: " · ")
    }

    private func tag(_ text: String, _ color: Color) -> some View {
        Text(text)
            .font(.system(size: 9)).foregroundStyle(.white)
            .padding(.horizontal, 4).padding(.vertical, 1)
            .background(Capsule().fill(color))
    }
}

/// 展开后只有策略选择。**没有凭据、没有地址** —— 设备端零配置是这个功能的全部意义，
/// 让用户去抄任何东西都等于没做。
private struct PolicyBox: View {
    @Environment(Theme.self) private var theme
    let record: DeviceStore.Device
    let targets: [String]
    var rejection: RedirectTargets.Rejection?
    let onRename: (String) -> Void
    let onPolicyChange: (DeviceStore.PolicyMode, String) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
        if let rejection {
            // 三种不可代理的情形各自说清楚 —— 只把开关变灰的话，用户只会觉得「点不动」，
            // 不知道是自己点错了对象还是程序坏了。
            Label(rejection.reason.t, systemImage: "info.circle")
                .font(.system(size: 11))
                .foregroundStyle(theme.textMuted)
        }
        HStack(spacing: 8) {
            Text("备注名".t).font(.system(size: 11)).foregroundStyle(theme.textMuted)
            TextField("为该设备起个名字".t, text: Binding(
                get: { record.alias },
                set: { onRename($0) }))
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 200)
            Spacer()
        }
        HStack(spacing: 8) {
            Text("策略".t).font(.system(size: 11)).foregroundStyle(theme.textMuted)
            Picker("", selection: Binding(
                get: { record.policyMode },
                set: { onPolicyChange($0, record.policyTarget) }
            )) {
                ForEach(DeviceStore.PolicyMode.allCases, id: \.self) { Text($0.title.t).tag($0) }
            }
            .labelsHidden().frame(width: 130)

            if record.policyMode == .global {
                Picker("", selection: Binding(
                    get: { record.policyTarget },
                    set: { onPolicyChange(.global, $0) }
                )) {
                    // 没选目标时 applyDevicePolicies 会跳过这条规则，给个明确占位而不是空白
                    Text("（未选）".t).tag("")
                    ForEach(targets, id: \.self) { Text($0).tag($0) }
                }
                .labelsHidden().frame(maxWidth: 200)
            }
            Spacer()
        }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
        .padding(.top, 4)
    }
}
