import CBPF
import CoastHelperProtocol
import Darwin
import Foundation

/// 零配置透明代理的 root 侧实现，**整个跑在 helper 里**。
///
/// 三件事：`ip.forwarding=1`、装 PF anchor、周期发 ARP 欺骗应答。
///
/// ★ **为什么欺骗循环在 helper 而不是 app**：被欺骗的设备把本机当网关，一旦我们停止转发
///   它就直接断网，而它的 ARP 缓存要十几分钟才过期。所以「谁负责复原」是这个功能的命门。
///   放在 helper 里，XPC 连接一断（app 正常退出、崩溃、被 SIGKILL）都能触发复原 ——
///   见 `HelperService` 的 `invalidationHandler`。如果像 Qt 版那样把 BPF fd 传回 app 自己发包，
///   app 被 SIGKILL 时没有任何人来发那几个复原包，设备就那么挂着。
final class Redirector: @unchecked Sendable {

    private let queue = DispatchQueue(label: "com.yuehongsun.coast.redirector")
    private var timer: DispatchSourceTimer?
    private var bpfFD: Int32 = -1

    // 当前接管的会话状态（复原时要原样用回去）。
    private var deviceIPs: [[UInt8]] = []
    private var deviceMACs: [ARPPacket.MAC] = []
    private var interface = ""
    private var gatewayIP: [UInt8] = []
    private var gatewayMAC = ARPPacket.MAC.broadcast
    private var selfMAC = ARPPacket.MAC.broadcast
    private var pfInstalled = false
    private var forwardingWasOn = false
    private var active = false

    // —— IPv6 会话状态 ——
    // v6 拓扑齐全（路由器 LL + 路由器 MAC 都解析到）时才启用。任一缺失 → `v6Active=false`,
    // 整段 v6 逻辑跳过（不改 ip6.forwarding、不装 inet6 规则、不发 NDP），行为与只做 v4 一致。
    // 为什么两样都要:能投毒(只需 routerLL6)却复原不了(缺 routerMAC6)是最坏情况 —— 设备会一直
    // 把本机当 v6 网关、v6 断网。宁可不接管 v6,也绝不接管一个还不回去的。
    private var routerLL6: [UInt8] = []            // 路由器链路本地地址（16 字节；NDP target）
    private var routerMAC6 = ARPPacket.MAC.broadcast
    private var v6Active = false
    private var forwarding6WasOn = false

    static let anchorName = "coast.redirect"

    // MARK: - 开始

    /// 返回 nil = 成功，否则是失败原因。**失败时保证已回滚**（不会留下半装的状态）。
    ///
    /// `deviceMACStrings` 与 `deviceIPStrings` **一一对应**。MAC 解析不了的那一台**直接跳过**，
    /// 不代理它 —— 宁可漏一台，也绝不退回广播欺骗（那会污染全网 ARP，见协议注释）。
    func start(deviceIPs deviceIPStrings: [String], deviceMACs deviceMACStrings: [String],
               interface: String,
               gatewayIP gatewayIPString: String, gatewayMAC gatewayMACString: String,
               redirPort: Int, dnsPort: Int,
               routerLL6 routerLL6String: String, routerMAC6 routerMAC6String: String,
               deviceV6s deviceV6Strings: [String]) -> String? {
        queue.sync {
            stopLocked()   // 幂等：换设备列表时先把上一轮干净收掉

            // ★ interface 会被原样拼进 PF 规则文本喂给 pfctl —— **必须白名单校验**,否则一个
            //   带换行/空格的 interface 串能注入任意 PF 规则(helper 以 root 跑 pfctl)。纵深防御。
            guard InputValidation.isValidInterface(interface) else { return "网卡名非法: \(interface)" }
            guard let gwIP = ARPPacket.ipv4Bytes(gatewayIPString) else { return "网关 IP 非法" }
            guard let gwMAC = ARPPacket.MAC(gatewayMACString) else { return "网关 MAC 非法" }
            guard let localMAC = Self.hardwareAddress(of: interface) else {
                return "取不到 \(interface) 的 MAC"
            }
            // IP 与 MAC 必须同时解析成功，且下标对齐 —— 任一解析失败就丢掉这一台，
            // 保证 deviceIPs[i] 与 deviceMACs[i] 始终指同一台设备。
            var ips: [[UInt8]] = []
            var macs: [ARPPacket.MAC] = []
            for (ipString, macString) in zip(deviceIPStrings, deviceMACStrings) {
                guard let ip = ARPPacket.ipv4Bytes(ipString),
                      let mac = ARPPacket.MAC(macString) else { continue }
                ips.append(ip)
                macs.append(mac)
            }
            guard !ips.isEmpty else { return "没有有效的设备（IP/MAC 都要能解析）" }

            // —— 解析 v6 拓扑 ——：路由器 LL + 路由器 MAC 都齐才启用 v6（见 v6Active 说明）。
            //    routerLL6 必须是**链路本地**（fe80::/10）—— 设备的默认路由下一跳就是它,写别的
            //    地址设备不会拿来当默认路由器。任一不满足 → v6 静默停用,只做 v4(不报错)。
            let ll6 = NDPPacket.ipv6Bytes(routerLL6String) ?? []
            let rmac6 = ARPPacket.MAC(routerMAC6String)
            let ll6IsLinkLocal = ll6.count == 16 && ll6[0] == 0xFE && (ll6[1] & 0xC0) == 0x80
            self.v6Active = ll6IsLinkLocal && rmac6 != nil
            self.routerLL6 = v6Active ? ll6 : []
            self.routerMAC6 = rmac6 ?? ARPPacket.MAC.broadcast
            // PF `from <v6>` 会拼进规则文本 → 每个都过 isValidIPv6（挡注入 + 丢掉解析不了的）。
            let v6sForPF = v6Active
                ? deviceV6Strings.filter { InputValidation.isValidIPv6($0) }
                : []

            self.interface = interface
            self.gatewayIP = gwIP
            self.gatewayMAC = gwMAC
            self.selfMAC = localMAC
            self.deviceIPs = ips
            self.deviceMACs = macs   // 真实 MAC，欺骗与复原都**单播**到它，不碰其它设备

            // 1) 开内核转发。记下原值，复原时**只在原本是关的时候才关回去** ——
            //    用户可能自己开着 forwarding 干别的，我们不该擅自关掉。v6 同理，且只在启用 v6 时才碰。
            forwardingWasOn = Self.ipForwarding()
            if !forwardingWasOn, !Self.setIPForwarding(true) {
                return "开启 ip.forwarding 失败"
            }
            if v6Active {
                forwarding6WasOn = Self.ip6Forwarding()
                if !forwarding6WasOn, !Self.setIP6Forwarding(true) {
                    if !forwardingWasOn { _ = Self.setIPForwarding(false) }
                    return "开启 ip6.forwarding 失败"
                }
            }

            // 2) 装 PF anchor。**用实际接管的这批 IP**（ips，已过滤掉 MAC 解析失败的），
            //    不是原始入参 —— 否则会给一台我们并不欺骗、流量根本不会到本机的设备装 rdr 规则。
            let ipStringsForPF = ips.map { $0.map(String.init).joined(separator: ".") }
            if let error = installPF(deviceIPs: ipStringsForPF, deviceV6s: v6sForPF,
                                     redirPort: redirPort, dnsPort: dnsPort) {
                if v6Active, !forwarding6WasOn { _ = Self.setIP6Forwarding(false) }
                if !forwardingWasOn { _ = Self.setIPForwarding(false) }
                return error
            }
            pfInstalled = true

            // 3) 打开 BPF，起欺骗定时器。失败同样回滚前两步。
            guard openBPF(interface: interface) else {
                uninstallPF()
                if v6Active, !forwarding6WasOn { _ = Self.setIP6Forwarding(false) }
                if !forwardingWasOn { _ = Self.setIPForwarding(false) }
                return "打开 BPF 失败"
            }

            active = true
            // 落一份崩溃恢复记录。**内核状态活得比进程久** —— PF anchor 和 ip.forwarding
            // 是内核里的东西，helper 被 SIGKILL 掉时它们原样留着，而新拉起的实例
            // `active == false`，`stop()` 会直接提前返回，于是这两样**永远回滚不了**。
            // 后果不是「功能失效」而是「rdr 规则继续把流量重定向到一个没人监听的端口」——
            // 正是这套设计极力避免的那种设备断网。
            Self.writeCrashRecord(forwardingWasOn: forwardingWasOn,
                                  forwarding6WasOn: v6Active ? forwarding6WasOn : nil)
            let source = DispatchSource.makeTimerSource(queue: queue)
            source.schedule(deadline: .now(), repeating: 1.0)  // 每秒重发，压过设备的正常 ARP/NDP 学习
            source.setEventHandler { [weak self] in
                self?.sendSpoof()
                self?.sendNDPSpoof()   // v6 未启用时自身 no-op
            }
            source.resume()
            timer = source
            sendSpoof()      // 立即发一轮，不等第一个 tick
            sendNDPSpoof()
            return nil
        }
    }

    // MARK: - 跨进程崩溃恢复

    /// 记录「此刻正处于接管中」，以及接管前 `ip.forwarding` 的原值。
    ///
    /// 放在 `/var/db/` 下：root 可写、不随用户数据迁移，重启后也还在（PF anchor 本身不跨
    /// 重启，但 `ip.forwarding` 若被写进 sysctl 配置就会跨；宁可多恢复一次也不要漏）。
    ///
    /// `COAST_REDIRECT_RECORD` 可改写路径，仅用于验证恢复逻辑本身。生产上够不着：
    /// launchd 拉起的 daemon 不继承用户环境，这个变量只有直接执行二进制时才可能存在。
    private static var crashRecordPath: String {
        ProcessInfo.processInfo.environment["COAST_REDIRECT_RECORD"]
            ?? "/var/db/com.yuehongsun.coast.helper.redirect"
    }

    /// 记录格式是两行 `v4=<0/1>` / `v6=<0/1>`。`forwarding6WasOn == nil` 表示这次没启用 v6
    /// （不写 `v6=` 行 → 恢复时不碰 ip6.forwarding）。
    ///
    /// 兼容旧格式:早于 v6 支持的记录是**单个字符** `"1"`/`"0"`,`recoverFromCrashIfNeeded` 里
    /// 找不到 `v4=` 时会退回按整串解读。旧记录必然没有 v6（功能是新加的），无需为它写 `v6=`。
    private static func writeCrashRecord(forwardingWasOn: Bool, forwarding6WasOn: Bool?) {
        var text = "v4=\(forwardingWasOn ? 1 : 0)"
        if let v6 = forwarding6WasOn { text += "\nv6=\(v6 ? 1 : 0)" }
        try? text.write(toFile: crashRecordPath, atomically: true, encoding: .utf8)
    }

    private static func clearCrashRecord() {
        try? FileManager.default.removeItem(atPath: crashRecordPath)
    }

    /// helper 启动时调用：上次是不是死在接管中间？是就把内核状态收拾干净。
    ///
    /// 只做**明确属于我们**的回滚：清空我们自己命名的 anchor（不碰别人的 PF 规则），
    /// 以及把 `ip.forwarding` 恢复成记录里的原值。没有记录就什么都不做 —— 正常退出的
    /// 上一条命已经自己收拾过了。
    ///
    /// ARP 不在此列：欺骗随进程消失，设备的 ARP 缓存会自行老化（十几分钟），
    /// 而我们已经不知道当时接管的是哪几台、真网关 MAC 是什么，无从定向复原。
    static func recoverFromCrashIfNeeded(log: (String) -> Void) {
        guard let record = try? String(contentsOfFile: crashRecordPath, encoding: .utf8) else { return }
        // 解析两行 `v4=`/`v6=`；找不到 `v4=` 则退回旧的单字符格式（整串 == "1"）。
        var fields: [String: String] = [:]
        for line in record.split(whereSeparator: { $0 == "\n" || $0 == "\r" }) {
            let parts = line.split(separator: "=", maxSplits: 1)
            if parts.count == 2 { fields[String(parts[0]).trimmingCharacters(in: .whitespaces)] =
                String(parts[1]).trimmingCharacters(in: .whitespaces) }
        }
        let trimmed = record.trimmingCharacters(in: .whitespacesAndNewlines)
        let forwardingWasOn = (fields["v4"] ?? trimmed) == "1"
        // v6 行缺失（旧记录 / 上次没启用 v6）→ 保持 nil，不去碰 ip6.forwarding。
        let forwarding6WasOn: Bool? = fields["v6"].map { $0 == "1" }

        log("检测到上次接管未正常收尾(helper 疑似崩溃/被强杀)，正在回滚内核状态")
        _ = runPfctlStatic(["-a", anchorName, "-F", "all"])   // 一并清掉 v4+v6 rdr 规则
        if !forwardingWasOn { _ = setIPForwarding(false) }
        if let v6WasOn = forwarding6WasOn, !v6WasOn { _ = setIP6Forwarding(false) }
        clearCrashRecord()
        let v6Note = forwarding6WasOn.map { "；ip6.forwarding 恢复为 \($0 ? "1(原本就开着)" : "0")" } ?? ""
        log("已清空 PF anchor \(anchorName);ip.forwarding 恢复为 \(forwardingWasOn ? "1(原本就开着)" : "0")\(v6Note)")
    }

    /// 静态版 pfctl —— 恢复发生在任何 Redirector 实例产生之前。
    private static func runPfctlStatic(_ arguments: [String]) -> Bool {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/sbin/pfctl")
        task.arguments = arguments
        task.standardOutput = FileHandle.nullDevice
        task.standardError = FileHandle.nullDevice
        do { try task.run() } catch { return false }
        task.waitUntilExit()
        return task.terminationStatus == 0
    }

    // MARK: - 停止 + 复原

    func stop() {
        queue.sync { stopLocked() }
    }

    /// 调用方必须已持有 `queue`。**复原是这里的第一要务，即使某步失败也要把后面都走完。**
    private func stopLocked() {
        guard active else {
            // 没在跑也要保证 BPF 关掉（start 半途失败可能留下它）
            if bpfFD >= 0 { close(bpfFD); bpfFD = -1 }
            return
        }
        timer?.cancel(); timer = nil

        // ★ 复原 ARP（+ v6 时复原 NDP）：给每台设备把网关条目改回**真网关 MAC**。发三遍加冗余 ——
        //   丢一个包就意味着一台设备断网十几分钟，这里的重发绝对值得。ARP 与 NDP 复原在同一个循环里、
        //   同一个 bpfFD 关闭之前发完 —— 顺序不重要，两族地址各走各的邻居缓存。
        if bpfFD >= 0 {
            for _ in 0..<3 {
                // deviceIPs 与 deviceMACs 下标对齐（start 里保证），直接 zip 单播给每台设备本人
                for (ip, mac) in zip(deviceIPs, deviceMACs) {
                    let frame = ARPPacket.reply(senderMAC: gatewayMAC, senderIP: gatewayIP,
                                                targetMAC: mac, targetIP: ip)
                    _ = frame.withUnsafeBytes { write(bpfFD, $0.baseAddress, $0.count) }
                }
                if v6Active {
                    for mac in deviceMACs {
                        let frame = NDPPacket.restore(deviceMAC: mac, selfMAC: selfMAC,
                                                      routerLL6: routerLL6, routerMAC6: routerMAC6)
                        _ = frame.withUnsafeBytes { write(bpfFD, $0.baseAddress, $0.count) }
                    }
                }
                usleep(50_000)
            }
            close(bpfFD); bpfFD = -1
        }

        if pfInstalled { uninstallPF(); pfInstalled = false }
        // forwarding 只回滚我们开的那一次（v6 同理，且只在这次启用过 v6 时才碰）
        if !forwardingWasOn { _ = Self.setIPForwarding(false) }
        if v6Active, !forwarding6WasOn { _ = Self.setIP6Forwarding(false) }
        Self.clearCrashRecord()   // 正常收尾了，不必再让下次启动去恢复

        active = false
        v6Active = false
        deviceIPs = []; deviceMACs = []
        routerLL6 = []
    }

    // MARK: - ARP 欺骗

    private func sendSpoof() {
        guard bpfFD >= 0 else { return }
        // **单播**给每台被选中的设备本人：告诉它「网关 IP 的 MAC 是本机」，它把上行发给我们。
        // 绝不广播 —— 那会让全网设备都把流量引过来，远超用户只选了这几台的意图。
        for (ip, mac) in zip(deviceIPs, deviceMACs) {
            let frame = ARPPacket.reply(senderMAC: selfMAC, senderIP: gatewayIP,
                                        targetMAC: mac, targetIP: ip)
            _ = frame.withUnsafeBytes { write(bpfFD, $0.baseAddress, $0.count) }
        }
    }

    // MARK: - NDP 欺骗（IPv6）

    /// `sendSpoof` 的 v6 对应物：给每台设备发 NDP 邻居通告，宣称「路由器链路本地地址在本机 MAC」。
    ///
    /// L2 单播到设备 MAC（只此设备收得到）、L3 组播 `ff02::1`（设备是成员必处理）—— 组合起来等价于
    /// 「只欺骗这一台」，与 v4 单播 ARP 同精神。**只需设备 MAC + 路由器 LL/MAC**，不依赖设备自己的
    /// v6 地址（那份只用来写 PF rdr 的源匹配）。v6 未启用（`v6Active==false`）时整段 no-op。
    private func sendNDPSpoof() {
        guard bpfFD >= 0, v6Active else { return }
        for mac in deviceMACs {
            let frame = NDPPacket.poison(deviceMAC: mac, selfMAC: selfMAC, routerLL6: routerLL6)
            _ = frame.withUnsafeBytes { write(bpfFD, $0.baseAddress, $0.count) }
        }
    }

    // MARK: - BPF

    private func openBPF(interface: String) -> Bool {
        // /dev/bpf0..255 里找一个能打开的
        for index in 0..<256 {
            let fd = open("/dev/bpf\(index)", O_RDWR)
            if fd < 0 { continue }
            var ifr = ifreq()
            withUnsafeMutableBytes(of: &ifr.ifr_name) { raw in
                for (offset, byte) in interface.utf8.prefix(15).enumerated() {
                    raw[offset] = byte
                }
            }
            if ioctl(fd, COAST_BIOCSETIF, &ifr) < 0 {   // 绑到网卡
                close(fd); continue
            }
            var enable: UInt32 = 1
            _ = ioctl(fd, COAST_BIOCSHDRCMPLT, &enable)  // 我们自己填以太源地址，别让内核覆盖
            bpfFD = fd
            return true
        }
        return false
    }

    // MARK: - PF

    /// 把规则写进一个专用 anchor，只影响这些源 IP，绝不动用户既有的 PF 配置。
    ///
    /// `deviceV6s`：被接管设备的**全局/ULA** v6 源地址（可空 = 无 v6 或没发现任何 v6 地址）。
    /// v6 只重定向 **TCP**：v4 那条 `udp port 53 → dns` 是为了让域名走 fake-ip；v6 的 DNS 走向
    /// 更杂（设备常有独立的 v6 解析器），且核心的 DNS 只监听 v4（`dns.listen: 0.0.0.0`），
    /// 硬把 v6 的 :53 重定向到 `::1` 会打到一个没人监听的口。所以 v6 的 DNS 让设备照旧自己解析
    /// —— 拿到真实 AAAA 后走 v6 TCP，仍被这条 rdr 收进核心（按 IP/GEOIP 规则代理，只是域名匹配
    /// 退化成按 IP 匹配，不影响是否代理）。**这是有意的取舍,不是遗漏。**
    private func installPF(deviceIPs: [String], deviceV6s: [String],
                          redirPort: Int, dnsPort: Int) -> String? {
        var rules = ""
        for ip in deviceIPs {
            // TCP 全部重定向到 redir 口；UDP :53 单独重定向到 DNS 口（域名要走 fake-ip）
            rules += "rdr pass on \(interface) inet proto tcp from \(ip) to any -> 127.0.0.1 port \(redirPort)\n"
            rules += "rdr pass on \(interface) inet proto udp from \(ip) to any port 53 -> 127.0.0.1 port \(dnsPort)\n"
        }
        for v6 in deviceV6s {
            // 目的用 `::1`（核心的 redir 监听在所有接口上，含 v6 环回）。只 TCP,理由见上。
            rules += "rdr pass on \(interface) inet6 proto tcp from \(v6) to any -> ::1 port \(redirPort)\n"
        }
        // 经 stdin 灌进 anchor：pfctl -a <anchor> -f -
        guard runPfctl(["-a", Self.anchorName, "-f", "-"], stdin: rules) else {
            return "pfctl 装规则失败"
        }
        // 确保 PF 本身是开的（-E 引用计数，卸载时 -X 对应）。已开时 -E 也安全。
        _ = runPfctl(["-E"], stdin: nil)
        return nil
    }

    private func uninstallPF() {
        _ = runPfctl(["-a", Self.anchorName, "-F", "all"], stdin: nil)  // 清空我们的 anchor
    }

    @discardableResult
    private func runPfctl(_ arguments: [String], stdin: String?) -> Bool {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/sbin/pfctl")
        task.arguments = arguments
        task.standardOutput = FileHandle.nullDevice
        task.standardError = FileHandle.nullDevice
        if let stdin {
            let pipe = Pipe()
            task.standardInput = pipe
            guard (try? task.run()) != nil else { return false }
            pipe.fileHandleForWriting.write(Data(stdin.utf8))
            pipe.fileHandleForWriting.closeFile()
        } else {
            guard (try? task.run()) != nil else { return false }
        }
        task.waitUntilExit()
        return task.terminationStatus == 0
    }

    // MARK: - sysctl

    static func ipForwarding() -> Bool {
        var value: Int32 = 0
        var size = MemoryLayout<Int32>.size
        return sysctlbyname("net.inet.ip.forwarding", &value, &size, nil, 0) == 0 && value == 1
    }

    static func setIPForwarding(_ on: Bool) -> Bool {
        var value: Int32 = on ? 1 : 0
        return sysctlbyname("net.inet.ip.forwarding", nil, nil, &value, MemoryLayout<Int32>.size) == 0
    }

    static func ip6Forwarding() -> Bool {
        var value: Int32 = 0
        var size = MemoryLayout<Int32>.size
        return sysctlbyname("net.inet6.ip6.forwarding", &value, &size, nil, 0) == 0 && value == 1
    }

    static func setIP6Forwarding(_ on: Bool) -> Bool {
        var value: Int32 = on ? 1 : 0
        return sysctlbyname("net.inet6.ip6.forwarding", nil, nil, &value, MemoryLayout<Int32>.size) == 0
    }

    // MARK: - 网卡 MAC

    static func hardwareAddress(of interface: String) -> ARPPacket.MAC? {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return nil }
        defer { freeifaddrs(head) }
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            guard String(cString: entry.pointee.ifa_name) == interface,
                  let addr = entry.pointee.ifa_addr,
                  addr.pointee.sa_family == UInt8(AF_LINK) else { continue }
            let bytes = addr.withMemoryRebound(to: sockaddr_dl.self, capacity: 1) { dl -> [UInt8] in
                let macOffset = Int(dl.pointee.sdl_nlen)
                return withUnsafeBytes(of: dl.pointee.sdl_data) { raw -> [UInt8] in
                    (0..<6).map { raw[macOffset + $0] }
                }
            }
            guard bytes.count == 6, bytes != [0, 0, 0, 0, 0, 0] else { continue }
            return ARPPacket.MAC(bytes: bytes)
        }
        return nil
    }
}
