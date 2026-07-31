import CoastKit
import SwiftUI

/// 新增 / 编辑一条自定义规则。**逐元素对齐** `qml/RuleEditorWindow.qml`：
/// 460×440、内距 16、行距 8、标题 16、说明标签 12（可换行）、三个 32 高的控件铺满宽、
/// 底部「取消 / 确定」。
///
/// 写的是 `configDir/rules.json`，消费者是 `ConfigBuilder.applyCustomRules` ——
/// 保存后必须重建配置并热重载，否则改了半天核心那边毫无变化（由调用方 `SettingsPage` 负责）。
struct RuleEditorSheet: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    @State var draft: RuleDraft
    let onSave: (RuleDraft) -> Void

    /// 「节点 / 策略组」候选：DIRECT / REJECT + full.yaml 里现有的全部策略组与节点名。
    /// 让用户从真实存在的名字里选，而不是手敲 —— 敲错一个字就是一条指向不存在目标的死规则。
    @State private var targets: [String] = []
    /// 类型是进程规则时，「值」变成可搜索的系统进程下拉。
    @State private var processChoices: [String] = []
    @State private var error = ""

    /// 类型候选，顺序与 `qml/RuleEditorWindow.qml` 的 `ruleTypes` 完全一致。
    private static let ruleTypes = ["DOMAIN-SUFFIX", "DOMAIN", "DOMAIN-KEYWORD", "DOMAIN-REGEX",
                                    "IP-CIDR", "IP-CIDR6", "GEOIP", "GEOSITE", "IP-ASN",
                                    "SRC-IP-CIDR", "SRC-PORT", "DST-PORT",
                                    "PROCESS-NAME", "PROCESS-PATH", "RULE-SET", "MATCH"]

    private var isProcessType: Bool {
        draft.rule.type == "PROCESS-NAME" || draft.rule.type == "PROCESS-PATH"
    }

    /// 现值不在候选里时**前插**，保证编辑旧规则时也显示得出来（否则下拉是空白，
    /// 用户以为规则丢了）。
    private var typeOptions: [String] {
        Self.ruleTypes.contains(draft.rule.type) ? Self.ruleTypes : [draft.rule.type] + Self.ruleTypes
    }

    private var nodeOptions: [String] {
        var list = ["DIRECT", "REJECT"] + targets
        if !draft.rule.node.isEmpty, !list.contains(draft.rule.node) {
            list.insert(draft.rule.node, at: 0)
        }
        return list
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(draft.index == nil ? "新增规则".t : "编辑规则".t)
                .font(.system(size: 16))
                .foregroundStyle(theme.textPrimary)

            fieldLabel("类型".t)
            ThemedCombo(options: typeOptions,
                        selection: Binding(
                            get: { typeOptions.firstIndex(of: draft.rule.type) ?? 0 },
                            set: { draft.rule.type = typeOptions[$0] }),
                        width: nil)

            fieldLabel("值（域名 / IP 段；进程规则从下拉搜索选择；MATCH 留空）".t)
            SearchableValueField(text: $draft.rule.value,
                                 choices: isProcessType ? processChoices : [],
                                 placeholder: "输入或从下拉选择".t)
                // MATCH 是兜底规则，没有匹配值这一段。
                .disabled(draft.rule.type == "MATCH")
                .opacity(draft.rule.type == "MATCH" ? 0.4 : 1)

            fieldLabel("节点 / 策略组".t)
            ThemedCombo(options: nodeOptions,
                        selection: Binding(
                            get: { nodeOptions.firstIndex(of: draft.rule.node) ?? 0 },
                            set: { draft.rule.node = nodeOptions[$0] }),
                        width: nil)

            if !error.isEmpty {
                Text(error).font(.system(size: 12)).foregroundStyle(theme.danger)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Spacer(minLength: 0)

            HStack(spacing: 8) {
                Spacer(minLength: 0)
                PillButton(title: "取消".t) { dismiss() }
                PillButton(title: "确定".t, primary: true) { confirm() }
            }
        }
        .padding(16)
        .frame(width: 460, height: 440)
        .background(theme.card)
        .task { await load() }
        .onChange(of: draft.rule.type) { _, _ in Task { await refreshProcesses() } }
    }

    private func fieldLabel(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 12))
            .foregroundStyle(theme.textSecondary)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func load() async {
        if let yaml = try? String(contentsOf: AppPaths.fullConfig, encoding: .utf8) {
            targets = ConfigBuilder.existingGroupNames(yaml) + ConfigBuilder.proxyNames(yaml)
        }
        await refreshProcesses()
    }

    private func refreshProcesses() async {
        guard isProcessType else { processChoices = []; return }
        let wantsPath = draft.rule.type == "PROCESS-PATH"
        processChoices = ProcessChoices.current(paths: wantsPath)
    }

    /// **保存前校验**。坏规则会让核心整份配置加载失败，而用户只会看到「突然全断网」，
    /// 完全无从关联到自己刚加的那条 —— 所以拦在这里，而不是等核心拒绝。
    private func confirm() {
        if let reason = RulesStore.validate(draft.rule) {
            error = reason
            return
        }
        onSave(draft)
        dismiss()
    }
}

/// 新增 / 编辑一个区域分组。对齐 Qt 设置页里那个 `areaEditor` 窗口的尺寸与骨架
/// （440×470、内距 16、行距 8、标题 16、底部「取消 / 确定」）。
///
/// ★ **字段与 Qt 那个编辑器不同，是有意的。** Qt 的编辑器写的是
/// `{name, type, url, interval, proxies}`（显式成员列表），而 **Qt 自己的
/// `ConfigBuilder::applyCustomRules` 读的是 `{name, type, rule}`，并且 `rule` 为空就整组跳过**
/// —— 也就是说，用 Qt 那个编辑器建出来的分组会被 Qt 自己的配置生成器静默丢掉。
/// 那是 Qt 侧两代 schema 并存留下的 bug，不是该照搬的行为。这里按**真正被消费的字段**
/// 做编辑器：名称 / 类型 / 节点名正则。
struct AreaEditorSheet: View {
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    @State var draft: AreaDraft
    let onSave: (AreaDraft) -> Void

    @State private var error = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(draft.index == nil ? "新增区域分组".t : "编辑区域分组".t)
                .font(.system(size: 16))
                .foregroundStyle(theme.textPrimary)

            fieldLabel("名称".t)
            // 编辑时锁名：改名会破坏已经引用这个组的规则。
            ThemedField(text: $draft.area.name, width: nil, enabled: draft.index == nil)

            fieldLabel("类型".t)
            ThemedCombo(options: RulesStore.areaTypes,
                        selection: Binding(
                            get: { RulesStore.areaTypes.firstIndex(of: draft.area.type) ?? 0 },
                            set: { draft.area.type = RulesStore.areaTypes[$0] }),
                        width: nil)

            fieldLabel("节点名正则（匹配到的节点会被收进这个组）".t)
            ThemedField(text: $draft.area.rule, width: nil)

            if !error.isEmpty {
                Text(error).font(.system(size: 12)).foregroundStyle(theme.danger)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Spacer(minLength: 0)

            HStack(spacing: 8) {
                Spacer(minLength: 0)
                PillButton(title: "取消".t) { dismiss() }
                PillButton(title: "确定".t, primary: true) { confirm() }
            }
        }
        .padding(16)
        .frame(width: 440, height: 470)
        .background(theme.card)
    }

    private func fieldLabel(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 12))
            .foregroundStyle(theme.textSecondary)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// 正则**当场校验**：`ConfigBuilder` 遇到非法正则会静默跳过那个组，
    /// 用户加了组却什么都没发生，不如在这里说清楚。
    private func confirm() {
        if let reason = RulesStore.validate(draft.area) {
            error = reason
            return
        }
        onSave(draft)
        dismiss()
    }
}

/// 可搜索的「值」输入框。对齐 QML 的 `ValueInput`：32 高、半径 3；
/// `choices` 非空时右侧出现 ▾ 并可按输入过滤，为空时就是一个纯文本框。
private struct SearchableValueField: View {
    @Environment(Theme.self) private var theme
    @Binding var text: String
    let choices: [String]
    var placeholder = ""

    @FocusState private var focused: Bool

    private var filtered: [String] {
        let needle = text.lowercased()
        guard !needle.isEmpty else { return choices }
        return choices.filter { $0.lowercased().contains(needle) }
    }

    var body: some View {
        HStack(spacing: 2) {
            TextField(placeholder, text: $text)
                .textFieldStyle(.plain)
                .font(.system(size: 13))
                .foregroundStyle(theme.textPrimary)
                .focused($focused)
                .padding(.leading, 8)

            if !choices.isEmpty {
                Menu {
                    // 候选可能上千条（系统进程），全塞进菜单会卡住主线程。
                    ForEach(filtered.prefix(200), id: \.self) { choice in
                        Button(choice) { text = choice }
                    }
                } label: {
                    Text("▾").font(.system(size: 12)).foregroundStyle(theme.textMuted)
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
                .padding(.trailing, 4)
            }
        }
        .frame(height: 32)
        .background {
            RoundedRectangle(cornerRadius: 3, style: .continuous).fill(theme.inputBg)
        }
        .overlay {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .stroke(focused ? theme.accent : theme.inputBorder, lineWidth: 1)
        }
    }
}

/// 系统进程候选。对齐 Qt `SettingsController::processChoices(bool paths)`。
enum ProcessChoices {
    /// `paths` 为真取可执行文件全路径（PROCESS-PATH），否则取进程名（PROCESS-NAME）。
    /// 去重 + 排序：`ps` 的输出里同一个程序常有几十个实例。
    static func current(paths: Bool) -> [String] {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/bin/ps")
        task.arguments = ["-Axo", "comm="]
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = FileHandle.nullDevice
        guard (try? task.run()) != nil else { return [] }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()
        guard let text = String(data: data, encoding: .utf8) else { return [] }

        var seen = Set<String>()
        for line in text.split(separator: "\n") {
            let full = line.trimmingCharacters(in: .whitespaces)
            guard !full.isEmpty else { continue }
            seen.insert(paths ? full : (full as NSString).lastPathComponent)
        }
        return seen.sorted()
    }
}
