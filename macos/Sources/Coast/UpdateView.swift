import AppKit
import CoastKit
import SwiftUI

/// 更新窗。**逐元素对齐** `qml/UpdateWindow.qml`：左侧三颗竖排 tab（程序 / 内核 / GeoIP）+
/// 右侧内容卡（版本 + 更新说明）+ 下载进度条（带 ✕ 取消）+ 底部动作行
/// （国内代理下载勾选 / 关闭 / 更新）。
///
/// Qt 那边是独立顶层窗（600×560）；这里沿用本项目既有做法以 sheet 呈现（`ConnectionsView`
/// 与 `RulesEditor` 同样如此），尺寸取 Qt 的 600×560。
///
/// 三页都是**真的**在下载安装。「程序」页走 `AppUpdater`：下载 dmg/zip → 校验 sha256 →
/// 放出替换脚本 → 退出进程，脚本等本进程死透后覆盖 .app 并重启（Qt 的
/// `launchSilentUpdateAndRestartMac` 同一套）。开发期直跑（不是 .app）没有可替换的目标，
/// 那条路回退成打开发布页。
struct UpdateView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    enum Tab: Int, CaseIterable, Identifiable {
        case app, core, geoip
        var id: Int { rawValue }

        @MainActor var title: String {
            switch self {
            case .app: return "程序".t
            case .core: return "内核".t
            case .geoip: return "GeoIP"   // 专有名词，不翻译（Qt 那边同样是字面量）
            }
        }

        /// Qt 用 Remix Icon 的私用区码点，这里换成同尺寸的 SF Symbols。
        var symbol: String {
            switch self {
            case .app: return "app.badge"
            case .core: return "cpu"
            case .geoip: return "globe.asia.australia"
            }
        }
    }

    @State private var tab: Tab = .app
    @State private var coreLocalVersion: String?
    @State private var coreLatest: (tag: String, notes: String)?
    /// 下载进度 0…100。nil = 没在下载（对应 QML 的 `updater.downloading == false`）。
    @State private var progress: Int?
    @State private var status = ""
    @State private var busy = false
    /// 正在跑的下载任务 —— ✕ 取消就是取消它。
    @State private var work: Task<Void, Never>?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Coast 更新".t)
                .lineLimit(1)
                .font(.system(size: 16))
                .foregroundStyle(theme.textPrimary)

            HStack(alignment: .top, spacing: 8) {
                VStack(spacing: 6) {
                    ForEach(Tab.allCases) { item in
                        tabButton(item)
                    }
                    Spacer(minLength: 0)
                }

                contentCard
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            if let progress {
                HStack(spacing: 8) {
                    progressBar(progress)
                    cancelButton
                }
            }

            if !status.isEmpty {
                Text(status)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }

            actionRow
        }
        .padding(10)
        // 默认 600×560 由 scene 的 `.defaultSize` 给；这里只写**下限**（Qt 是 460×420）。
        // 写死 `width/height` 的话窗口就成了不可缩放的死尺寸 —— 而 Qt 那边它是能拖小的。
        .frame(minWidth: 460, minHeight: 420)
        .background(theme.card)
        .task { await load() }
    }

    // MARK: 左侧 tab

    /// 78×46、半径 5，选中态品牌色底 + 白字，未选中是一块比卡更深/更浅的底。
    private func tabButton(_ item: Tab) -> some View {
        let selected = tab == item
        return Button { tab = item } label: {
            HStack(spacing: 6) {
                Image(systemName: item.symbol)
                    .font(.system(size: 16))
                Text(item.title)
                    .font(.system(size: 13))
            }
            .foregroundStyle(selected ? .white : theme.textSecondary)
            .frame(width: 78, height: 46)
            .background {
                RoundedRectangle(cornerRadius: 5, style: .continuous)
                    .fill(selected ? theme.accent : (theme.dark ? Color(hex: 0x25_25_25) : Color(hex: 0xEE_EE_EE)))
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .pointingHandCursor()
    }

    // MARK: 右侧内容卡

    private var contentCard: some View {
        VStack(alignment: .leading, spacing: 6) {
            switch tab {
            case .app:
                Text("当前版本: ".t + AppInfo.version)
                    .lineLimit(1)
                    .font(.system(size: 13))
                    .foregroundStyle(theme.textSecondary)
                Text(state.appRelease?.tag ?? "")
                    .font(.system(size: 15))
                    .foregroundStyle(theme.textPrimary)
                notesLabel("更新说明".t)
                notesCard(state.appRelease?.notes ?? "")

            case .core:
                Text(coreLocalVersion.map { "mihomo " + $0 } ?? "内核版本: 检测中...".t)
                    .font(.system(size: 15))
                    .foregroundStyle(theme.textPrimary)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                notesLabel("更新说明".t)
                notesCard(coreLatest?.notes ?? "")

            case .geoip:
                Text("GeoIP 数据库".t)
                    .lineLimit(1)
                    .font(.system(size: 15))
                    .foregroundStyle(theme.textPrimary)
                notesLabel("说明".t)
                notesCard("Country.mmdb 是「按 IP 归属地区分流」所用的地理数据库（来源 MetaCubeX/meta-rules-dat）。\n\n点击「更新」下载最新数据库到用户目录与资源目录，重启核心后生效。".t)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background {
            RoundedRectangle(cornerRadius: 5, style: .continuous)
                .fill(theme.dark ? Color(hex: 0x16_16_16) : Color(hex: 0xF6_F6_F6))
        }
        .overlay {
            RoundedRectangle(cornerRadius: 5, style: .continuous)
                .stroke(theme.divider, lineWidth: 1)
        }
    }

    private func notesLabel(_ text: String) -> some View {
        Text(text).font(.system(size: 12)).foregroundStyle(theme.textMuted)
    }

    /// 「更新说明」正文卡：只读、可滚动。半径 4、内距 8、12px 正文、行距 1.3。
    private func notesCard(_ text: String) -> some View {
        ScrollView {
            Text(text)
                .font(.system(size: 12))
                .lineSpacing(12 * 0.3)
                .foregroundStyle(theme.textSecondary)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(8)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background {
            RoundedRectangle(cornerRadius: 4, style: .continuous)
                .fill(theme.dark ? Color(hex: 0x0D_0D_0D) : Color(hex: 0xFF_FF_FF))
        }
        .overlay {
            RoundedRectangle(cornerRadius: 4, style: .continuous)
                .stroke(theme.divider, lineWidth: 1)
        }
    }

    // MARK: 进度

    /// 高 22、半径 4、1px 描边，进度条本体内缩 1，百分比压在正中。
    private func progressBar(_ percent: Int) -> some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .fill(theme.dark ? Color(hex: 0x0D_0D_0D) : Color(hex: 0xEE_EE_EE))
                RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .fill(theme.accent)
                    .frame(width: max(0, (geo.size.width - 2) * Double(percent) / 100))
                    .padding(1)
                Text("\(percent)%")
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textPrimary)
                    .frame(maxWidth: .infinity)
            }
            .overlay {
                RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .stroke(theme.divider, lineWidth: 1)
            }
        }
        .frame(height: 22)
    }

    /// ✕ 取消：24×24 的圆，悬停转红底白字。
    private var cancelButton: some View {
        CancelDot { work?.cancel() }
    }

    // MARK: 底部动作行

    private var actionRow: some View {
        HStack(spacing: 10) {
            // 国内加速：与设置页那一项**同一个 config 键**，两处必须同步。
            HStack(spacing: 5) {
                MirrorCheckbox(isOn: state.config.mirror) { toggleMirror() }
                Text("国内代理下载".t)
                    .lineLimit(1)
                    .font(.system(size: 12))
                    .foregroundStyle(theme.textSecondary)
                    .onTapGesture { toggleMirror() }
            }

            Spacer(minLength: 0)

            SecondaryButton(title: "关闭".t, width: 80) { dismiss() }

            PrimaryButton(title: busy ? "更新中…".t : "更新".t, width: 100, disabled: busy) {
                startUpdate()
            }
        }
    }

    // MARK: 动作

    private func toggleMirror() {
        var next = state.config
        next.mirror.toggle()
        AppConfigLoader.persist(key: "mirror", bool: next.mirror)
        state.applyConfig(next)
    }

    private func load() async {
        coreLocalVersion = CoreVersion.local()
        let checker = UpdateChecker(includePrerelease: state.config.receiveBeta,
                                    proxyPort: state.controller.isCoreRunning ? state.config.mixedPort : nil)
        coreLatest = try? await checker.latestCoreRelease()
        if state.appRelease == nil { await state.checkUpdates() }
    }

    private func startUpdate() {
        guard !busy else { return }
        switch tab {
        case .app:
            work = Task { await runAppUpdate() }
        case .core:
            work = Task { await runCoreUpdate() }
        case .geoip:
            work = Task { await runGeoIPUpdate() }
        }
    }

    private func runAppUpdate() async {
        guard let release = state.appRelease else {
            // 打开窗就查过一轮（`load()`），还拿不到就是检查失败（限流/断网），
            // 让用户先看到检查为什么失败，而不是在这儿闷头再试一次下载。
            status = "尚未取得发布信息，请稍后重试".t
            return
        }
        busy = true
        defer { busy = false; progress = nil }
        let updater = AppUpdater(useMirror: state.config.mirror,
                                 proxyPort: state.controller.isCoreRunning ? state.config.mixedPort : nil)
        do {
            let staged = try await updater.stage(release: release) { step in
                Task { @MainActor in
                    switch step {
                    case .downloading(let percent): progress = percent
                    case .verifying: progress = nil; status = "校验中…".t
                    case .installing: progress = nil; status = "准备安装…".t
                    }
                }
            }
            // 替换脚本已经在外面等着了，本进程必须退出它才动手。
            // terminate 走 AppDelegate 的清理路径（停核心、还原系统代理）——
            // 脚本最多等 60 秒，比清理所需的时间富余得多。
            status = String(format: "已就绪（%@），正在退出安装…".t, staged.assetName)
            try? await Task.sleep(for: .milliseconds(300))   // 让状态那行来得及画出来
            // ★ 必须走「真退」入口：⌘Q 那道守门把普通 terminate 当「收窗口」拦下 ——
            //   拦下的话替换脚本等不到进程退出，更新永远装不上。
            AppDelegate.shared?.terminateForReal() ?? NSApplication.shared.terminate(nil)
        } catch AppUpdater.UpdateError.notInAppBundle {
            // 开发期直跑：没有可替换的 .app，退回打开发布页手动装。
            state.openReleasePage()
            status = "当前不是从 .app 运行，已打开发布页请手动安装".t
        } catch {
            status = String(format: "程序更新失败：%@".t, error.localizedDescription)
        }
    }

    private func runCoreUpdate() async {
        busy = true
        defer { busy = false; progress = nil }
        // ★ 查询/下载阶段**不停核心**（用户的网还靠它跑着；下载本身不走代理，
        //   只有「国内加速」镜像开关）。拿到产物后才停核 → 替换 → 重启。
        let wasRunning = state.controller.isCoreRunning
        let downloader = CoreDownloader(useMirror: state.config.mirror,
                                        includePrerelease: state.config.receiveBeta)
        do {
            let downloaded = try await downloader.fetchAndExtract { step in
                Task { @MainActor in apply(step) }
            }
            apply(.installing)
            if wasRunning { await state.controller.stopCore() }
            let installResult = Result { try CoreDownloader.finishInstall(downloaded) }
            // 停了才装的，装失败也要拉回来 —— 别让一次更新失败断了网。
            if wasRunning { await state.controller.startCore() }
            try installResult.get()
            status = String(format: "内核已更新到 %@".t, downloaded.version)
            coreLocalVersion = CoreVersion.local()
        } catch {
            status = String(format: "内核更新失败：%@".t, "\(error)")
        }
    }

    private func runGeoIPUpdate() async {
        busy = true
        defer { busy = false; progress = nil }
        let updater = GeoIPUpdater(useMirror: state.config.mirror,
                                   proxyPort: state.controller.isCoreRunning ? state.config.mixedPort : nil)
        do {
            status = try await updater.update { step in
                Task { @MainActor in
                    switch step {
                    case .downloading(let percent): progress = percent
                    case .validating: progress = nil; status = "校验中…".t
                    }
                }
            }
        } catch {
            status = error.localizedDescription
        }
    }

    @MainActor
    private func apply(_ step: CoreDownloader.Progress) {
        switch step {
        case .checking: progress = nil; status = "检查中…".t
        case .downloading(let percent): progress = percent
        case .installing: progress = nil; status = "安装中…".t
        case .done, .failed: progress = nil
        }
    }
}

/// ✕ 取消按钮。24×24 的圆，悬停转红底白字 —— 对齐 QML 的 `cancelHover` 三处联动。
private struct CancelDot: View {
    @Environment(Theme.self) private var theme
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Text("✕")
                .font(.system(size: 12))
                .foregroundStyle(hovering ? .white : theme.textSecondary)
                .frame(width: 24, height: 24)
                .background {
                    Circle().fill(hovering ? Color(hex: 0xF5_6C_6C)
                                  : (theme.dark ? Color(hex: 0x25_25_25) : Color(hex: 0xEE_EE_EE)))
                }
                .overlay {
                    Circle().stroke(hovering ? Color(hex: 0xF5_6C_6C) : theme.divider, lineWidth: 1)
                }
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .pointingHandCursor()
    }
}

/// 16×16、半径 3 的方形勾选框。QML 手画的那个，不是系统 checkbox ——
/// 系统 checkbox 在这一排里比周围元素高一截，且底色跟不上主题。
private struct MirrorCheckbox: View {
    @Environment(Theme.self) private var theme
    let isOn: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .fill(isOn ? theme.accent : .clear)
                .frame(width: 16, height: 16)
                .overlay {
                    RoundedRectangle(cornerRadius: 3, style: .continuous)
                        .stroke(isOn ? theme.accent : theme.divider, lineWidth: 1)
                }
                .overlay {
                    if isOn {
                        Text("✓").font(.system(size: 11)).foregroundStyle(.white)
                    }
                }
        }
        .buttonStyle(.plain)
        .pointingHandCursor()
    }
}

/// 底部次要按钮（关闭）：80×30、半径 5、1px 描边。
private struct SecondaryButton: View {
    @Environment(Theme.self) private var theme
    let title: String
    let width: CGFloat
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 13))
                .foregroundStyle(theme.textSecondary)
                .frame(width: width, height: 30)
                .background {
                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                        .fill(hovering ? theme.hover
                              : (theme.dark ? Color(hex: 0x25_25_25) : Color(hex: 0xEE_EE_EE)))
                }
                .overlay {
                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                        .stroke(theme.divider, lineWidth: 1)
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .pointingHandCursor()
    }
}

/// 底部主要按钮（更新）：100×30、半径 5、品牌色底白字，悬停加深；忙时半透明且不可点。
private struct PrimaryButton: View {
    @Environment(Theme.self) private var theme
    let title: String
    let width: CGFloat
    let disabled: Bool
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 13))
                .foregroundStyle(.white)
                .frame(width: width, height: 30)
                .background {
                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                        .fill(hovering && !disabled ? theme.accentStrong : theme.accent)
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(disabled)
        .opacity(disabled ? 0.5 : 1)
        .onHover { hovering = $0 }
    }
}
