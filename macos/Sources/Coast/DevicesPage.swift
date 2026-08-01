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
    /// 搜索框是否展开。同节点页：默认只有一颗放大镜钮，点开才出输入框
    /// （✕ 清空并收起）—— 单行顶栏里常驻输入框会把左右两组挤到一起。
    @State private var searchShown = false

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
        // Qt 的设备页没有任何分隔线：概览条 / 列表靠 10 的行距分开。
        VStack(spacing: 10) {
            if rows.isEmpty { emptyState } else { list }
        }
        // 安全告警横幅在**最上面**（Qt 的顺序：告警 → 概览条 → 列表）——
        // 有人正在冒充网关时，那条「已接管 N 台」远没它要紧。26 上它和概览条
        // 一起进顶部导航栏（告警钉着不随滚动走，只会更醒目）。
        .pageHeaderBar(spacing: 10) {
            VStack(spacing: 10) {
                securityBanner
                header
            }
        }
        .task { await scan() }
        // 详情窗改完台账后立刻重读（否则列表要等下一轮扫描才更新图标/名字）
        .onChange(of: state.ledgerRevision) { _, _ in reloadLedger() }
    }

    /// 概览条。**逐元素对齐** `qml/DevicesPage.qml` 顶部那一行：
    /// 顶栏：**单行**。设备(18) | 在线 N/M | （搜索展开时的输入框，吃掉中间空间）|
    /// 新设备提醒 + 34×18 开关 | 🔍 | 仅在线 | 重扫。
    /// 「代理中 / 今日 / 导出 / 网关」已裁掉 —— 单行放不下，且都是低频信息：
    /// 代理数在底部横幅（已接管 N 台）里有，今日流量状态页有。
    /// 搜索同节点页：默认一颗放大镜钮，点开才展开成输入框（✕ 清空并收起）。
    private var header: some View {
        HStack(spacing: 16) {
            Text("设备".t)
                .lineLimit(1)
                .font(.system(size: 18))
                .foregroundStyle(theme.textPrimary)
            stat("在线".t, "\(allRows.filter(\.online).count)/\(allRows.count)")

            Spacer(minLength: 16)

            HStack(spacing: 5) {
                Text("新设备提醒".t).font(.system(size: 11))
                    .foregroundStyle(theme.textMuted).fixedSize()
                // 34×18 的小开关（比设置页那颗 46×24 小一圈）—— Qt 这里就是两套尺寸。
                SmallSwitch(isOn: state.config.newDeviceAlert) {
                    state.setNewDeviceAlert(!state.config.newDeviceAlert)
                }
            }

            searchControls
        }
        // 右内距在这一行自己补（设备列表仍铺到页面最右缘，滚动条贴窗口右侧）。
        // 26 上左缘贴边（pageLeadingInset = 0）。
        .padding(.leading, pageLeadingInset)
        .padding(.trailing, 10)
        .padding(.top, 10)
    }

    /// 液态玻璃形变要的命名空间（26 才用得上）。
    @Namespace private var searchNS

    /// 搜索 + 仅在线 + 重扫。
    ///
    /// 26：整组放进 `GlassEffectContainer`，搜索钮和展开的搜索框共享同一个
    /// `glassEffectID` —— 点击时是**同一块玻璃**从按钮形态向左拉伸成输入框
    /// （系统 Liquid Glass 形变），输入框本身也是玻璃，没有「藏按钮、换控件」。
    /// 26 以下：按钮/输入框互换 + 平面输入底（旧系统没有这套形变）。
    @ViewBuilder
    private var searchControls: some View {
        if #available(macOS 26.0, *) {
            GlassEffectContainer(spacing: 2) {
                HStack(spacing: 6) {
                    Group {
                        if searchShown {
                            glassSearchField
                                .glassEffect(.regular, in: .capsule)
                                .glassEffectID("search", in: searchNS)
                        } else {
                            glassSearchButton
                                .glassEffect(.regular, in: .capsule)
                                .glassEffectID("search", in: searchNS)
                        }
                    }
                    glassScanGroup
                }
            }
        } else {
            HStack(spacing: 6) {
                if searchShown {
                    legacySearchField
                } else {
                    SquareToggle(symbol: "magnifyingglass", on: false) {
                        withAnimation(.snappy(duration: 0.22)) { searchShown = true }
                    }
                }
                scanToggles
            }
        }
    }

    /// 26：玻璃胶囊里的搜索框（220×28），✕ 清空并收回按钮形态。
    private var glassSearchField: some View {
        ZStack(alignment: .trailing) {
            TextField("搜索设备 / IP / 厂商".t, text: $search)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
                .padding(.leading, 12)
                .padding(.trailing, 26)
            Button {
                search = ""
                withAnimation(.snappy(duration: 0.28)) { searchShown = false }
            } label: {
                Image(systemName: "xmark").font(.system(size: 13))
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
            .padding(.trailing, 9)
        }
        .frame(width: 220, height: 28)
    }

    /// 26：收起态的放大镜钮（28×28，与旁边一组同尺寸）。图标用系统默认前景色。
    private var glassSearchButton: some View {
        Button {
            withAnimation(.snappy(duration: 0.28)) { searchShown = true }
        } label: {
            Image(systemName: "magnifyingglass").font(.system(size: 12))
                .frame(width: 28, height: 28)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    /// 26：「仅在线 + 重扫」连体液态玻璃按钮组 —— 两段共一层玻璃、只有外侧是圆的
    /// （页脚模式组同款画法），「仅在线」开启时段内衬品牌色。图标用系统默认前景色。
    private var glassScanGroup: some View {
        HStack(spacing: 0) {
            Button { onlineOnly.toggle() } label: {
                Image(systemName: "circle.fill").font(.system(size: 12))
                    .frame(width: 28, height: 28)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .background {
                // 开启衬底用**系统 tint**（跟随用户的系统强调色），不用品牌色。
                if onlineOnly { Capsule().fill(.tint.opacity(0.35)) }
            }
            .help("仅显示在线设备".t)

            Button {
                Task { await scan() }
            } label: {
                Text("\u{F064}") // refresh-line —— 与状态页延迟卡的刷新同一枚字形
                    .font(.custom(IconFont.remix, size: 15))
                    .rotationEffect(.degrees(scanning ? 360 : 0))
                    .animation(scanning ? .linear(duration: 0.9).repeatForever(autoreverses: false)
                               : .default, value: scanning)
                    .frame(width: 28, height: 28)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(scanning)
            .help("重新扫描".t)
        }
        .glassCapsule()
    }

    /// 26 以下的平面搜索框（220×28，inputBg + 描边）。
    private var legacySearchField: some View {
        ZStack(alignment: .trailing) {
            TextField("搜索设备 / IP / 厂商".t, text: $search)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
                .foregroundStyle(theme.textPrimary)
                .padding(.leading, 8)
                .padding(.trailing, 24)
                .frame(width: 220, height: 28)
                .background {
                    RoundedRectangle(cornerRadius: 3, style: .continuous).fill(theme.inputBg)
                }
                .overlay {
                    RoundedRectangle(cornerRadius: 3, style: .continuous)
                        .stroke(theme.inputBorder, lineWidth: 1)
                }
            Button {
                search = ""
                withAnimation(.snappy(duration: 0.22)) { searchShown = false }
            } label: {
                Image(systemName: "xmark").font(.system(size: 14))
                    .foregroundStyle(theme.textMuted)
            }
            .buttonStyle(.plain)
            .padding(.trailing, 7)
        }
    }

    /// 「仅在线」圆点 + 重扫，两个分支共用。
    @ViewBuilder
    private var scanToggles: some View {
        SquareToggle(symbol: "circle.fill", on: onlineOnly) { onlineOnly.toggle() }
        SquareToggle(symbol: "arrow.clockwise", remixGlyph: "\u{F064}",
                     on: false, accent: true, spinning: scanning) {
            Task { await scan() }
        }
        .disabled(scanning)
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
        // 本机那一行要认 `127.0.0.1` / TUN 地址 —— 只比局域网 IP 的话它永远是空的
        // （与速率那处同一个坑，判据抽在 `DeviceStore.connectionBelongs`）。
        let isLocal = row.discovered.ip == DeviceStore.localLANAddress()
        return state.connections
            .filter { !$0.host.isEmpty
                && DeviceStore.connectionBelongs(sourceIP: $0.sourceIP,
                                                 deviceIP: row.discovered.ip,
                                                 isLocalMachine: isLocal) }
            .max { $0.start < $1.start }?
            .host ?? ""
    }

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
            // 行铺到列表最右缘（悬浮滚动条压在行上，系统列表的常态）——
            // 原来右让 10 是配旧的两行顶栏对齐用的，单行顶栏后只剩一条空缝。
            .listRowInsets(EdgeInsets(top: 2, leading: 0, bottom: 2, trailing: 0))
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
    /// 走 remixicon 字体的码点，给了就**优先于** `symbol` —— 刷新钮要与
    /// 状态页延迟卡用同一枚 refresh-line 字形。
    var remixGlyph: String?
    let on: Bool
    var accent = false
    var spinning = false
    let action: () -> Void

    @State private var hovering = false

    /// 图标本体（remix 字形优先，否则 SF），两个版本分支共用。
    private var icon: some View {
        Group {
            if let remixGlyph {
                Text(remixGlyph).font(.custom(IconFont.remix, size: accent ? 15 : 12))
            } else {
                Image(systemName: symbol).font(.system(size: accent ? 15 : 12))
            }
        }
        .rotationEffect(.degrees(spinning ? 360 : 0))
        .animation(spinning ? .linear(duration: 0.9).repeatForever(autoreverses: false)
                   : .default, value: spinning)
        .frame(width: 28, height: 28)
    }

    var body: some View {
        if #available(macOS 26.0, *) {
            // 26：液态玻璃圆钮，开启态是品牌色 tint 的玻璃（不再手画方角底+描边）。
            Button(action: action) {
                icon
                    // 开启底衬在玻璃**里面**（glassEffect 的 .tint 实测发白看不出来）。
                    .background {
                        if on { Capsule().fill(theme.accent.opacity(0.8)) }
                    }
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .glassCapsule()
        } else {
            Button(action: action) {
                icon
                    .foregroundStyle(on ? .white : (accent ? theme.accent : theme.textMuted))
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
