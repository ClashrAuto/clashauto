import CoastKit
import SwiftUI

/// 应用主窗骨架：左侧栏（logo / 导航 / 版本）+ 内容卡 + 页脚（日志 / 开关 / 模式）。
/// 对齐 `qml/Main.qml` 的布局与尺寸。
struct MainView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    /// 模式按钮组是否展开。`COAST_MODE_EXPANDED=1` 时启动即展开 ——
    /// 与 `COAST_INITIAL_PAGE` 同类的 UI 调试钩子：展开态只能点出来，
    /// 无头截图验证页脚布局时需要一个不经交互的入口。
    @State private var modeExpanded =
        ProcessInfo.processInfo.environment["COAST_MODE_EXPANDED"] == "1"
    /// 打开独立窗口用（更新窗）。
    @Environment(\.openWindow) private var openWindow
    /// 启动即打开更新窗（`COAST_OPEN_UPDATE=1`）。与 `COAST_MODE_EXPANDED` 同类的 UI 调试钩子：
    /// 那个窗只能点出来，而无头截图验证它的版式时需要一个不经交互的入口。
    private let opensUpdateWindow =
        ProcessInfo.processInfo.environment["COAST_OPEN_UPDATE"] == "1"

    var body: some View {
        HStack(spacing: 0) {
            sidebar
                .frame(width: theme.sidebarWidth)

            // 主内容**不是一块带底的卡**，走系统应用的条形层级 —— 页脚是 `safeAreaBar`
            // 压在滚动区之上，各页自己的顶栏经 `pageHeaderBar` 抬成钉在最顶部的导航栏
            //（无顶栏的状态页放一条不可见 bar）；页面里的 ScrollView 把内容滚进这些条
            // **底下**，系统 scroll edge effect（`.soft`）把穿过去的内容渐进淡出。
            //
            // ★ 必须是 `safeAreaBar` 而不是 `safeAreaInset`：只有 bar 会让其内的
            //   滚动视图自动挂上边缘效果 —— 用 inset 时内容照样从页脚底下穿过，
            //   但没有任何淡出，footer 的字直接叠在内容的字上（实测截图看得一清二楚）。
            page
                .safeAreaBar(edge: .bottom, spacing: 0) {
                    // 页脚成了「底部条」：内容从它底下穿过，本体仍旧透玻璃。
                    // 右侧留 7（列边距 5 再收 2）—— 只给页脚，
                    // **中间的主内容**占满整个内容区宽度，不跟着让。
                    footer.frame(height: theme.footerHeight)
                        .padding(.trailing, theme.inset + 2)
                }
                .scrollEdgeEffectStyle(.soft, for: .all)
                // 主内容列**越过标题栏安全区**、顶到窗顶（距离 0）——
                // 红绿灯在左边压着侧栏，各页顶部导航栏与它同一条带。
                .ignoresSafeArea(.container, edges: .top)
        }
        // mac 上窗体本身透明、露出毛玻璃（见 CoastApp 的 .windowGlass(.sidebar)）
        .background(.clear)
        .preferredColorScheme(theme.dark ? .dark : .light)
        .task {
            // 延一拍再开：启动时 `WindowRestore.adopt()` 会把「系统恢复出来的附属窗」一律关掉
            // （见那边），不等它跑完就开，窗会被当成恢复出来的当场关回去 —— 时序不定，
            // 表现就是这个钩子时灵时不灵。
            guard opensUpdateWindow else { return }
            try? await Task.sleep(for: .seconds(2))
            openWindow(id: UpdateWindowID.value)
        }
    }

    // MARK: 侧栏

    /// 导航项右侧的留白，与左侧 20 对称 —— 高亮是四角全圆的独立胶囊（见 `NavButton`），
    /// 两侧等距才像悬浮的一颗。
    private let navTrailingInset: CGFloat = 20

    /// `Page` → ⌘数字。顺序与侧栏一致（`Page.allCases`）。
    private func shortcutKey(for page: AppState.Page) -> KeyEquivalent {
        let digits: [KeyEquivalent] = ["1", "2", "3", "4", "5", "6", "7", "8", "9"]
        let idx = AppState.Page.allCases.firstIndex(of: page) ?? 0
        return idx < digits.count ? digits[idx] : "0"
    }

    private var sidebar: some View {
        VStack(spacing: 0) {
            logo
                .frame(height: 118)

            ForEach(AppState.Page.allCases) { page in
                NavButton(title: page.title,
                          icon: page.icon,
                          isCurrent: state.currentPage == page) {
                    state.currentPage = page
                }
                // ⌘1..⌘7 切页。两条线原本**都没有**切页快捷键 —— macOS 上这属于基本预期
                // （Finder/Xcode/浏览器都有），补上。顺带也让「当前在哪一页」可自动化验证：
                // `ClashService.nodesVisible` 那套按页分档的轮询频率，此前只验得了非节点页那一侧 ——
                // SwiftUI 的侧栏不暴露成可访问按钮（`every button of window 1` 只拿得到三个无名的
                // 窗口控制按钮），没有快捷键就没法在脚本里切页。
                .keyboardShortcut(shortcutKey(for: page), modifiers: .command)
                .padding(.leading, 20)
                .padding(.trailing, navTrailingInset)
                .padding(.top, page == .status ? 0 : 5)
            }

            Spacer(minLength: 0)
            versionRow
                .padding(.bottom, 5)
        }
        // ★ 侧栏**留在顶部安全区里**（不加 `ignoresSafeArea`）：起点落在标题栏那条带子
        //   下面（空 toolbar 把它抬到了 50），logo 因此不会钻到红绿灯底下。
        //   右边的主内容列是越过安全区的（见 `body`）—— 两列本来就该不一样：
        //   那一列顶上是各页自己的顶栏，与红绿灯同处一条带子；这一列顶上是 logo。
    }

    private var logo: some View {
        ZStack(alignment: .bottomTrailing) {
            // Qt `qml/Main.qml` logoBox：iconfont 字体的地球字形 U+E600、74px ——
            // 不是 SF 的 "globe"（形状不同，60pt 时也比 Qt 的小一圈）。
            Text("\u{E600}")
                .font(.custom(IconFont.logo, size: 74))
                .foregroundStyle(theme.accent)
                .frame(width: 74, height: 74)

            StatusBadge(tunEnabled: state.controller.isTunEnabled,
                        proxyEnabled: state.controller.isProxyEnabled,
                        coreRunning: state.controller.isCoreRunning)
        }
        .frame(width: 74, height: 74)
    }

    /// 侧栏底部的版本行。对齐 `qml/Main.qml` 的 `verRow`：版本号**恒为灰色**、
    /// 刻进侧栏里（下沿 1pt 高光）；有更新时右上角浮一颗角标。
    ///
    /// ★ 版本号原来会**跟着角标一起转红**。两件事说的是同一句话，而红字比角标醒目得多 ——
    ///   侧栏底部于是长期挂着一行红字，等于把「有个更新可以看看」喊成了「出事了」。
    ///   红色在这套 UI 里另有职责（错误、断线、被争抢的设备），被这行占着就贬值了。
    ///
    /// 点它打开更新窗 —— 与 Qt 一样是**独立顶层窗**（`Window(id:)` scene）。
    /// （早期这里是 sheet，因为 600×560 比主窗还高被裁掉了底部动作行，已改掉；
    /// 这句注释一直没跟上，顺手更正。）
    private var versionRow: some View {
        Button {
            openWindow(id: UpdateWindowID.value)
        } label: {
            versionText
                .foregroundStyle(theme.versionColor)
                .contentShape(Rectangle())
                // 「刻进去」的下沿高光：同一串字、往下 1pt 的一层副本画在正文**之下**。
                // 这就是文字版的内阴影 —— SwiftUI 的 `.shadow()` 是外投影，做不出凹槽。
                // 用 `.background` 而不是 `.overlay`，副本才压在正文底下（露出的只有下边缘那 1pt）。
                .background {
                    versionText
                        .foregroundStyle(theme.versionEmboss)
                        .offset(y: 1)
                }
                // 角标**浮**在版本文字右上角：用 overlay 而不是并排放进 HStack ——
                // 版本行在侧栏里是居中的，角标一旦占布局宽度，检查跑完角标一出现，
                // 整行版本号就往左跳一格（跳多远还随 "new"/"core" 字宽而变）。
                // overlay 不参与父视图定尺寸，位置就与「有没有更新」无关了。
                .overlay(alignment: .topTrailing) { badge }
        }
        .buttonStyle(.plain)
        .pointingHandCursor()
    }

    /// 版本号本体。正文与那层高光副本必须**同字同字号**，所以只写一处。
    private var versionText: Text {
        Text("Ver: \(AppInfo.version)").font(.system(size: 12))
    }

    /// 更新角标。**同时只显示一颗**，程序更新压过内核更新（new > core）。
    ///
    /// ★ 原来两颗会一起亮。8px 的字挤在版本号右上角连成一团（"newcore"），读不出是两个词，
    ///   侧栏也没有第二颗的宽度。合成一颗不丢信息：两者的下一步是同一个 —— 点开更新窗，
    ///   那里分页签写着各自的详情。程序优先是因为程序更新往往连内核一起带新
    ///   （打包时内嵌最新正式版内核）。
    ///
    /// 两条 `alignmentGuide` 逐字对应 QML 的 `anchors.left: verText.right` +
    /// `anchors.leftMargin: 3` + `anchors.verticalCenter: verText.top`：
    /// 左缘落在文字右缘外 3、竖直中线对着文字顶边（半颗浮在文字上方）。
    @ViewBuilder
    private var badge: some View {
        if anyUpdate {
            UpdateBadge(text: state.appUpdateAvailable ? "new" : "core")
                .alignmentGuide(.trailing) { _ in -3 }
                .alignmentGuide(.top) { $0[VerticalAlignment.center] }
        }
    }

    private var anyUpdate: Bool { state.appUpdateAvailable || state.coreUpdateAvailable }

    // MARK: 内容

    @ViewBuilder
    private var page: some View {
        switch state.currentPage {
        case .status: StatusPage()
        case .nodes: NodesPage()
        case .devices: DevicesPage()
        case .subscriptions: SubscriptionsPage()
        case .settings: SettingsPage()
        case .logs: LogsPage()
        case .about: AboutPage()
        }
    }

    // MARK: 页脚

    private var footer: some View {
        HStack(spacing: 5) {
            // 这一行只放**程序侧、值得看见的**那条（`LogKind.notice`）：核心原文和
            // 「Start sysproxy ok!」这类每次开关都会刷的例行回执都不进来 ——
            // 页脚只有一行，被它们占着的话真正要看的错误永远露不出来。
            //
            // 空着时**连图标一起收掉**：一个孤零零的终端图标后面什么都没有，看着像坏了。
            // 刚启动、还没有任何值得说的事情时就是这个状态。
            if state.lastLog.isEmpty {
                Spacer(minLength: 0)
            } else {
                // Qt 这个图标是 14（比旁边 12 的日志文字大一档），原来写成了 12。
                Image(systemName: "terminal")
                    .font(.system(size: 14))
                    .foregroundStyle(theme.textMuted)

                Text(state.lastLog)
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    // ★ 让日志行**先**被压缩。不给优先级的话，SwiftUI 会去截右侧按钮上的
                    //   文字 —— 展开模式按钮组时第一个选项就变成了「…」，按钮上的字没了，
                    //   而左边那行日志本来就是可有可无的信息。
                    .layoutPriority(0)
            }

            FooterSwitch(label: "增强".t, isOn: state.controller.isTunEnabled) { state.toggleTun() }
            FooterSwitch(label: "网页".t, isOn: state.controller.isProxyEnabled) { state.toggleProxy() }
            FooterSwitch(label: "核心".t, isOn: state.controller.isCoreRunning) { state.toggleCore() }

            // ★ `.fixedSize()` 是关键：按钮永远不被压缩。
            //   只加 layoutPriority 不够 —— 那只影响「谁先拿到空间」，空间真不够时
            //   SwiftUI 仍会去截按钮上的字，展开时第一个选项就成了「…」，
            //   而按钮少一个字就不知道点的是什么。左边那行日志才是该先让位的。
            modePicker
                .fixedSize()
        }
    }

    /// 模式切换。**按钮点开菜单**，不是下拉框。
    ///
    /// 原来用 `.menuStyle(.borderlessButton)` + 手画一个方角底，点上去像个 combo box：
    /// 有边框、有固定宽、右侧一个小箭头。改成 `.button` 样式后它就是一颗普通按钮，
    /// 和旁边三个开关同属一排、同样的玻璃胶囊 —— 视觉上是「四个可点的东西」，
    /// 而不是「三个开关加一个表单控件」。
    ///
    /// 当前模式直接作为按钮标题，省掉那行冗余的箭头指示：按钮上写着「规则」，
    /// 点开就是三个模式，没有第二种解释。
    /// 模式切换：收起是一颗按钮，点开就地展开成**一个连体的按钮组**（分段控件）。
    ///
    /// 三段紧挨、没有各自的圆角，整组只有一层玻璃、只有外侧是圆的 ——
    /// 这才是「同一组、三选一」；三颗各自成形的胶囊读起来仍是三个独立按钮。
    ///
    /// 每段与 `FooterSwitch` 共用同一组定宽定高（`Theme.footerButtonWidth/Height`），
    /// 且两者都走 `.glassCapsule()`，所以单颗开关与整组分段必然同高，
    /// 整组正好是单颗的三倍宽。段里**没有**呼吸圆点 —— 选中态由底色 + 文字色表达，
    /// 圆点留给三个开关去表达「开/关」；这里是「三选一」，不是开关。
    private var modePicker: some View {
        Group {
            if modeExpanded {
                HStack(spacing: 0) {
                    ForEach(Array(AppState.modeTitles.enumerated()), id: \.offset) { index, title in
                        Button {
                            state.setMode(title)
                            withAnimation(.snappy(duration: 0.22)) { modeExpanded = false }
                        } label: {
                            modeSegment(title, active: index == state.modeIndex)
                        }
                        .buttonStyle(.plain)
                        .background {
                            // 选中段的底压在整组玻璃**里面**，不会给整组带来额外圆角
                            if index == state.modeIndex {
                                Capsule().fill(theme.accent.opacity(0.45))
                            }
                        }
                    }
                }
                .glassCapsule()
            } else {
                Button {
                    withAnimation(.snappy(duration: 0.22)) { modeExpanded = true }
                } label: {
                    modeSegment(AppState.modeTitles[state.modeIndex], active: true)
                }
                .buttonStyle(.plain)
                .glassCapsule()
            }
        }
    }

    /// 一段的内容。与 `FooterSwitch` 同一组定宽定高，但**不带**呼吸圆点：
    /// 选中态由底色 + 文字色表达（收起时按钮恒为选中态）。
    private func modeSegment(_ title: String, active: Bool) -> some View {
        Text(title)
            .font(.system(size: 12))
            .foregroundStyle(active ? theme.textPrimary : theme.textMuted)
            .lineLimit(1).truncationMode(.tail)
            .padding(.horizontal, 8)
            .frame(width: theme.footerButtonWidth, height: theme.footerButtonHeight)
            .contentShape(Rectangle())
    }
}

/// 尚未实现的页面占位。**明确写出属于哪个阶段** —— 空白页会让人以为是坏了。
struct PlaceholderPage: View {
    @Environment(Theme.self) private var theme
    let title: String
    let note: String

    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "hammer")
                .font(.system(size: 28))
                .foregroundStyle(theme.textMuted)
            Text(title).font(.system(size: 16)).foregroundStyle(theme.textPrimary)
            Text(note).font(.system(size: 12)).foregroundStyle(theme.textMuted)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

enum AppInfo {
    static var version: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev"
    }
}
