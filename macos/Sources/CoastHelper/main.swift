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
    private let redirector = Redirector()
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

    func startRedirect(deviceIPsCommaSep: String, deviceMACsCommaSep: String,
                       interface: String, gatewayIP: String, gatewayMAC: String,
                       redirPort: Int, dnsPort: Int,
                       routerLL6: String, routerMAC6: String, deviceV6sCommaSep: String,
                       withReply reply: @escaping (Bool, String) -> Void) {
        // 接管别人的流量是本程序做过的最重的一件事，日志必须记全:接管了谁、冒充哪个网关、
        // 走哪块网卡。出问题时(局域网异常、某台设备断网)这是唯一能自证「我做了什么」的记录。
        let v6Note = routerLL6.isEmpty ? "" : " v6路由器=\(routerLL6)/\(routerMAC6) v6设备=[\(deviceV6sCommaSep)]"
        Audit.log("startRedirect 设备=[\(deviceIPsCommaSep)] 网卡=\(interface) "
                  + "网关=\(gatewayIP)/\(gatewayMAC) redir=\(redirPort) dns=\(dnsPort)\(v6Note)",
                  caller: NSXPCConnection.current())
        // 先取住这条连接:reply 之后 `NSXPCConnection.current()` 就不在本次调用上下文里了。
        let caller = NSXPCConnection.current()
        let ips = deviceIPsCommaSep.split(separator: ",", omittingEmptySubsequences: false).map(String.init)
        let macs = deviceMACsCommaSep.split(separator: ",", omittingEmptySubsequences: false).map(String.init)
        // v6 地址表用 omittingEmptySubsequences:true —— 空串（无 v6）不该变成一个空元素。
        let v6s = deviceV6sCommaSep.split(separator: ",", omittingEmptySubsequences: true).map(String.init)
        if let error = redirector.start(deviceIPs: ips, deviceMACs: macs, interface: interface,
                                        gatewayIP: gatewayIP, gatewayMAC: gatewayMAC,
                                        redirPort: redirPort, dnsPort: dnsPort,
                                        routerLL6: routerLL6, routerMAC6: routerMAC6,
                                        deviceV6s: v6s) {
            reply(false, error)
        } else {
            // **只在真的接管成功后**才记下发起方。失败时记下的话会留一个悬空 owner ——
            // 那条连接稍后断开时会去撤销一个根本不存在的接管（无害但会误导日志），
            // 更糟的是它会挡住下一次真正的接管去认领 owner。
            queue.sync { redirectOwner = caller }
            reply(true, "")
        }
    }

    /// 发起接管的那条连接。只有它断开才意味着「app 没了，把设备放回去」。
    private var redirectOwner: NSXPCConnection?

    private func clientVanished(_ connection: NSXPCConnection?, reason: String) {
        queue.sync {
            guard let owner = redirectOwner, owner === connection else { return }
            Audit.log("接管发起方的连接已\(reason) —— app 可能已崩溃/被强杀，撤销接管")
            redirectOwner = nil
        }
        redirector.stop()
    }

    func terminate(withReply reply: @escaping () -> Void) {
        Audit.log("terminate:收到退出请求,先还原接管与核心")
        // 顺序要紧:先还原 ARP 接管(否则局域网里那些设备会继续把流量发给一个不存在的网关),
        // 再停核心。反过来的话,接管还在、转发目标已经没了 —— 被接管的设备直接断网。
        redirector.stop()
        queue.sync { stopCoreLocked() }
        Audit.log("terminate:已收拾干净,退出")
        reply()
        // 让 reply 有机会送达对端再退。launchd 会在下次连接时按需拉起新版。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { exit(0) }
    }

    func stopRedirect(withReply reply: @escaping (Bool, String) -> Void) {
        Audit.log("stopRedirect", caller: NSXPCConnection.current())
        queue.sync { redirectOwner = nil }
        redirector.stop()
        reply(true, "")
    }

    /// 调用方必须已持有 `queue`。
    private func stopCoreLocked() {
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

Audit.log("Coast helper 启动 版本=\(HelperVersion.current) "
          + "路径=\(Bundle.main.executableURL?.path ?? "?")")

let service = HelperService()
let listener = NSXPCListener(machServiceName: HelperConstants.machServiceName)
listener.delegate = service
listener.resume()
// launchd 按需拉起本进程，之后 runloop 常驻等连接。
RunLoop.main.run()
