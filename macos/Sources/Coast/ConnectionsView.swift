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
    /// 搜索是否已展开。收起时只是一颗放大镜钮（设备页同款）。
    @State private var searchShown = false
    /// 液态玻璃形变要的命名空间：钮和输入框共享一个 `glassEffectID`，
    /// 点开时是**同一块玻璃**拉长成输入框，不是「藏钮、换控件」。
    @Namespace private var searchNS

    /// 顶栏控件高度：分段按钮与搜索框一致。26 以下沿用 Qt 的 26。
    ///
    /// 26 上是 **24**：带子高 50（见 `bandHeight`），控件在里面垂直居中。
    /// 早先那版是带子 54 + 顶栏另占 44，480 高的窗顶上先去掉 98 —— 现在这两条合成一条。
    private var toolbarHeight: CGFloat {
        if #available(macOS 26.0, *) { 24 } else { 26 }
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
            // 26：顶栏钉进标题栏那条带子（`safeAreaBar` + 整列越过顶部安全区），
            // 列表从它底下穿过、由系统 scroll edge effect 渐隐。
            //
            // ★ 顶栏**必须右对齐**。带子是 macOS 唯一的拖动区，早先那版从左边排起、
            //   把整条占满，结果**窗口拖不动了**（附属窗没有主窗那个
            //   `isMovableByWindowBackground`，见 `WindowRestore`，只能靠系统窗口菜单挪）。
            //   靠右之后左半条是空的：红绿灯 + 一段留白，按住那儿就能拖 ——
            //   系统自带的窗口也都是这么排的。
            //
            // ★ 带子抬到 `Self.bandHeight`（空 toolbar，`unifiesTitleBar: true`）——
            //   红绿灯由系统在这条带子里垂直居中，顶栏给同样的定高、内容也居中，
            //   两边就在一条水平线上。
            //   全屏时系统把标题栏整个收走，安全区归零，这一条也就跟着没了 ——
            //   顶栏直接顶到屏幕上沿，不留空带。
            list
                .safeAreaBar(edge: .top, spacing: 0) { header }
                .scrollEdgeEffectStyle(.soft, for: .all)
                .ignoresSafeArea(.container, edges: .top)
                // 附属窗默认进不了全屏，得自己把 collectionBehavior 换成 primary。
                .allowsFullScreen()
                // 整窗玻璃，和主窗同一层材质；行自带一层 `.regularMaterial`（见 `row`），
                // 两层材质叠出「卡片浮在玻璃上」的分层。
                .windowGlass(.sidebar)
        } else {
            VStack(spacing: 5) {
                HStack(spacing: 10) {
                    segmentedToggles
                    legacySearchBox
                }
                .padding(.top, 5)
                .padding(.horizontal, 5)
                list
            }
            .background(theme.card)
        }
    }

    // MARK: 顶栏

    /// 标题栏那条带子的高度。控件在里面垂直居中，红绿灯由系统居中，两边对齐。
    static let bandHeight: CGFloat = 50

    /// 钉在标题栏带子里的顶栏，**靠右**。左边那段空白是窗口的拖动区（见 `content`）。
    ///
    /// 定高 `bandHeight`、内容垂直居中（HStack 默认就是 `.center`）——
    /// 不用上下内距去凑：内距是「顶着上沿往下推」，控件一改高就又不居中了。
    private var header: some View {
        HStack(spacing: 8) {
            // 窗口标题。系统那份被 `hiddenTitleBar` 关掉了（它会画在带子正中，和右边
            // 这一组撞在一起），这里自己在左边摆一个。
            //
            // ★ 左内距要让开红绿灯：它们的右缘在窗口左缘往里 78（三颗 14 的按钮、
            //   起点 20、间距 20），再留 12 的呼吸。
            // ★ 只有它可压缩（其余都 `fixedSize`）——窗口拖到最小宽 480 且搜索展开时，
            //   总宽超出，让标题先截断，不能去挤按钮上的字。
            Text("连接管理器".t)
                .font(.system(size: 15))
                .foregroundStyle(.primary)
                .lineLimit(1)
                .truncationMode(.tail)
                .padding(.leading, Self.titleLeadingInset)
            Spacer(minLength: 8)
            segmentedToggles
            searchControls
        }
        .frame(height: Self.bandHeight)
        .padding(.trailing, 10)
    }

    /// 红绿灯右缘（78）+ 12 的呼吸。
    private static let titleLeadingInset: CGFloat = 78 + 12

    /// Online / Offline 两个筛选开关。两段各自独立（可以同时开），不是二选一。
    @ViewBuilder
    private var segmentedToggles: some View {
        if #available(macOS 26.0, *) {
            // **一颗玻璃胶囊，里面两个按钮**：`glassCapsule()` 罩整组，段自己什么底都不画。
            //
            // ★ 开/关只用**文字颜色**表达，不加底色。玻璃本身就是这一组的形，再往里塞
            //   一块色底等于在玻璃上贴纸 —— 折射、高光全被那块不透明的底盖掉，
            //   液态玻璃的质感就没了。关态压到 `.secondary`（浅一档），开态 `.primary`。
            //
            // ★ 也别再试 `.buttonStyle(.glass)`（不管外面套 `GlassEffectContainer` 还是
            //   `ControlGroup`）：那个样式**每颗自己画一个胶囊**，谁也拼不到一起，
            //   渲染出来永远是「中间带缝的两颗独立胶囊」。两条都走过了。
            //
            //   两段各是独立开关（可以同时开、也可以同时关），不是三选一。
            HStack(spacing: 0) {
                filterSegment(title: "Online (\(state.connectionLedger.onlineCount))",
                              on: showOnline) { showOnline.toggle() }
                filterSegment(title: "Offline (\(state.connectionLedger.offlineCount))",
                              on: showOffline) { showOffline.toggle() }
            }
            .glassCapsule()
        } else {
            legacySegmentedToggles
        }
    }

    /// 组里的一段：只有文字，开态 `.primary`、关态 `.secondary`。
    @available(macOS 26.0, *)
    private func filterSegment(title: String, on: Bool,
                               action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 12))
                .foregroundStyle(on ? AnyShapeStyle(.primary) : AnyShapeStyle(.secondary))
                .fixedSize()
                .padding(.horizontal, 12)
                .frame(height: toolbarHeight)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    /// 26 以下沿用 Qt 的画法：离线段左端**塞到在线段底下 3px**，中间无缝、只外侧圆角。
    ///
    /// ★ 用 `HStack(spacing: -3)` 让两段**自己量自己**，不要去算文字宽度。
    ///   先前的写法是用 `NSString.size(withAttributes:)` 量一遍再把和式写死给 `ZStack`，
    ///   结果量出来比 SwiftUI 实际排版**偏窄** —— 截图上「Offline (0)」的计数被裁掉了，
    ///   只剩「Offline」。负间距 + `zIndex` 就能同时拿到「重叠 3px」和「在线段盖在上面」，
    ///   一个数都不用量。
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

    /// 搜索：默认只有一颗放大镜钮，点开才拉成 200 宽的输入框。
    ///
    /// 26 上钮和输入框共享同一个 `glassEffectID` —— 点击时是**同一块玻璃**从钮的形状
    /// 拉长成输入框（系统 Liquid Glass 形变），不是「藏一个、显另一个」。
    /// 设备页、节点页是同一套做法。
    ///
    /// 为什么不常驻一个输入框：这条带子左半边要留给拖动（见 `content`），
    /// 常驻一个 200 的框会把可拖区挤没。
    @ViewBuilder
    private var searchControls: some View {
        if #available(macOS 26.0, *) {
            GlassEffectContainer(spacing: 2) {
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
            }
        } else {
            legacySearchBox
        }
    }

    /// 展开态：200 宽的输入框，右侧 ✕ 清空并收回钮形态。
    @available(macOS 26.0, *)
    private var glassSearchField: some View {
        ZStack(alignment: .trailing) {
            TextField("Search", text: $query)   // i18n-ignore: 与 Qt 一致保留英文
                .textFieldStyle(.plain)
                .font(.system(size: 12))
                .padding(.leading, 10)
                .padding(.trailing, 24)
            Button {
                query = ""
                withAnimation(.snappy(duration: 0.28)) { searchShown = false }
            } label: {
                Image(systemName: "xmark").font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
            .padding(.trailing, 8)
        }
        .frame(width: 200, height: toolbarHeight)
    }

    /// 收起态：一颗与分段同高的放大镜钮。
    @available(macOS 26.0, *)
    private var glassSearchButton: some View {
        Button {
            withAnimation(.snappy(duration: 0.28)) { searchShown = true }
        } label: {
            Image(systemName: "magnifyingglass").font(.system(size: 12))
                .frame(width: toolbarHeight, height: toolbarHeight)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
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
