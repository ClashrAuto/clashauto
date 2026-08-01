import CoastHelperProtocol
import Foundation

/// 默认网关的三要素：IP、MAC、出口网卡。接管设备时要把它们下发给 helper
/// （欺骗时冒充网关、复原时用真网关 MAC）。
public enum LanTopology {

    public struct Gateway: Sendable, Equatable {
        public let ip: String
        public let mac: String
        public let interface: String
    }

    /// IPv6 默认路由器：**链路本地**地址 + MAC + 出口网卡。是 NDP 投毒/复原的目标
    /// （对应 v4 的 `Gateway`）。取不到（网络无 v6、或路由器 MAC 还没进邻居表）返回 nil，
    /// 调用方据此**完全跳过 v6** —— 只做 v4，绝不带着残缺的 v6 拓扑硬上。
    public struct GatewayV6: Sendable, Equatable {
        public let routerLL: String   // fe80::… （不含 %zone）
        public let routerMAC: String
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

    // MARK: - IPv6 拓扑

    /// v6 默认路由器 = `netstat -rn -f inet6` 的默认路由下一跳(链路本地) + 从 `ndp -an` 查它的 MAC。
    ///
    /// 与 v4 `defaultGateway()` 同构、同要求：路由器 MAC 查不到就返回 nil（NDP 复原发不出去，
    /// 设备会 v6 断网十几分钟，宁可不接管 v6）。两条命令都不需要 root。
    public static func defaultGatewayV6() -> GatewayV6? {
        guard let netstat = run("/usr/sbin/netstat", ["-rn", "-f", "inet6"]),
              let route = parseDefaultRouteV6(netstat) else { return nil }
        guard let ndp = run("/usr/sbin/ndp", ["-an"]) else { return nil }
        let neighbors = parseNdpNeighbors(ndp)
        guard let mac = macOf(routerLL: route.gateway, in: neighbors) else { return nil }
        return GatewayV6(routerLL: route.gateway, routerMAC: mac, interface: route.interface)
    }

    /// 被接管设备的**全局/ULA** v6 源地址（供 helper 写 PF `inet6 ... from <v6>` 规则）。
    ///
    /// 从 `ndp -an` 的邻居表里，按 MAC 反查每台设备的 v6 地址，只留可路由的（2000::/3 或 fc00::/7），
    /// 丢掉链路本地/组播（它们不出网，不需要 rdr）。一台设备可能有多个（SLAAC + 隐私地址），全收。
    /// 查不到 = 该设备当前没有可路由 v6 或还没进邻居表：它照样会被 NDP 投毒，只是 v6 走转发而非
    /// 代理，直到下次同步补上（隐私地址会轮换，本就是尽力而为）。
    public static func deviceV6Addresses(ofMACs macs: Set<String>) -> [String] {
        guard !macs.isEmpty, let ndp = run("/usr/sbin/ndp", ["-an"]) else { return [] }
        let wanted = Set(macs.map { $0.lowercased() })
        var seen = Set<String>()
        var result: [String] = []
        for n in parseNdpNeighbors(ndp) where wanted.contains(n.mac) && isRoutableV6(n.ip) {
            if seen.insert(n.ip).inserted { result.append(n.ip) }
        }
        return result
    }

    // MARK: - IPv6 解析（纯函数，可单测）

    /// 解析 `netstat -rn -f inet6` 里的默认路由：取第一条下一跳为**链路本地**的 `default`。
    /// 返回的 gateway 已去掉 `%zone`；interface 优先取下一跳的 zone，缺失时退回 Netif 列。
    static func parseDefaultRouteV6(_ output: String) -> (gateway: String, interface: String)? {
        for line in output.split(separator: "\n") {
            let fields = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
            guard fields.count >= 4, fields[0] == "default" else { continue }
            let rawGateway = fields[1]
            let gateway = stripZone(rawGateway)
            // 只认链路本地下一跳:设备的 v6 默认路由下一跳就是路由器 LL(fe80::/10),别的不是投毒目标。
            guard isLinkLocalV6(gateway) else { continue }
            let zone = rawGateway.contains("%") ? String(rawGateway.split(separator: "%")[1]) : ""
            let interface = zone.isEmpty ? fields[3] : zone
            return (gateway, interface)
        }
        return nil
    }

    /// 解析 `ndp -an` 的邻居表 → `(ip 去 zone, mac 规范化)` 列表。
    /// 跳过表头、`(incomplete)`、组播/广播 MAC（`normalizeMAC` 里按 I/G 位滤掉）。
    static func parseNdpNeighbors(_ output: String) -> [(ip: String, mac: String)] {
        var result: [(ip: String, mac: String)] = []
        for line in output.split(separator: "\n") {
            let fields = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
            guard fields.count >= 2 else { continue }
            if fields[0] == "Neighbor" { continue }   // 表头
            guard let mac = LanBrowser.normalizeMAC(fields[1]) else { continue }  // (incomplete) 等在此丢弃
            result.append((ip: stripZone(fields[0]), mac: mac))
        }
        return result
    }

    /// 在邻居表里找 `routerLL`（去 zone）对应的 MAC。地址用规范化字节比较，避免压缩写法不同导致漏配。
    static func macOf(routerLL: String, in neighbors: [(ip: String, mac: String)]) -> String? {
        guard let target = NDPPacket.ipv6Bytes(routerLL) else { return nil }
        for n in neighbors where NDPPacket.ipv6Bytes(n.ip) == target { return n.mac }
        return nil
    }

    /// 去掉 IPv6 字面量的 `%zone` 尾巴。
    static func stripZone(_ address: String) -> String {
        address.split(separator: "%", maxSplits: 1, omittingEmptySubsequences: false)
            .first.map(String.init) ?? address
    }

    /// 链路本地 `fe80::/10`。
    static func isLinkLocalV6(_ address: String) -> Bool {
        guard let b = NDPPacket.ipv6Bytes(address), b.count == 16 else { return false }
        return b[0] == 0xFE && (b[1] & 0xC0) == 0x80
    }

    /// 可路由的 v6：全局单播 `2000::/3` 或唯一本地 `fc00::/7`。链路本地、组播、未指定都排除。
    static func isRoutableV6(_ address: String) -> Bool {
        guard let b = NDPPacket.ipv6Bytes(address), b.count == 16 else { return false }
        let global = (b[0] & 0xE0) == 0x20   // 2000::/3
        let ula = (b[0] & 0xFE) == 0xFC      // fc00::/7
        return global || ula
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
