import Foundation

/// 默认网关的三要素：IP、MAC、出口网卡。接管设备时要把它们下发给 helper
/// （欺骗时冒充网关、复原时用真网关 MAC）。
public enum LanTopology {

    public struct Gateway: Sendable, Equatable {
        public let ip: String
        public let mac: String
        public let interface: String
    }

    /// 读 `netstat -rn -f inet` 拿默认路由的下一跳 + 出口网卡，再从 ARP 表查它的 MAC。
    ///
    /// 三样缺一不可：没有网关 MAC 就没法发复原包 —— 而复原包发不出去，被接管的设备会断网
    /// 十几分钟。所以任何一样取不到都返回 nil，让调用方**根本不开始接管**，而不是带着残缺信息硬上。
    /// 本机所有网卡的 MAC（含虚拟网卡）。
    ///
    /// 用途是「这个 IP 现在归本机占着，不是别人在冒充」。必须**含虚拟网卡** ——
    /// 判据是「是不是我们自己」，而不是「是不是主网卡」，漏掉一张就会把自己报成攻击者。
    /// `getifaddrs` 不需要 root。
    public static func localMACs() -> Set<String> {
        var result: Set<String> = []
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return result }
        defer { freeifaddrs(head) }
        for pointer in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard let address = pointer.pointee.ifa_addr,
                  address.pointee.sa_family == UInt8(AF_LINK) else { continue }
            let link = UnsafeRawPointer(address).assumingMemoryBound(to: sockaddr_dl.self)
            let length = Int(link.pointee.sdl_alen)
            guard length == 6 else { continue }
            let base = UnsafeRawPointer(link) + MemoryLayout<sockaddr_dl>.offset(of: \.sdl_data)!
                + Int(link.pointee.sdl_nlen)
            let bytes = base.assumingMemoryBound(to: UInt8.self)
            let mac = (0..<6).map { String(format: "%02x", bytes[$0]) }.joined(separator: ":")
            if mac != "00:00:00:00:00:00" { result.insert(mac) }
        }
        return result
    }

    public static func defaultGateway() -> Gateway? {
        guard let (ip, interface) = defaultRoute(), let mac = arpLookup(ip: ip) else { return nil }
        return Gateway(ip: ip, mac: mac, interface: interface)
    }

    static func defaultRoute() -> (ip: String, interface: String)? {
        guard let output = run("/usr/sbin/netstat", ["-rn", "-f", "inet"]) else { return nil }
        for line in output.split(separator: "\n") {
            let fields = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
            guard fields.count >= 4, fields[0] == "default", LanBrowser.isIPv4(fields[1]) else { continue }
            return (fields[1], fields[3])
        }
        return nil
    }

    /// 从 `arp -n <ip>` 查 MAC。复用 LanBrowser 的行解析（同一套「不补零」的坑）。
    static func arpLookup(ip: String) -> String? {
        guard let output = run("/usr/sbin/arp", ["-n", ip]) else { return nil }
        for line in output.split(separator: "\n") {
            if let device = LanBrowser.parseARPLine(String(line)), device.ip == ip {
                return device.mac
            }
        }
        return nil
    }

    private static func run(_ path: String, _ arguments: [String]) -> String? {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: path)
        task.arguments = arguments
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = FileHandle.nullDevice
        guard (try? task.run()) != nil else { return nil }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()
        return String(data: data, encoding: .utf8)
    }
}
