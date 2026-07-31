import CoastKit
import SwiftUI

/// 自定义规则 / 区域分组编辑器。对齐 `qml/RuleEditorWindow.qml`。
///
/// 写的是 `configDir/rules.json`，消费者是 `ConfigBuilder.applyCustomRules` ——
/// 保存后必须重建配置并热重载，否则改了半天核心那边毫无变化。
struct RulesEditor: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    @State private var rules: [RulesStore.Rule] = []
    @State private var areas: [RulesStore.Area] = []
    @State private var filter = ""
    @State private var message = ""
    /// 这条消息是不是错误。
    ///
    /// **不能靠 `message.hasPrefix("已")` 去猜** —— 成功文案是 `"已保存并应用".t`，
    /// 翻译之后英文是 “Saved and applied”、日文是「保存して適用しました」，都不以「已」开头，
    /// 于是**每一种非中文语言里保存成功都会被标成红色错误**。判据必须由设置方直接给出。
    @State private var messageIsError = false
    @State private var tab = 0

    /// full.yaml 里现有的节点与策略组，供「目标策略」下拉用。
    /// 让用户从真实存在的名字里选，而不是手敲 —— 敲错一个字就是一条指向不存在目标的死规则。
    @State private var targets: [String] = []

    private let store = RulesStore()

    var body: some View {
        VStack(spacing: 0) {
            Picker("", selection: $tab) {
                Text("路由规则".t).tag(0)
                Text("区域分组".t).tag(1)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding(10)

            Divider().overlay(theme.divider)

            if tab == 0 { rulesList } else { areasList }

            Divider().overlay(theme.divider)

            HStack(spacing: 8) {
                if !message.isEmpty {
                    Text(message)
                        .font(.system(size: 11))
                        .foregroundStyle(messageIsError ? theme.danger : theme.textMuted)
                }
                Spacer()
                Button("关闭".t) { dismiss() }
                Button("保存并应用".t) { Task { await saveAndApply() } }
                    .keyboardShortcut(.defaultAction)
            }
            .padding(10)
        }
        .frame(width: 620, height: 460)
        .task { reload() }
    }

    // MARK: 路由规则

    private var rulesList: some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                TextField("筛选".t, text: $filter)
                    .textFieldStyle(.roundedBorder)
                Button {
                    rules.append(RulesStore.Rule(node: targets.first ?? "DIRECT"))
                } label: { Label("添加".t, systemImage: "plus") }
                Spacer()
                Text("\(filteredRuleIndices.count)/\(rules.count)")
                    .font(.system(size: 10)).foregroundStyle(theme.textMuted)
            }
            .padding(8)

            List {
                ForEach(filteredRuleIndices, id: \.self) { index in
                    HStack(spacing: 6) {
                        Picker("", selection: $rules[index].type) {
                            ForEach(RulesStore.ruleTypes, id: \.self) { Text($0).tag($0) }
                        }
                        .labelsHidden().frame(width: 150)

                        TextField("匹配值".t, text: $rules[index].value)
                            // MATCH 是兜底规则，没有匹配值这一段
                            .disabled(rules[index].type == "MATCH")
                            .opacity(rules[index].type == "MATCH" ? 0.4 : 1)

                        targetPicker(selection: $rules[index].node)

                        Button {
                            rules.remove(at: index)
                        } label: { Image(systemName: "trash") }
                            .buttonStyle(.borderless)
                    }
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .textFieldStyle(.roundedBorder)
        }
    }

    /// 过滤后的**真实下标**。
    ///
    /// 返回下标而不是过滤后的副本，是因为编辑要能写回原数组：过滤视图里改第 0 行，
    /// 改的必须是原数组里那一条，不是第 0 条。
    private var filteredRuleIndices: [Int] {
        let keyword = filter.trimmingCharacters(in: .whitespaces).lowercased()
        guard !keyword.isEmpty else { return Array(rules.indices) }
        return rules.indices.filter { index in
            let rule = rules[index]
            return rule.type.lowercased().contains(keyword)
                || rule.value.lowercased().contains(keyword)
                || rule.node.lowercased().contains(keyword)
        }
    }

    private func targetPicker(selection: Binding<String>) -> some View {
        Picker("", selection: selection) {
            // DIRECT/REJECT 恒可用，即使核心没在跑、拿不到策略组列表
            Text("DIRECT").tag("DIRECT")
            Text("REJECT").tag("REJECT")
            if !targets.isEmpty {
                Divider()
                ForEach(targets, id: \.self) { Text($0).tag($0) }
            }
            // 已存在但不在候选里的值（例如核心没跑时读到的旧配置）也要能显示出来，
            // 否则 Picker 会显示空白，用户以为规则丢了
            if !targets.contains(selection.wrappedValue),
               selection.wrappedValue != "DIRECT", selection.wrappedValue != "REJECT" {
                Divider()
                Text(selection.wrappedValue).tag(selection.wrappedValue)
            }
        }
        .labelsHidden().frame(width: 160)
    }

    // MARK: 区域分组

    private var areasList: some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                Button {
                    areas.append(RulesStore.Area())
                } label: { Label("添加".t, systemImage: "plus") }
                Spacer()
                Text("按正则匹配节点名，自动生成一个策略组".t)
                    .font(.system(size: 10)).foregroundStyle(theme.textMuted)
            }
            .padding(8)

            List {
                ForEach(areas.indices, id: \.self) { index in
                    HStack(spacing: 6) {
                        TextField("分组名".t, text: $areas[index].name).frame(width: 140)
                        Picker("", selection: $areas[index].type) {
                            ForEach(RulesStore.areaTypes, id: \.self) { Text($0).tag($0) }
                        }
                        .labelsHidden().frame(width: 120)
                        TextField("节点名正则".t, text: $areas[index].rule)
                        Button {
                            areas.remove(at: index)
                        } label: { Image(systemName: "trash") }
                            .buttonStyle(.borderless)
                    }
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .textFieldStyle(.roundedBorder)
        }
    }

    // MARK: 动作

    private func reload() {
        let loaded = store.load()
        rules = loaded.rules
        areas = loaded.areas
        // 从生成好的 full.yaml 里读现有策略组与节点名
        if let yaml = try? String(contentsOf: AppPaths.fullConfig, encoding: .utf8) {
            targets = ConfigBuilder.existingGroupNames(yaml) + ConfigBuilder.proxyNames(yaml)
        }
    }

    private func saveAndApply() async {
        // 逐条校验后才写。坏规则会让核心**整份配置**加载失败，而用户只会看到「突然全断网」，
        // 完全无从关联到自己刚加的那条。
        for (offset, rule) in rules.enumerated() {
            if let reason = RulesStore.validate(rule) {
                message = String(format: "第 %d 条规则：%@".t, offset + 1, reason)
                messageIsError = true
                return
            }
        }
        for (offset, area) in areas.enumerated() {
            if let reason = RulesStore.validate(area) {
                message = String(format: "第 %d 个分组：%@".t, offset + 1, reason)
                messageIsError = true
                return
            }
        }
        guard store.save(rules: rules, areas: areas) else {
            message = "写入 rules.json 失败".t
            messageIsError = true
            return
        }
        message = "已保存，正在重建配置…".t
        messageIsError = false
        await state.controller.rebuildConfig()
        message = "已保存并应用".t
        messageIsError = false
    }
}
