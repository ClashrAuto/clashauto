import Foundation
import Observation

/// mihomo 核心进程的启停与日志转发。对齐 C++ `CoreController` 里与进程相关的那一半
/// （系统代理 / TUN / helper 在阶段 3 单独做，见下方 `privilegedLauncher` 这个接缝）。
@MainActor
@Observable
public final class CoreProcess {

    public enum StartFailure: Error, Sendable, Equatable {
        /// 核心二进制不在。打包的 .app 默认集成最新正式版内核（`make_app.sh` 放进
        /// Contents/Resources/core，`start()` 首次运行落位），所以正式包基本走不到这里；
        /// 开发期（`swift run`）和 `--no-core` 的包没有这份资源 —— 引导用户去
        /// 「设置 → 系统」下载。
        case coreMissing(path: String)
        case configMissing(path: String)
        case launchFailed(String)
    }

    public private(set) var isRunning = false
    /// 核心是否由特权 helper 以 root 启动。TUN 依赖它 —— 非 root 起的核心建不了 utun。
    public private(set) var isPrivileged = false

    public var onLog: ((String, LogKind) -> Void)?
    /// 核心二进制缺失时触发，UI 据此引导下载。
    public var onCoreMissing: ((String) -> Void)?

    /// **有人正在看核心日志**（日志页开着）。为假时核心的 stdout 原文一律丢弃，
    /// helper 路径连 `core.log` 都不去 tail —— 那是每 500ms 一次的真实 I/O，
    /// 没人看的时候纯属白跑。
    ///
    /// ★ 「不处理」只能做到**不往上送**，管道**必须继续读**：不读的话核心写满 64KB
    ///   管道缓冲就阻塞在 `write` 上，整个核心卡死 —— 那是比多几个字符串昂贵得多的代价。
    ///   所以下面 `streamOutput` 里照旧收字节、照旧切行，只是不再跳回主 actor。
    ///
    /// 订阅是**从此刻起**：helper 路径从 `core.log` 的当前末尾开始 tail，不回灌历史，
    /// 这样两条路径（管道 / tail）的行为一致 —— 管道那条本来就没法重放。
    public var streamsCoreOutput = false {
        didSet {
            guard streamsCoreOutput != oldValue else { return }
            coreOutputWanted.set(streamsCoreOutput)
            if streamsCoreOutput {
                if isRunning, isPrivileged { startLogTail() }
            } else {
                logTask?.cancel()
                logTask = nil
            }
        }
    }

    /// `streamsCoreOutput` 的**跨线程副本**。`readabilityHandler` 跑在 Foundation
    /// 自己的串行队列上，那里读不到 `@MainActor` 的属性。
    private let coreOutputWanted = OutputSwitch()

    private final class OutputSwitch: @unchecked Sendable {
        private let lock = NSLock()
        private var value = false
        var isOn: Bool { lock.lock(); defer { lock.unlock() }; return value }
        func set(_ on: Bool) { lock.lock(); value = on; lock.unlock() }
    }

    private var config: AppConfig
    private var process: Process?

    /// 核心**非我方要求**地退出了（崩溃、被 kill、panic）。
    ///
    /// 必须和「我们主动停它」分开：后者是正常流程，前者意味着系统此刻处在一个危险状态 ——
    /// 系统代理还指着一个没人监听的端口，被接管的设备还在把流量发给一个不再转发的网关。
    /// 界面却毫不知情。没有这个回调，那个状态会一直挂着。
    public var onUnexpectedExit: (() -> Void)?

    /// 是否是我方主动要求停止。`stop()` 置位，`start()` 复位。
    private var stopRequested = false

    /// 正在运行的核心 PID。诊断用（日志里对得上系统里的进程），测试里也靠它模拟意外死亡。
    public var coreProcessIdentifier: Int32? {
        guard let task = process, task.isRunning else { return nil }
        return task.processIdentifier
    }
    private var logTask: Task<Void, Never>?

    /// 以 root 启动核心的通道（阶段 3 由 helper 客户端注入）。为 nil 时走普通非特权 `Process`，
    /// 此时 TUN 不会生效 —— 开着 TUN 却没有 helper 是要**明确记一条日志**的，
    /// 因为用户那边的表象只是「增强灯亮着却不全局」，无从查起。
    public var privilegedLauncher: PrivilegedCoreLauncher?

    public init(config: AppConfig) {
        self.config = config
    }

    public func updateConfig(_ config: AppConfig) { self.config = config }

    /// 核心二进制是否已就位。
    public var isCoreInstalled: Bool {
        FileManager.default.isExecutableFile(atPath: AppPaths.coreExecutable.path)
    }

    // MARK: - 启停

    /// 外部发现核心其实已经没了 —— 把本对象的状态复位，好让下一次 `start()` 真的去启动。
    ///
    /// ★ 只有 helper（root）那条路需要它：那条路把进程交给 helper，本进程**没有句柄也收不到
    ///   退出通知**，`isRunning` 会一直停在 true。于是 `start()` 开头那句
    ///   `guard !isRunning else { return .success(()) }` 会**直接早退并报成功**，
    ///   调用方以为核心起来了，实际什么都没发生 —— 自愈重启会变成一个空转的循环。
    ///   真机实测就是这个样子：日志里「自动重启（第 1/3 次）」「第 2/3 次」都记了，
    ///   而 `ps` 里始终没有核心，最后耗尽预算放弃、PF 规则还挂着。
    ///   非特权那条路有 `terminationHandler` 会自己复位，用不到这个方法。
    public func markDead() {
        guard isRunning else { return }
        isRunning = false
        isPrivileged = false
        logTask?.cancel(); logTask = nil
        if let pipe = logPipe {
            pipe.fileHandleForReading.readabilityHandler = nil
            try? pipe.fileHandleForReading.close()
            logPipe = nil
        }
    }

    @discardableResult
    public func start(tunEnabled: Bool, fullConfigPath: URL) async -> Result<Void, StartFailure> {
        guard !isRunning else { return .success(()) }
        stopRequested = false

        // 打包时集成的内核首次运行落到用户目录 —— 全新安装开箱即用。
        // 必须在取 coreExecutable **之前**：它的「扁平优先、回退老路径」判定依赖文件是否存在。
        Self.seedCoreIfMissing()

        let exe = AppPaths.coreExecutable
        guard FileManager.default.fileExists(atPath: exe.path) else {
            log("未检测到 mihomo 内核，请在「设置 → 系统」中下载: \(exe.path)")
            onCoreMissing?(exe.path)
            return .failure(.coreMissing(path: exe.path))
        }
        guard FileManager.default.fileExists(atPath: fullConfigPath.path) else {
            log("找不到 Clash 配置: \(fullConfigPath.path)")
            return .failure(.configMissing(path: fullConfigPath.path))
        }

        seedGeoIP()
        refreshGeoIP()

        // ★ 先收掉上一世遗留的孤儿核心 —— 必须在**两条启动路径之前**
        //   （helper root 那条会提前 return，放在 launchPlain 里 macOS 上就永远走不到，
        //    Qt 线正是这么踩过的）。
        reapOrphanCores(executable: exe)

        // 有 helper 就以 root 起（TUN/增强才能建 utun、改路由）。失败**不静默回退** ——
        // 把原因讲清楚再降级，否则「装了 helper、开了增强、核心却不是 root」无从排查。
        if let launcher = privilegedLauncher, await launcher.isEnabled {
            do {
                try await launcher.startCore(executable: exe, config: fullConfigPath, userDir: AppPaths.userDir)
                isPrivileged = true
                isRunning = true
                if streamsCoreOutput { startLogTail() }
                log("核心已由特权 helper 以 root 启动（支持 TUN）")
                return .success(())
            } catch {
                log("经特权 helper 以 root 启动核心失败：\(error)；回退为非 root 启动，TUN 将不生效")
            }
        } else if tunEnabled {
            log("增强(TUN) 已开启，但免密 helper 未启用——核心将以非 root 启动、TUN 不会生效。"
                + "请在「设置 → 系统」安装并批准免密助手后重开增强。")
        }

        return launchPlain(executable: exe, config: fullConfigPath)
    }


    /// 收掉上一次会话遗留的孤儿核心。
    ///
    /// app 被 SIGKILL（崩溃、强杀、OOM）时来不及停核心，核心作为子进程被 init 收养、
    /// **继续活着**。下次启动时新核心绑不上端口 —— mihomo 绑不上时**并不退出**，
    /// 只记一条 "address already in use" 然后照常运行，于是"进程在跑但一个端口都没监听"，
    /// 表现为代理完全不通且毫无提示。而且每崩一次就多留一批：
    /// 本机实测开会话时清出过 **9 个**孤儿（ppid 全是 1、共占 326 MB、0 个网络 fd）。
    ///
    /// 判据从严，宁可漏杀不可误杀：只收
    ///   ① 命令行里出现我们这份核心可执行文件的**绝对路径**，且
    ///   ② **ppid == 1**（已被 init 收养 = 父进程确实没了 = 是孤儿）
    /// 的进程。还有活父进程的实例（用户手动跑的、另一个正常实例）ppid 不是 1，不会被碰。
    /// 与 Qt 线的 `CoreController::reapOrphanCore()` 同一设计。
    /// 供**启动早期**调用的入口：自己解析核心路径，不需要调用方先算好。
    ///
    /// ★ 为什么要在 `CoreProcess.start()` 之外再暴露一个入口：
    ///   `CoastController.clearStaleSystemProxy()` 的判据之一是「我们的 mixedPort
    ///   **无人监听**」，而上一世遗留的孤儿核心**正占着那个端口**。
    ///   若先清代理、后收孤儿，清代理那步会因为"有人在听"直接跳过 ——
    ///   两项自愈单独都对，**凑在一起就互相抵消**。真机实测过这个组合场景：
    ///   2 个孤儿占着 7890/9191，此时残留的系统代理擦不掉。
    ///   所以启动序列必须是 **先收孤儿、再清代理**。
    public func reapOrphansAtStartup() {
        let exe = AppPaths.coreExecutable
        guard FileManager.default.fileExists(atPath: exe.path) else { return }
        reapOrphanCores(executable: exe)
    }

    private func reapOrphanCores(executable: URL) {
        let exePath = executable.resolvingSymlinksInPath().path
        guard !exePath.isEmpty else { return }
        let ps = Process()
        ps.executableURL = URL(fileURLWithPath: "/bin/ps")
        ps.arguments = ["-axo", "pid=,ppid=,command="]
        let pipe = Pipe()
        ps.standardOutput = pipe
        do { try ps.run() } catch { return }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        ps.waitUntilExit()
        guard let out = String(data: data, encoding: .utf8) else { return }
        var reaped = 0
        for line in out.split(separator: "\n") {
            guard line.contains(exePath) else { continue }
            let f = line.split(separator: " ", omittingEmptySubsequences: true)
            guard f.count >= 3, let cpid = Int32(f[0]), let cppid = Int32(f[1]) else { continue }
            guard cppid == 1 else { continue }              // 还有活父进程 → 不是孤儿
            guard cpid != ProcessInfo.processInfo.processIdentifier else { continue }  // 自保
            kill(cpid, SIGKILL)
            reaped += 1
        }
        if reaped > 0 {
            log("已收掉 \(reaped) 个上次异常退出遗留的内核进程")
            Thread.sleep(forTimeInterval: 1.0)   // 等它们真的消失，否则新核心照样绑不上端口
        }
    }

    private func launchPlain(executable: URL, config configPath: URL) -> Result<Void, StartFailure> {
        let task = Process()
        task.executableURL = executable
        // **只传 -d/-f**：stock mihomo 没有 -token（那是 Clashr 定制核心才有的），
        // 传了会「flag provided but not defined」→ 打印用法并以退出码 2 结束，核心根本起不来。
        task.arguments = ["-d", AppPaths.userDir.path, "-f", configPath.path]
        task.currentDirectoryURL = executable.deletingLastPathComponent()

        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = pipe

        task.terminationHandler = { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.isRunning = false
                self.isPrivileged = false
                if self.stopRequested {
                    self.log("核心已退出")
                } else {
                    self.log("核心意外退出")
                    self.onUnexpectedExit?()
                }
            }
        }

        do {
            try task.run()
        } catch {
            log("启动 Clash 核心失败: \(error.localizedDescription)")
            return .failure(.launchFailed(error.localizedDescription))
        }

        // ★ **父进程必须关掉管道的写端**。`Process` 只把写端交给子进程，父进程手里那一份
        //   仍然开着 —— 于是子进程死了之后读端**永远等不到 EOF**，下面那个读日志的
        //   `for try await line in ….bytes.lines` 会一直卡在 `read(2)` 里。
        //
        //   后果不是「多一个闲置任务」那么轻：`AsyncBytes` 的 `read` 是**阻塞调用**，
        //   而它跑在 Swift 并发的协作线程池上 —— 每起一次核心就永久占掉一个池线程。
        //   在测试进程里连起几次核心，池子被占满，整个 `swift test` 就挂住不动了
        //   （实测：`swift test --filter CoreCrashE2ETests` 跑 400 秒不结束，
        //   `sample` 抓到的正是这个 `read`）。测试进程挂住 → 子核心也没人收 →
        //   `ps` 里留下一串指向已删除临时目录的 mihomo。
        try? pipe.fileHandleForWriting.close()
        logPipe = pipe

        process = task
        isRunning = true
        isPrivileged = false
        streamOutput(from: pipe)
        log("Start clash is OK!", .routine)
        return .success(())
    }

    public func stop() async {
        stopRequested = true
        logTask?.cancel(); logTask = nil
        // 只 `cancel()` 不够：任务此刻多半正阻塞在 `read(2)` 里，而取消**打断不了**
        // 已经进内核的读。关掉读端才会让那个 read 立刻返回，任务随之结束。
        if let pipe = logPipe {
            pipe.fileHandleForReading.readabilityHandler = nil
            try? pipe.fileHandleForReading.close()
            logPipe = nil
        }

        if isPrivileged, let launcher = privilegedLauncher {
            try? await launcher.stopCore()
            isPrivileged = false
            isRunning = false
            log("核心已停止（helper）")
            return
        }

        guard let task = process, task.isRunning else {
            isRunning = false
            return
        }
        // SIGTERM 先礼后兵：mihomo 收到会优雅退出（关 utun、还原路由）。宽限 2.5s，
        // 到点还在就 SIGKILL —— 不能让「退出应用」卡在这里干等。
        task.terminate()
        let deadline = Date().addingTimeInterval(2.5)
        while task.isRunning, Date() < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        if task.isRunning { kill(task.processIdentifier, SIGKILL) }
        process = nil
        isRunning = false
    }

    // MARK: - 日志

    /// 非特权路径：直接读子进程的 stdout/stderr。
    ///
    /// ★ **绝不能用 `for try await line in handle.bytes.lines`。** `CoreProcess` 整个类是
    ///   `@MainActor`，`Task { }` 会继承这个 actor —— 而 `AsyncBytes` 底下是**阻塞的
    ///   `read(2)`**，于是那一行代码等于「在主线程上死等管道来数据」。核心不吭声的时候，
    ///   主线程就卡在内核里：界面不响应，任何 `@MainActor` 的活都排不上。
    ///
    ///   在测试进程里后果更直白：`swift test --filter CoreCrashE2ETests` 跑到 400 秒
    ///   不结束，`sample` 抓到的栈就是 main-thread → `streamOutput` → `read`。
    ///   进程挂住，子核心也没人收，`ps` 里于是留下一串指向已删除临时目录的 mihomo。
    ///
    ///   改用 `readabilityHandler`：它由 Foundation 在**自己的串行队列**上回调，
    ///   一次拿走当前可读的字节，谁也不阻塞；只有在真的凑出一整行时才跳回主 actor 记一条日志。
    private func streamOutput(from pipe: Pipe) {
        let handle = pipe.fileHandleForReading
        let buffer = LineBuffer()
        handle.readabilityHandler = { [weak self] fileHandle in
            let chunk = fileHandle.availableData
            guard !chunk.isEmpty else {
                // 空 = EOF：核心已退出，摘掉回调，别再被叫起来。
                fileHandle.readabilityHandler = nil
                return
            }
            // 字节照收、行照切（缓冲要靠它保持有界），但没人在看时**到此为止** ——
            // 不跳主 actor、不建日志条目。见 `streamsCoreOutput` 的说明。
            let lines = buffer.append(chunk)
            guard self?.coreOutputWanted.isOn == true, !lines.isEmpty else { return }
            Task { @MainActor in
                guard let self else { return }
                for line in lines { self.log(line, .core) }
            }
        }
    }

    /// 把字节流切成整行。半行留着等下一块 —— 管道不保证按行边界到达。
    /// 只在 `readabilityHandler` 那条串行队列上使用，故 `@unchecked Sendable`。
    private final class LineBuffer: @unchecked Sendable {
        private var pending = Data()

        func append(_ chunk: Data) -> [String] {
            pending.append(chunk)
            var lines: [String] = []
            while let index = pending.firstIndex(of: 0x0A) {
                let line = String(data: pending[pending.startIndex..<index], encoding: .utf8)
                pending.removeSubrange(pending.startIndex...index)
                if let line, !line.isEmpty { lines.append(line) }
            }
            // 单行长到离谱（核心吐了一大坨没有换行的东西）就丢掉，别让缓冲无界增长。
            if pending.count > 1 << 20 { pending.removeAll() }
            return lines
        }
    }

    /// 非特权路径的日志管道。`stop()` 要拿它关读端 —— 见那里的说明。
    private var logPipe: Pipe?

    /// helper 路径：核心是 root 起的，拿不到它的管道，改为 tail `logs/core.log`。
    ///
    /// 起点是**订阅那一刻的文件末尾**，不是 0：日志页没开的时候我们什么都不收
    /// （见 `streamsCoreOutput`），开的时候再把此前攒下的几十万行回灌一遍就自相矛盾了，
    /// 而且管道那条路径本来也没法重放 —— 两条路径得是同一种行为。
    private func startLogTail() {
        logTask?.cancel()
        let path = AppPaths.userDir.appendingPathComponent("logs/core.log")
        let start = (try? FileManager.default
            .attributesOfItem(atPath: path.path)[.size] as? UInt64) ?? 0
        logTask = Task { [weak self] in
            var offset: UInt64 = start
            while !Task.isCancelled {
                if let handle = try? FileHandle(forReadingFrom: path) {
                    defer { try? handle.close() }
                    try? handle.seek(toOffset: offset)
                    if let data = try? handle.readToEnd(), !data.isEmpty {
                        offset += UInt64(data.count)
                        let text = String(data: data, encoding: .utf8) ?? ""
                        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
                            guard let self else { return }
                            await MainActor.run { self.log(String(line), .core) }
                        }
                    }
                }
                try? await Task.sleep(for: .milliseconds(500))
            }
        }
    }

    /// 起核心前的 GeoIP 收尾：换上暂存的新库 + 给线上那份体检。
    ///
    /// 体检不是多余的：坏掉的 mmdb **核心不报错** —— 打得开、不 fatal，只是每次查询返回空，
    /// 于是 `GEOIP,CN` 静默失效、国内流量全部出海，日志里一个字都没有。
    /// 一旦发现坏了就退回内置种子：宁可用一份旧的、对的，也不能用一份新的、坏的。
    private func refreshGeoIP() {
        let target = AppPaths.userDir.appendingPathComponent("Country.mmdb")
        if MmdbFile.applyStaged(target: target) {
            log("GeoIP 数据库已更新")
        }
        guard FileManager.default.fileExists(atPath: target.path) else { return }
        let health = MmdbFile.validateFile(target)
        guard !health.ok else { return }
        log("GeoIP 数据库损坏（\(health.why)），已退回内置版本")
        if let seed = Resources.seed("Country.mmdb") {
            try? FileManager.default.removeItem(at: target)
            try? FileManager.default.copyItem(at: seed, to: target)
            AppPaths.makeWritable(target)
        }
    }

    // MARK: - 内核种子

    /// 首次运行把打包时集成的内核放到 `command/core`。
    ///
    /// `make_app.sh` 默认把最新**正式版**内核放进 `Contents/Resources/core`（随 .app 一起
    /// 签名），这里只做「缺了才补」：**绝不覆盖**已装的内核 —— 用户手动升级过（或切了
    /// 测试版通道）的内核被一次启动静默回滚是最难查的那种坑。集成只是让全新安装
    /// 开箱即用，之后的版本管理仍归「设置 → 系统」的下载/更新。
    ///
    /// 开发期（`swift run`）没有打包资源，`Resources.asset` 返回 nil，安静跳过。
    /// 参数可注入是给单测用的；产品代码一律用默认值。
    nonisolated public static func seedCoreIfMissing(
        from bundled: URL? = Resources.asset("core"),
        installedAt existing: URL = AppPaths.coreExecutable,
        to target: URL = AppPaths.userDir.appendingPathComponent("command/core")
    ) {
        guard let bundled,
              !FileManager.default.isExecutableFile(atPath: existing.path) else { return }
        try? FileManager.default.createDirectory(at: target.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: target)
        guard (try? FileManager.default.copyItem(at: bundled, to: target)) != nil else { return }
        // 0755：不给执行位内核起不来,报错只是一句含糊的「启动失败」（同 CoreDownloader.install）。
        try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: target.path)
        // .app 若带隔离标记（从 DMG 拖出来的都带）,拷出的副本会继承 —— 摘掉,
        // 否则首次 exec 会被 Gatekeeper 拦下,表现为「启动失败」且日志里毫无线索。
        removexattr(target.path, "com.apple.quarantine", 0)
    }

    // MARK: - GeoIP 种子

    /// 首次运行把内置 `Country.mmdb` 放到 userDir。
    ///
    /// 位置必须是 userDir 根 —— 核心以 `-d userDir` 起，GeoIP 就从这里读；放别处会让老核心
    /// 回退到 `~/.config/clash`，与设置页「更新 GeoIP」的落盘路径对不上。
    ///
    /// C++ 版在这里还做了「暂存库换上 + 线上库体检」两件事（`MmdbFile::applyStaged` /
    /// `validateFile`）。那部分随「更新 GeoIP」功能一起在阶段 5 补 —— 现在还没有下载入口，
    /// 也就不会有暂存文件。**不要忘**：坏掉的 GeoIP 库核心能正常 Load、只是查什么都返回空，
    /// 表现为 `GEOIP,CN` 静默失配、国内流量集体出海，非常难查。
    private func seedGeoIP() {
        let target = AppPaths.userDir.appendingPathComponent("Country.mmdb")
        guard !FileManager.default.fileExists(atPath: target.path),
              let seed = Resources.seed("Country.mmdb") else { return }
        try? FileManager.default.copyItem(at: seed, to: target)
        AppPaths.makeWritable(target)
        log("Country.mmdb 已就位: \(target.path)", .routine)
    }

    /// 默认 `.notice`：这个类里绝大多数 `log(...)` 都是程序自己的动作/结论。
    /// 核心吐出来的原文由 `streamOutput`/`startLogTail` 显式标 `.core`。
    private func log(_ message: String, _ kind: LogKind = .notice) { onLog?(message, kind) }
}

/// 以 root 启动核心的通道。阶段 3 由特权 helper 的 XPC 客户端实现。
public protocol PrivilegedCoreLauncher: Sendable {
    /// helper 是否已注册并被用户批准。用「已启用」而不是「ping 得通」做判据：
    /// 冷启动的 daemon 首个 XPC 偶发慢/超时，用 ping 当门槛会让 helper 明明装了却被判为不可用。
    var isEnabled: Bool { get async }
    func startCore(executable: URL, config: URL, userDir: URL) async throws
    func stopCore() async throws
}
