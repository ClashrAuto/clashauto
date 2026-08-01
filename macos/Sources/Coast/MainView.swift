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

    var body: some View {
        HStack(spacing: 0) {
            sidebar
                .frame(width: theme.sidebarWidth)

            if #available(macOS 26.0, *) {
                // macOS 26（Liquid Glass）：主内容**不再是一块带底的卡**，改用系统应用
                // 的条形层级 —— 页脚是 `safeAreaBar` 压在滚动区之上，各页自己的顶栏
                // 经 `pageHeaderBar` 抬成钉在最顶部的导航栏（无顶栏的状态页放一条
                // 不可见 bar）；页面里的 ScrollView 把内容滚进这些条**底下**，
                // 系统 scroll edge effect（`.soft`）把穿过去的内容渐进淡出。
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
            } else {
                VStack(spacing: 0) {
                    Card { page }
                        .padding(.top, theme.inset)
                        .padding(.trailing, theme.inset)
                    footer
                        .frame(height: theme.footerHeight)
                        // 页脚右侧同样让出 5（它和内容卡在同一列里，Qt 那边这 5 是加在整列上的）。
                        // 左侧**不留内距** —— Qt 的注释写着「底部状态栏左侧内容贴左对齐」。
                        .padding(.trailing, theme.inset)
                }
            }
        }
        // mac 上窗体本身透明、露出毛玻璃（见 CoastApp 的 .windowGlass(.sidebar)）
        .background(.clear)
        .preferredColorScheme(theme.dark ? .dark : .light)
    }

    // MARK: 侧栏

    /// 26 上导航项右侧的留白（与左侧 20 对称）；26 以下贴内容卡、不留。
    private var navTrailingInset: CGFloat {
        if #available(macOS 26.0, *) { 20 } else { 0 }
    }

    /// 侧栏整列的顶距：Qt 是 16；26 上标题栏加高后减 10。
    private var sidebarTopInset: CGFloat {
        if #available(macOS 26.0, *) { 6 } else { 16 }
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
                .padding(.leading, 20)
                // 26：右侧留和左侧一样的 20 —— 高亮是四角全圆的独立胶囊（见
                // NavButton），两侧等距才像悬浮的一颗；26 以下右缘要贴内容卡，不留。
                .padding(.trailing, navTrailingInset)
                .padding(.top, page == .status ? 0 : 5)
            }

            Spacer(minLength: 0)
            versionRow
                .padding(.bottom, 5)
        }
        // 26：logo 顶距在 Qt 的 16 基础上减 10（标题栏加高后原值显得空）。
        .padding(.top, sidebarTopInset)
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

    /// 侧栏底部的版本行。对齐 `qml/Main.qml` 的 `verRow`：
    /// 平时灰；程序有新版 → 右上角 "new" 角标，内核有新版 → "core" 角标；
    /// **任一有更新，版本文字本身转红**（全 UI 不加粗，靠颜色说话）。
    ///
    /// 点它打开更新窗 —— 与 Qt 一样是**独立顶层窗**（`Window(id:)` scene）。
    /// （早期这里是 sheet，因为 600×560 比主窗还高被裁掉了底部动作行，已改掉；
    /// 这句注释一直没跟上，顺手更正。）
    private var versionRow: some View {
        Button {
            openWindow(id: UpdateWindowID.value)
        } label: {
            // 角标贴在版本文字的**右上角**：QML 里 badgeRow 的
            // `verticalCenter` 对的是 `verText.top`，也就是整组有一半浮在文字上方。
            HStack(alignment: .top, spacing: 3) {
                Text("Ver: \(AppInfo.version)")
                    .font(.system(size: 12))
                    .foregroundStyle(anyUpdate ? theme.danger : theme.versionColor)

                if anyUpdate {
                    HStack(spacing: 3) {
                        if state.appUpdateAvailable { UpdateBadge(text: "new") }
                        if state.coreUpdateAvailable { UpdateBadge(text: "core") }
                    }
                    .alignmentGuide(.top) { $0[VerticalAlignment.center] }
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .pointingHandCursor()
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
