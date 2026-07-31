import Foundation

/// 局域网设备**发现**（只读）。
///
/// 刻意只做「看得到」这一半：读系统的 ARP/邻居表 + OUI 厂商表 + 反查主机名，
/// 列出这个网络里有哪些设备。**不包含**把它们的流量引到本机（那需要二层投毒 + 用户态栈，
/// 见 `docs/gateway-evaluation.md`，尚待决策）。
///
/// 这样切分是刻意的：设备发现本身是个**完整的小功能**，不依赖网关；
/// 而「代理某台设备」如果没有网关就只能是个点了没反应的假开关，不如不放。
public struct LanBrowser: Sendable {

    public struct Device: Sendable, Equatable, Identifiable {
        /// 规范化后的 MAC（小写、冒号分隔、每段补零）。局域网设备的 IP 会随 DHCP 漂移，
        /// MAC 才是稳定身份。
        public var mac: String
        public var ip: String
        public var interface: String
        public var vendor: String = ""
        public var hostname: String = ""
        /// 是否本机默认网关（路由器）。
        public var isGateway: Bool = false

        public var id: String { mac }

        /// 显示名：主机名 > 厂商 > IP。
        public var displayName: String {
            if !hostname.isEmpty { return hostname }
            if !vendor.isEmpty { return vendor }
            return ip
        }

        /// 设备类型（用于选图标）。口径对齐 C++ `LanScanner::classify` 的关键词表，
        /// 但**只用得到主机名/厂商** —— 端口扫描与 mDNS 服务发现属于网关那条线，这里不做。
        public var typeKey: String {
            if isGateway { return "router" }
            let text = (hostname + " " + vendor).lowercased()
            func has(_ keyword: String) -> Bool { text.contains(keyword) }

            if has("ipad") { return "tablet" }
            if has("iphone") { return "phone" }
            if has("macbook") || has("imac") || has("mac-mini") || has("mac mini")
                || has("desktop") || has("laptop") { return "computer" }
            if has("android") || has("pixel") || has("redmi") || has("xiaomi") || has("huawei")
                || has("honor") || has("oppo") || has("vivo") || has("oneplus")
                || has("galaxy") || has("samsung") { return "phone" }
            if has("tv") || has("bravia") || has("aquos") || has("chromecast")
                || has("firetv") || has("appletv") || has("shield") { return "tvbox" }
            if has("router") || has("gateway") || has("openwrt") || has("mikrotik")
                || has("tp-link") || has("tplink") || has("netgear") || has("ubiquiti") { return "router" }
            if has("synology") || has("qnap") || has("truenas") || has("nas") { return "nas" }
            if has("playstation") || has("nintendo") || has("xbox") { return "game" }
            if has("printer") || has("epson") || has("canon") || has("brother") { return "printer" }
            if has("camera") || has("ipcam") || has("hikvision") || has("dahua") { return "camera" }
            if has("sonos") || has("homepod") || has("echo") { return "speaker" }
            if has("apple") { return "computer" }
            return "unknown"
        }
    }

    public init() {}

    /// 扫一轮。**不发包、不需要 root** —— 只读系统已有的邻居表，所以它只看得到
    /// 「最近有过通信」的设备。想看到全部在线设备需要主动探测，那属于网关那条线。
    public func scan() async -> [Device] {
        let gateways = Self.defaultGateways()
        var devices = Self.arpTable()

        for index in devices.indices {
            devices[index].vendor = OUIDatabase.shared.vendor(for: devices[index].mac)
            devices[index].isGateway = gateways.contains(devices[index].ip)
        }
        // 反查主机名会走 DNS/mDNS，逐个串行会很慢（一台超时就拖住整轮），并发做。
        devices = await Self.resolveHostnames(devices)
        // 网关置顶，其余按 IP 的数值顺序 —— 字符串排序会把 .10 排在 .2 前面。
        return devices.sorted { lhs, rhs in
            if lhs.isGateway != rhs.isGateway { return lhs.isGateway }
            return Self.ipSortKey(lhs.ip) < Self.ipSortKey(rhs.ip)
        }
    }

    // MARK: - ARP 表

    /// 解析 `arp -an`。
    ///
    /// 用子进程而不是自己 `sysctl(NET_RT_FLAGS)`：后者要手工走 `rt_msghdr` + `sockaddr_dl`
    /// 的变长结构，在不同 macOS 版本上是出过变动的地方；而这个调用每几秒才一次，
    /// 一个短命子进程的代价完全可以接受。
    static func arpTable() -> [Device] {
        guard let output = run("/usr/sbin/arp", ["-an"]) else { return [] }
        return output.split(separator: "\n").compactMap { parseARPLine(String($0)) }
    }

    /// 一行形如：
    /// `? (192.168.1.1) at 3c:84:6a:1:2:3 on en0 ifscope [ethernet]`
    /// 未解析出来的是：`? (192.168.1.9) at (incomplete) on en0 ...`
    static func parseARPLine(_ line: String) -> Device? {
        // IP 在第一对圆括号里
        guard let open = line.firstIndex(of: "("),
              let close = line[open...].firstIndex(of: ")") else { return nil }
        let ip = String(line[line.index(after: open)..<close])
        guard isUnicastIPv4(ip) else { return nil }

        let fields = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
        guard let atIndex = fields.firstIndex(of: "at"), atIndex + 1 < fields.count else { return nil }
        let rawMAC = fields[atIndex + 1]
        guard let mac = normalizeMAC(rawMAC) else { return nil }   // (incomplete) 会在这里被丢掉

        var interface = ""
        if let onIndex = fields.firstIndex(of: "on"), onIndex + 1 < fields.count {
            interface = fields[onIndex + 1]
        }
        return Device(mac: mac, ip: ip, interface: interface)
    }

    /// MAC 规范化：小写、冒号分隔、**每段补足两位**。
    ///
    /// ★ 补零这一步是必需的：macOS 的 `arp -an` 打印时**不补前导零**（`3c:84:6a:1:2:3`），
    ///   而 OUI 表里是 `3C846A` 这种定长写法。不补零的话前缀取出来是 `3c846a` 还算对，
    ///   但像 `1:2:3:4:5:6` 这种就会取成 `1:2:3` —— 厂商永远查不到，而且不会报错，
    ///   只是所有设备的厂商栏莫名其妙全空。
    static func normalizeMAC(_ raw: String) -> String? {
        let parts = raw.split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count == 6 else { return nil }
        var octets: [String] = []
        for part in parts {
            guard part.count <= 2, !part.isEmpty,
                  let value = UInt8(part, radix: 16) else { return nil }
            octets.append(String(format: "%02x", value))
        }
        let mac = octets.joined(separator: ":")
        // 全 0 不是设备
        guard mac != "00:00:00:00:00:00" else { return nil }
        // ★ 组播/广播 MAC 不是设备。判据是**首字节的最低位**（I/G 位）为 1 —— 这是以太网标准，
        //   一条规则同时盖住 IPv4 组播 `01:00:5e:*`、IPv6 组播 `33:33:*` 和广播 `ff:ff:*`。
        //   邻居表里**确实会有**这些条目（实测 `224.0.0.251 / 01:00:5e:00:00:fb` 就在里面），
        //   不滤掉的话「mdns.mcast.net」会作为一台设备出现在列表里。
        guard let firstOctet = UInt8(octets[0], radix: 16), firstOctet & 0x01 == 0 else { return nil }
        return mac
    }

    // MARK: - 默认网关

    /// `netstat -rn -f inet` 里 `default` 那几行的下一跳。可能不止一个（多网卡）。
    static func defaultGateways() -> Set<String> {
        guard let output = run("/usr/sbin/netstat", ["-rn", "-f", "inet"]) else { return [] }
        var result = Set<String>()
        for line in output.split(separator: "\n") {
            let fields = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
            guard fields.count >= 2, fields[0] == "default", isIPv4(fields[1]) else { continue }
            result.insert(fields[1])
        }
        return result
    }

    // MARK: - 主机名

    static func resolveHostnames(_ devices: [Device]) async -> [Device] {
        await withTaskGroup(of: (Int, String).self) { group in
            for (index, device) in devices.enumerated() {
                group.addTask { (index, reverseLookup(device.ip)) }
            }
            var result = devices
            for await (index, name) in group where !name.isEmpty {
                result[index].hostname = name
            }
            return result
        }
    }

    /// 反查主机名。查不到就留空 —— 局域网里多数设备本来就没有 PTR 记录，这是常态不是错误。
    static func reverseLookup(_ ip: String) -> String {
        var hints = addrinfo(ai_flags: 0, ai_family: AF_INET, ai_socktype: SOCK_STREAM,
                             ai_protocol: 0, ai_addrlen: 0, ai_canonname: nil,
                             ai_addr: nil, ai_next: nil)
        var info: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(ip, nil, &hints, &info) == 0, let first = info else { return "" }
        defer { freeaddrinfo(info) }

        var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        let code = getnameinfo(first.pointee.ai_addr, first.pointee.ai_addrlen,
                               &host, socklen_t(NI_MAXHOST), nil, 0, NI_NAMEREQD)
        guard code == 0 else { return "" }
        var name = String(cString: host)
        // 去掉 mDNS 的 .local. 后缀，列表里那一串没有信息量
        if name.hasSuffix(".") { name.removeLast() }
        if name.hasSuffix(".local") { name.removeLast(6) }
        return name == ip ? "" : name
    }

    // MARK: - 工具

    static func isIPv4(_ text: String) -> Bool {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return false }
        return parts.allSatisfy { part in
            guard let value = Int(part), value >= 0, value <= 255 else { return false }
            return true
        }
    }

    /// 组播（224.0.0.0/4）与受限广播。与上面的 MAC I/G 位判定互为双保险：
    /// 两边都查是因为邻居表里这两类条目的写法并不总是配套的。
    static func isUnicastIPv4(_ text: String) -> Bool {
        guard isIPv4(text) else { return false }
        let parts = text.split(separator: ".").compactMap { Int($0) }
        guard let first = parts.first else { return false }
        if first >= 224 { return false }              // 224-239 组播、240+ 保留
        if text == "255.255.255.255" { return false } // 受限广播
        return true
    }

    /// IP → 可比较的整数。按字符串排会把 `.10` 排在 `.2` 前面。
    static func ipSortKey(_ ip: String) -> UInt32 {
        let parts = ip.split(separator: ".").compactMap { UInt32($0) }
        guard parts.count == 4 else { return 0 }
        return parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]
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

/// OUI（MAC 前三字节）→ 厂商名。表来自 `assets/oui.txt`，与 Qt 版共用同一份。
public final class OUIDatabase: @unchecked Sendable {
    public static let shared = OUIDatabase()

    private var table: [String: String] = [:]
    private var loaded = false
    private let lock = NSLock()

    private init() {}

    public func vendor(for mac: String) -> String {
        lock.lock()
        defer { lock.unlock() }
        if !loaded { load() }
        // 前三字节，去掉冒号、转大写 —— 表里是 "AABBCC<TAB>Vendor"
        let prefix = mac.replacingOccurrences(of: ":", with: "").uppercased().prefix(6)
        return table[String(prefix)] ?? ""
    }

    /// 调用方必须已持有 `lock`。近 4 万行，只在首次查询时读一次。
    private func load() {
        loaded = true
        guard let url = Resources.asset("oui.txt"),
              let text = try? String(contentsOf: url, encoding: .utf8) else { return }
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            guard !line.hasPrefix("#") else { continue }
            let parts = line.split(separator: "\t", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2, parts[0].count == 6 else { continue }
            table[String(parts[0]).uppercased()] = String(parts[1])
        }
    }
}
