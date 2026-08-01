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

/// 26 上主内容（中间那一片）左缘**贴住侧栏边**（0）；26 以下保留 Qt 的 10。
/// 页脚不适用 —— 它保留右侧 5 的列边距（见 MainView）。
var pageLeadingInset: CGFloat {
    if #available(macOS 26.0, *) { 0 } else { 10 }
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

/// 页脚开关左侧那颗状态圆点：12×12 本体 + 外环。
/// 启用时外环**一圈圈向外扩散并淡出**（见 `PulsingRing`），停用时是静态灰环。
///
/// 圆点是这排按钮里**唯一**表达开关状态的东西（文字色只跟着变一档），
/// 所以它的动效不是装饰：一眼扫过去，在往外发散的那颗就是开着的。
struct BreathingDot: View {
    @Environment(Theme.self) private var theme
    let isOn: Bool

    var body: some View {
        Circle()
            .fill(isOn ? theme.accent : theme.switchTrackOff)
            .frame(width: 12, height: 12)
            .overlay {
                // ★ 开态和关态是**两个不同的视图**，不是同一个环换颜色。
                //
                //   原来是一个 `Circle().stroke(ringColor)`，`ringColor` 里按 `isOn` 分档，
                //   脉动由 `withAnimation(.repeatForever)` 驱动一个 `pulse` 状态。
                //   `repeatForever` 是条**永不结束**的动画：把 `pulse` 赋回 0 停不掉它
                //   （连 `Transaction(disablesAnimations: true)` 也不行 —— 动画挂在
                //   stroke 那个**颜色属性**上，不只挂在 `pulse` 上），而关开关时
                //   「蓝 → 灰」这次颜色变化正好被它接管，于是环色在蓝灰之间无限自动往返：
                //   开过一次以后，关掉了光晕还在一亮一暗。
                //
                //   拆成 if/else 之后两支的结构标识不同，关掉时脉动那支**整个从视图树上
                //   消失**，它身上的动画随之作废；静态灰环是另一个视图，从来没有过动画。
                if isOn {
                    PulsingRing()
                } else {
                    Circle().stroke(Color(white: 0.4, opacity: 0.15), lineWidth: 3)
                }
            }
    }
}

/// 开启态那圈光晕：从圆点边缘**向外扩散**并淡出，一轮一轮地发出去。
///
/// ★ 不是「原地换颜色」。早先那版是一个定尺寸的 3px 环在蓝↔灰之间来回，
///   一亮一暗读起来是**闪烁**，而不是「有东西在往外发散」。
///
/// ★ 也不能用 `autoreverses: true`：那会让环扩出去再缩回来，缩回去的那半程
///   同样是在闪。扩散必须是单向的 —— 到头就消失，从头再来。
///
/// 三段相位而不是两段，是为了**没有起跳的那一下**：第 0 段在圆点边缘、
/// 完全透明，所以每轮重置（瞬时）时看不见任何东西冒出来；第 1 段快速浮现，
/// 第 2 段一边继续张开一边淡尽。两段的话重置那一帧会突然亮出一个环，
/// 那正是我们要去掉的「闪」。
///
/// **独立成一个视图不是为了整洁**，是靠它被销毁来终止那条永不结束的循环动画
/// —— 理由见 `BreathingDot` 里那段注释，别把它合回去。
private struct PulsingRing: View {
    @Environment(Theme.self) private var theme

    /// (缩放, 不透明度)。1.0 = 正好贴着圆点边缘（半径 6）。
    ///
    /// 最大只到 1.8：`scaleEffect` 连描边一起放大，1.8 时外缘落在半径
    /// ≈ 6×1.8 + 1.8 ≈ 12.6 —— 而标签文字从圆心 12 处开始（圆点半径 6 + HStack 间距 6）。
    /// 再大就压到字上去了（2.05 实测能探到 15）。
    private static let phases: [(scale: CGFloat, opacity: Double)] = [
        (1.00, 0.00),   // 起点：贴着圆点、看不见
        (1.30, 0.55),   // 浮现
        (1.80, 0.00),   // 张开到最大、淡尽
    ]

    var body: some View {
        Circle()
            .stroke(theme.accent, lineWidth: 2)
            .phaseAnimator(Array(Self.phases.indices)) { ring, index in
                ring
                    .scaleEffect(Self.phases[index].scale)
                    .opacity(Self.phases[index].opacity)
            } animation: { index in
                switch index {
                // 回到第 0 段是**重置**，必须瞬时：给了时长就成了「缩回去」。
                case 0: .linear(duration: 0)
                case 1: .easeOut(duration: 0.35)
                default: .easeOut(duration: 1.05)
                }
            }
    }
}
