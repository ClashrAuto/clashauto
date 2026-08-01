import AppKit
import SwiftUI

// 设置页用的一组手画控件。**逐个复刻** `qml/SettingsPage.qml` 顶部那批 inline component
// （ThemedSwitch / ThemedField / ThemedCombo / ThemedEditCombo / ThemedSpin / PillButton /
// SettingCard / SettingRow / CardDivider / SettingTab），尺寸与配色照抄。
//
// 为什么不用系统原生控件：Qt 那边每一个都是手画的，尺寸是写死的具体数字
// （开关 46×24、输入框半径 3、按钮高 30…）。用 `Toggle`/`TextField` 的话高度由系统
// 决定、随 macOS 版本变，和「精确匹配 Qt」这个目标直接冲突。

/// 设置分组卡。底色 `metricBg`，左右内距 14、上下 12；
/// 头部是「图标 + 标题」（均 `accentStrong`，图标 15、标题 13），与首行之间 6。
struct SettingCard<Content: View>: View {
    @Environment(Theme.self) private var theme
    let symbol: String
    let title: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 6) {
                Image(systemName: symbol)
                    .font(.system(size: 15))
                    .foregroundStyle(theme.accentStrong)
                Text(title)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.accentStrong)
                Spacer(minLength: 0)
            }
            .padding(.bottom, 6)

            content
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
    }
}

/// 卡内设置行：标签靠左（撑满、超长省略号），控件按声明顺序靠右。行高固定 40。
struct SettingRow<Content: View>: View {
    @Environment(Theme.self) private var theme
    let label: String
    @ViewBuilder var content: Content

    var body: some View {
        HStack(spacing: 8) {
            Text(label)
                .font(.system(size: 13))
                .foregroundStyle(theme.textSecondary)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)
            content
        }
        .frame(height: 40)
    }
}

/// 卡内行间细分隔线：1px、`divider`、透明度 0.6。
struct CardDivider: View {
    @Environment(Theme.self) private var theme
    var body: some View {
        Rectangle()
            .fill(theme.divider)
            .frame(height: 1)
            .opacity(0.6)
    }
}

/// 手画开关。外框 46×24，轨道 40×20 半径 10，滑块 16×16 白色、内缩 2；
/// 颜色与位置都有 120ms 过渡（QML 里那两条 `Behavior`）。
struct ThemedSwitch: View {
    @Environment(Theme.self) private var theme
    @Binding var isOn: Bool
    var enabled = true

    var body: some View {
        ZStack(alignment: .leading) {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(isOn ? theme.accent : theme.switchTrackOff)
                .overlay {
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .stroke(isOn ? theme.accent : theme.inputBorder, lineWidth: 1)
                }
                .frame(width: 40, height: 20)

            Circle()
                .fill(.white)
                .frame(width: 16, height: 16)
                .offset(x: isOn ? 40 - 16 - 2 : 2)
        }
        .frame(width: 46, height: 24, alignment: .leading)
        .animation(.easeInOut(duration: 0.12), value: isOn)
        .opacity(enabled ? 1 : 0.5)
        .contentShape(Rectangle())
        .onTapGesture { if enabled { isOn.toggle() } }
    }
}

/// 手画单行输入框。半径 3、`inputBg` 底、1px 描边（获焦转品牌色）、13px 正文。
struct ThemedField: View {
    @Environment(Theme.self) private var theme
    @Binding var text: String
    var placeholder = ""
    /// nil = 撑满可用宽度。`.frame(width: .infinity)` 不是「撑满」而是一个无穷宽的框，
    /// 会把整行的布局算崩 —— 想撑满必须走 `maxWidth`。
    var width: CGFloat?
    var enabled = true

    @FocusState private var focused: Bool

    var body: some View {
        TextField(placeholder, text: $text)
            .textFieldStyle(.plain)
            .font(.system(size: 13))
            .foregroundStyle(theme.textPrimary)
            .focused($focused)
            .disabled(!enabled)
            .padding(.horizontal, 8)
            .frame(width: width, height: 30)
            .frame(maxWidth: width == nil ? .infinity : nil)
            .background {
                RoundedRectangle(cornerRadius: 3, style: .continuous).fill(theme.inputBg)
            }
            .overlay {
                RoundedRectangle(cornerRadius: 3, style: .continuous)
                    .stroke(focused ? theme.accent : theme.inputBorder, lineWidth: 1)
            }
            .opacity(enabled ? 1 : 0.5)
    }
}

/// 手画下拉（不可编辑）。对齐公共组件 `qml/ThemedCombo.qml`：
/// 30 高、半径 3、**文字左对齐、右侧一个 ▾**。
///
/// 视觉自己画，但整个画好的样子**就是 `Menu` 的 label**（`.buttonStyle(.plain)`
/// 让它原样渲染）—— 整块控件都是原生点击区，命中交给系统。
///
/// ★ 历史坑：早先用的是 `.menuStyle(.borderlessButton)` + 一层 0.001 透明度的
///   点击层盖在画好的样子上。borderlessButton 会吃掉/居中 label（两次截图都栽在
///   这上面），而透明点击层的命中时灵时不灵 —— 「下拉框不容易点开」就是它。
///   `.buttonStyle(.plain)` 的 Menu 没有这两个问题，别再回去。
///
/// 用 `Menu` 而不是 `Picker`：`Picker` 在 macOS 上会渲染成系统 pop-up button，
/// 高度和描边都不受控。
struct ThemedCombo: View {
    @Environment(Theme.self) private var theme
    let options: [String]
    @Binding var selection: Int
    /// nil = 撑满（见 `ThemedField.width` 的说明）。
    var width: CGFloat?
    var enabled = true

    private var title: String {
        options.indices.contains(selection) ? options[selection] : ""
    }

    var body: some View {
        Menu {
            ForEach(Array(options.enumerated()), id: \.offset) { index, option in
                Button(option) { selection = index }
            }
        } label: {
            HStack(spacing: 0) {
                Text(title)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.textPrimary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Text("▾")
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textMuted)
            }
            .padding(.horizontal, 8)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background {
                RoundedRectangle(cornerRadius: 3, style: .continuous).fill(theme.inputBg)
            }
            .overlay {
                RoundedRectangle(cornerRadius: 3, style: .continuous)
                    .stroke(theme.inputBorder, lineWidth: 1)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .menuStyle(.button)
        .menuIndicator(.hidden)
        .frame(width: width, height: 30)
        .frame(maxWidth: width == nil ? .infinity : nil)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.5)
    }
}

/// 手画**可编辑**下拉（Host / 允许规则 / 排除规则）：能手输，也能从预置项里挑。
/// **点进输入框（获得焦点）或点右侧 ▾ 都会弹出预置项**；选中填入；
/// 按 Esc 或点旁边收起菜单后，焦点还在框里，可以直接手输。
///
/// 弹出用的是**程序化的 `NSMenu`** 而不是 SwiftUI `Menu`：后者没法在「输入框
/// 获得焦点」时打开（只能由它自己的 label 触发）；自绘浮层又会被卡片圆角和
/// ScrollView 裁掉、还压不过后画的兄弟行。原生菜单自带独立窗口，两个问题都没有。
struct ThemedEditCombo: View {
    @Environment(Theme.self) private var theme
    @Binding var text: String
    let options: [String]
    var placeholder = ""
    var width: CGFloat = 300
    var enabled = true

    @FocusState private var focused: Bool
    @State private var menuHost = ComboMenuHost()

    var body: some View {
        HStack(spacing: 0) {
            TextField(placeholder, text: $text)
                .textFieldStyle(.plain)
                .font(.system(size: 13))
                .foregroundStyle(theme.textPrimary)
                .focused($focused)
                .padding(.leading, 8)

            Button { popMenu() } label: {
                Text("▾")
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textMuted)
                    // 点击区做到接近整个控件高（28×28）—— 原来 ▾ 加 6 内距只有
                    // ~24pt 见方，「不容易点中」多半就是在够这个小目标。
                    .frame(width: 28, height: 28)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .fixedSize()
            .padding(.trailing, 1)
        }
        .frame(width: width, height: 30)
        .background {
            RoundedRectangle(cornerRadius: 3, style: .continuous).fill(theme.inputBg)
        }
        // 铺满整控件的隐形 NSView，给 NSMenu 定位（菜单贴着控件下缘弹）。
        .background { ComboMenuAnchor(host: menuHost) }
        .overlay {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .stroke(focused ? theme.accent : theme.inputBorder, lineWidth: 1)
        }
        .onChange(of: focused) { _, isFocused in
            // 等这次点击处理完再弹 —— 在 mouseDown 里同步开菜单会吃掉焦点切换。
            if isFocused { DispatchQueue.main.async { popMenu() } }
        }
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.5)
    }

    private func popMenu() {
        guard enabled else { return }
        menuHost.show(options: options) { text = $0 }
    }
}

/// `ThemedEditCombo` 的菜单执行器：持有锚点 NSView，把预置项做成原生 NSMenu
/// 贴着控件下缘弹出。类而不是结构体 —— NSMenuItem 的 target-action 需要一个
/// 稳定的 objc 对象。
private final class ComboMenuHost: NSObject {
    weak var anchor: NSView?
    private var onPick: ((String) -> Void)?

    func show(options: [String], onPick: @escaping (String) -> Void) {
        guard let anchor, anchor.window != nil else { return }
        self.onPick = onPick
        let menu = NSMenu()
        for option in options {
            let item = NSMenuItem(title: option, action: #selector(pick(_:)), keyEquivalent: "")
            item.target = self
            menu.addItem(item)
        }
        menu.minimumWidth = anchor.bounds.width
        // 非翻转坐标系里 (0,0) 是锚点左下角 —— 菜单顶边正好接在控件底下，留 4 的缝。
        menu.popUp(positioning: nil, at: NSPoint(x: 0, y: -4), in: anchor)
    }

    @objc private func pick(_ sender: NSMenuItem) {
        onPick?(sender.title)
    }
}

/// 铺在 `ThemedEditCombo` 底下的隐形锚点视图（NSMenu 需要一个真实的 NSView 定位）。
private struct ComboMenuAnchor: NSViewRepresentable {
    let host: ComboMenuHost

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        host.anchor = view
        return view
    }

    func updateNSView(_ view: NSView, context: Context) {
        host.anchor = view
    }
}

/// 手画数字框。正文居中，右侧上下各半格的 ＋ / −。
struct ThemedSpin: View {
    @Environment(Theme.self) private var theme
    @Binding var value: Int
    var range: ClosedRange<Int> = 0...1440
    var width: CGFloat = 200

    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 0) {
            TextField("", value: $value, format: .number)
                .textFieldStyle(.plain)
                .font(.system(size: 13))
                .foregroundStyle(theme.textPrimary)
                .multilineTextAlignment(.center)
                .focused($focused)
                .frame(maxWidth: .infinity)
                .onChange(of: value) { _, new in
                    value = min(max(new, range.lowerBound), range.upperBound)
                }

            VStack(spacing: 0) {
                stepper("+") { value = min(value + 1, range.upperBound) }
                stepper("−") { value = max(value - 1, range.lowerBound) }
            }
            .frame(width: 24)
        }
        .frame(width: width, height: 30)
        .background {
            RoundedRectangle(cornerRadius: 3, style: .continuous).fill(theme.inputBg)
        }
        .overlay {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .stroke(focused ? theme.accent : theme.inputBorder, lineWidth: 1)
        }
    }

    private func stepper(_ glyph: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(glyph)
                .font(.system(size: 13))
                .foregroundStyle(theme.textSecondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

/// 手画按钮。高 30、半径 4；主按钮品牌色底白字（按下转 `accentStrong`），
/// 次按钮 `inputBg` + 1px 描边（按下转 `hover`）。
struct PillButton: View {
    @Environment(Theme.self) private var theme
    let title: String
    var primary = false
    var width: CGFloat?
    var height: CGFloat = 30
    var enabled = true
    let action: () -> Void

    @State private var pressed = false

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 13))
                .foregroundStyle(primary ? .white : theme.textPrimary)
                .lineLimit(1)
                .truncationMode(.tail)
                .opacity(enabled ? 1 : 0.5)
                .padding(.horizontal, 8)
                .frame(width: width, height: height)
                .background {
                    RoundedRectangle(cornerRadius: 4, style: .continuous)
                        .fill(primary ? (pressed ? theme.accentStrong : theme.accent)
                              : (pressed ? theme.hover : theme.inputBg))
                        .opacity(enabled ? 1 : 0.6)
                }
                .overlay {
                    if !primary {
                        RoundedRectangle(cornerRadius: 4, style: .continuous)
                            .stroke(theme.inputBorder, lineWidth: 1)
                    }
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .onLongPressGesture(minimumDuration: 0, pressing: { pressed = $0 }, perform: {})
    }
}

/// 设置页顶部的标签按钮：84×32、14px，选中时文字转品牌色并在底部画一条 2px 的条。
struct SettingTab: View {
    @Environment(Theme.self) private var theme
    let title: String
    let isCurrent: Bool
    let action: () -> Void

    var body: some View {
        if #available(macOS 26.0, *) {
            // 26：按钮组里的一段（玻璃由**外层的组**统一上，见 SettingsPage.header）。
            // 配色全交给系统：文字 primary/secondary，选中底衬用系统 tint。
            Button(action: action) {
                Text(title)
                    .font(.system(size: 13))
                    .foregroundStyle(isCurrent ? AnyShapeStyle(.primary) : AnyShapeStyle(.secondary))
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(width: 84, height: 28)
                    .background {
                        if isCurrent { Capsule().fill(.tint.opacity(0.35)) }
                    }
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
        } else {
            Button(action: action) {
                Text(title)
                    .font(.system(size: 14))
                    .foregroundStyle(isCurrent ? theme.accent : theme.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(width: 84, height: 32)
                    .overlay(alignment: .bottom) {
                        Rectangle()
                            .fill(isCurrent ? theme.accent : .clear)
                            .frame(height: 2)
                    }
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
        }
    }
}
