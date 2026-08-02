import CoastKit
import SwiftUI

/// 全部连接。**功能**逐项对齐 `qml/ConnectionsWindow.qml`：720×480、
/// Online(N)/Offline(N) 两个独立开关 + 搜索、卡片列表（● 圆点 + `[type] host` +
/// 进程/出口链/下载/上传四枚徽标 + ✕ 删除 + 右键「添加规则」）。
///
/// **观感则按 mac 来，不照抄 Qt 的画法**（26 起）：顶栏钉进标题栏那条带子、
/// 分段与搜索是液态玻璃、整窗透玻璃、行是浮在上面的材质卡片。Qt 那套
/// 「蓝灰实色方块 + Search 前缀标签 + 1px 竖线」是 web 时代的样子，在 mac 上很扎眼。
/// 26 以下没有这套材质，仍走原来的 Qt 画法（见各处的 `legacy*`）。
///
/// **独立顶层窗**，与 Qt 一致（720×480，最小 480×320）。做成 sheet 的话主窗被拖到
/// 最小宽（640）时它会横向溢出 —— 实测左边的「Online (0)」被切掉半截。
struct ConnectionsView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @State private var query = ""
    @State private var showOnline = true
    @State private var showOffline = true
    /// 右键「添加规则」用本行地址预填 value —— 与 Qt 的 `openForValue` 同义。
    @State private var addingRule: RuleDraft?

    /// 顶栏统一高度：两颗分段按钮与搜索框一致。26 以下沿用 Qt 的 26；
    /// 26 上给 28 —— 与全 app 的玻璃控件（页脚开关、日志页标签）同高。
    private var toolbarHeight: CGFloat {
        if #available(macOS 26.0, *) { 28 } else { 26 }
    }

    private var rows: [ConnectionLedger.Entry] {
        state.connectionLedger.filtered(online: showOnline, offline: showOffline, query: query)
    }

    var body: some View {
        content
            .frame(minWidth: 480, minHeight: 320)
            // 每次打开清空账本，避免上次会话的离线连接残留（与 Qt 的 onVisibleChanged 一致）。
            .task { state.resetConnectionLedger() }
            .sheet(item: $addingRule) { draft in
                RuleEditorSheet(draft: draft) { saved in save(rule: saved) }
                    .environment(state).environment(theme)
            }
    }

    @ViewBuilder
    private var content: some View {
        if #available(macOS 26.0, *) {
            // 26：顶栏**钉进标题栏那条带子**（`safeAreaBar` + 整列越过顶部安全区），
            // 于是它距窗顶为 0、和红绿灯同一带；标题栏本身由 `unifiedTitleBar()` 抬到
            // 同高，系统标题文字隐去 —— 顶栏自己就是这个窗口的标题。
            // 列表从这条带子底下穿过，由系统 scroll edge effect 渐隐（与主窗一个机制）。
            list
                .safeAreaBar(edge: .top, spacing: 0) { header }
                .scrollEdgeEffectStyle(.soft, for: .all)
                .ignoresSafeArea(.container, edges: .top)
                .unifiedTitleBar()
                // 整窗玻璃，和主窗同一层材质；行自带一层 `.regularMaterial`（见 `row`），
                // 两层材质叠出「卡片浮在玻璃上」的分层。
                .windowGlass(.sidebar)
        } else {
            VStack(spacing: 5) {
                HStack(spacing: 10) {
                    segmentedToggles
                    searchBox
                }
                .padding(.top, 5)
                .padding(.leading, ConnectionsView.headerLeadingInset)
                .padding(.trailing, 5)
                list
            }
            .background(theme.card)
        }
    }

    // MARK: 顶栏

    /// 钉在标题栏那条带子里的顶栏。
    ///
    /// 上内距 10：加上 28 的控件高正好 38，红绿灯在这条带子里的中心落在 25 附近，
    /// 控件中心 24 —— 两边看着是一排的。
    private var header: some View {
        HStack(spacing: 10) {
            segmentedToggles
            searchBox
        }
        .padding(.top, 10)
        .padding(.leading, ConnectionsView.headerLeadingInset)
        .padding(.trailing, 10)
    }

    /// 顶栏左内距。
    ///
    /// ★ 红绿灯就在这条带子的左端，量出来右缘在窗口左边缘往里 **78**（三颗 14 的按钮，
    ///   起点 20、间距 20）。所以「左边留 20」只能是**从红绿灯右侧算起**的 20 ——
    ///   从窗口边缘算的话第一颗控件会被红绿灯整个压住（实测截图：「Online (0)」
    ///   的 nline 被三颗灯盖掉）。
    private static let headerLeadingInset: CGFloat = 78 + 20

    /// 分段按钮组：离线段左端**塞到在线段底下 3px**，中间无缝、只外侧圆角。
    /// 两段各自是独立开关（可以同时开），不是二选一。
    ///
    /// ★ 用 `HStack(spacing: -3)` 让两段**自己量自己**，不要去算文字宽度。
    ///   先前的写法是用 `NSString.size(withAttributes:)` 量一遍再把和式写死给 `ZStack`，
    ///   结果量出来比 SwiftUI 实际排版**偏窄** —— 截图上「Offline (0)」的计数被裁掉了，
    ///   只剩「Offline」。负间距 + `zIndex` 就能同时拿到「重叠 3px」和「在线段盖在上面」，
    ///   一个数都不用量。
    @ViewBuilder
    private var segmentedToggles: some View {
        if #available(macOS 26.0, *) {
            // **一颗玻璃胶囊，里面两个按钮** —— 与页脚的模式切换、日志页的标签同一套
            //   （`glassCapsule()` 罩整组，段自己只画选中底）。
            //
            // ★ 走过两条死路，都别再试：
            //   1. `.buttonStyle(.glass)`（不管外面套 `GlassEffectContainer` 还是
            //      `ControlGroup`）—— 那个样式**每颗自己画一个胶囊**，谁也拼不到一起，
            //      渲染出来永远是「中间带缝的两颗独立胶囊」。
            //   2. 段的选中底用 `Capsule()` —— 两段同时开时就是两颗药丸并排，
            //      中间那道缝正是「看着像两个按钮」的来源。
            //
            //   所以选中底用 `UnevenRoundedRectangle`：**只有外侧那一头是圆的**，
            //   朝里的一头切平。两段都开时两块底严丝合缝连成一条，只有整组的外缘是圆的
            //   —— 这才是「一个胶囊里的两个按钮」。（NavButton 里用的是同一招。）
            //
            //   两段仍各是独立开关（可以同时开、也可以同时关），不是三选一。
            HStack(spacing: 0) {
                filterSegment(title: "Online (\(state.connectionLedger.onlineCount))",
                              on: showOnline, outerEdge: .leading) { showOnline.toggle() }
                filterSegment(title: "Offline (\(state.connectionLedger.offlineCount))",
                              on: showOffline, outerEdge: .trailing) { showOffline.toggle() }
            }
            .glassCapsule()
        } else {
            legacySegmentedToggles
        }
    }

    /// 组里的一段。`outerEdge` 说明它靠哪一头 —— 只有那一头的两个角是圆的。
    @available(macOS 26.0, *)
    private func filterSegment(title: String, on: Bool,
                               outerEdge: HorizontalEdge,
                               action: @escaping () -> Void) -> some View {
        // 圆角取半个高，才和外层胶囊的弧度对得上；小了会在外缘露出一圈直角。
        let radius = toolbarHeight / 2
        let leading = outerEdge == .leading ? radius : 0
        let trailing = outerEdge == .trailing ? radius : 0
        return Button(action: action) {
            Text(title)
                .font(.system(size: 12))
                .foregroundStyle(on ? AnyShapeStyle(.primary) : AnyShapeStyle(.secondary))
                .fixedSize()
                .padding(.horizontal, 12)
                .frame(height: toolbarHeight)
                .background {
                    if on {
                        UnevenRoundedRectangle(topLeadingRadius: leading,
                                               bottomLeadingRadius: leading,
                                               bottomTrailingRadius: trailing,
                                               topTrailingRadius: trailing,
                                               style: .continuous)
                            .fill(.tint.opacity(0.35))
                    }
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private var legacySegmentedToggles: some View {
        HStack(spacing: -3) {
            segment(title: "Online (\(state.connectionLedger.onlineCount))",
                    on: showOnline, extraWidth: 24)
                .zIndex(1)                       // 在上：盖住离线段的左端圆角
                .onTapGesture { showOnline.toggle() }

            segment(title: "Offline (\(state.connectionLedger.offlineCount))",
                    on: showOffline, extraWidth: 27)
                .zIndex(0)
                .onTapGesture { showOffline.toggle() }
        }
        .fixedSize()
    }

    /// 一段的样子。开 = 品牌蓝，关 = 灰；两个颜色都是 QML 里的字面量，不走主题令牌
    /// （那边也是写死的）。
    private func segment(title: String, on: Bool, extraWidth: CGFloat) -> some View {
        Text(title)
            .font(.system(size: 12))
            .foregroundStyle(.white)
            .fixedSize()
            .padding(.horizontal, extraWidth / 2)
            .frame(height: toolbarHeight)
            .background {
                RoundedRectangle(cornerRadius: 3, style: .continuous)
                    .fill(on ? Color(hex: 0x48_98_F8) : Color(hex: 0x90_93_99))
            }
            .contentShape(Rectangle())
    }

    @ViewBuilder
    private var searchBox: some View {
        if #available(macOS 26.0, *) {
            // 26：一颗玻璃胶囊 + 放大镜 + 清空钮，也就是系统各处搜索框的样子。
            // Qt 那个「Search 前缀标签 + 1px 竖线」是 web 时代的画法，mac 上没人这么做。
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                TextField("Search", text: $query)   // i18n-ignore: 与 Qt 一致保留英文
                    .textFieldStyle(.plain)
                    .font(.system(size: 12))
                if !query.isEmpty {
                    Button { query = "" } label: {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 12))
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 10)
            .frame(height: toolbarHeight)
            .glassCapsule()
        } else {
            legacySearchBox
        }
    }

    /// Search：整块圆角，左侧「Search」前缀标签 + 一条 1px 竖线 + 输入框。
    private var legacySearchBox: some View {
        HStack(spacing: 0) {
            Text("Search")
                .font(.system(size: 12))
                .foregroundStyle(theme.dark ? .white : Color(hex: 0x33_33_33))
                .padding(.horizontal, 10)
                .frame(maxHeight: .infinity)

            Rectangle()
                .fill(theme.dark ? Color(hex: 0x33_33_33) : Color(hex: 0xCC_CC_CC))
                .frame(width: 1)

            TextField("", text: $query)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
                .foregroundStyle(theme.dark ? .white : Color(hex: 0x33_33_33))
                .padding(.leading, 8)
        }
        .frame(height: toolbarHeight)
        .background {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .fill(theme.dark ? Color(hex: 0x44_44_44) : Color(hex: 0xEA_EA_EA))
        }
        .overlay {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .stroke(theme.dark ? Color(hex: 0x33_33_33) : Color(hex: 0xCC_CC_CC), lineWidth: 1)
        }
    }

    // MARK: 列表

    /// ★ 内距全部加在**滚动内容**上，不加在 `ScrollView` 外面：26 上顶栏是
    ///   `safeAreaBar`，列表要从它底下穿过去再由系统边缘效果渐隐。外面垫一圈的话，
    ///   列表会在离顶栏还有一段的地方被自己的边界硬切一刀（节点页踩过同一个坑）。
    private var list: some View {
        ScrollView {
            LazyVStack(spacing: 1) {
                ForEach(rows) { entry in
                    row(entry)
                }
            }
            .padding(.horizontal, 5)
            .padding(.bottom, 5)
        }
        .overlay {
            // Qt 那边空着就是一片空白。这里补一句 —— 「一条连接都没有」和「窗口坏了」
            // 在一片空白面前长得一模一样。
            if rows.isEmpty {
                Text("暂无连接".t)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.textMuted)
            }
        }
    }

    // MARK: 连接行

    /// 行高 42、半径 5、左右内距 10、元素间距 10。
    private func row(_ entry: ConnectionLedger.Entry) -> some View {
        let conn = entry.row
        let offline = entry.offline
        return HStack(spacing: 10) {
            Text("●")
                .font(.system(size: 10))
                .foregroundStyle(offline ? Color(hex: 0x99_99_99) : Color(hex: 0x67_C2_3A))

            Text("[\(conn.type)] \(conn.host)")
                .font(.system(size: 14))
                .foregroundStyle(offline ? Color(hex: 0x99_99_99)
                                 : (theme.dark ? Color(hex: 0xEE_EE_EE) : Color(hex: 0x33_33_33)))
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)

            // 发起连接的进程（核心的 find-process-mode 填的）。只有本机连接查得到，
            // 局域网设备的进程在别人机器上 —— 那种情况留空，不占位。
            if !conn.process.isEmpty {
                ConnBadge(symbol: nil, label: conn.process,
                          background: Color(red: 70 / 255, green: 110 / 255, blue: 168 / 255, opacity: 0.55),
                          foreground: .white)
            }
            ConnBadge(symbol: "arrow.triangle.branch", label: conn.chain,
                      background: Color.black.opacity(0.35), foreground: .white)
            ConnBadge(symbol: "arrow.down", label: conn.download > 0 ? Self.speed(conn.download) : "-",
                      background: Color.green.opacity(0.5), foreground: Color(hex: 0x33_33_33))
            ConnBadge(symbol: "arrow.up", label: conn.upload > 0 ? Self.speed(conn.upload) : "-",
                      background: Color.red.opacity(0.5), foreground: Color(hex: 0x33_33_33))

            CloseDot(disabled: offline) { state.closeConnection(id: conn.id) }
        }
        .padding(.horizontal, 10)
        .frame(height: 42)
        // 26：行浮在整窗玻璃上，得自己有一层材质才立得住 —— Qt 那个 `#eeeeee`
        // 压在浅色玻璃上几乎看不见（实测：行和背景糊成一片，只剩几枚徽标在飘）。
        // 26 以下窗口本来就是实底卡片色，沿用 Qt 的两个字面量。
        .background {
            if #available(macOS 26.0, *) {
                RoundedRectangle(cornerRadius: 5, style: .continuous).fill(.regularMaterial)
            } else {
                (theme.dark ? Color(hex: 0x22_22_22) : Color(hex: 0xEE_EE_EE))
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 5, style: .continuous))
        // 右键：拿本行的东西预填一条规则，再交给编辑器（类型/出口都还能改）。
        //
        // 有进程名时**多给一条**「添加进程规则」—— 这一行上其实有两个可以成规则的东西，
        // 而它们回答的是不同的问题：域名规则管「这个站怎么走」，进程规则管
        // 「这个程序怎么走」。想让某个 app 整个走直连时，按域名一条条加是加不完的。
        // 进程名只有本机连接才有（核心的 find-process-mode 填的），局域网设备的
        // 进程在别人机器上 —— 所以这条是按需出现，不占位。
        .contextMenu {
            Button("添加域名规则".t) {
                addingRule = RuleDraft(index: nil,
                                       rule: RulesStore.Rule(type: "DOMAIN-SUFFIX",
                                                             node: "DIRECT",
                                                             value: conn.host))
            }
            if !conn.process.isEmpty {
                Button("添加进程规则".t) {
                    addingRule = RuleDraft(index: nil,
                                           rule: RulesStore.Rule(type: "PROCESS-NAME",
                                                                 node: "DIRECT",
                                                                 value: conn.process))
                }
            }
        }
    }

    private func save(rule draft: RuleDraft) {
        let store = RulesStore()
        var loaded = store.load()
        loaded.rules.insert(draft.rule, at: 0)   // 新增前插，与设置页一致
        _ = store.save(rules: loaded.rules, areas: loaded.areas)
        Task { await state.controller.rebuildConfig() }
    }

    // MARK: 小工具

    /// 速率**不带空格**（`1.50KB`）—— 与 QML 的 `spd()` 一致。
    /// 行里四枚徽标横排，多一个空格就多挤掉一截 host。
    static func speed(_ value: Int64) -> String {
        var n = Double(max(0, value))
        let units = ["B", "KB", "MB", "GB", "TB"]
        var index = 0
        while n >= 1024, index < units.count - 1 {
            n /= 1024
            index += 1
        }
        return String(format: "%.2f%@", n, units[index])
    }

}

/// 连接行上的徽标：22 高、半径 5、宽 = 内容 + 12、图标与文字间距 4、字号 12。
private struct ConnBadge: View {
    let symbol: String?
    let label: String
    let background: Color
    let foreground: Color

    var body: some View {
        HStack(spacing: 4) {
            if let symbol {
                Image(systemName: symbol).font(.system(size: 12))
            }
            Text(label).font(.system(size: 12))
        }
        .foregroundStyle(foreground)
        .fixedSize()
        .padding(.horizontal, 6)
        .frame(height: 22)
        .background {
            RoundedRectangle(cornerRadius: 5, style: .continuous).fill(background)
        }
    }
}

/// ✕ 关闭这条连接：30×30、半径 3，悬停转红底白字；离线时置灰不可点
/// （连接已经断了，再发一次关闭没有意义）。
private struct CloseDot: View {
    @Environment(Theme.self) private var theme
    let disabled: Bool
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Text("✕")
                .font(.system(size: 12))
                .foregroundStyle(disabled ? (theme.dark ? Color(hex: 0x66_66_66) : Color(hex: 0xCC_CC_CC))
                                 : (hovering ? .white : Color(hex: 0xF5_6C_6C)))
                .frame(width: 30, height: 30)
                .background {
                    RoundedRectangle(cornerRadius: 3, style: .continuous)
                        .fill(hovering && !disabled ? Color(hex: 0xF5_6C_6C)
                              : (theme.dark ? Color(hex: 0x33_33_33) : Color(hex: 0xEE_EE_EE)))
                }
                .overlay {
                    if !theme.dark {
                        RoundedRectangle(cornerRadius: 3, style: .continuous)
                            .stroke(Color(hex: 0xDD_DD_DD), lineWidth: 1)
                    }
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(disabled)
        .onHover { hovering = $0 }
    }
}
