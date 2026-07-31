import CoastKit
import SwiftUI

/// 关于页。**逐元素对齐** `qml/AboutPage.qml`：居中 logo + 「Coast」标题 +
/// 版本行（点击查更新，发现新版转红）+ 一次性提示 + 简介 + 三颗链接药丸。
///
/// 这一版把原来那个「左对齐的设置式分组页」整个换掉了 —— 它有更新/更新说明/路径三个卡，
/// 与 Qt 的居中 hero 布局没有任何对应关系。两处 Qt 侧没有的东西一并去掉：
/// **路径卡**（Qt 的关于页与设置页都没有这块）和 **更新说明正文**
/// （Qt 把它放在独立的 `UpdateWindow.qml` 里，不在关于页上）。
/// 内核版本与 GeoIP 的更新入口本来就在设置页，没有随之丢失。
struct AboutPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    /// 链接数据。label 与 url 都与 `qml/AboutPage.qml` 的 `links` 逐条相同。
    private var links: [(label: String, url: String)] {
        [("官网/文档".t, "https://clashr-auto.gitbook.io"),
         ("项目地址".t, "https://github.com/clashrauto/clashr-auto-desktop"),
         ("用户讨论".t, "https://t.me/clashr_auto")]
    }

    var body: some View {
        Card {
            GeometryReader { geo in
                VStack(spacing: 14) {
                    // —— logo ——（Qt 用 iconfont 字形 84px，这里是同尺寸的 SF Symbol）
                    Image(systemName: "globe")
                        .font(.system(size: 84))
                        .foregroundStyle(theme.accent)

                    Text("Coast")
                        .font(.system(size: 30))
                        .foregroundStyle(theme.textPrimary)

                    versionLine

                    // —— 一次性提示（已最新 / 检查失败），淡入淡出 ——
                    if !state.updateHint.isEmpty {
                        Text(state.updateHint)
                            .font(.system(size: 12))
                            .foregroundStyle(state.updateHintIsError ? theme.danger : theme.textMuted)
                            .multilineTextAlignment(.center)
                            .frame(maxWidth: .infinity)
                            .transition(.opacity)
                    }

                    Text("基于 Clash / mihomo 内核的跨平台代理客户端，自动订阅、节点测速、规则分流，简单好用。".t)
                        .font(.system(size: 13))
                        // Qt 的 `lineHeight: 1.35` —— 13px 字号下行距比默认松一档。
                        .lineSpacing(13 * 0.35)
                        .foregroundStyle(theme.textSecondary)
                        .multilineTextAlignment(.center)
                        .fixedSize(horizontal: false, vertical: true)
                        .frame(maxWidth: .infinity)

                    // QML 里那个 `Item { Layout.preferredHeight: 6 }`：链接行前多让 6。
                    Color.clear.frame(height: 6)

                    HStack(spacing: 10) {
                        ForEach(links, id: \.url) { link in
                            LinkPill(label: link.label, url: link.url)
                        }
                    }
                }
                // 列宽 = min(卡宽 - 80, 420)，整列居中 —— 与 QML 的 `anchors.centerIn` 同义。
                .frame(width: min(geo.size.width - 80, 420))
                .frame(width: geo.size.width, height: geo.size.height)
            }
        }
        .padding(10)
        .animation(.easeInOut(duration: 0.18), value: state.updateHint)
        .task { await state.checkUpdates() }
    }

    /// 版本行的三种状态与配色，逐条对齐 QML：
    /// 检查中=灰、有新版=红（点击开发布页）、其余=绿（点击检查更新）。
    /// 绿色 `#4da13e` 是 QML 里的字面量，不是主题令牌 —— 那边也是写死的。
    private var versionLine: some View {
        Button {
            if state.appUpdateAvailable {
                state.openReleasePage()
            } else {
                Task { await state.checkUpdates() }
            }
        } label: {
            Text(versionText)
                .font(.system(size: 14))
                .foregroundStyle(state.checkingUpdate ? theme.textMuted
                                 : state.appUpdateAvailable ? theme.danger
                                 : Color(hex: 0x4D_A1_3E))
                .multilineTextAlignment(.center)
        }
        .buttonStyle(.plain)
        .disabled(state.checkingUpdate)
        .pointingHandCursor()
    }

    /// 文案的**拼法**照抄 QML（三段各自是翻译表里的一条），不合并成一条格式串 ——
    /// 合并的话这三段在与 Qt 共用的翻译表里就都失配了。
    private var versionText: String {
        if state.checkingUpdate { return "正在检查更新…".t }
        if let release = state.appRelease, state.appUpdateAvailable {
            return "v" + AppInfo.version + "  →  " + release.tag + "  有新版本".t
        }
        return "版本 ".t + AppInfo.version + "  ·  点击检查更新".t
    }


}

/// 关于页底部那颗链接药丸。对齐 QML 的 delegate：
/// 高 30、半径 15（正好是胶囊）、宽 = 文字宽 + 26、1px 描边；
/// 悬停时底色、描边、文字色三样一起变。
private struct LinkPill: View {
    @Environment(Theme.self) private var theme
    let label: String
    let url: String

    @State private var hovering = false

    var body: some View {
        Button {
            if let target = URL(string: url) { NSWorkspace.shared.open(target) }
        } label: {
            Text(label)
                .font(.system(size: 12))
                .foregroundStyle(hovering ? theme.accentStrong : theme.textSecondary)
                // 宽 = 文字宽 + 26 → 左右各 13。
                .padding(.horizontal, 13)
                .frame(height: 30)
                .background {
                    RoundedRectangle(cornerRadius: 15, style: .continuous)
                        .fill(hovering ? theme.hover : theme.metricBg)
                }
                .overlay {
                    RoundedRectangle(cornerRadius: 15, style: .continuous)
                        .stroke(hovering ? theme.accent : theme.divider, lineWidth: 1)
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
    }
}

extension View {
    /// QML 的 `HoverHandler { cursorShape: Qt.PointingHandCursor }`。
    /// SwiftUI 没有等价修饰符，只能在 hover 进出时自己推/弹光标。
    func pointingHandCursor() -> some View {
        onHover { inside in
            if inside { NSCursor.pointingHand.push() } else { NSCursor.pop() }
        }
    }
}
