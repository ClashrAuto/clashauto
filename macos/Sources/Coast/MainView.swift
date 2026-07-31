import CoastKit
import SwiftUI

/// 应用主窗骨架：左侧栏（logo / 导航 / 版本）+ 内容卡 + 页脚（日志 / 开关 / 模式）。
/// 对齐 `qml/Main.qml` 的布局与尺寸。
struct MainView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    /// 模式按钮组是否展开。
    @State private var modeExpanded = false

    var body: some View {
        HStack(spacing: 0) {
            sidebar
                .frame(width: theme.sidebarWidth)

            VStack(spacing: 0) {
                Card { page }
                    .padding(.top, theme.inset)
                    .padding(.trailing, theme.inset)
                footer
                    .frame(height: theme.footerHeight)
            }
        }
        // mac 上窗体本身透明、露出毛玻璃（见 CoastApp 的 .background(.ultraThinMaterial)）
        .background(.clear)
        .preferredColorScheme(theme.dark ? .dark : .light)
    }

    // MARK: 侧栏

    private var sidebar: some View {
        VStack(spacing: 0) {
            logo
                .frame(height: 118)

            ForEach(AppState.Page.allCases) { page in
                NavButton(title: page.title,
                          symbol: page.symbol,
                          isCurrent: state.currentPage == page) {
                    state.currentPage = page
                }
                .padding(.leading, 20)
                .padding(.top, page == .status ? 0 : 5)
            }

            Spacer(minLength: 0)
            versionRow
                .padding(.bottom, 5)
        }
        .padding(.top, 16)
    }

    private var logo: some View {
        ZStack(alignment: .bottomTrailing) {
            Image(systemName: "globe")
                .font(.system(size: 60))
                .foregroundStyle(theme.accent)
                .frame(width: 74, height: 74)

            StatusBadge(tunEnabled: state.controller.isTunEnabled,
                        proxyEnabled: state.controller.isProxyEnabled,
                        coreRunning: state.controller.isCoreRunning)
        }
        .frame(width: 74, height: 74)
    }

    private var versionRow: some View {
        HStack(alignment: .top, spacing: 3) {
            Text("Ver: \(AppInfo.version)")
                .font(.system(size: 12))
                .foregroundStyle(theme.versionColor)
        }
    }

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
            Image(systemName: "terminal")
                .font(.system(size: 12))
                .foregroundStyle(theme.textMuted)

            Text(state.lastLog)
                .font(.system(size: 12))
                .foregroundStyle(theme.textSecondary)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)

            FooterSwitch(label: "增强".t, isOn: state.controller.isTunEnabled) { state.toggleTun() }
            FooterSwitch(label: "网页".t, isOn: state.controller.isProxyEnabled) { state.toggleProxy() }
            FooterSwitch(label: "核心".t, isOn: state.controller.isCoreRunning) { state.toggleCore() }

            modePicker
        }
        .padding(.horizontal, 8)
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
    /// 模式切换：收起是一颗按钮，点开就地展开成三颗，选完自动收回。
    ///
    /// ★ 直接复用 `FooterSwitch` 本身，不另写一套外观。
    ///   先前手写的那套(自己拼 HStack + .glassCapsule)与它走的是两条尺寸路径 ——
    ///   FooterSwitch 用 .glassButton()（系统按钮样式，自带内边距）、宽度随文字，
    ///   手写那套用 .glassCapsule()（纯背景）、宽度写死 76 —— 高和宽都对不上，
    ///   而且改一处就得同步另一处。用同一个组件，尺寸一致是结构保证的，不是对出来的。
    private var modePicker: some View {
        HStack(spacing: 6) {
            if modeExpanded {
                ForEach(Array(AppState.modeTitles.enumerated()), id: \.offset) { index, title in
                    FooterSwitch(label: title, isOn: index == state.modeIndex) {
                        state.setMode(title)
                        withAnimation(.snappy(duration: 0.22)) { modeExpanded = false }
                    }
                }
            } else {
                FooterSwitch(label: AppState.modeTitles[state.modeIndex], isOn: true) {
                    withAnimation(.snappy(duration: 0.22)) { modeExpanded = true }
                }
            }
        }
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
