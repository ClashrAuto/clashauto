import CoastKit
import SwiftUI

/// 应用主窗骨架：左侧栏（logo / 导航 / 版本）+ 内容卡 + 页脚（日志 / 开关 / 模式）。
/// 对齐 `qml/Main.qml` 的布局与尺寸。
struct MainView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

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

    private var modePicker: some View {
        Menu {
            ForEach(Array(AppState.modeTitles.enumerated()), id: \.offset) { index, title in
                Button(title) { state.setMode(title) }
            }
        } label: {
            HStack(spacing: 4) {
                Text(AppState.modeTitles[state.modeIndex])
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textPrimary)
                Spacer(minLength: 0)
                Image(systemName: "chevron.down")
                    .font(.system(size: 8))
                    .foregroundStyle(theme.textMuted)
            }
            .padding(.horizontal, 10)
            .frame(width: 120, height: 28)
            .background(RoundedRectangle(cornerRadius: 3).fill(theme.footerComboBg))
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .frame(width: 120, height: 28)
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
