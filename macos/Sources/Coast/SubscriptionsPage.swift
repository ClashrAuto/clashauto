import CoastKit
import SwiftUI

/// 订阅管理：左列订阅、右列该订阅的节点。对齐 `qml/SubscriptionsPage.qml`。
struct SubscriptionsPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    @State private var summaries: [SubscriptionSummary] = []
    @State private var selectedIndex: Int?
    @State private var nodes: [SubscriptionNodeSummary] = []
    @State private var editing: EditingSubscription?
    @State private var busy = false
    @State private var message = ""

    /// 新增/编辑共用一份草稿。`index == nil` 表示新增。
    struct EditingSubscription: Identifiable {
        var index: Int?
        var name = ""
        var url = ""
        var id: Int { index ?? -1 }
    }

    var body: some View {
        HSplitView {
            subscriptionList
                .frame(minWidth: 260, idealWidth: 300)
            nodeList
                .frame(minWidth: 260)
        }
        .task { reload() }
        .alert("确定删除订阅？".t, isPresented: Binding(
            get: { confirmDelete != nil },
            set: { if !$0 { confirmDelete = nil } })) {
            Button("取消".t, role: .cancel) { confirmDelete = nil }
            Button("删除".t, role: .destructive) {
                if let index = confirmDelete {
                    _ = state.subscriptions.removeSubscription(at: index)
                    selectedIndex = nil
                    reload()
                    Task { await state.controller.rebuildConfig() }
                }
                confirmDelete = nil
            }
        } message: {
            Text("该订阅下的所有节点会一并移除，且无法撤销。".t)
        }
    }

    /// 订阅行上的一个动作按钮。三个动作长得一样，抽出来免得三处各写一遍内距和字号。
    private func rowAction(_ symbol: String, _ help: String, danger: Bool = false,
                           _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 11))
                .foregroundStyle(danger ? theme.danger : theme.textSecondary)
                .padding(.horizontal, 7)
                .frame(height: 22)
        }
        .buttonStyle(.plain)
        .glassCapsule()
        .help(help)
    }

    /// 待确认删除的订阅下标。
    ///
    /// 删除是不可撤销的（连同它下面所有节点的启用状态一起没），而这一行上其余两个动作
    /// 都是可逆的 —— 三颗按钮长得一样，误点的代价却差一个量级，必须拦一道。
    @State private var confirmDelete: Int?

    // MARK: 订阅列

    private var subscriptionList: some View {
        VStack(spacing: 0) {
            // 顶栏对齐 Qt：「订阅」标题(18pt) + 计数 | 右侧「添加订阅 / 更新全部」
            HStack(spacing: 8) {
                Text("订阅".t)
                    .font(.system(size: 18, weight: .medium))
                    .foregroundStyle(theme.textPrimary)
                Text("(\(summaries.count))")
                    .font(.system(size: 9))
                    .foregroundStyle(theme.textMuted)
                Spacer()
                Button {
                    editing = EditingSubscription()
                } label: {
                    Label("添加订阅".t, systemImage: "plus")
                        .font(.system(size: 12))
                        .padding(.horizontal, 10).frame(height: 24)
                }
                .buttonStyle(.plain)
                .glassCapsule()
                Button {
                    Task { await updateAll() }
                } label: {
                    Label("更新全部".t, systemImage: "arrow.clockwise")
                        .font(.system(size: 12))
                        .padding(.horizontal, 10).frame(height: 24)
                }
                .buttonStyle(.plain)
                .glassCapsule()
                .disabled(busy || summaries.isEmpty)
            }
            .frame(height: 30)
            .padding(.horizontal, 10)
            .padding(.vertical, 8)

            Divider().overlay(theme.divider)

            if summaries.isEmpty {
                VStack(spacing: 6) {
                    Text("暂无订阅，点击右上角「添加订阅」".t).foregroundStyle(theme.textMuted)
                    Text("支持订阅链接或本地文件路径".t)
                        .font(.system(size: 11)).foregroundStyle(theme.textMuted)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(selection: $selectedIndex) {
                    ForEach(Array(summaries.enumerated()), id: \.offset) { index, summary in
                        subscriptionRow(index: index, summary: summary)
                            .tag(index)
                            .listRowBackground(Color.clear)
                    }
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
                .onChange(of: selectedIndex) { loadNodes() }
            }

            if !message.isEmpty {
                Text(message)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .lineLimit(2)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(10)
            }
        }
        .sheet(item: $editing) { draft in
            SubscriptionEditor(draft: draft) { name, url in
                apply(draft: draft, name: name, url: url)
            }
            .environment(theme)
        }
    }

    private func subscriptionRow(index: Int, summary: SubscriptionSummary) -> some View {
        HStack(spacing: 8) {
            Toggle("", isOn: Binding(
                get: { summary.use },
                set: { newValue in
                    _ = state.subscriptions.setSubscriptionEnabled(at: index, newValue)
                    reload()
                    Task { await state.controller.rebuildConfig() }
                }
            ))
            .labelsHidden()
            .toggleStyle(.switch)
            .controlSize(.mini)

            VStack(alignment: .leading, spacing: 2) {
                Text(summary.name)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.textPrimary)
                    .lineLimit(1)
                Text(String(format: "%d/%d 个节点".t, summary.enabledNodeCount, summary.nodeCount))
                    .font(.system(size: 10))
                    .foregroundStyle(theme.textMuted)
            }

            Spacer(minLength: 4)

            // 操作**并排摆出来**，不收进「…」菜单 —— 对齐 Qt。
            //
            // 折叠成菜单省的是宽度，代价是每个动作都多一次点击、而且看不见有哪些动作。
            // 这一行本来就有富余空间，没必要拿它换。删除仍然二次确认（见 confirmDelete）。
            GlassGroup(spacing: 2) {
                rowAction("arrow.clockwise", "更新".t) { Task { await update(index: index) } }
                rowAction("square.and.pencil", "编辑".t) {
                    editing = EditingSubscription(index: index, name: summary.name, url: summary.url)
                }
                rowAction("trash", "删除".t, danger: true) { confirmDelete = index }
            }
        }
        .padding(.vertical, 3)
    }

    // MARK: 节点列

    private var nodeList: some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                Text(selectedIndex.map { summaries.indices.contains($0) ? summaries[$0].name : "" } ?? "选择一个订阅".t)
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textSecondary)
                    .lineLimit(1)
                Spacer()
                if let index = selectedIndex {
                    Button("全启用".t) { setAll(index: index, enabled: true) }
                    Button("全禁用".t) { setAll(index: index, enabled: false) }
                }
            }
            .padding(10)

            Divider().overlay(theme.divider)

            List {
                ForEach(Array(nodes.enumerated()), id: \.offset) { nodeIndex, node in
                    HStack(spacing: 8) {
                        Toggle("", isOn: Binding(
                            get: { node.use },
                            set: { newValue in
                                guard let index = selectedIndex else { return }
                                _ = state.subscriptions.setNodeEnabled(subscription: index,
                                                                       node: nodeIndex,
                                                                       newValue)
                                loadNodes(); reload()
                                Task { await state.controller.rebuildConfig() }
                            }
                        ))
                        .labelsHidden()
                        .toggleStyle(.switch)
                        .controlSize(.mini)

                        Text(node.name)
                            .font(.system(size: 12))
                            .foregroundStyle(theme.textPrimary)
                            .lineLimit(1)
                        Spacer(minLength: 4)
                        Text("\(node.server):\(node.port)")
                            .font(.system(size: 10))
                            .foregroundStyle(theme.textMuted)
                            .lineLimit(1)
                    }
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
        }
    }

    // MARK: 动作

    private func reload() {
        summaries = state.subscriptions.load()
        if let index = selectedIndex, !summaries.indices.contains(index) { selectedIndex = nil }
        loadNodes()
    }

    private func loadNodes() {
        nodes = selectedIndex.map { state.subscriptions.nodes(at: $0) } ?? []
    }

    private func setAll(index: Int, enabled: Bool) {
        _ = state.subscriptions.setAllNodesEnabled(subscription: index, enabled)
        loadNodes(); reload()
        Task { await state.controller.rebuildConfig() }
    }

    private func apply(draft: EditingSubscription, name: String, url: String) {
        if let index = draft.index {
            _ = state.subscriptions.editSubscription(at: index, name: name, url: url, type: "sub")
            reload()
            Task { await update(index: index) }
        } else {
            guard state.subscriptions.addSubscription(name: name, url: url, type: "sub") else { return }
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

private struct SubscriptionEditor: View {
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    let draft: SubscriptionsPage.EditingSubscription
    let onSave: (String, String) -> Void

    @State private var name = ""
    @State private var url = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(draft.index == nil ? "添加订阅".t : "编辑订阅".t)
                .font(.system(size: 14, weight: .medium))

            TextField("名称（留空则用链接）".t, text: $name)
            TextField("订阅链接".t, text: $url)

            HStack {
                Spacer()
                Button("取消".t) { dismiss() }
                Button("保存".t) {
                    onSave(name.trimmingCharacters(in: .whitespaces),
                           url.trimmingCharacters(in: .whitespaces))
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
                .disabled(url.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .textFieldStyle(.roundedBorder)
        .padding(16)
        .frame(width: 420)
        .onAppear { name = draft.name; url = draft.url }
    }
}
