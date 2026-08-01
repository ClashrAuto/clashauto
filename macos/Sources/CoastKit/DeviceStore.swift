import Foundation

/// 设备台账（`coast.db` 的 `device` 表）——「设备」页的持久层。
///
/// ## macOS 原生的**零配置**透明代理
///
/// 设备端**什么都不用配**，用户只在软件里点一下开关。链路是：
///
///   1. **ARP 欺骗**让目标设备把本机当成网关，于是它的流量发到我们网卡上；
///   2. **`net.inet.ip.forwarding=1`** 让内核愿意转发这些不是发给自己的包；
///   3. **PF 的 `rdr` 规则**把该设备的 TCP 重定向到 mihomo 的 `redir-port`、
///      UDP :53 重定向到 mihomo 的 DNS 口；
///   4. mihomo 按规则分流。
///
/// **关键点：不需要用户态 TCP/IP 栈。** Qt 版为此背了 13 万行 lwIP，是因为它在二层自己
/// 收发帧、自己重组 TCP；而 macOS 上内核转发 + PF 重定向把这一整层都省掉了 ——
/// 包由内核处理，我们只负责「让它来」和「让它拐弯」。这就是同一个问题的 macOS 原生答案。
///
/// ## 由此决定的身份口径
///
/// 透明重定向看不到任何凭据，设备只能**按源 IP** 识别，所以每设备策略走
/// `SRC-IP-CIDR,<ip>/32,<目标>` 而不是 Qt 版的 `IN-USER`。
/// 台账主键仍是 **MAC** —— IP 会随 DHCP 漂移，MAC 才是稳定身份；每轮扫描把当前 IP
/// 刷进来，规则随之重新生成。
public final class DeviceStore: @unchecked Sendable {

    /// 每设备的代理策略。写进 `full.yaml` 就是 `SRC-IP-CIDR` 规则。
    public enum PolicyMode: String, Sendable, CaseIterable, Codable {
        case follow   // 跟随全局（不生成专属规则）
        case rule     // 规则分流（走主选择组）
        case global   // 指定具体节点/策略组
        case direct   // 强制直连
        case reject   // 禁止上网

        public var title: String {
            switch self {
            case .follow: return "跟随全局"
            case .rule: return "规则分流"
            case .global: return "指定节点"
            case .direct: return "强制直连"
            case .reject: return "禁止上网"
            }
        }
    }

    public struct Device: Sendable, Equatable, Identifiable {
        /// 主键。规范化小写冒号分隔。
        public var mac: String
        /// 用户备注名（优先于自动识别名显示）。
        public var alias = ""
        /// 是否代理这台设备（ARP 欺骗 + PF 重定向）。
        public var proxyEnabled = false
        public var policyMode: PolicyMode = .follow
        /// `global` 模式下的目标节点/策略组名。
        public var policyTarget = ""
        /// 最近一次看到的 IP。生成 `SRC-IP-CIDR` 规则要用它。
        ///
        /// 落库而不是只放内存：设备暂时不在邻居表里（睡眠、刚重启）时，
        /// 规则不能因此凭空消失 —— 那会让它在恢复的一瞬间直连出去。
        public var lastIP = ""
        /// 用户手动指定的设备类型（覆盖按主机名/厂商的自动识别）。空 = 自动。
        ///
        /// 自动识别是拿主机名和厂商猜的，猜错很常见（一堆设备的厂商都是 "Apple"）。
        /// 让用户改一次并记住，比每次看着错图标强。
        public var typeOverride = ""
        public var firstSeen = Date()

        public var id: String { mac }

        public init(mac: String) { self.mac = mac }
    }

    /// mihomo 的透明代理端口（`redir-port`）。PF 把被代理设备的 TCP 重定向到这里。
    ///
    /// 与主混合端口分开：混合口仍 `allow-lan: false` 只监听本机，
    /// 而这个口只接受**经 PF 重定向进来的**流量，不对外广播。
    public static let redirPort = 7893
    /// mihomo 的 DNS 监听端口。被代理设备的 UDP :53 重定向到这里，
    /// 让它拿到 fake-ip 结果，域名规则才匹配得上。
    public static let dnsPort = 1053

    private let database: SQLiteDatabase?

    public init(configDir: URL = AppPaths.configDir) {
        database = try? SQLiteDatabase(path: SQLitePaths.databasePath(configDir: configDir))
        createSchema()
    }

    public var isOpen: Bool { database?.isOpen ?? false }

    private func createSchema() {
        guard let database else { return }
        database.exec("""
            CREATE TABLE IF NOT EXISTS device (
              mac TEXT PRIMARY KEY,
              alias TEXT NOT NULL DEFAULT '',
              proxy_enabled INTEGER NOT NULL DEFAULT 0,
              policy_mode TEXT NOT NULL DEFAULT 'follow',
              policy_target TEXT NOT NULL DEFAULT '',
              last_ip TEXT NOT NULL DEFAULT '',
              first_seen INTEGER NOT NULL DEFAULT 0,
              type_override TEXT NOT NULL DEFAULT '')
            """)
        // 老库补列。`ALTER TABLE ... ADD COLUMN` 在列已存在时会报错，
        // 而 SQLite 没有 `ADD COLUMN IF NOT EXISTS` —— 直接执行、忽略失败即可
        // （比先查 `PRAGMA table_info` 再判断少一次往返，语义一样）。
        //
        // ★ `last_ip` 也在这儿补：它是 8e3463a 把 `password` 列改名过来的，**当时没写迁移**。
        //   改名前建的库里那一列还叫 `password`，于是之后每一条
        //   `SELECT … last_ip …` / `INSERT … last_ip …` 都以「no such column」失败 ——
        //   台账整个哑掉：备注名存不下、代理开关记不住、策略选了等于没选，
        //   界面上却一点报错都没有（`save()` 只是返回 false，没人看它）。
        //   本机的库正是这种，`device` 表一条记录都写不进去。
        //   旧 `password` 的值是设备密码、与 IP 毫无关系，**不搬**，只补空列。
        for column in ["last_ip TEXT NOT NULL DEFAULT ''",
                       "type_override TEXT NOT NULL DEFAULT ''"] {
            database.exec("ALTER TABLE device ADD COLUMN \(column)")
        }
        // 生成配置时只查「开着代理的」，给它一条索引。
        database.exec("CREATE INDEX IF NOT EXISTS device_proxy ON device(proxy_enabled)")
    }

    // MARK: - 读写

    public func all() -> [Device] {
        var result: [Device] = []
        database?.query("""
            SELECT mac, alias, proxy_enabled, policy_mode, policy_target, last_ip, first_seen,
                   type_override
            FROM device ORDER BY mac
            """) { row in
            var device = Device(mac: row.text(0))
            device.alias = row.text(1)
            device.proxyEnabled = row.int(2) != 0
            device.policyMode = PolicyMode(rawValue: row.text(3)) ?? .follow
            device.policyTarget = row.text(4)
            device.lastIP = row.text(5)
            device.firstSeen = Date(timeIntervalSince1970: Double(row.int(6)))
            device.typeOverride = row.text(7)
            result.append(device)
        }
        return result
    }

    public func device(mac: String) -> Device? {
        all().first { $0.mac == mac }
    }

    @discardableResult
    public func save(_ device: Device) -> Bool {
        guard let database else { return false }
        return database.run("""
            INSERT INTO device (mac, alias, proxy_enabled, policy_mode, policy_target, last_ip,
                                first_seen, type_override)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(mac) DO UPDATE SET
              alias = excluded.alias,
              proxy_enabled = excluded.proxy_enabled,
              policy_mode = excluded.policy_mode,
              policy_target = excluded.policy_target,
              last_ip = excluded.last_ip,
              type_override = excluded.type_override
            """, [
            .text(device.mac), .text(device.alias),
            .int(device.proxyEnabled ? 1 : 0),
            .text(device.policyMode.rawValue), .text(device.policyTarget),
            .text(device.lastIP),
            .int(Int64(device.firstSeen.timeIntervalSince1970)),
            .text(device.typeOverride),
        ])
    }

    @discardableResult
    public func remove(mac: String) -> Bool {
        database?.run("DELETE FROM device WHERE mac = ?", [.text(mac)]) ?? false
    }

    /// 给设备起个名字（备注名）。空串表示清除，回落到自动识别出的名字。
    ///
    /// `alias` 这一列建库时就有，却一直没有写入口 —— 台账里认得出这台设备，
    /// 用户却只能对着 `a4:83:e7:…` 或厂商名猜是哪一台。
    @discardableResult
    public func setAlias(mac: String, _ alias: String) -> Device? {
        guard var record = device(mac: mac) else { return nil }
        record.alias = alias.trimmingCharacters(in: .whitespacesAndNewlines)
        return save(record) ? record : nil
    }

    /// 开/关某台设备的代理。`ip` 是这一刻看到的地址，用来生成 `SRC-IP-CIDR` 规则。
    @discardableResult
    public func setProxyEnabled(mac: String, _ enabled: Bool, ip: String = "") -> Device? {
        var device = self.device(mac: mac) ?? Device(mac: mac)
        device.proxyEnabled = enabled
        // 空 IP 不覆盖已有值：设备这轮没扫到不代表它换地址了，抹掉反而会让规则消失。
        if !ip.isEmpty { device.lastIP = ip }
        return save(device) ? device : nil
    }

    /// 扫描时把当前 IP 刷进台账。IP 变了就得重生成规则，否则策略会挂在旧地址上。
    /// 返回 true 表示确实变了，调用方据此决定要不要重建配置。
    @discardableResult
    public func updateAddress(mac: String, ip: String) -> Bool {
        guard !ip.isEmpty, var device = self.device(mac: mac), device.lastIP != ip else { return false }
        device.lastIP = ip
        return save(device)
    }

    /// 生成配置/装 PF 规则时用的快照：开着代理**且知道地址**的那些。
    /// 不知道地址就没法写规则，跳过（设备一旦出现在邻居表里，下一轮就补上了）。
    public func proxiedDevices() -> [Device] {
        all().filter { $0.proxyEnabled && !$0.lastIP.isEmpty }
    }

    // MARK: - 工具


    /// 本机自己的全部 IPv4（含回环与 TUN 的 198.18.0.1）。
    ///
    /// 用来判断「这条连接是本机自己发的」。缓存 30 秒：每条连接都要问一次，
    /// 而网卡地址不会秒级变化（与 Qt `DeviceStore::isLocalMachineIp` 的缓存同期）。
    public static func localMachineIPs() -> Set<String> {
        if let cached = localIPCache, Date().timeIntervalSince(localIPCacheAt) < 30 {
            return cached
        }
        var result: Set<String> = ["127.0.0.1", "::1"]
        var head: UnsafeMutablePointer<ifaddrs>?
        if getifaddrs(&head) == 0, let first = head {
            defer { freeifaddrs(head) }
            var cursor: UnsafeMutablePointer<ifaddrs>? = first
            while let entry = cursor {
                defer { cursor = entry.pointee.ifa_next }
                guard let address = entry.pointee.ifa_addr,
                      address.pointee.sa_family == UInt8(AF_INET) else { continue }
                var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                guard getnameinfo(address, socklen_t(address.pointee.sa_len),
                                  &host, socklen_t(NI_MAXHOST), nil, 0, NI_NUMERICHOST) == 0
                else { continue }
                result.insert(String(cString: host))
            }
        }
        localIPCache = result
        localIPCacheAt = Date()
        return result
    }

    private nonisolated(unsafe) static var localIPCache: Set<String>?
    private nonisolated(unsafe) static var localIPCacheAt = Date.distantPast

    /// 这条连接是不是本机自己发出的。回环、本机任一网卡、以及 TUN 的 198.18/15
    /// （开增强模式后本机流量的 sourceIP 就是它）。
    public static func isLocalMachineIP(_ ip: String) -> Bool {
        guard !ip.isEmpty else { return false }
        if ip.hasPrefix("127.") || ip == "::1" { return true }
        if ip.hasPrefix("198.18.") || ip.hasPrefix("198.19.") { return true }
        return localMachineIPs().contains(ip)
    }

    /// 本机在局域网上的 IPv4 地址（给用户填到设备里的那个）。
    /// 取第一个非回环、非链路本地的 IPv4。
    public static func localLANAddress() -> String? {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return nil }
        defer { freeifaddrs(head) }

        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            let flags = Int32(entry.pointee.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_LOOPBACK == 0,
                  let address = entry.pointee.ifa_addr,
                  address.pointee.sa_family == UInt8(AF_INET) else { continue }

            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            guard getnameinfo(address, socklen_t(address.pointee.sa_len),
                              &host, socklen_t(NI_MAXHOST), nil, 0, NI_NUMERICHOST) == 0
            else { continue }
            let ip = String(cString: host)
            // 169.254/16 是没拿到 DHCP 时的自分配地址，填给别的设备是没用的
            guard !ip.hasPrefix("127."), !ip.hasPrefix("169.254.") else { continue }
            return ip
        }
        return nil
    }
}
