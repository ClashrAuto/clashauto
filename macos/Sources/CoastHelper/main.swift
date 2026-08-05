import CoastHelperProtocol
import Foundation
import os.log
import SystemConfiguration

// 特权 helper（root daemon）。由 SMAppService 注册、launchd 按需拉起。
//
// 这个进程以 root 跑，因此**只做三件事**：设系统代理、起/停核心、报版本。
// 任何多余的能力都是攻击面 —— 不解析用户数据、不联网、不读配置文件。
// 客户端身份由 setCodeSigningRequirement 把关（见 HelperConstants.clientCodeRequirement）。

final class HelperService: NSObject, CoastHelperProtocol, NSXPCListenerDelegate {

    /// 由本 helper 启动的核心进程。只允许有一个。
    private var core: Process?
    /// 透明代理接管器（root：ARP 欺骗 + PF + ip.forwarding）。
    /// **每张网卡一个 Redirector**（各自的 PF anchor 与崩溃记录，见 Redirector 顶部）。
    /// 一张卡就是长度 1 的数组 —— 没有单网卡这个特例。startRedirect 时按下发的网卡数重建。
    private var redirectors: [Redirector] = []
    private let queue = DispatchQueue(label: "com.yuehongsun.coast.helper.state")

    // MARK: - NSXPCListenerDelegate

    func listener(_ listener: NSXPCListener, shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        // ★ 这道校验是本 helper 唯一的门。少了它，任何本机进程都能驱动一个 root 服务。
        //   macOS 13+ 才有；更早的系统上我们干脆拒绝连接，而不是降级放行。
        guard #available(macOS 13.0, *) else { return false }
        connection.setCodeSigningRequirement(HelperConstants.clientCodeRequirement)

        connection.exportedInterface = NSXPCInterface(with: CoastHelperProtocol.self)
        connection.exportedObject = self
        // ★ 命门：连接一断（app 正常退出 / 崩溃 / 被 SIGKILL）就把被欺骗的设备复原。
        //   没有这一步，app 意外死掉时那些设备会一直把本机当网关、直接断网十几分钟。
        // ★ 只认**发起接管的那一条**连接。
        //
        //   客户端是「每次调用建一条连接、用完即弃」的，随便一次 getVersion 结束都会走到这里。
        //   早先这里无条件 `redirector.stop()`，等于 startRedirect 一返回、那条连接一 invalidate
        //   就把刚建立的接管撤掉 —— 接管从来不可能维持住。（实测：任何一次普通调用之后
        //   日志里都会出现一条撤销。）
        connection.invalidationHandler = { [weak self, weak connection] in
            self?.clientVanished(connection, reason: "invalidated")
        }
        connection.interruptionHandler = { [weak self, weak connection] in
            self?.clientVanished(connection, reason: "interrupted")
        }
        connection.resume()
        return true
    }

    // MARK: - CoastHelperProtocol

    func getVersion(withReply reply: @escaping (String) -> Void) {
        reply(HelperVersion.current)
    }

    func setSystemProxy(enabled: Bool, host: String, port: Int,
                        bypassCommaSeparated: String,
                        withReply reply: @escaping (Bool, String) -> Void) {
        Audit.log("setSystemProxy enabled=\(enabled) \(host):\(port)", caller: NSXPCConnection.current())
        // root 身份下 SCPreferencesCreate 直接可写，不需要 AuthorizationRef —— 全程免密。
        guard let prefs = SCPreferencesCreate(nil, "CoastHelper" as CFString, nil) else {
            reply(false, "SCPreferencesCreate 返回空")
            return
        }
        guard SCPreferencesLock(prefs, true) else {
            reply(false, "SCPreferencesLock 失败（配置被占用？）")
            return
        }
        defer { SCPreferencesUnlock(prefs) }

        guard let services = SCNetworkServiceCopyAll(prefs) as? [SCNetworkService] else {
            reply(false, "SCNetworkServiceCopyAll 返回空")
            return
        }
        let bypass = bypassCommaSeparated
            .split(separator: ",", omittingEmptySubsequences: true)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }

        for service in services {
            guard SCNetworkServiceGetEnabled(service),
                  let serviceID = SCNetworkServiceGetServiceID(service) as String? else { continue }
            let path = "/NetworkServices/\(serviceID)/Proxies" as CFString
            // 在现有字典上改：FTP/PAC 等我们不管的键要保留，整份替换会抹掉用户自己的配置。
            var proxies = (SCPreferencesPathGetValue(prefs, path) as? [String: Any]) ?? [:]
            if enabled {
                proxies[kCFNetworkProxiesHTTPEnable as String] = 1
                proxies[kCFNetworkProxiesHTTPProxy as String] = host
                proxies[kCFNetworkProxiesHTTPPort as String] = port
                proxies[kCFNetworkProxiesHTTPSEnable as String] = 1
                proxies[kCFNetworkProxiesHTTPSProxy as String] = host
                proxies[kCFNetworkProxiesHTTPSPort as String] = port
                proxies[kCFNetworkProxiesSOCKSEnable as String] = 1
                proxies[kCFNetworkProxiesSOCKSProxy as String] = host
                proxies[kCFNetworkProxiesSOCKSPort as String] = port
                proxies[kCFNetworkProxiesExceptionsList as String] = bypass
            } else {
                // 只翻 Enable 位、保留 host/port，下次秒开
                proxies[kCFNetworkProxiesHTTPEnable as String] = 0
                proxies[kCFNetworkProxiesHTTPSEnable as String] = 0
                proxies[kCFNetworkProxiesSOCKSEnable as String] = 0
            }
            SCPreferencesPathSetValue(prefs, path, proxies as CFDictionary)
        }

        guard SCPreferencesCommitChanges(prefs), SCPreferencesApplyChanges(prefs) else {
            reply(false, "SCPreferencesCommit/Apply 失败")
            return
        }
        reply(true, "")
    }

    func startCore(executable: String, config: String, userDir: String,
                   withReply reply: @escaping (Bool, String) -> Void) {
        Audit.log("startCore exe=\(executable) config=\(config)", caller: NSXPCConnection.current())
        queue.sync {
            stopCoreLocked()

            // ★ 纵深防御:helper 以 root 起 executable、按 userDir 拼日志路径。codesign 门是
            //   主防线,这里再挡一层路径卫生 —— 必须绝对路径、不含 `..` 段(防路径遍历)。
            for (label, path) in [("核心", executable), ("配置", config), ("数据目录", userDir)] {
                guard InputValidation.isSanePath(path) else {
                    reply(false, "\(label)路径非法(需绝对路径且不含 ..): \(path)")
                    return
                }
            }

            let fm = FileManager.default
            guard fm.isExecutableFile(atPath: executable) else {
                reply(false, "核心不可执行: \(executable)")
                return
            }
            guard fm.fileExists(atPath: config) else {
                reply(false, "配置不存在: \(config)")
                return
            }

            let logDirectory = (userDir as NSString).appendingPathComponent("logs")
            try? fm.createDirectory(atPath: logDirectory, withIntermediateDirectories: true)
            let logPath = (logDirectory as NSString).appendingPathComponent("core.log")
            // 每次启动**截断**日志：应用侧从头 tail（见 CoreProcess.startLogTail），
            // 两边对这个约定必须一致，否则应用会把上一次运行的日志当成本次的重放一遍。
            fm.createFile(atPath: logPath, contents: nil)
            guard let logHandle = FileHandle(forWritingAtPath: logPath) else {
                reply(false, "无法写入日志: \(logPath)")
                return
            }

            let process = Process()
            process.executableURL = URL(fileURLWithPath: executable)
            // 只传 -d/-f：stock mihomo 没有 -token，传了会「flag provided but not defined」直接退出。
            process.arguments = ["-d", userDir, "-f", config]
            process.currentDirectoryURL = URL(fileURLWithPath: executable).deletingLastPathComponent()
            process.standardOutput = logHandle
            process.standardError = logHandle
            do {
                try process.run()
            } catch {
                reply(false, "启动失败: \(error.localizedDescription)")
                return
            }
            core = process
            // 落盘 pid + exe：helper 换命之后靠它认回核心（见 reapRecordedCoreIfAlive）。
            Self.writeCoreRecord(pid: process.processIdentifier, executable: executable)
            reply(true, "")
        }
    }

    func stopCore(withReply reply: @escaping (Bool, String) -> Void) {
        Audit.log("stopCore", caller: NSXPCConnection.current())
        queue.sync {
            stopCoreLocked()
            reply(true, "")
        }
    }

    /// 停掉全部网卡上的接管并复原。幂等。
    private func stopAllRedirectors() {
        let list = queue.sync { redirectors }
        for redirector in list { redirector.stop() }
    }

    func startRedirect(nicsJSON: String, dnsPort: Int,
                       withReply reply: @escaping (Bool, String) -> Void) {
        let nics = RedirectNicSpec.decode(nicsJSON)
        guard !nics.isEmpty else { reply(false, "没有要接管的网卡（nicsJSON 为空或解析失败）"); return }
        guard nics.count <= Redirector.maxNics else {
            reply(false, "网卡数超过上限 \(Redirector.maxNics)")
            return
        }
        // 接管别人的流量是本程序做过的最重的一件事,日志必须记全:接管了谁、冒充哪个网关、
        // 走哪块网卡。出问题时(局域网异常、某台设备断网)这是唯一能自证「我做了什么」的记录。
        for (index, nic) in nics.enumerated() {
            let v6Note = nic.routerLL6.isEmpty
                ? "" : " v6路由器=\(nic.routerLL6)/\(nic.routerMAC6) v6设备=[\(nic.deviceV6s.joined(separator: ","))]"
            Audit.log("startRedirect[\(index)] 设备=[\(nic.deviceIPs.joined(separator: ","))] "
                      + "网卡=\(nic.interface) 网关=\(nic.gatewayIP)/\(nic.gatewayMAC) "
                      + "redir=\(nic.redirPort) dns=\(dnsPort)\(v6Note)",
                      caller: NSXPCConnection.current())
        }
        // 先取住这条连接:reply 之后 `NSXPCConnection.current()` 就不在本次调用上下文里了。
        let caller = NSXPCConnection.current()

        // 换一批网卡时先把上一轮整个收干净（Redirector.start 自己也幂等，但实例数可能变少）。
        stopAllRedirectors()
        let fresh = (0..<nics.count).map { Redirector(index: $0) }
        queue.sync { redirectors = fresh }

        // ★ **任何一张卡失败就整体回滚**：半开着比不开更糟 —— 那张卡上的设备已经被投毒、
        //   却没人转发它的流量，直接断网到 ARP 缓存老化。
        for (redirector, nic) in zip(fresh, nics) {
            if let error = redirector.start(deviceIPs: nic.deviceIPs, deviceMACs: nic.deviceMACs,
                                            interface: nic.interface,
                                            gatewayIP: nic.gatewayIP, gatewayMAC: nic.gatewayMAC,
                                            redirPort: nic.redirPort, dnsPort: dnsPort,
                                            routerLL6: nic.routerLL6, routerMAC6: nic.routerMAC6,
                                            deviceV6s: nic.deviceV6s) {
                stopAllRedirectors()
                queue.sync { redirectors = [] }
                reply(false, "\(nic.interface): \(error)")
                return
            }
        }
        // **只在真的接管成功后**才记下发起方。失败时记下的话会留一个悬空 owner ——
        // 那条连接稍后断开时会去撤销一个根本不存在的接管（无害但会误导日志），
        // 更糟的是它会挡住下一次真正的接管去认领 owner。
        queue.sync { redirectOwner = caller }
        reply(true, "")
    }

    /// 发起接管的那条连接。只有它断开才意味着「app 没了，把设备放回去」。
    private var redirectOwner: NSXPCConnection?

    private func clientVanished(_ connection: NSXPCConnection?, reason: String) {
        queue.sync {
            guard let owner = redirectOwner, owner === connection else { return }
            Audit.log("接管发起方的连接已\(reason) —— app 可能已崩溃/被强杀，撤销接管")
            redirectOwner = nil
        }
        stopAllRedirectors()
    }

    func terminate(withReply reply: @escaping () -> Void) {
        Audit.log("terminate:收到退出请求,先还原接管与核心")
        // 顺序要紧:先还原 ARP 接管(否则局域网里那些设备会继续把流量发给一个不存在的网关),
        // 再停核心。反过来的话,接管还在、转发目标已经没了 —— 被接管的设备直接断网。
        stopAllRedirectors()
        queue.sync { stopCoreLocked() }
        Audit.log("terminate:已收拾干净,退出")
        reply()
        // 让 reply 有机会送达对端再退。launchd 会在下次连接时按需拉起新版。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { exit(0) }
    }

    func stopRedirect(withReply reply: @escaping (Bool, String) -> Void) {
        Audit.log("stopRedirect", caller: NSXPCConnection.current())
        queue.sync { redirectOwner = nil }
        stopAllRedirectors()
        reply(true, "")
    }

    /// 调用方必须已持有 `queue`。
    private func stopCoreLocked() {
        // ★ 顺序要紧：先收掉本实例**追踪得到**的那个，再按记录收「上一条命留下的」，最后才清记录。
        //   两步都要 —— helper 是按需 daemon，多数时候 `core` 是 nil 而记录里那个还活着。
        defer {
            Self.reapRecordedCoreIfAlive { Audit.log($0) }
            Self.clearCoreRecord()
        }
        guard let process = core, process.isRunning else {
            core = nil
            return
        }
        // 先 SIGTERM：mihomo 会优雅退出，关掉 utun 并还原路由表。
        // 直接 SIGKILL 会把系统留在「默认路由指向已消失的 utun」的状态上 —— 用户直接断网。
        process.terminate()
        let deadline = Date().addingTimeInterval(3)
        while process.isRunning, Date() < deadline {
            usleep(50_000)
        }
        if process.isRunning { kill(process.processIdentifier, SIGKILL) }
        core = nil
    }

    // MARK: - 孤儿核心

    /// 核心是 helper 的**子进程**，而 helper 是**按需 daemon** —— `launchctl print` 上
    /// 常态是 `state = not running`（实测 `runs = 7`）：它被 XPC 连接拉起、干完活空闲就退。
    /// 于是「核心 ppid 变成 1（被 launchd 收养）」**是常态而非异常**，`kickstart -k` 只是
    /// 让它发生得更明显而已。后果是新一条命的 helper 里 `core` 恒为 nil：
    ///   · app 调 `stopCore` 变成空操作，**一个 root 进程永久残留**；
    ///   · 再 `startCore` 会起第二个核心，而端口可能还被孤儿占着 —— 新核心绑不上，
    ///     真正在服务的是**拿着旧配置**的那个孤儿，界面上却什么都看不出来。
    ///   真机实测确实见到过两个核心同时活着（一个 ppid=1）。
    ///
    /// 所以要一条不依赖父子关系的回收路径：起核心时把 pid 和**可执行文件路径**落到 `/var/db/`。
    ///
    /// ★ **但绝不能在 helper 启动时回收** —— 那正是这个坑最容易踩反的地方。既然 helper
    ///   本来就反复起落，「启动时看到记录里有个活着的核心」的**常见情形恰恰是它正在正常服役**，
    ///   启动即回收等于每次被唤醒都把用户的核心杀一遍。只在两个语义明确的时刻收：
    ///   `startCore`（要起新的，旧的按定义已作废）和 `stopCore`（用户/app 就是要它停）。
    ///
    /// ★ **落 exe 路径不是冗余** —— pid 会被系统复用，只凭 pid 就 `kill` 等于以 root 身份
    ///   随机杀一个无辜进程。回收前必须用 `proc_pidpath` 核对当前占用该 pid 的进程确实是
    ///   我们那个核心，对不上就只删记录、不动任何进程。
    private static var coreRecordPath: String {
        ProcessInfo.processInfo.environment["COAST_CORE_RECORD"]
            ?? "/var/db/com.yuehongsun.coast.helper.core"
    }

    private static func writeCoreRecord(pid: pid_t, executable: String) {
        try? "pid=\(pid)\nexe=\(executable)\n"
            .write(toFile: coreRecordPath, atomically: true, encoding: .utf8)
    }

    private static func clearCoreRecord() {
        try? FileManager.default.removeItem(atPath: coreRecordPath)
    }

    /// 取 pid 当前对应的可执行文件路径；进程不存在或取不到返回 nil。
    private static func executablePath(ofPID pid: pid_t) -> String? {
        var buffer = [CChar](repeating: 0, count: Int(MAXPATHLEN))
        let length = proc_pidpath(pid, &buffer, UInt32(buffer.count))
        guard length > 0 else { return nil }
        return String(cString: buffer)
    }

    /// 按记录回收：记录里那个核心若还活着且身份对得上，就收掉它。
    /// **只在 startCore / stopCore 里调**，理由见上方注释。
    static func reapRecordedCoreIfAlive(log: (String) -> Void) {
        guard let text = try? String(contentsOfFile: coreRecordPath, encoding: .utf8) else { return }
        var fields: [String: String] = [:]
        for line in text.split(separator: "\n") {
            let parts = line.split(separator: "=", maxSplits: 1)
            if parts.count == 2 {
                fields[String(parts[0]).trimmingCharacters(in: .whitespaces)] =
                    String(parts[1]).trimmingCharacters(in: .whitespaces)
            }
        }
        guard let pidText = fields["pid"], let pid = pid_t(pidText), pid > 1,
              let expected = fields["exe"] else { return }
        // ★ 身份核对：pid 会复用，对不上就什么都别做。
        guard let actual = executablePath(ofPID: pid) else { return }   // 进程早没了
        guard actual == expected else {
            log("发现上次的核心记录 pid=\(pid)，但该 pid 现在是 \(actual)，不是我们的核心 —— 不动它")
            return
        }
        log("记录里的核心 pid=\(pid) 仍在运行（helper 已换过一条命），正在收回")
        kill(pid, SIGTERM)
        let deadline = Date().addingTimeInterval(3)
        while kill(pid, 0) == 0, Date() < deadline { usleep(50_000) }
        if kill(pid, 0) == 0 {
            kill(pid, SIGKILL)
            log("孤儿核心 pid=\(pid) 未响应 SIGTERM，已 SIGKILL")
        } else {
            log("孤儿核心 pid=\(pid) 已退出")
        }
    }
}

enum HelperVersion {
    /// 版本取自**所在 .app 的** Info.plist，而不是嵌进本可执行文件的那份。
    ///
    /// 嵌入段（`-sectcreate __TEXT __info_plist`）是链接期产物，而那个 plist 只作为一条
    /// `-Xlinker` 参数存在，**不在 swift build 的依赖图里**：改了它，构建系统照样判定
    /// 「无需重建」，嵌入段留着上一次的内容 —— 改了等于没改（实测如此，连 touch 源码强制
    /// 重编都不保证重链）。于是嵌入的版本号永远停在模板里的 1.0。
    ///
    /// 而这个值不是装饰：`getVersion` 是唯一能问出「现在跑的到底是哪一份 helper」的探针，
    /// 「.app 换了但 launchd 还拽着旧注册」这个真实故障就靠它才可观测
    /// （见 `MacHelperClient.ensureRegisteredForCurrentBuild`）。恒为 1.0 等于探针失灵。
    ///
    /// helper 必然位于 `<App>.app/Contents/MacOS/` 下（launchd plist 的 BundleProgram 就这么写的），
    /// 往上两级即 `Contents/Info.plist` —— 那份由 make_app.sh 真正打了版本号。
    static let current: String = {
        if let executable = Bundle.main.executableURL {
            let info = executable
                .deletingLastPathComponent()      // MacOS/
                .deletingLastPathComponent()      // Contents/
                .appendingPathComponent("Info.plist")
            if let dict = NSDictionary(contentsOf: info) as? [String: Any],
               let version = dict["CFBundleVersion"] as? String {
                return version
            }
        }
        // 兜底：不在 .app 里跑（开发期直接执行）时退回嵌入段。
        return Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "0"
    }()
}

/// 特权操作的审计日志。
///
/// 这个 daemon 以 root 改系统代理、起进程、往局域网发 ARP 接管别人的流量。这些动作**必须**
/// 留痕：它没有终端(launchd 拉起)、没有界面，出了问题——某台设备断网、系统代理被改成意外的
/// 值、核心以 root 跑起了不该跑的东西——除了系统日志没有第二处可查。
/// 用 `%{public}@` 是因为 os_log 默认把字符串参数打码成 `<private>`，那样等于没记。
/// 但也**只记调用参数**：这些是 IP/MAC/端口/路径，没有凭据 —— 系统日志是全机可读的。
enum Audit {
    private static let channel = OSLog(subsystem: "com.yuehongsun.coast.helper", category: "audit")

    /// 先把消息拼好，再用单一 `%{public}@` 交给 os_log。
    ///
    /// **不要**写成 `os_log(format, log:, type:, args)` 转发变参 —— Swift 的 `CVarArg...`
    /// 无法这样展开，数组会被当成**一个**参数，编译通过但输出全错。审计日志错了比没有更糟。
    static func log(_ message: String) {
        os_log("%{public}@", log: channel, type: .default, message)
    }

    /// 带调用方 PID 的审计行。光记「做了什么」不够 —— 出事时还要能回答「谁让它做的」。
    /// codesign 门保证了调用方是 Coast 本体，但同一台机器上可能有多个实例/多次启动。
    /// PID 由 `NSXPCConnection.current()` 提供（官方指定的「本次调用来自哪条连接」入口），
    /// 不需要自己按连接铺一套 exported object。
    static func log(_ message: String, caller: NSXPCConnection?) {
        let pid = caller?.processIdentifier ?? -1
        os_log("%{public}@ [caller pid=%d]", log: channel, type: .default, message, pid)
    }
}

// 启动即把「我是谁、从哪来」写进系统日志。
//
// 这是排查「launchd 到底在跑哪一份 helper」时**唯一**的线索来源：daemon 是 launchd 按需拉起的，
// 没有终端可看；而它一旦跑起来就常驻，之后 .app 被替换掉它也照跑不误（版本值在进程启动时就
// 固化了）。没有这行日志，「界面显示助手已启用、行为却是旧版的」这种状态无从辨认。
// 用 os_log 而非 print：print 到 stdout 在 launchd 下直接进 /dev/null。
// 上一条命可能死在接管中间(崩溃/被 SIGKILL/系统强制退出),那时 PF anchor 与
// ip.forwarding 这些**内核状态**原样留着，而新实例的 `active` 是 false，
// 走正常的 stop 路径会直接提前返回 —— 不在这里主动收拾就永远收拾不掉。
Redirector.recoverFromCrashIfNeeded { Audit.log($0) }

// ★ 这里**故意不回收遗留的核心**：helper 是按需 daemon，反复起落是常态，
//   启动时看到「记录里有个活着的核心」多半是它正在正常服役（详见 reapRecordedCoreIfAlive）。

Audit.log("Coast helper 启动 版本=\(HelperVersion.current) "
          + "路径=\(Bundle.main.executableURL?.path ?? "?")")

let service = HelperService()
let listener = NSXPCListener(machServiceName: HelperConstants.machServiceName)
listener.delegate = service
listener.resume()
// launchd 按需拉起本进程，之后 runloop 常驻等连接。
RunLoop.main.run()
