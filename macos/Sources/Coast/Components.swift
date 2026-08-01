import SwiftUI

/// 内容卡：浮在毛玻璃上的不透明圆角面。对齐 `qml/Card.qml`。
struct Card<Content: View>: View {
    @Environment(Theme.self) private var theme
    @ViewBuilder var content: Content

    var body: some View {
        if #available(macOS 26.0, *) {
            // macOS 26：整窗都是玻璃、主内容不垫底（见 MainView），残留一块面板的
            // 只剩日志时间线和关于页 —— 一并去掉，内容直接浮在玻璃上；
            // 里面的滚动列表自然接上页面顶栏/页脚的系统边缘渐隐。
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
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
}

extension View {
    /// 页面顶栏的挂载方式。
    ///
    /// macOS 26：顶栏抬成钉在**最顶部**的系统导航栏（`safeAreaBar(edge: .top)`）——
    /// 滚动内容从它底下穿过，由系统 scroll edge effect 渐隐（与 MainView 的页脚
    /// 同一机制）；26 以下：按原布局插回内容列顶部（与 Qt 一致），观感不变。
    /// `spacing` 只作用于 26 以下那条路径（= 原来 VStack 的行距）。
    @ViewBuilder
    func pageHeaderBar<Header: View>(spacing: CGFloat = 8,
                                     @ViewBuilder header: () -> Header) -> some View {
        if #available(macOS 26.0, *) {
            safeAreaBar(edge: .top, spacing: 0) { header() }
        } else {
            VStack(spacing: spacing) {
                header()
                self
            }
        }
    }
}

/// 侧栏导航项。**逐元素对齐** `qml/NavButton.qml`：高 40、图标 17（左内距 12）、
/// 文字 14（距图标 9、右留白 8、超长省略号）。
///
/// 选中态的底色就是 **`Theme.card`** —— 也就是右侧内容卡的颜色。加上「右侧超出一个圆角」
/// 的处理（右角落在内容卡里、同色无缝），选中项看起来是从侧栏**长进内容区**的一块，
/// 而不是一个悬在侧栏上的高亮块。用品牌色填充的话，侧栏会冒出一整块饱和的蓝。
struct NavButton: View {
    @Environment(Theme.self) private var theme
    let title: String
    let icon: String
    let isCurrent: Bool
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 0) {
                // Remix 字形（与 Qt 同一份 remixicon.ttf）而不是 SF Symbol ——
                // SF 各字形视觉尺寸/字宽不一，同一字号下侧栏图标大小参差；
                // Remix 是统一 em 方格，17pt 下每个都一样大。定宽 17 让文字列对齐。
                Text(icon)
                    .font(.custom(IconFont.remix, size: 17))
                    // 图标：深色主题下浅灰、浅色主题下深灰（与文字不同色，Qt 同）。
                    .foregroundStyle(theme.dark ? Color(hex: 0xAA_AA_AA) : Color(hex: 0x66_66_66))
                    .frame(width: 17)
                    .padding(.leading, 12)
                Text(title)
                    .font(.system(size: 14))
                    // Qt：选中用 textSecondary、未选中用 textPrimary —— 选中项底下是
                    // 内容卡的实色，压一档反而更稳；未选中那些浮在侧栏底色上，要更亮才看得清。
                    .foregroundStyle(isCurrent ? theme.textSecondary : theme.textPrimary)
                    .lineLimit(1).truncationMode(.tail)
                    .padding(.leading, 9)
                Spacer(minLength: 8)
            }
            .frame(height: 40)
            .background {
                let fill = isCurrent ? theme.card : (hovering ? theme.hover : .clear)
                if #available(macOS 26.0, *) {
                    // macOS 26 上主内容**没有卡**（页面直接浮在玻璃上，见 MainView），
                    // 「右侧直角贴卡」的理由不存在了 —— 高亮就是一颗独立的圆角块，
                    // 四角都圆。
                    RoundedRectangle(cornerRadius: theme.radius, style: .continuous)
                        .fill(fill)
                } else {
                    // 右侧直角、只有左侧圆角：Qt 用「多铺一个圆角再让内容卡盖住」实现同一
                    // 效果，但 mac 的内容卡是半透明的（0.55），压不住 —— 负内距伸进去的
                    // 那条会透出来，看着就是高亮块越进了内容区。改成本来就不越界的不对称圆角。
                    UnevenRoundedRectangle(topLeadingRadius: theme.radius,
                                           bottomLeadingRadius: theme.radius,
                                           bottomTrailingRadius: 0,
                                           topTrailingRadius: 0,
                                           style: .continuous)
                        .fill(fill)
                }
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
                // 呼吸圆点：12×12 本体 + 3px 外环。启用时外环在「蓝 → 灰」之间脉动
                // （Qt 那条 1s 循环的 keyframes），停用时是静态灰。
                BreathingDot(isOn: isOn)
                Text(label)
                    .font(.system(size: 12))
                    // 文字**恒为 textPrimary**：状态只由圆点表达（Qt 就是这样，
                    // 这行原来还写着「Qt 也只靠圆点区分」，代码却又把关掉的文字调灰了）。
                    // 两处都表达同一件事时，浅色主题下的 #999999 只是让标签更难读。
                    .foregroundStyle(theme.textPrimary)
                    .lineLimit(1).truncationMode(.tail)
            }
            // 定宽定高（Theme.footerButtonWidth/Height），内容居中：
            // 页脚四颗按钮（三个开关 + 模式按钮）尺寸完全一致，长译文在框内省略。
            .padding(.horizontal, 8)
            .frame(width: theme.footerButtonWidth, height: theme.footerButtonHeight)
            .contentShape(Rectangle())
        }
        // ★ 用 `.glassCapsule()` 而不是 `.glassButton()`。
        //
        //   `.glassButton()` 是**系统按钮样式**，会自己加一层内边距，我控制不了；
        //   而模式按钮组只能整组上一层玻璃（`.glassCapsule()`）—— 两条路径的最终高度
        //   永远差一截，怎么调都对不齐。统一走 glassCapsule 之后，尺寸完全由这里的
        //   `padding` + `frame` 决定，单颗开关与整组分段必然同高。
        //
        //   也不用 prominent：它的度量比普通玻璃大一圈，同一排里开着的按钮会更高更宽。
        //   状态有圆点和文字色两重表达（Qt 也只靠圆点区分），没必要再拿尺寸去说同一件事。
        .buttonStyle(.plain)
        .glassCapsule()
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

/// 页脚开关左侧那颗呼吸圆点。对齐 `qml/FooterSwitch.qml`：12×12 本体 + 3px 外环，
/// 启用时外环在蓝↔灰之间以 1s 周期脉动，停用时是静态灰。
///
/// 圆点是这排按钮里**唯一**表达开关状态的东西（文字色只跟着变一档），
/// 所以它的动效不是装饰：一眼扫过去，脉动的那颗就是开着的。
struct BreathingDot: View {
    @Environment(Theme.self) private var theme
    let isOn: Bool

    @State private var pulse: Double = 0

    private var ringColor: Color {
        guard isOn else { return Color(white: 0.4, opacity: 0.15) }
        return Color(red: (72 + 30 * pulse) / 255,
                     green: (152 - 50 * pulse) / 255,
                     blue: (248 - 146 * pulse) / 255,
                     opacity: 0.5 - 0.35 * pulse)
    }

    var body: some View {
        Circle()
            .fill(isOn ? theme.accent : theme.switchTrackOff)
            .frame(width: 12, height: 12)
            .overlay(Circle().stroke(ringColor, lineWidth: 3))
            .onAppear { start() }
            .onChange(of: isOn) { _, _ in start() }
    }

    private func start() {
        pulse = 0
        guard isOn else { return }
        withAnimation(.easeInOut(duration: 0.5).repeatForever(autoreverses: true)) {
            pulse = 1
        }
    }
}
