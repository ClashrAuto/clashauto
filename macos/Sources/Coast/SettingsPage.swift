import CoastKit
import SwiftUI

/// 设置页。**逐元素对齐** `qml/SettingsPage.qml`：顶部 3 个标签（系统 / 区域 / 规则）
/// + 右上「应用」，下面是各标签的内容。
///
/// 这一版把原来那张「一列 section 从上滚到底」的表整个换掉了 —— Qt 是**分标签**的，
/// 而且系统页是几张分组卡、每行固定 40 高、标签靠左控件靠右。原来那张表既没有标签，
/// 也没有卡，控件全是系统原生的（高度随 macOS 版本变，尺寸对不上）。
///
/// 落盘语义也按 Qt 分成两类，不再是「全部等点应用」：
///   • **开关 / 下拉即时生效**（系统页里的每一项开关）；
///   • **Host / 端口 / 过滤正则等文本输入等「应用」**（改一半的中间值直接生效会打断核心：
///     用户把 7890 改成 1080，输到 "108" 时就已经重启过一次核心了）。
///   Qt 的「应用」按钮也只在系统标签上出现，区域 / 规则是改完即生效。
struct SettingsPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    /// 「过滤」原本是独立的第二个标签，现在并进系统页的「节点过滤」卡 —— 它总共就四行，
    /// 撑不起一个标签页，而那四行本来就属于「节点与订阅」这一类设置。Qt 端同步照做。
    enum Tab: Int, CaseIterable, Identifiable {
        case system, area, rule
        var id: Int { rawValue }
        @MainActor var title: String {
            switch self {
            case .system: return "系统".t
            case .area: return "区域".t
            case .rule: return "规则".t
            }
        }
    }

    @State private var tab: Tab = .system

    /// 文本类输入的草稿（Host / 两个端口 / 两条正则）。开关类不进草稿 —— 它们即时生效。
    @State private var host = ""
    @State private var mixedPort = ""
    @State private var uiPort = ""
    @State private var allowRule = ""
    @State private var noAllowRule = ""

    @State private var message = ""
    @State private var coreStatus = ""
    @State private var geoipStatus = ""
    @State private var coreBusy = false
    @State private var geoipBusy = false
    @State private var helperStatus = ""

    // 区域 / 规则表
    @State private var rules: [RulesStore.Rule] = []
    @State private var areas: [RulesStore.Area] = []
    @State private var ruleFilter = ""
    @State private var editingRule: RuleDraft?
    @State private var editingArea: AreaDraft?

    /// Qt 的 `allowRulePresets` / `noAllowRulePresets`，逐条相同。
    private let allowPresets = [#"\[0\.[0-9]+\]"#]
    private let noAllowPresets = [#"\[[0-9]+\]"#, #"[0-9\[\]]+"#, #"[\[0-9\]]+$"#,
                                 #"[0-9]{1}\]$"#, "CN", "^CN", "CN_", " "]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if !message.isEmpty {
                Text(message)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .padding(.trailing, 10)
                    .padding(.leading, pageLeadingInset)
            }

            switch tab {
            case .system: systemTab
            case .area: areaTab
            case .rule: ruleTab
            }
        }
        .pageHeaderBar(spacing: 8) { header }
        .task { reload() }
        .sheet(item: $editingRule) { draft in
            RuleEditorSheet(draft: draft) { saved in save(rule: saved) }
                .environment(state).environment(theme)
        }
        .sheet(item: $editingArea) { draft in
            AreaEditorSheet(draft: draft) { saved in save(area: saved) }
                .environment(state).environment(theme)
        }
    }

    // MARK: 顶栏

    private var header: some View {
        HStack(spacing: 4) {
            if #available(macOS 26.0, *) {
                // 26：左侧标签并成**一颗连体玻璃按钮组**（设备页 ●⟳ 同款）——
                // 段与段紧挨、共一层玻璃，选中段由段内的系统 tint 底衬表达。
                HStack(spacing: 0) {
                    ForEach(Tab.allCases) { item in
                        SettingTab(title: item.title, isCurrent: tab == item) { tab = item }
                    }
                }
                .glassCapsule()
            } else {
                ForEach(Tab.allCases) { item in
                    SettingTab(title: item.title, isCurrent: tab == item) { tab = item }
                }
            }
            Spacer(minLength: 0)

            // 区域 / 规则页没有「应用」。QML 用的是**透明占位**而不是隐藏 ——
            // 隐藏会让它被布局移除、标签栏随之变宽，切标签时整条顶栏跳一下。
            if #available(macOS 26.0, *) {
                // 26：中性液态玻璃按钮，配色交给系统（无自定义底色）。
                Button {
                    Task { await apply() }
                } label: {
                    Text("应用".t)
                        .font(.system(size: 13))
                        .lineLimit(1)
                        .frame(width: 66, height: 28)
                        .contentShape(Capsule())
                }
                .buttonStyle(.plain)
                .glassCapsule()
                .disabled(tab != .system)
                .opacity(tab == .system ? 1 : 0)
            } else {
                PillButton(title: "应用".t, primary: true, width: 82,
                           enabled: tab == .system) {
                    Task { await apply() }
                }
                .opacity(tab == .system ? 1 : 0)
            }
        }
        .padding(.top, 10)
        .padding(.trailing, 10)
        .padding(.leading, pageLeadingInset)
    }

    // MARK: TAB 0 系统

    private var systemTab: some View {
        ScrollView {
            VStack(spacing: 10) {
                SettingCard(symbol: "desktopcomputer", title: "系统".t) {
                    SettingRow(label: "开机自启".t) {
                        ThemedSwitch(isOn: Binding(
                            get: { state.config.autoStart },
                            set: { setAutoStart($0) }))
                    }
                    CardDivider()
                    SettingRow(label: "启动到托盘".t) {
                        ThemedSwitch(isOn: bool(\.closeToTray, key: "mini"))
                    }
                    CardDivider()
                    SettingRow(label: "系统代理".t) {
                        ThemedSwitch(isOn: Binding(
                            get: { state.controller.isProxyEnabled },
                            set: { _ in state.toggleProxy() }))
                    }
                    CardDivider()
                    SettingRow(label: "切换通知".t) {
                        ThemedSwitch(isOn: bool(\.nodeSwitchNote, key: "note"))
                    }
                }

                SettingCard(symbol: "square.stack.3d.up", title: "节点与订阅".t) {
                    SettingRow(label: "仅可用节点".t) {
                        ThemedSwitch(isOn: bool(\.nodeOnlyAvailable, key: "node"))
                    }
                    CardDivider()
                    SettingRow(label: "增量更新".t) {
                        ThemedSwitch(isOn: bool(\.increment, key: "increment"))
                    }
                    CardDivider()
                    SettingRow(label: "订阅自动更新周期".t) {
                        ThemedSpin(value: Binding(
                            get: { state.config.autoUpdateMinutes },
                            set: { persist(key: "autoUpdate", int: $0, apply: { $0.autoUpdateMinutes = $1 }) }
                        ), width: 140)
                        Text("分钟".t).font(.system(size: 12)).foregroundStyle(theme.textMuted).lineLimit(1)
                    }
                }

                // 原「过滤」标签页的四行。两个开关即时落盘，两条正则是**草稿**、等右上
                // 「应用」一起写 —— 正则改到一半就生效的话，每敲一个字符都会按半截表达式
                // 重筛一次节点表（同 Host/端口，见 `apply()`）。
                SettingCard(symbol: "line.3.horizontal.decrease", title: "节点过滤".t) {
                    SettingRow(label: "启用允许规则".t) {
                        ThemedSwitch(isOn: bool(\.allowRuleEnabled, section: "use_rule", key: "allowUse"))
                    }
                    CardDivider()
                    SettingRow(label: "允许规则(正则)".t) {
                        ThemedEditCombo(text: $allowRule, options: allowPresets, width: 300)
                    }
                    CardDivider()
                    SettingRow(label: "启用排除规则".t) {
                        ThemedSwitch(isOn: bool(\.noAllowRuleEnabled, section: "use_rule", key: "noallowUse"))
                    }
                    CardDivider()
                    SettingRow(label: "排除规则(正则)".t) {
                        ThemedEditCombo(text: $noAllowRule, options: noAllowPresets, width: 300)
                    }
                }

                SettingCard(symbol: "paintbrush", title: "界面与语言".t) {
                    SettingRow(label: "主题".t) {
                        // 跟随系统时手选无效，置灰（与下面「语言」跟随系统语言时同一套路）。
                        // 值始终显示**手选**的那份（config `theme:`）—— 跟随开着时它和当前
                        // 生效的明暗可以不一样，是关掉跟随后要回到的那个主题。
                        ThemedCombo(options: ["黑色".t, "白色".t],
                                    selection: Binding(
                                        get: { state.config.theme.lowercased() == "light" ? 1 : 0 },
                                        set: { setThemeLight($0 == 1) }),
                                    width: 140,
                                    enabled: !state.config.autoTheme)
                    }
                    CardDivider()
                    SettingRow(label: "跟随系统深浅色".t) {
                        ThemedSwitch(isOn: bool(\.autoTheme, key: "autoTheme"))
                    }
                    CardDivider()
                    SettingRow(label: "跟随系统语言".t) {
                        ThemedSwitch(isOn: bool(\.autoLanguage, key: "autoLanguage"))
                    }
                    CardDivider()
                    SettingRow(label: "语言".t) {
                        ThemedCombo(options: I18n.languageNames,
                                    selection: Binding(
                                        get: { max(0, I18n.languageCodes.firstIndex(of: state.config.language) ?? 0) },
                                        set: { setLanguage(I18n.languageCodes[$0]) }),
                                    width: 180,
                                    enabled: !state.config.autoLanguage)
                    }
                }

                SettingCard(symbol: "cpu", title: "内核".t) {
                    SettingRow(label: "Host") {
                        ThemedEditCombo(text: $host, options: ["127.0.0.1", "localhost"], width: 180)
                    }
                    CardDivider()
                    SettingRow(label: "端口".t) {
                        ThemedField(text: $mixedPort, width: 140)
                    }
                    CardDivider()
                    SettingRow(label: "API 端口".t) {
                        ThemedField(text: $uiPort, width: 140)
                            .help("mihomo REST API 端口；默认 9191，改后会重启核心".t)
                    }
                    CardDivider()
                    SettingRow(label: "切换时清理连接".t) {
                        ThemedSwitch(isOn: Binding(
                            get: { state.config.clearConnections },
                            set: { newValue in
                                persist(key: "clearConnections", bool: newValue) { $0.clearConnections = $1 }
                                state.clash.setClearConnectionsOnSwitch(newValue)
                            }))
                    }
                    CardDivider()
                    SettingRow(label: "GeoIP 数据".t) {
                        PillButton(title: geoipBusy ? geoipStatus : "更新 GeoIP".t,
                                   width: 120, enabled: !geoipBusy) {
                            Task { await updateGeoIP() }
                        }
                    }
                    CardDivider()
                    SettingRow(label: "程序更新".t) {
                        Text("接收测试版".t).font(.system(size: 13)).foregroundStyle(theme.textSecondary).lineLimit(1)
                        ThemedSwitch(isOn: bool(\.receiveBeta, key: "beta"))
                    }
                    CardDivider()
                    SettingRow(label: "mihomo 内核".t) {
                        Text("国内加速".t).font(.system(size: 13)).foregroundStyle(theme.textSecondary).lineLimit(1)
                        ThemedSwitch(isOn: bool(\.mirror, key: "mirror"))
                        PillButton(title: coreBusy ? coreStatus : "更新内核".t,
                                   width: 120, enabled: !coreBusy) {
                            Task { await updateCore() }
                        }
                    }
                    CardDivider()
                    SettingRow(label: "免密助手".t) {
                        Text(helperStatus.isEmpty ? helperText : helperStatus)
                            .font(.system(size: 12))
                            .foregroundStyle(theme.textMuted)
                            .lineLimit(1).truncationMode(.tail)
                            .frame(width: 200, alignment: .trailing)
                        PillButton(title: "安装/启用".t) {
                            Task { helperStatus = await state.controller.installHelper() }
                        }
                        PillButton(title: "卸载".t) {
                            Task { helperStatus = await state.controller.uninstallHelper() }
                        }
                    }
                }
            }
            .padding(.trailing, 10)
            .padding(.leading, pageLeadingInset)
            // QML 末尾那个 `Item { Layout.preferredHeight: 0 }`：列 spacing 10 即末卡距底距离。
            .padding(.bottom, 10)
        }
    }

    // MARK: TAB 1 区域

    private var areaTab: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                PillButton(title: "＋ 添加".t, primary: true, width: 96) {
                    editingArea = AreaDraft(index: nil, area: RulesStore.Area())
                }
                Spacer(minLength: 0)
                Text(String(format: "共 %d 组".t, areas.count))
                    .font(.system(size: 12)).foregroundStyle(theme.textMuted).lineLimit(1)
            }
            .padding(.trailing, 10)
            .padding(.leading, pageLeadingInset)

            ScrollView {
                LazyVStack(spacing: 4) {
                    ForEach(Array(areas.enumerated()), id: \.offset) { index, area in
                        listRow {
                            Text(area.name).font(.system(size: 13)).foregroundStyle(theme.textPrimary)
                                .lineLimit(1).truncationMode(.tail)
                                .frame(width: 150, alignment: .leading)
                            Text(area.type).font(.system(size: 12)).foregroundStyle(theme.textMuted)
                                .frame(width: 90, alignment: .leading)
                            Text(area.rule).font(.system(size: 12)).foregroundStyle(theme.textSecondary)
                                .lineLimit(1).truncationMode(.tail)
                                .frame(maxWidth: .infinity, alignment: .leading)
                            PillButton(title: "✎", width: 30, height: 24) {
                                editingArea = AreaDraft(index: index, area: area)
                            }
                            PillButton(title: "✕", width: 30, height: 24) {
                                areas.remove(at: index); persistRules()
                            }
                        }
                    }
                }
                .padding(.bottom, 10)
            }
        }
    }

    // MARK: TAB 2 规则

    private var ruleTab: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                PillButton(title: "＋ 添加".t, primary: true, width: 96) {
                    editingRule = RuleDraft(index: nil, rule: RulesStore.Rule())
                }
                ThemedField(text: $ruleFilter, placeholder: "搜索规则（类型/节点/值）".t, width: 220)
                Spacer(minLength: 0)
                // 与 Qt 同一句：只有真的少显示了才说「显示前 M 条」。
                Text(filteredRules.count < rules.count
                     ? String(format: "共 %d 条，显示前 %d 条".t, rules.count, filteredRules.count)
                     : String(format: "共 %d 条".t, rules.count))
                    .font(.system(size: 12)).foregroundStyle(theme.textMuted).lineLimit(1)
            }
            .padding(.trailing, 10)
            .padding(.leading, pageLeadingInset)

            ScrollView {
                LazyVStack(spacing: 4) {
                    // 过滤后仍带**真实下标**：过滤视图里改第 0 行，改的必须是原数组里那一条。
                    ForEach(filteredRules, id: \.0) { index, rule in
                        listRow {
                            Text(rule.type).font(.system(size: 13)).foregroundStyle(theme.textPrimary)
                                .lineLimit(1).truncationMode(.tail)
                                .frame(width: 150, alignment: .leading)
                            Text(rule.node).font(.system(size: 12)).foregroundStyle(theme.accentStrong)
                                .lineLimit(1).truncationMode(.tail)
                                .frame(width: 130, alignment: .leading)
                            Text(rule.value).font(.system(size: 12)).foregroundStyle(theme.textSecondary)
                                .lineLimit(1).truncationMode(.tail)
                                .frame(maxWidth: .infinity, alignment: .leading)
                            PillButton(title: "✎", width: 30, height: 24) {
                                editingRule = RuleDraft(index: index, rule: rule)
                            }
                            PillButton(title: "✕", width: 30, height: 24) {
                                rules.remove(at: index); persistRules()
                            }
                        }
                    }
                }
                .padding(.bottom, 10)
            }
        }
    }

    /// 列表最多渲染这么多行。与 Qt 的 `SettingsController::reloadRules` 里那个 `cap = 400`
    /// 同一个数 —— 一份完整规则集动辄几千条，全铺出来滚起来是卡的。
    ///
    /// 截断**必须说出来**（下面那行「共 N 条，显示前 M 条」）：不说的话，用户看到的是
    /// 一份看起来完整、实际上少了一大半的列表，而且完全没有线索。
    private static let ruleRenderCap = 400

    /// 过滤 + 截断后的行。下标是**完整数组里的真实下标**，改第 12 行改的就是原数组第 12 条。
    private var filteredRules: [(Int, RulesStore.Rule)] {
        let needle = ruleFilter.trimmingCharacters(in: .whitespaces).lowercased()
        let matched = rules.enumerated().compactMap { index, rule -> (Int, RulesStore.Rule)? in
            guard needle.isEmpty
                    || rule.type.lowercased().contains(needle)
                    || rule.node.lowercased().contains(needle)
                    || rule.value.lowercased().contains(needle)
            else { return nil }
            return (index, rule)
        }
        return Array(matched.prefix(Self.ruleRenderCap))
    }

    /// 区域 / 规则表的一行：高 36、左右内缩 10、半径 3、`nodeRowBg`，内容左 10 右 6。
    private func listRow<Content: View>(@ViewBuilder content: () -> Content) -> some View {
        HStack(spacing: 8) { content() }
            .padding(.leading, 10)
            .padding(.trailing, 6)
            .frame(height: 36)
            .background(theme.nodeRowBg)
            .clipShape(RoundedRectangle(cornerRadius: 3, style: .continuous))
            .padding(.trailing, 10)
            .padding(.leading, pageLeadingInset)
    }

    // MARK: 落盘

    /// 开关类：**即时生效**。绑定的 get 读 `state.config`，set 落盘 + 回灌内存。
    private func bool(_ path: WritableKeyPath<AppConfig, Bool>, key: String) -> Binding<Bool> {
        Binding(
            get: { state.config[keyPath: path] },
            set: { newValue in
                AppConfigLoader.persist(key: key, bool: newValue)
                var next = state.config
                next[keyPath: path] = newValue
                state.applyConfig(next)
            }
        )
    }

    /// 同上，但键在某个二级段下（`use_rule`）。顶层的 `persist` 写不进段里。
    private func bool(_ path: WritableKeyPath<AppConfig, Bool>, section: String, key: String) -> Binding<Bool> {
        Binding(
            get: { state.config[keyPath: path] },
            set: { newValue in
                AppConfigLoader.persist(section: section, key: key, bool: newValue)
                var next = state.config
                next[keyPath: path] = newValue
                state.applyConfig(next)
            }
        )
    }

    private func persist(key: String, int value: Int, apply: (inout AppConfig, Int) -> Void) {
        AppConfigLoader.persist(key: key, int: value)
        var next = state.config
        apply(&next, value)
        state.applyConfig(next)
    }

    private func persist(key: String, bool value: Bool, apply: (inout AppConfig, Bool) -> Void) {
        AppConfigLoader.persist(key: key, bool: value)
        var next = state.config
        apply(&next, value)
        state.applyConfig(next)
    }

    private func setAutoStart(_ enabled: Bool) {
        // 开机自启是**系统状态**，不能等「应用」：立刻同步，并把系统的真实结果读回来。
        if case .failure(let error) = LaunchAtLogin.setEnabled(enabled) {
            message = String(format: "设置开机自启失败：%@".t, error.localizedDescription)
        }
        persist(key: "sys", bool: LaunchAtLogin.isEnabled) { $0.autoStart = $1 }
    }

    private func setThemeLight(_ light: Bool) {
        AppConfigLoader.persist(key: "theme", raw: light ? "light" : "black")
        var next = state.config
        next.theme = light ? "light" : "black"
        // 换肤交给 `applyConfig` —— 它按 `autoTheme` 分流。这里再补一句 `theme.dark = !light`
        // 就会绕过跟随态，把手选值硬套上去（下拉已置灰，但别留这条后门）。
        state.applyConfig(next)
    }

    private func setLanguage(_ code: String) {
        AppConfigLoader.persist(key: "language", raw: code)
        var next = state.config
        next.language = code
        state.applyConfig(next)
        I18n.shared.applyConfig(next)
    }

    /// 「应用」只落**文本输入**那几项 —— 开关早在拨动的那一刻就写进去了。
    private func apply() async {
        let old = state.config
        var next = state.config
        next.host = host.trimmingCharacters(in: .whitespaces)
        next.mixedPort = Int(mixedPort) ?? old.mixedPort
        next.uiPort = Int(uiPort) ?? old.uiPort
        next.allowRule = allowRule
        next.noAllowRule = noAllowRule

        AppConfigLoader.persist(key: "host", raw: next.host)
        AppConfigLoader.persist(key: "port", int: next.mixedPort)
        AppConfigLoader.persist(key: "ui", int: next.uiPort)
        // 正则要加引号：里面常有空格、`|`、`#`。
        AppConfigLoader.persist(section: "use_rule", key: "allow", raw: YAMLText.quoted(next.allowRule))
        AppConfigLoader.persist(section: "use_rule", key: "noallow", raw: YAMLText.quoted(next.noAllowRule))

        state.applyConfig(next)
        state.clash.setMixedPort(next.mixedPort)

        // 只有 **API 端口**（`external-controller`）改了才需要重启核心 —— 它不在
        // `full.yaml` 里，热重载碰不到它，核心会继续 bind 在旧端口而 UI 已按新端口去连。
        //
        // ★ 混合端口**不重启**：它就写在 `full.yaml` 里，`rebuildConfig()` 的热重载
        //   会让核心重新监听。原来两个端口任一变化都重启，白白把所有连接断一次
        //   （Qt 那边分得很清楚：只有 uiPort 变了才 stop/start）。
        let apiPortChanged = next.uiPort != old.uiPort
        if apiPortChanged, state.controller.isCoreRunning {
            message = "端口已变更，正在重启核心…".t
            await state.controller.stopCore()
            await state.controller.startCore()
            state.clash.setEndpoint(host: next.host, port: next.uiPort)
            message = "已应用（核心已重启）".t
        } else {
            if apiPortChanged { state.clash.setEndpoint(host: next.host, port: next.uiPort) }
            await state.controller.rebuildConfig()
            // 核心没在跑时改 API 端口：不能说「已重启」，也不能什么都不说 ——
            // 用户改完得知道它什么时候生效（Qt 的第三条分支）。
            message = apiPortChanged ? "已应用（下次启动核心生效）".t : "已应用".t
        }
    }

    private func reload() {
        host = state.config.host
        mixedPort = String(state.config.mixedPort)
        uiPort = String(state.config.uiPort)
        allowRule = state.config.allowRule
        noAllowRule = state.config.noAllowRule
        coreStatus = "更新内核".t
        geoipStatus = "更新 GeoIP".t
        let loaded = RulesStore().load()
        rules = loaded.rules
        areas = loaded.areas
    }

    private func save(rule: RulesStore.Rule, at index: Int?) {
        if let index, rules.indices.contains(index) { rules[index] = rule }
        else { rules.insert(rule, at: 0) }   // 新增前插，与 Qt 一致
        persistRules()
    }

    private func save(rule draft: RuleDraft) { save(rule: draft.rule, at: draft.index) }

    private func save(area draft: AreaDraft) {
        if let index = draft.index, areas.indices.contains(index) { areas[index] = draft.area }
        else { areas.append(draft.area) }    // 区域是追加，与 Qt 一致
        persistRules()
    }

    private func persistRules() {
        _ = RulesStore().save(rules: rules, areas: areas)
        Task { await state.controller.rebuildConfig() }
        message = "已保存并重建配置".t
    }

    private var helperText: String {
        switch state.controller.helperStatus {
        case .enabled: return "已启用".t
        case .requiresApproval: return "已注册，待在「系统设置 → 登录项」批准".t
        case .notRegistered: return "未安装".t
        case .notFound: return "找不到（应用包结构异常）".t
        case .unknown: return "未知".t
        }
    }

    // MARK: 内核 / GeoIP 更新

    /// 下载来的字节先过结构校验、只写暂存，**绝不原地覆盖线上库** —— 核心把它 mmap 着且
    /// 只加载一次，原地换根本不会生效，唯一效果是有概率毁掉好文件（Qt 版真机上为此坏过一次）。
    private func updateGeoIP() async {
        geoipBusy = true
        defer { geoipBusy = false; geoipStatus = "更新 GeoIP".t }
        let updater = GeoIPUpdater(useMirror: state.config.mirror,
                                   proxyPort: state.controller.isCoreRunning ? state.config.mixedPort : nil)
        do {
            message = try await updater.update { step in
                Task { @MainActor in
                    switch step {
                    case let .downloading(percent, _, _): geoipStatus = String(format: "下载中 %d%%".t, percent)
                    case .validating: geoipStatus = "校验中…".t
                    }
                }
            }
        } catch {
            message = error.localizedDescription
        }
    }

    private func updateCore() async {
        coreBusy = true
        defer { coreBusy = false; coreStatus = "更新内核".t }
        let wasRunning = state.controller.isCoreRunning
        // ★ 查询/下载阶段**不停核心**（用户的网还靠它跑着；下载本身不走代理，
        //   只有「国内加速」镜像开关）。拿到产物后才停核 → 替换 → 重启。
        let downloader = CoreDownloader(useMirror: state.config.mirror,
                                        includePrerelease: state.config.receiveBeta)
        do {
            let downloaded = try await downloader.fetchAndExtract { step in
                Task { @MainActor in
                    switch step {
                    case .checking: coreStatus = "检查中…".t
                    case let .downloading(percent, _, _): coreStatus = String(format: "下载中 %d%%".t, percent)
                    case .installing: coreStatus = "安装中…".t
                    case .done, .failed: coreStatus = "更新内核".t
                    }
                }
            }
            coreStatus = "安装中…".t
            if wasRunning { await state.controller.stopCore() }
            let installResult = Result { try CoreDownloader.finishInstall(downloaded) }
            // 核心是停了才装的，所以**装失败也要拉回来** —— 别让一次更新失败断了网。
            if wasRunning { await state.controller.startCore() }
            try installResult.get()
            message = String(format: "内核已更新到 %@".t, downloaded.version)
            // 换内核有**两个**入口（这里和更新窗的内核页），两处都得重判侧栏那颗
            // "core" 角标 —— 少一处，从那个入口更新完角标就一直红着。
            await state.refreshCoreUpdateBadge()
        } catch {
            message = String(format: "内核更新失败：%@".t, "\(error)")
        }
    }

}

/// 规则编辑草稿。`Identifiable` 是为了直接喂给 `.sheet(item:)` —— 用
/// `isPresented` + 另一个 `@State` 的话，两者更新有一拍时差，会闪一下旧内容。
struct RuleDraft: Identifiable {
    let id = UUID()
    let index: Int?
    var rule: RulesStore.Rule
}

struct AreaDraft: Identifiable {
    let id = UUID()
    let index: Int?
    var area: RulesStore.Area
}
