import CoastKit
import SwiftUI

/// 设置页。对齐 `qml/SettingsPage.qml` 的分组与项目。
///
/// 编辑的是**本地草稿**，点「应用」才落盘并重建配置 —— 端口这类改一半的中间值直接生效会把
/// 核心打断（例如用户正在把 7890 改成 1080，输到 "108" 时就已经重启了一次核心）。
/// 开关类没有这个问题，但统一走同一条路，界面语义才一致。
struct SettingsPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    @State private var draft = AppConfig()
    @State private var message = ""
    @State private var coreStatus = "更新内核".t
    @State private var coreBusy = false
    @State private var helperStatus = ""
    /// `COAST_OPEN_RULES=1` 时直接弹出规则编辑器 —— 同 COAST_INITIAL_PAGE，
    /// 是为了让「这个弹窗渲染对不对」可复现地验，而不是每次手点进去。
    @State private var showingRules =
        ProcessInfo.processInfo.environment["COAST_OPEN_RULES"] == "1"

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                section("网络".t) {
                    labeledField("REST 端口".t, value: Binding(
                        get: { String(draft.uiPort) },
                        set: { draft.uiPort = Int($0) ?? draft.uiPort }
                    ))
                    labeledField("混合端口".t, value: Binding(
                        get: { String(draft.mixedPort) },
                        set: { draft.mixedPort = Int($0) ?? draft.mixedPort }
                    ))
                    Text("改端口需要重启核心才能重新监听（热重载改不了）".t)
                        .font(.system(size: 10)).foregroundStyle(theme.textMuted)
                }

                section("行为".t) {
                    Toggle("只显示可用节点".t, isOn: $draft.nodeOnlyAvailable)
                    Toggle("切换节点时清理连接".t, isOn: $draft.clearConnections)
                    Toggle("切换节点时弹出通知".t, isOn: $draft.nodeSwitchNote)
                    Toggle("订阅增量更新（保留本次没出现的旧节点）".t, isOn: $draft.increment)
                    Toggle("启动时静默到托盘".t, isOn: $draft.closeToTray)
                    Toggle("开机自启".t, isOn: Binding(
                        get: { draft.autoStart },
                        set: { newValue in
                            draft.autoStart = newValue
                            // 开机自启是系统状态，不能等「应用」—— 立刻同步，并把系统的真实结果读回来。
                            if case .failure(let error) = LaunchAtLogin.setEnabled(newValue) {
                                message = "设置开机自启失败：\(error.localizedDescription)"
                            }
                            draft.autoStart = LaunchAtLogin.isEnabled
                        }
                    ))
                    if LaunchAtLogin.requiresApproval {
                        // 这个状态下再调 register() 不生效，只能引导用户自己去开
                        HStack(spacing: 6) {
                            Text("开机自启被系统拦下，需在「登录项」中允许".t)
                                .font(.system(size: 10)).foregroundStyle(theme.danger)
                            Button("打开设置".t) { LaunchAtLogin.openLoginItemsSettings() }
                                .controlSize(.small)
                        }
                    }
                }

                section("规则".t) {
                    HStack(spacing: 8) {
                        Button("编辑自定义规则与分组".t) { showingRules = true }
                        Spacer()
                    }
                    Text("规则前插到核心规则表顶部；分组按正则匹配节点名自动生成".t)
                        .font(.system(size: 10)).foregroundStyle(theme.textMuted)
                }

                section("节点过滤".t) {
                    Toggle("只保留匹配的节点".t, isOn: $draft.allowRuleEnabled)
                    TextField("允许规则（正则）".t, text: $draft.allowRule)
                        .disabled(!draft.allowRuleEnabled)
                    Toggle("排除匹配的节点".t, isOn: $draft.noAllowRuleEnabled)
                    TextField("排除规则（正则）".t, text: $draft.noAllowRule)
                        .disabled(!draft.noAllowRuleEnabled)
                    Text("正则写错时按「没设」处理，不会让节点全部消失".t)
                        .font(.system(size: 10)).foregroundStyle(theme.textMuted)
                }

                section("外观".t) {
                    Toggle("跟随系统语言".t, isOn: $draft.autoLanguage)
                    Picker("界面语言".t, selection: $draft.language) {
                        ForEach(I18n.availableLanguages, id: \.self) { code in
                            Text(I18n.displayName(code)).tag(code)
                        }
                    }
                    .disabled(draft.autoLanguage)
                    .frame(maxWidth: 260)

                    Toggle("跟随系统深浅色".t, isOn: $draft.autoTheme)
                    Toggle("浅色主题".t, isOn: Binding(
                        get: { draft.theme.lowercased() == "light" },
                        set: { draft.theme = $0 ? "light" : "black"; theme.dark = !$0 }
                    ))
                    .disabled(draft.autoTheme)
                }

                section("更新".t) {
                    Toggle("接收测试版".t, isOn: $draft.receiveBeta)
                    Toggle("下载走国内镜像（ghfast.top）".t, isOn: $draft.mirror)
                    HStack(spacing: 8) {
                        Button(coreStatus) { Task { await updateCore() } }
                            .disabled(coreBusy)
                        if !state.controller.isCoreInstalled {
                            Text("内核尚未安装，需先下载".t)
                                .font(.system(size: 10)).foregroundStyle(theme.danger)
                        }
                    }
                }

                section("系统".t) {
                    HStack(spacing: 8) {
                        Text("免密助手：\(helperText)")
                            .font(.system(size: 12)).foregroundStyle(theme.textSecondary)
                        Spacer()
                        Button("安装".t) { Task { helperStatus = await state.controller.installHelper() } }
                        Button("卸载".t) { Task { helperStatus = await state.controller.uninstallHelper() } }
                    }
                    Text("TUN（增强模式）依赖它以 root 启动核心；没有它，增强开着也不会生效".t)
                        .font(.system(size: 10)).foregroundStyle(theme.textMuted)
                    if !helperStatus.isEmpty {
                        Text(helperStatus).font(.system(size: 10)).foregroundStyle(theme.textMuted)
                    }

                    Button("系统代理机制自检".t) {
                        let result = SystemProxy.selfTest(host: draft.host, port: draft.mixedPort)
                        message = result.message
                    }
                    Text("读当前值 → 设测试值 → 读回核对 → 还原。几秒钟确认这台机器上机制通不通".t)
                        .font(.system(size: 10)).foregroundStyle(theme.textMuted)
                }

                HStack(spacing: 8) {
                    Button("应用".t) { Task { await applySettings() } }
                        .keyboardShortcut(.defaultAction)
                    Button("放弃改动".t) { draft = state.config }
                    if !message.isEmpty {
                        Text(message).font(.system(size: 11)).foregroundStyle(theme.textMuted)
                    }
                }
                .padding(.top, 4)
            }
            .padding(14)
        }
        .textFieldStyle(.roundedBorder)
        .toggleStyle(.switch)
        .sheet(isPresented: $showingRules) {
            RulesEditor().environment(state).environment(theme)
        }
        .task {
            draft = state.config
            draft.autoStart = LaunchAtLogin.isEnabled   // 系统状态为准，不信配置文件里那份
        }
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

    // MARK: 布局

    private func section<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(theme.textMuted)
            content()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
    }

    private func labeledField(_ label: String, value: Binding<String>) -> some View {
        HStack(spacing: 8) {
            Text(label).font(.system(size: 12)).foregroundStyle(theme.textSecondary)
                .frame(width: 80, alignment: .leading)
            TextField("", text: value).frame(width: 120)
        }
    }

    // MARK: 动作

    private func applySettings() async {
        let old = state.config
        AppConfigLoader.persist(key: "ui", int: draft.uiPort)
        AppConfigLoader.persist(key: "port", int: draft.mixedPort)
        AppConfigLoader.persist(key: "node", bool: draft.nodeOnlyAvailable)
        AppConfigLoader.persist(key: "clearConnections", bool: draft.clearConnections)
        AppConfigLoader.persist(key: "note", bool: draft.nodeSwitchNote)
        AppConfigLoader.persist(key: "increment", bool: draft.increment)
        AppConfigLoader.persist(key: "mini", bool: draft.closeToTray)
        AppConfigLoader.persist(key: "sys", bool: draft.autoStart)
        AppConfigLoader.persist(key: "autoTheme", bool: draft.autoTheme)
        AppConfigLoader.persist(key: "theme", raw: draft.theme)
        AppConfigLoader.persist(key: "beta", bool: draft.receiveBeta)
        AppConfigLoader.persist(key: "mirror", bool: draft.mirror)
        AppConfigLoader.persist(key: "autoLanguage", bool: draft.autoLanguage)
        AppConfigLoader.persist(key: "language", raw: draft.language)
        I18n.shared.applyConfig(draft)

        state.applyConfig(draft)
        state.clash.setClearConnectionsOnSwitch(draft.clearConnections)
        state.clash.setMixedPort(draft.mixedPort)

        // 端口变更**必须重启核心** —— 热重载改不了监听端口，核心会继续 bind 在旧端口上，
        // 而 UI 已经按新端口去连了，表现为「应用之后一切都断了」。
        if draft.uiPort != old.uiPort || draft.mixedPort != old.mixedPort {
            message = "端口已变更，正在重启核心…".t
            await state.controller.stopCore()
            await state.controller.startCore()
            state.clash.setEndpoint(host: draft.host, port: draft.uiPort)
            message = "已应用（核心已重启）".t
        } else {
            await state.controller.rebuildConfig()
            message = "已应用".t
        }
    }

    private func updateCore() async {
        coreBusy = true
        defer { coreBusy = false }
        let wasRunning = state.controller.isCoreRunning
        // 正在跑的核心文件替换不掉，先停。
        if wasRunning { await state.controller.stopCore() }

        let downloader = CoreDownloader(useMirror: draft.mirror)
        do {
            let tag = try await downloader.install { progress in
                Task { @MainActor in
                    switch progress {
                    case .checking: coreStatus = "检查中…".t
                    case .downloading(let percent): coreStatus = "下载中 \(percent)%"
                    case .installing: coreStatus = "安装中…".t
                    case .done: coreStatus = "更新内核".t
                    case .failed: coreStatus = "更新内核".t
                    }
                }
            }
            message = "内核已更新到 \(tag)"
        } catch {
            message = "内核更新失败：\(error)"
        }
        coreStatus = "更新内核".t
        if wasRunning { await state.controller.startCore() }
    }
}
