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

    static let anchorName = "coast.redirect"

    // MARK: - 开始

    /// 返回 nil = 成功，否则是失败原因。**失败时保证已回滚**（不会留下半装的状态）。
    ///
    /// `deviceMACStrings` 与 `deviceIPStrings` **一一对应**。MAC 解析不了的那一台**直接跳过**，
    /// 不代理它 —— 宁可漏一台，也绝不退回广播欺骗（那会污染全网 ARP，见协议注释）。
    func start(deviceIPs deviceIPStrings: [String], deviceMACs deviceMACStrings: [String],
               interface: String,
               gatewayIP gatewayIPString: String, gatewayMAC gatewayMACString: String,
               redirPort: Int, dnsPort: Int) -> String? {
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

            self.interface = interface
            self.gatewayIP = gwIP
            self.gatewayMAC = gwMAC
            self.selfMAC = localMAC
            self.deviceIPs = ips
            self.deviceMACs = macs   // 真实 MAC，欺骗与复原都**单播**到它，不碰其它设备

            // 1) 开内核转发。记下原值，复原时**只在原本是关的时候才关回去** ——
            //    用户可能自己开着 forwarding 干别的，我们不该擅自关掉。
            forwardingWasOn = Self.ipForwarding()
            if !forwardingWasOn, !Self.setIPForwarding(true) {
                return "开启 ip.forwarding 失败"
            }

            // 2) 装 PF anchor。**用实际接管的这批 IP**（ips，已过滤掉 MAC 解析失败的），
            //    不是原始入参 —— 否则会给一台我们并不欺骗、流量根本不会到本机的设备装 rdr 规则。
            let ipStringsForPF = ips.map { $0.map(String.init).joined(separator: ".") }
            if let error = installPF(deviceIPs: ipStringsForPF, redirPort: redirPort, dnsPort: dnsPort) {
                if !forwardingWasOn { _ = Self.setIPForwarding(false) }
                return error
            }
            pfInstalled = true

            // 3) 打开 BPF，起欺骗定时器。失败同样回滚前两步。
            guard openBPF(interface: interface) else {
                uninstallPF()
                if !forwardingWasOn { _ = Self.setIPForwarding(false) }
                return "打开 BPF 失败"
            }

            active = true
            let source = DispatchSource.makeTimerSource(queue: queue)
            source.schedule(deadline: .now(), repeating: 1.0)  // 每秒重发，压过设备的正常 ARP 学习
            source.setEventHandler { [weak self] in self?.sendSpoof() }
            source.resume()
            timer = source
            sendSpoof()   // 立即发一轮，不等第一个 tick
            return nil
        }
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

        // ★ 复原 ARP：给每台设备发「网关 IP → 真网关 MAC」。发三遍加冗余 ——
        //   丢一个包就意味着一台设备断网十几分钟，这里的重发绝对值得。
        if bpfFD >= 0 {
            for _ in 0..<3 {
                // deviceIPs 与 deviceMACs 下标对齐（start 里保证），直接 zip 单播给每台设备本人
                for (ip, mac) in zip(deviceIPs, deviceMACs) {
                    let frame = ARPPacket.reply(senderMAC: gatewayMAC, senderIP: gatewayIP,
                                                targetMAC: mac, targetIP: ip)
                    _ = frame.withUnsafeBytes { write(bpfFD, $0.baseAddress, $0.count) }
                }
                usleep(50_000)
            }
            close(bpfFD); bpfFD = -1
        }

        if pfInstalled { uninstallPF(); pfInstalled = false }
        // forwarding 只回滚我们开的那一次
        if !forwardingWasOn { _ = Self.setIPForwarding(false) }

        active = false
        deviceIPs = []; deviceMACs = []
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
    private func installPF(deviceIPs: [String], redirPort: Int, dnsPort: Int) -> String? {
        var rules = ""
        for ip in deviceIPs {
            // TCP 全部重定向到 redir 口；UDP :53 单独重定向到 DNS 口（域名要走 fake-ip）
            rules += "rdr pass on \(interface) inet proto tcp from \(ip) to any -> 127.0.0.1 port \(redirPort)\n"
            rules += "rdr pass on \(interface) inet proto udp from \(ip) to any port 53 -> 127.0.0.1 port \(dnsPort)\n"
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
