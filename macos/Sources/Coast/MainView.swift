import CoastKit
import SwiftUI

/// 应用主窗骨架：左侧栏（logo / 导航 / 版本）+ 内容卡 + 页脚（日志 / 开关 / 模式）。
/// 对齐 `qml/Main.qml` 的布局与尺寸。
struct MainView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    /// 模式按钮组是否展开。
    @State private var modeExpanded = false
    /// 更新窗（点侧栏版本行打开）。
    @State private var showingUpdate = false

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
        .sheet(isPresented: $showingUpdate) {
            UpdateView().environment(state).environment(theme)
        }
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

    /// 侧栏底部的版本行。对齐 `qml/Main.qml` 的 `verRow`：
    /// 平时灰；程序有新版 → 右上角 "new" 角标，内核有新版 → "core" 角标；
    /// **任一有更新，版本文字本身转红**（全 UI 不加粗，靠颜色说话）。
    ///
    /// 点它打开更新窗 —— 与 Qt 相同（那边是独立顶层窗，这里是 sheet，
    /// 沿用本项目对 `ConnectionsWindow` / `RuleEditorWindow` 的既有做法）。
    private var versionRow: some View {
        Button {
            showingUpdate = true
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
            Image(systemName: "terminal")
                .font(.system(size: 12))
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
    /// 模式切换：收起是一颗按钮，点开就地展开成**一个连体的按钮组**（分段控件）。
    ///
    /// 三段紧挨、没有各自的圆角，整组只有一层玻璃、只有外侧是圆的 ——
    /// 这才是「同一组、三选一」；三颗各自成形的胶囊读起来仍是三个独立按钮。
    ///
    /// 每段的内部排版与 `FooterSwitch` 完全一致（呼吸圆点 12 + 间距 6 + 字号 12 +
    /// 左内距 8 / 右 10 + 高度 28），且两者都走 `.glassCapsule()`，所以
    /// 单颗开关与整组分段必然同高，整组正好是单颗的三倍宽。
    ///
    /// 尺寸跟着 `FooterSwitch` 一起从 24 抬到 28、圆点从 8 换成 12 —— 那是 Qt
    /// `qml/FooterSwitch.qml` 的实际度量；**两处必须一起改**，否则这一排就参差不齐。
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

    /// 一段的内容。与 `FooterSwitch` 的内部排版逐项一致。
    private func modeSegment(_ title: String, active: Bool) -> some View {
        HStack(spacing: 6) {
            BreathingDot(isOn: active)
            Text(title)
                .font(.system(size: 12))
                .foregroundStyle(active ? theme.textPrimary : theme.textMuted)
                .fixedSize()
        }
        .padding(.leading, 8)
        .padding(.trailing, 10)
        .frame(height: 28)
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
