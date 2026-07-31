import CoastHelperProtocol
import Foundation
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
        connection.invalidationHandler = { [weak self] in self?.redirector.stop() }
        connection.interruptionHandler = { [weak self] in self?.redirector.stop() }
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
        queue.sync {
            stopCoreLocked()

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
        queue.sync {
            stopCoreLocked()
            reply(true, "")
        }
    }

    func startRedirect(deviceIPsCommaSep: String, interface: String,
                       gatewayIP: String, gatewayMAC: String,
                       redirPort: Int, dnsPort: Int,
                       withReply reply: @escaping (Bool, String) -> Void) {
        let ips = deviceIPsCommaSep.split(separator: ",").map(String.init)
        if let error = redirector.start(deviceIPs: ips, interface: interface,
                                        gatewayIP: gatewayIP, gatewayMAC: gatewayMAC,
                                        redirPort: redirPort, dnsPort: dnsPort) {
            reply(false, error)
        } else {
            reply(true, "")
        }
    }

    func stopRedirect(withReply reply: @escaping (Bool, String) -> Void) {
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
    static let current = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "0"
}

let service = HelperService()
let listener = NSXPCListener(machServiceName: HelperConstants.machServiceName)
listener.delegate = service
listener.resume()
// launchd 按需拉起本进程，之后 runloop 常驻等连接。
RunLoop.main.run()
