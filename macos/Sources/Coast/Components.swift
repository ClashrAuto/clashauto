import SwiftUI

/// 内容卡：浮在毛玻璃上的不透明圆角面。对齐 `qml/Card.qml`。
struct Card<Content: View>: View {
    @Environment(Theme.self) private var theme
    @ViewBuilder var content: Content

    var body: some View {
        content
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            // 内容卡半透：整窗玻璃的关键一层。
            //
            // 原来是实底 `theme.card`，于是只有侧栏和页脚透、中间一大块是死的 ——
            // 「整个窗口的毛玻璃」其实只做了个边。这里压到 0.55 而不是全透：
            // 全透的话卡片上的正文会直接压在桌面壁纸上，深色壁纸尚可，
            // 亮色壁纸下小字基本读不了。
            .background(theme.card.opacity(0.55))
            .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
    }
}

/// 侧栏导航按钮。对齐 `qml/NavButton.qml`：选中态用品牌色底，未选中悬停时给一层浅底。
struct NavButton: View {
    @Environment(Theme.self) private var theme
    let title: String
    let symbol: String
    let isCurrent: Bool
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                Image(systemName: symbol)
                    .font(.system(size: 14))
                    .frame(width: 18)
                Text(title)
                    .font(.system(size: 13))
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12)
            .frame(height: 34)
            // 选中态是**深色块 + 白字**，不是主题色高亮 —— 对齐 Qt 侧栏。
            // 用 accent 填充的话，侧栏会冒出一整块饱和的蓝，和右侧内容区的深灰打架；
            // Qt 那边选中项只比背景更深一档，靠白字区分。
            .foregroundStyle(isCurrent ? theme.textPrimary : theme.textSecondary)
            .background {
                RoundedRectangle(cornerRadius: theme.radius, style: .continuous)
                    .fill(isCurrent ? theme.navSelected : (hovering ? theme.hover : .clear))
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
    }
}

/// 页脚开关（增强/网页/核心）。对齐 `qml/FooterSwitch.qml`：一个带状态点的小胶囊。
struct FooterSwitch: View {
    @Environment(Theme.self) private var theme
    let label: String
    let isOn: Bool
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Circle()
                    .fill(isOn ? theme.accent : theme.switchTrackOff)
                    .frame(width: 8, height: 8)
                Text(label)
                    .font(.system(size: 12))
                    .foregroundStyle(isOn ? theme.textPrimary : theme.textMuted)
            }
            .padding(.horizontal, 10)
            .frame(height: 24)
            .contentShape(Rectangle())
        }
        // 液态玻璃：macOS 26 起的系统控件外观，比自己画一个圆角底更贴系统。
        // 开着的用 prominent（更实、更亮）、关着的用普通玻璃 —— 状态差别一眼可见，
        // 不必只靠那颗小圆点承担。旧系统由 glassButton 自动回落到 .bordered。
        .glassButton(prominent: isOn)
        .onHover { hovering = $0 }
        .help(label)
    }
}

/// 状态角标：压在 logo 右下角的白圆角小方块，一个字母表示当前最高优先级的状态。
/// 优先级与 Qt 版一致：增强 T > 网页 W > 核心开 C > 核心关 N。
struct StatusBadge: View {
    @Environment(Theme.self) private var theme
    let tunEnabled: Bool
    let proxyEnabled: Bool
    let coreRunning: Bool

    private var letter: String {
        if tunEnabled { return "T" }
        if proxyEnabled { return "W" }
        return coreRunning ? "C" : "N"
    }

    var body: some View {
        RoundedRectangle(cornerRadius: 7, style: .continuous)
            .fill(.white)
            .frame(width: 26, height: 26)
            .overlay {
                Text(letter)
                    // 全 UI 不加粗是设计约定，这个 26px 角标里的字母是**唯一例外** ——
                    // 不加粗在这个尺寸下看不清。
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(theme.accent)
            }
    }
}

/// 版本行上的红色小角标（new / core）。
struct UpdateBadge: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.system(size: 8))
            .foregroundStyle(.white)
            .padding(.horizontal, 4)
            .padding(.vertical, 1.5)
            .background(Capsule().fill(Color(hex: 0xF56C6C)))
            .overlay(Capsule().stroke(.white, lineWidth: 1))
    }
}
