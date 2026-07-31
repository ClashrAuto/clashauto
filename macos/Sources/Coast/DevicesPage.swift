import CoastKit
import SwiftUI

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

    private let browser = LanBrowser()

    /// 当前网络的网关 IP（概览头显示）。
    private var gatewayIP: String { LanTopology.defaultGateway()?.ip ?? "" }
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
                Button { Task { await scan() } } label: {
                    Label(scanning ? "扫描中…".t : "重新扫描".t, systemImage: "arrow.clockwise")
                }
                .disabled(scanning)
            }
            HStack(spacing: 8) {
                Text("网关 ".t + (gatewayIP.isEmpty ? "-" : gatewayIP))
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
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
                          expanded: expanded == row.id,
                          onToggleProxy: { enabled in setProxy(row: row, enabled: enabled) },
                          onToggleExpand: { expanded = expanded == row.id ? nil : row.id })
                if expanded == row.id, let record = row.record, record.proxyEnabled {
                    PolicyBox(record: record,
                              targets: state.clash.groups,
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
        ForEach(state.securityAlerts) { alert in
            HStack(spacing: 6) {
                Image(systemName: "exclamationmark.shield.fill")
                    .font(.system(size: 11)).foregroundStyle(theme.danger)
                Text(alert.kind == .gatewaySpoofed
                     ? String(format: "%@ 正在冒充网关，可能在监听或代理你的流量".t, alert.offenderMAC)
                     : String(format: "%@ 也在劫持你代理的设备 %@".t, alert.offenderMAC, alert.subjectIP))
                    .font(.system(size: 11)).foregroundStyle(theme.danger)
                Spacer()
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
        discovered = await browser.scan()
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
    let expanded: Bool
    let onToggleProxy: (Bool) -> Void
    let onToggleExpand: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .fill(theme.deviceColor(row.discovered.typeKey))
                .frame(width: 30, height: 30)
                .overlay {
                    Image(systemName: theme.deviceSymbol(row.discovered.typeKey))
                        .font(.system(size: 14)).foregroundStyle(.white)
                }
                .opacity(row.online ? 1 : 0.45)

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
                .help("代理".t)
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
    let onPolicyChange: (DeviceStore.PolicyMode, String) -> Void

    var body: some View {
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
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
        .padding(.top, 4)
    }
}
