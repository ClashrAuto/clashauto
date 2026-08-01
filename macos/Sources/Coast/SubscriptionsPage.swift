import CoastKit
import SwiftUI

/// 订阅页。**逐元素对齐** `qml/SubscriptionsPage.qml`：单列 108 高的订阅卡
/// （类型徽章 + 名称 + URL + 元信息 + 五颗圆形动作按钮），节点在**弹窗**里看，
/// 添加/编辑与删除确认各自一个弹窗。
///
/// 这一版把原来那个左右分栏（HSplitView：左订阅、右节点）整个换掉了 ——
/// Qt 是单列卡片 + 弹窗，分栏既没有 URL 行、没有类型、没有更新周期，
/// 也没有「应用」和「查看节点」两个动作。
struct SubscriptionsPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    @State private var summaries: [SubscriptionSummary] = []
    @State private var editing: EditingSubscription?
    @State private var viewingNodes: NodesTarget?
    @State private var pendingDelete: PendingDelete?
    @State private var busy = false
    @State private var message = ""

    /// 新增/编辑共用一份草稿。`index == nil` 表示新增。
    struct EditingSubscription: Identifiable {
        var index: Int?
        var name = ""
        var url = ""
        var type = "sub"
        var updateTime = 0
        var id: Int { index ?? -1 }
    }

    struct NodesTarget: Identifiable {
        let index: Int
        let name: String
        var id: Int { index }
    }

    struct PendingDelete: Identifiable {
        let index: Int
        let name: String
        var id: Int { index }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if summaries.isEmpty {
                Text("暂无订阅，点击右上角「添加订阅」".t)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.textMuted)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVStack(spacing: 10) {
                        ForEach(Array(summaries.enumerated()), id: \.offset) { index, summary in
                            card(index: index, summary: summary)
                        }
                    }
                    .padding(.bottom, 10)
                }
            }

            if !message.isEmpty {
                Text(message)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .lineLimit(2)
                    .padding(.trailing, 10)
                    .padding(.leading, pageLeadingInset)
                    .padding(.bottom, 6)
            }
        }
        .pageHeaderBar(spacing: 8) { toolbar }
        .task { reload() }
        .sheet(item: $editing) { draft in
            SubscriptionEditor(draft: draft) { saved in apply(saved) }
                .environment(theme)
        }
        .sheet(item: $viewingNodes) { target in
            SubscriptionNodesSheet(index: target.index)
                .environment(state).environment(theme)
        }
        .sheet(item: $pendingDelete) { target in
            DeleteConfirmSheet(name: target.name) {
                _ = state.subscriptions.removeSubscription(at: target.index)
                reload()
                Task { await state.controller.rebuildConfig() }
            }
            .environment(theme)
        }
    }

    // MARK: 顶栏（上/左/右内距 10，间距 6）

    private var toolbar: some View {
        HStack(spacing: 6) {
            Text("订阅".t)
                .lineLimit(1)
                .font(.system(size: 18))
                .foregroundStyle(theme.textPrimary)
            Text("(\(summaries.count))")
                .font(.system(size: 10))
                .foregroundStyle(theme.textMuted)
                .padding(.bottom, 3)
            Spacer(minLength: 0)

            TextBtn(label: "添加订阅".t, symbol: "plus", primary: false) {
                editing = EditingSubscription()
            }
            // 「应用」= 拿当前订阅重建一次配置。Qt 有这颗按钮，Swift 侧一直没有 ——
            // 用户手改过 subscribe.yaml 或想强制重来时没有任何入口。
            TextBtn(label: "应用".t, symbol: "checkmark", primary: false) {
                Task {
                    message = "正在应用…".t
                    await state.controller.rebuildConfig()
                    message = "已应用".t
                }
            }
            TextBtn(label: "更新全部".t, symbol: "arrow.clockwise", primary: true) {
                Task { await updateAll() }
            }
            .disabled(busy || summaries.isEmpty)
        }
        .padding(.top, 10)
        .padding(.trailing, 10)
        .padding(.leading, pageLeadingInset)
    }

    // MARK: 订阅卡（高 108，左右内缩 10；卡内左 12 右 10 上下 8，行距 3）

    private func card(index: Int, summary: SubscriptionSummary) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 6) {
                Text(summary.type)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background {
                        RoundedRectangle(cornerRadius: 4, style: .continuous).fill(theme.metricBg)
                    }
                Text(summary.name)
                    .font(.system(size: 14))
                    .foregroundStyle(theme.textPrimary)
                    .lineLimit(1).truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }

            // URL 从**中间**省略：订阅链接的头尾（域名和 token 尾巴）都比中间那段有用。
            Text(summary.url)
                .font(.system(size: 11))
                .foregroundStyle(theme.textMuted)
                .lineLimit(1).truncationMode(.middle)
                .frame(maxWidth: .infinity, alignment: .leading)

            Text(metaText(summary))
                .font(.system(size: 11))
                .foregroundStyle(theme.textSecondary)
                .lineLimit(1).truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)

            Spacer(minLength: 0)

            HStack(spacing: 4) {
                CircleBtn(symbol: "checkmark", help: "启用/停用".t, on: summary.use) {
                    _ = state.subscriptions.setSubscriptionEnabled(at: index, !summary.use)
                    reload()
                    Task { await state.controller.rebuildConfig() }
                }
                CircleBtn(symbol: "eye", help: "查看节点".t) {
                    viewingNodes = NodesTarget(index: index, name: summary.name)
                }
                CircleBtn(symbol: "square.and.pencil", help: "编辑".t) {
                    editing = EditingSubscription(index: index, name: summary.name,
                                                  url: summary.url, type: summary.type,
                                                  updateTime: summary.updateTime)
                }
                CircleBtn(symbol: "arrow.clockwise", help: "更新".t) {
                    Task { await update(index: index) }
                }
                Spacer(minLength: 0)
                // 删除**不可撤销**（连同它下面所有节点的启用状态一起没），而这一行其余动作
                // 都是可逆的 —— 长得一样、代价差一个量级，必须拦一道。
                CircleBtn(symbol: "trash", help: "删除".t, danger: true) {
                    pendingDelete = PendingDelete(index: index, name: summary.name)
                }
            }
        }
        .padding(.leading, 12)
        .padding(.trailing, 10)
        .padding(.vertical, 8)
        .frame(height: 108)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
        .padding(.trailing, 10)
        .padding(.leading, pageLeadingInset)
    }

    private func metaText(_ summary: SubscriptionSummary) -> String {
        var text = "\(summary.enabledNodeCount) / \(summary.nodeCount)" + " 节点".t
        if summary.updateTime > 0 {
            text += " · 每 ".t + "\(summary.updateTime)" + " 分钟".t
        }
        return text
    }

    // MARK: 动作

    private func reload() {
        summaries = state.subscriptions.load()
    }

    private func apply(_ draft: EditingSubscription) {
        if let index = draft.index {
            _ = state.subscriptions.editSubscription(at: index, name: draft.name,
                                                     url: draft.url, type: draft.type)
            _ = state.subscriptions.setSubscriptionUpdateTime(at: index, minutes: draft.updateTime)
            reload()
            Task { await update(index: index) }
        } else {
            guard state.subscriptions.addSubscription(name: draft.name, url: draft.url,
                                                      type: draft.type) else { return }
            reload()
            // 新加的订阅立刻拉一次 —— 否则用户看到的是一条 0 个节点的空记录，会以为没加上。
            Task { await update(index: summaries.count - 1) }
        }
    }

    private func update(index: Int) async {
        busy = true
        defer { busy = false }
        message = "正在更新…".t
        let result = await state.subscriptions.updateSubscription(at: index)
        message = result.message
        reload()
        // 内容没变就不重建 —— 自动更新周期短时这是常态，白重载一次核心没有意义。
        if result.changed { await state.controller.rebuildConfig() }
    }

    private func updateAll() async {
        busy = true
        defer { busy = false }
        var changed = false
        for index in summaries.indices {
            message = String(format: "正在更新 %@…".t, summaries[index].name)
            let result = await state.subscriptions.updateSubscription(at: index)
            changed = changed || result.changed
        }
        message = "全部更新完成".t
        reload()
        if changed { await state.controller.rebuildConfig() }
    }
}

// MARK: - 复用控件

/// 圆形图标按钮：28×28、半径 14。选中态品牌色底白图标，危险态红字 + 淡红底。
struct CircleBtn: View {
    @Environment(Theme.self) private var theme
    let symbol: String
    let help: String
    var on = false
    var danger = false
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 16))
                .foregroundStyle(on ? .white : (danger ? theme.danger : theme.textSecondary))
                .frame(width: 28, height: 28)
                .background {
                    Circle().fill(background)
                }
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(.easeInOut(duration: 0.12), value: hovering)
        .help(help)
    }

    private var background: Color {
        if danger {
            return Color(red: 1, green: 0.30, blue: 0.31, opacity: hovering ? 0.28 : 0.12)
        }
        if on { return theme.accent }
        return hovering ? theme.hover : theme.metricBg
    }
}

/// 文字按钮（可带前置图标）：高 30、半径 `Theme.radius`、最小宽 84。
struct TextBtn: View {
    @Environment(Theme.self) private var theme
    let label: String
    var symbol: String?
    var primary = true
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        if #available(macOS 26.0, *) {
            // 26：液态玻璃胶囊按钮，配色交给系统（默认前景色、无自定义底）——
            // 顶栏统一系统语言，主/次按钮不再靠底色区分。
            Button(action: action) {
                HStack(spacing: 6) {
                    if let symbol {
                        Image(systemName: symbol).font(.system(size: 15))
                    }
                    Text(label).font(.system(size: 13))
                }
                .fixedSize()
                .padding(.horizontal, 12)
                .frame(minHeight: 28)
                .contentShape(Capsule())
            }
            .buttonStyle(.plain)
            .glassCapsule()
        } else {
            Button(action: action) {
                HStack(spacing: 6) {
                    if let symbol {
                        Image(systemName: symbol).font(.system(size: 15))
                    }
                    Text(label).font(.system(size: 13))
                }
                .foregroundStyle(primary ? .white : theme.textSecondary)
                .fixedSize()
                .padding(.horizontal, 12)
                .frame(minWidth: 84, minHeight: 30)
                .background {
                    RoundedRectangle(cornerRadius: theme.radius, style: .continuous)
                        .fill(primary ? (hovering ? theme.accentStrong : theme.accent)
                              : (hovering ? theme.hover : theme.metricBg))
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .onHover { hovering = $0 }
        }
    }
}

// MARK: - 添加 / 编辑弹窗（宽 420）

private struct SubscriptionEditor: View {
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    let draft: SubscriptionsPage.EditingSubscription
    let onSave: (SubscriptionsPage.EditingSubscription) -> Void

    @State private var name = ""
    @State private var url = ""
    @State private var type = "sub"
    @State private var period = "0"

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(draft.index == nil ? "添加订阅".t : "编辑订阅".t)
                .font(.system(size: 16))
                .foregroundStyle(theme.textPrimary)
                .padding(.top, 14)

            VStack(alignment: .leading, spacing: 8) {
                label("URL 或本地路径".t)
                ThemedField(text: $url, placeholder: "https://...", width: nil)
                label("名称".t)
                ThemedField(text: $name, placeholder: "留空则用 URL".t, width: nil)

                HStack(spacing: 14) {
                    label("类型".t)
                    // sub / clash 两个分段：60×26。**不翻译** —— 它们是配置文件里的字面值。
                    ForEach(["sub", "clash"], id: \.self) { option in
                        Text(option)
                            .font(.system(size: 12))
                            .foregroundStyle(type == option ? .white : theme.textSecondary)
                            .frame(width: 60, height: 26)
                            .background {
                                RoundedRectangle(cornerRadius: theme.radius, style: .continuous)
                                    .fill(type == option ? theme.accent : theme.metricBg)
                            }
                            .contentShape(Rectangle())
                            .onTapGesture { type = option }
                    }
                    Spacer(minLength: 0)
                }

                // 自动更新周期**仅编辑时**可设（新增时那条订阅还不存在，没有可写的目标）。
                if draft.index != nil {
                    HStack(spacing: 10) {
                        label("自动更新(分钟, 0=全局)".t)
                        ThemedField(text: $period, width: 90)
                        Spacer(minLength: 0)
                    }
                }
            }

            HStack(spacing: 8) {
                Spacer(minLength: 0)
                TextBtn(label: "取消".t, primary: false) { dismiss() }
                TextBtn(label: "保存".t, primary: true) { save() }
            }
            .padding(.top, 4)
            .padding(.bottom, 14)
        }
        .padding(.horizontal, 16)
        .frame(width: 420)
        .background(theme.card)
        .onAppear {
            name = draft.name
            url = draft.url
            type = draft.type.lowercased() == "clash" ? "clash" : "sub"
            period = "\(draft.updateTime)"
        }
    }

    private func label(_ text: String) -> some View {
        Text(text).font(.system(size: 12)).foregroundStyle(theme.textSecondary)
    }

    private func save() {
        let trimmedURL = url.trimmingCharacters(in: .whitespaces)
        // URL 为空直接不保存（与 Qt 同）：一条没有地址的订阅永远拉不到东西，
        // 存下来只会在列表里挂一条永远失败的记录。
        guard !trimmedURL.isEmpty else { return }
        var saved = draft
        saved.name = name.trimmingCharacters(in: .whitespaces)
        saved.url = trimmedURL
        saved.type = type
        saved.updateTime = min(1440, max(0, Int(period) ?? 0))
        onSave(saved)
        dismiss()
    }
}

// MARK: - 删除确认弹窗（宽 340）

private struct DeleteConfirmSheet: View {
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    let name: String
    let onConfirm: () -> Void

    @State private var hovering = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("确定删除订阅「".t + name + "」?".t)
                .font(.system(size: 14))
                .foregroundStyle(theme.textPrimary)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.top, 18)

            HStack(spacing: 8) {
                Spacer(minLength: 0)
                TextBtn(label: "取消".t, primary: false) { dismiss() }
                Button {
                    onConfirm()
                    dismiss()
                } label: {
                    Text("删除".t)
                        .lineLimit(1)
                        .font(.system(size: 13))
                        .foregroundStyle(.white)
                        .frame(width: 84, height: 30)
                        .background {
                            RoundedRectangle(cornerRadius: theme.radius, style: .continuous)
                                .fill(hovering ? theme.danger.opacity(0.85) : theme.danger)
                        }
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .onHover { hovering = $0 }
            }
            .padding(.bottom, 16)
        }
        .padding(.horizontal, 18)
        .frame(width: 340)
        .background(theme.card)
    }
}

// MARK: - 节点弹窗（620×460）

private struct SubscriptionNodesSheet: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    let index: Int
    @State private var nodes: [SubscriptionNodeSummary] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 4) {
                Text("订阅节点".t).font(.system(size: 16)).foregroundStyle(theme.textPrimary).lineLimit(1)
                Text("(\(nodes.count))").font(.system(size: 11)).foregroundStyle(theme.textMuted)
                    .padding(.bottom, 2)
                Spacer(minLength: 0)
            }
            .padding(.top, 12)

            if nodes.isEmpty {
                Text("暂无节点，请先点击「更新」".t)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.textMuted)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .padding(.top, 30)
                Spacer(minLength: 0)
            } else {
                ScrollView {
                    LazyVStack(spacing: 6) {
                        ForEach(Array(nodes.enumerated()), id: \.offset) { position, node in
                            row(position: position, node: node)
                        }
                    }
                }
            }

            HStack(spacing: 8) {
                TextBtn(label: "全选".t, primary: false) { setAll(true) }
                TextBtn(label: "全不选".t, primary: false) { setAll(false) }
                Spacer(minLength: 0)
                TextBtn(label: "关闭".t, primary: true) { dismiss() }
            }
            .padding(.bottom, 12)
        }
        .padding(.horizontal, 14)
        .frame(width: 620, height: 460)
        .background(theme.card)
        .task { reload() }
    }

    /// 行高 46、半径 4；已启用的整行用 `nodeRowActive` 底 —— 与 Qt 一样靠**底色**
    /// 表达启用态，而不是一个小勾（一屏几十条时，底色一眼就能扫出启用了哪些）。
    private func row(position: Int, node: SubscriptionNodeSummary) -> some View {
        HStack(spacing: 8) {
            VStack(alignment: .leading, spacing: 1) {
                Text(node.name)
                    .font(.system(size: 12)).foregroundStyle(theme.textPrimary)
                    .lineLimit(1).truncationMode(.tail)
                Text("\(node.server):\(node.port)")
                    .font(.system(size: 10)).foregroundStyle(theme.textMuted)
                    .lineLimit(1).truncationMode(.tail)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            ToggleNodeButton(title: node.use ? "停用".t : "使用".t) {
                _ = state.subscriptions.setNodeEnabled(subscription: index,
                                                       node: position, !node.use)
                reload()
                Task { await state.controller.rebuildConfig() }
            }
        }
        .padding(.leading, 10)
        .padding(.trailing, 6)
        .frame(height: 46)
        .background(node.use ? theme.nodeRowActive : theme.nodeRowBg)
        .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
    }

    private func reload() { nodes = state.subscriptions.nodes(at: index) }

    private func setAll(_ enabled: Bool) {
        _ = state.subscriptions.setAllNodesEnabled(subscription: index, enabled)
        reload()
        Task { await state.controller.rebuildConfig() }
    }
}

/// 节点行右侧那颗 76×30 的「使用 / 停用」。
private struct ToggleNodeButton: View {
    @Environment(Theme.self) private var theme
    let title: String
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 12))
                .foregroundStyle(theme.textSecondary)
                .frame(width: 76, height: 30)
                .background {
                    RoundedRectangle(cornerRadius: theme.radius, style: .continuous)
                        .fill(hovering ? theme.hover : theme.metricBg)
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
    }
}
