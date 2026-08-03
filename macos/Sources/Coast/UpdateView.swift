import AppKit
import CoastKit
import SwiftUI

/// 更新窗。
///
/// 版式（macOS 26 的液态玻璃）：
///   · 系统标题栏抬到 50（空 toolbar + `unifiesTitleBar`，红绿灯由系统在带子里垂直居中）；
///   · **导航栏也是 50**，钉在标题栏那条带子里（`safeAreaBar(edge: .top)`）：
///     左内距 20 是「当前版本 → 最新版本」，右侧是**一颗胶囊里的三段玻璃按钮组**
///     （程序 / 内核 / GeoIP）；
///   · 中间只放更新内容（滚动，从导航栏底下穿过，由系统 scroll edge effect 渐隐）；
///   · 底栏是一条状态机：默认「获取更新」→ 查出有新版就换成「更新」→ 点了之后换成
///     「进度条 + 关闭」，速度与「下载量/总量」在下面**居中**。
///
/// ★ 这个窗必须 `isMovableByWindowBackground`：导航栏左右都被内容占满了，而带子是
///   macOS 上唯一的拖动区 —— 附属窗又没有主窗那个自动拖动（见 `WindowRestore`），
///   不开这一位窗口就**拖不动**（连接窗当初正是踩了这个，才把它的顶栏整条靠右排）。
///
/// 「国内加速」不在这一页了 —— 它是**设置页里的同一个 config 键**（`mirror`），
/// 两处各放一份只会让人以为是两个开关。
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
    }

    /// 带子/导航栏的高度。红绿灯由系统在这条带子里垂直居中，导航栏给同样的定高、
    /// 内容也居中，两边就在一条水平线上（与连接窗同值）。
    static let bandHeight: CGFloat = 50

    @State private var tab: Tab = .app
    @State private var coreLocalVersion: String?
    @State private var coreLatest: (tag: String, notes: String)?

    /// 已经查过一轮的页签。GeoIP 没有版本号可比，查过就当「可以更新」——
    /// 上游那份库天天在重建，本地也没有任何可比对的版本信息，说「已是最新」才是撒谎。
    @State private var checked: Set<Tab> = []
    @State private var checking = false

    /// 下载进度 0…100。nil = 没在下载。
    @State private var progress: Int?
    @State private var received: Int64 = 0
    @State private var total: Int64 = 0
    /// 实时速度（字节/秒），由相邻两次进度回调的字节差/时间差算出。
    @State private var speed: Int64 = 0
    @State private var lastSample: (bytes: Int64, at: Date)?
    @State private var status = ""
    @State private var busy = false
    /// 正在跑的下载任务 —— 「关闭」就是取消它。
    @State private var work: Task<Void, Never>?

    var body: some View {
        notes
            .safeAreaBar(edge: .top, spacing: 0) { navBar }
            .safeAreaBar(edge: .bottom, spacing: 0) { bottomBar }
            .scrollEdgeEffectStyle(.soft, for: .all)
            .frame(minWidth: 460, minHeight: 420)
            .windowGlass(.sidebar, movableByBackground: true)
            .task { await load() }
    }

    // MARK: 导航栏（50，钉在标题栏那条 50 的下面）

    /// ★ 导航栏是**标题栏之下单独的一条**，不是塞进标题栏那条带子里。两个理由：
    ///   ① 塞进去的话左边 20 正好是红绿灯的位置，版本号会**压在三颗灯上**（第一版就是这样）；
    ///   ② 标题栏那条是 macOS 上的拖动区，占满了窗口就不好拖（连接窗当初正是为此把顶栏靠右排）。
    ///   于是让系统标题栏（空 toolbar 抬到 50）继续当安全区，这一条 50 紧跟其下 ——
    ///   **不要**再给内容加 `ignoresSafeArea(.top)`：那会把标题栏那 50 的安全区也一起抹掉，
    ///   正文就从 y=0 画起、连红绿灯和版本号都被盖住（试过，截图里正文直接压在版本号上）。
    private var navBar: some View {
        HStack(spacing: 12) {
            versionLine
            Spacer(minLength: 12)
            tabPicker
        }
        .padding(.horizontal, 20)
        .frame(height: Self.bandHeight)
    }

    /// 左侧：当前版本 → 最新版本。查不到的那一头显示「—」，不编。
    private var versionLine: some View {
        HStack(spacing: 8) {
            Text(localVersion)
                .font(.system(size: 13))
                .foregroundStyle(theme.textPrimary)
            Text("→")
                .font(.system(size: 12))
                .foregroundStyle(theme.textMuted)
            Text(remoteVersion)
                .font(.system(size: 13))
                .foregroundStyle(hasUpdate ? theme.accent : theme.textSecondary)
        }
        .lineLimit(1)
    }

    /// 右侧：一颗胶囊里的三段。选中段的底压在整组玻璃**里面**，
    /// 不会给整组带来额外圆角（与页脚模式组同一画法）。
    private var tabPicker: some View {
        HStack(spacing: 0) {
            ForEach(Tab.allCases) { item in
                Button { tab = item } label: {
                    Text(item.title)
                        .font(.system(size: 12))
                        .foregroundStyle(tab == item ? .white : theme.textSecondary)
                        .frame(width: 56, height: 28)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .background {
                    if tab == item { Capsule().fill(theme.accent.opacity(0.8)) }
                }
            }
        }
        .glassCapsule()
    }

    // MARK: 内容：更新说明

    private var notes: some View {
        ScrollView {
            Text(notesText.isEmpty ? "暂无更新说明".t : notesText)
                .font(.system(size: 12))
                .lineSpacing(12 * 0.35)
                .foregroundStyle(theme.textSecondary)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 20)
                .padding(.vertical, 12)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: 底栏

    private var bottomBar: some View {
        VStack(spacing: 6) {
            if let progress {
                HStack(spacing: 10) {
                    progressBar(progress)
                    // 「关闭」= 取消这次下载并关窗。不取消的话下载会在没有界面的情况下
                    // 继续跑完，程序更新那条更糟 —— 它跑完是要**退出进程去装**的。
                    Button("关闭".t) { cancelAndClose() }
                        .glassButton()
                }
                Text(transferLine)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .lineLimit(1)
                    .frame(maxWidth: .infinity)          // 居中
            } else if hasUpdate {
                Button(busy ? "更新中…".t : "更新".t) { startUpdate() }
                    .glassButton(prominent: true)
                    .disabled(busy)
            } else {
                Button(checking ? "检查中…".t : "获取更新".t) { Task { await check() } }
                    .glassButton(prominent: true)
                    .disabled(checking || busy)
            }

            if !status.isEmpty {
                Text(status)
                    .font(.system(size: 11))
                    .foregroundStyle(theme.textMuted)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity)          // 居中
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity)
    }

    /// ★ 底栏这几颗按钮走**系统的玻璃按钮样式**，不用本文件原来那两个手绘按钮。
    ///   不是为了统一观感（虽然也更贴这个窗的语言）—— 手绘按钮放进 `safeAreaBar` 里，
    ///   标签文字会被**旋转 180°** 画出来（截图里「Close」是倒着的，稳定复现；换成
    ///   `.glassButton()` 立刻正常）。系统按钮样式不受影响，`safeAreaBar` 本来也是给它们用的。

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

    /// 「12.3 MB/s (5.2 MB / 40.1 MB)」。一个字节都还没来时是「0 B/s (0 B / 0 B)」——
    /// 留空的话用户分不清「还没开始」和「界面坏了」。
    private var transferLine: String {
        "\(Formatting.rate(speed)) (\(Formatting.bytes(received)) / \(Formatting.bytes(total)))"
    }

    // MARK: 版本 / 更新说明 的取值

    private var localVersion: String {
        switch tab {
        case .app: return String(format: "当前 %@".t, AppInfo.version)
        case .core: return String(format: "当前 %@".t, coreLocalVersion ?? "—")
        case .geoip: return String(format: "当前 %@".t, localMmdbDate ?? "—")
        }
    }

    /// 本地 Country.mmdb 的日期。GeoIP 没有版本号，「你手上这份是哪天的」就是全部信息。
    private var localMmdbDate: String? {
        let url = AppPaths.userDir.appendingPathComponent("Country.mmdb")
        guard let date = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
        else { return nil }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.dateFormat = "yyyy-MM-dd"
        return formatter.string(from: date)
    }

    private var remoteVersion: String {
        switch tab {
        case .app: return String(format: "最新 %@".t, state.appRelease?.tag ?? "—")
        case .core: return String(format: "最新 %@".t, coreLatest?.tag ?? "—")
        // GeoIP 上游没有版本号（同一个 tag 天天重建），只能说「最新」。
        case .geoip: return String(format: "最新 %@".t, checked.contains(.geoip) ? "latest" : "—")
        }
    }

    /// 这个页签有没有可装的更新。
    private var hasUpdate: Bool {
        switch tab {
        case .app: return state.appUpdateAvailable
        case .core:
            guard let local = coreLocalVersion, let remote = coreLatest?.tag else { return false }
            return UpdateChecker.coreHasUpdate(remote: remote, local: local)
        case .geoip: return checked.contains(.geoip)
        }
    }

    private var notesText: String {
        switch tab {
        case .app: return state.appRelease?.notes ?? ""
        case .core: return coreLatest?.notes ?? ""
        case .geoip:
            return "Country.mmdb 是「按 IP 归属地区分流」所用的地理数据库（来源 MetaCubeX/meta-rules-dat）。\n\n点击「更新」下载最新数据库到用户目录与资源目录，重启核心后生效。".t
        }
    }

    // MARK: 动作

    private func load() async {
        coreLocalVersion = CoreVersion.local()
        let checker = UpdateChecker(includePrerelease: state.config.receiveBeta,
                                    proxyPort: state.controller.isCoreRunning ? state.config.mixedPort : nil)
        coreLatest = try? await checker.latestCoreRelease()
    }

    /// 「获取更新」：只查这一个页签，查完由 `hasUpdate` 决定底栏换不换成「更新」。
    private func check() async {
        guard !checking else { return }
        checking = true
        status = ""
        defer { checking = false }
        switch tab {
        case .app:
            await state.checkUpdates()
            if !state.appUpdateAvailable {
                status = state.updateHint.isEmpty ? "已是最新版本".t : state.updateHint
            }
        case .core:
            let checker = UpdateChecker(includePrerelease: state.config.receiveBeta,
                                        proxyPort: state.controller.isCoreRunning ? state.config.mixedPort : nil)
            coreLocalVersion = CoreVersion.local()
            coreLatest = try? await checker.latestCoreRelease()
            if coreLatest == nil {
                status = "检查失败：".t + "GitHub"
            } else if !hasUpdate {
                status = "已是最新版本".t
            }
        case .geoip:
            // 没有可比的版本，查一下能不能连上就算「可以更新」。
            checked.insert(.geoip)
        }
    }

    /// 下载进度回调的落点：百分比、字节数、以及由相邻两拍算出的速度。
    @MainActor
    private func note(percent: Int, received newReceived: Int64, total newTotal: Int64) {
        let now = Date()
        if let last = lastSample {
            let seconds = now.timeIntervalSince(last.at)
            // 太密的两拍算出来的速度会剧烈跳动，隔 0.3 秒以上才更新一次。
            if seconds >= 0.3 {
                speed = Int64(max(0, Double(newReceived - last.bytes) / seconds))
                lastSample = (newReceived, now)
            }
        } else {
            lastSample = (newReceived, now)
        }
        progress = percent
        received = newReceived
        total = newTotal
    }

    private func resetTransfer() {
        progress = nil
        received = 0
        total = 0
        speed = 0
        lastSample = nil
    }

    private func cancelAndClose() {
        work?.cancel()
        work = nil
        resetTransfer()
        dismiss()
    }

    private func startUpdate() {
        guard !busy else { return }
        status = ""
        resetTransfer()
        progress = 0            // 立刻进入「进度条 + 关闭」那一态，别让按钮停在原地
        switch tab {
        case .app: work = Task { await runAppUpdate() }
        case .core: work = Task { await runCoreUpdate() }
        case .geoip: work = Task { await runGeoIPUpdate() }
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
        defer { busy = false; resetTransfer() }
        let updater = AppUpdater(useMirror: state.config.mirror,
                                 proxyPort: state.controller.isCoreRunning ? state.config.mixedPort : nil)
        do {
            let staged = try await updater.stage(release: release) { step in
                Task { @MainActor in
                    switch step {
                    case let .downloading(percent, received, total):
                        note(percent: percent, received: received, total: total)
                    case .verifying: resetTransfer(); status = "校验中…".t
                    case .installing: resetTransfer(); status = "准备安装…".t
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
        defer { busy = false; resetTransfer() }
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
        defer { busy = false; resetTransfer() }
        let updater = GeoIPUpdater(useMirror: state.config.mirror,
                                   proxyPort: state.controller.isCoreRunning ? state.config.mixedPort : nil)
        do {
            status = try await updater.update { step in
                Task { @MainActor in
                    switch step {
                    case let .downloading(percent, received, total):
                        note(percent: percent, received: received, total: total)
                    case .validating: resetTransfer(); status = "校验中…".t
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
        case .checking: resetTransfer(); status = "检查中…".t
        case let .downloading(percent, received, total):
            note(percent: percent, received: received, total: total)
        case .installing: resetTransfer(); status = "安装中…".t
        case .done, .failed: resetTransfer()
        }
    }
}
