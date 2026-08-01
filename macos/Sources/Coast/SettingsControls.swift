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
/// ★ **视觉自己画，`Menu` 只当一层透明的点击层盖在上面。**
///   `.menuStyle(.borderlessButton)` 对 `label:` 的处理很霸道，两次都栽在这上面：
///   ① 装饰画在 label 里 → 被整个吃掉，控件变成一段裸文字（设备详情窗「策略」那行的截图）；
///   ② 把底色描边挪到 `Menu` 外面之后底是有了，但 label 里的 `HStack` 仍被**居中**、
///      右侧那个 ▾ 直接不见（第二张截图）。
///   所以别再试图让它按我们的排版渲染 —— 自己画一份完整的样子，
///   再叠一个几乎全透明的 `Menu` 接管点击。
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
        ZStack {
            // —— 自己画的样子 ——
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

            // —— 只负责弹菜单的那一层 ——
            // 不能用 `.opacity(0)`：完全透明的视图不参与命中测试，点下去没反应。
            Menu {
                ForEach(Array(options.enumerated()), id: \.offset) { index, option in
                    Button(option) { selection = index }
                }
            } label: {
                Rectangle().fill(Color.white.opacity(0.001))
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
        }
        .frame(width: width, height: 30)
        .frame(maxWidth: width == nil ? .infinity : nil)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.5)
    }
}

/// 手画**可编辑**下拉（Host / 允许规则 / 排除规则）：能手输，也能从预置项里挑。
/// 输入框铺满，右侧 ▾ 单独可点 —— 与 QML 里那个自定义 `indicator` 同义
/// （那边同样因为 contentItem 吃掉了整控件的点击，才给箭头单挂了一个 TapHandler）。
struct ThemedEditCombo: View {
    @Environment(Theme.self) private var theme
    @Binding var text: String
    let options: [String]
    var placeholder = ""
    var width: CGFloat = 300
    var enabled = true

    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 0) {
            TextField(placeholder, text: $text)
                .textFieldStyle(.plain)
                .font(.system(size: 13))
                .foregroundStyle(theme.textPrimary)
                .focused($focused)
                .padding(.leading, 8)

            Menu {
                ForEach(options, id: \.self) { option in
                    Button(option) { text = option }
                }
            } label: {
                Text("▾")
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textMuted)
                    .padding(6)
                    .contentShape(Rectangle())
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
            .padding(.trailing, 2)
        }
        .frame(width: width, height: 30)
        .background {
            RoundedRectangle(cornerRadius: 3, style: .continuous).fill(theme.inputBg)
        }
        .overlay {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .stroke(focused ? theme.accent : theme.inputBorder, lineWidth: 1)
        }
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.5)
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
