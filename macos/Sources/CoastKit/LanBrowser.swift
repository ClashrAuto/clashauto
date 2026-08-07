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
        /// 型号串（`iPhone15,2` / `MacBookPro18,3`）。来自 Bonjour `_device-info._tcp`
        /// 的 TXT `model=`，与 Qt 的 `DeviceStore::model` 是同一份数据。
        public var model: String = ""
        /// 是否本机默认网关（路由器）。
        public var isGateway: Bool = false

        public var id: String { mac }

        /// 台账里有、但这轮没扫到的设备也要能构造出来（离线行）——
        /// 凭据还在，不能因为设备暂时不响应就从界面上消失。
        public init(mac: String, ip: String, interface: String) {
            self.mac = mac
            self.ip = ip
            self.interface = interface
        }

        /// 显示名：主机名 > 厂商 > IP > MAC。
        ///
        /// ★ 最后那一档兜底是必需的：离线设备的 `ip` 是空的，而主机名/厂商本来就常常查不到
        ///   （随机 MAC 的手机既没有 PTR 记录、OUI 表里也查不到）—— 三档全空时原来直接返回
        ///   空字符串，界面上就是**一行没有名字、什么都没有的设备**。MAC 至少是这台设备的
        ///   真实身份，认不出是谁也比一片空白强。
        public var displayName: String {
            for candidate in [hostname, vendor, ip] where !candidate.isEmpty { return candidate }
            return mac
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
        // 一台设备只留一行（见 `dedupeByMAC`）。放在反查主机名**之前** ——
        // 反查是每台一次 DNS，重复的那几条白等一轮。
        devices = Self.dedupeByMAC(devices)
        // 反查主机名会走 DNS/mDNS，逐个串行会很慢（一台超时就拖住整轮），并发做。
        devices = await Self.resolveHostnames(devices)
        // 型号靠主机名连接，所以只能在反查之后填。浏览器是常驻的，这里只是取一次快照。
        DeviceModelBrowser.shared.start()
        for index in devices.indices {
            devices[index].model = DeviceModelBrowser.shared.model(mac: devices[index].mac,
                                                                   hostname: devices[index].hostname)
        }
        // 网关置顶，其余按 IP 的数值顺序 —— 字符串排序会把 .10 排在 .2 前面。
        return devices.sorted { lhs, rhs in
            if lhs.isGateway != rhs.isGateway { return lhs.isGateway }
            return Self.ipSortKey(lhs.ip) < Self.ipSortKey(rhs.ip)
        }
    }

    // MARK: - 去重

    /// 按 MAC 去重，一台设备只留一行。
    ///
    /// ★ **同一个 MAC 在邻居表里出现好几次是常态，不是异常。** 两种来源都很普遍：
    ///   一台设备除了 DHCP 地址还留着一个 `169.254.*` 链路本地地址（拿到租约之前
    ///   自配的那个），两条都在表里；多网卡的机器还会在每张网卡上各看到同一台一次。
    ///   实测这台开发 Mac 的 `arp -an`：24 行里有 3 个 MAC 各出现两次。
    ///
    ///   而 `Device.id` **就是 MAC** —— 把重复的 id 交给 SwiftUI 的 `ForEach` 是未定义行为
    ///   （控制台只有一句 "ID … is used by multiple child views"），界面上表现为
    ///   那几行内容串台、闪烁、点开的是另一台。台账那边同样会被同一个 MAC 连写两次。
    ///
    /// 留哪一条：**网关优先 → 可路由地址优先于 169.254 → IP 小的 → 接口名小的**。
    /// 第二档是要紧的：留下 `169.254` 那条的话，这台设备在界面上顶着一个没用的地址，
    /// 而且因为不在本机网段里会被判成「其它网络」—— 一台明明就在同一个局域网里的设备
    /// 就此变成不可代理。整个判据**全序且与输入行序无关**，次序不会随邻居表漂移。
    public static func dedupeByMAC(_ devices: [Device]) -> [Device] {
        var best: [String: Device] = [:]
        var order: [String] = []
        for device in devices {
            guard let existing = best[device.mac] else {
                best[device.mac] = device
                order.append(device.mac)
                continue
            }
            if prefers(device, over: existing) { best[device.mac] = device }
        }
        return order.compactMap { best[$0] }
    }

    /// `dedupeByMAC` 的取舍判据。
    static func prefers(_ lhs: Device, over rhs: Device) -> Bool {
        if lhs.isGateway != rhs.isGateway { return lhs.isGateway }
        let lhsLinkLocal = isLinkLocalIPv4(lhs.ip)
        let rhsLinkLocal = isLinkLocalIPv4(rhs.ip)
        if lhsLinkLocal != rhsLinkLocal { return rhsLinkLocal }
        let lhsKey = ipSortKey(lhs.ip)
        let rhsKey = ipSortKey(rhs.ip)
        if lhsKey != rhsKey { return lhsKey < rhsKey }
        return lhs.interface < rhs.interface
    }

    /// `169.254.0.0/16` —— DHCP 没拿到租约时自配的链路本地地址。
    public static func isLinkLocalIPv4(_ text: String) -> Bool { text.hasPrefix("169.254.") }

    // MARK: - ARP 表

    /// ARP 表的 IP → MAC 映射（已补零规范化、小写）。给 `ArpWatch` 用。
    public static func arpMap() -> [String: String] {
        var map: [String: String] = [:]
        for device in arpTable() where !device.ip.isEmpty && !device.mac.isEmpty {
            map[device.ip] = device.mac.lowercased()
        }
        return map
    }

    /// 解析 `arp -an`。
    ///
    /// 用子进程而不是自己 `sysctl(NET_RT_FLAGS)`：后者要手工走 `rt_msghdr` + `sockaddr_dl`
    /// 的变长结构，在不同 macOS 版本上是出过变动的地方；而这个调用每几秒才一次，
    /// 一个短命子进程的代价完全可以接受。
    /// public 是给 `COAST_DEVICES_SELFTEST` 用的：它要拿**去重之前**的原始行数
    /// 跟 `scan()` 的结果对一下，才能确认这台机器确实覆盖到了「同一个 MAC 出现多次」。
    public static func arpTable() -> [Device] {
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
    public static func ipSortKey(_ ip: String) -> UInt32 {
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
