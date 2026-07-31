import Foundation

/// 设备台账（`coast.db` 的 `device` 表）——「设备」页的持久层。
///
/// ## 与 Qt 版的架构差异（重要）
///
/// Qt 版靠**二层 ARP/NDP 投毒**把局域网设备的流量劫持到本机，设备端零配置。
/// Swift 版走 **macOS 原生路子**：核心开一个绑 `0.0.0.0` 的**带认证的代理入站**，
/// 设备端手动指向 `本机IP:端口` 并填凭据。代价是设备要配一次，换来的是
/// 不做投毒、不需要用户态 TCP/IP 栈、不需要 root、不需要 BPF —— 少了整整一个子系统。
///
/// 两个由此产生的、**必须**与 Qt 版不同的决定：
///   1. **listener 绑 `0.0.0.0` 而不是 `127.0.0.1`** —— 不劫持就得让设备够得着。
///   2. **每台设备一个随机密码**，而不是 Qt 版那个固定字面量 `coast`。
///      端口现在暴露在局域网上，固定密码等于开放代理 —— 谁扫到这个口都能白嫖，
///      甚至拿它当跳板。这是架构变化**强制**的，不是可选的加固。
///
/// 身份也随之变了：Qt 版按 MAC 认设备（投毒时看得到二层地址）；这里按**我们签发的凭据**
/// 认设备（`IN-USER` 规则的用户名）。MAC 仍然是台账主键 —— 它是设备发现时唯一稳定的身份，
/// 用户在界面上看到的也是它。
public final class DeviceStore: @unchecked Sendable {

    /// 每设备的代理策略。写进 `full.yaml` 就是 `IN-USER` 规则。
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
        /// 是否为它签发了代理凭据。
        public var proxyEnabled = false
        public var policyMode: PolicyMode = .follow
        /// `global` 模式下的目标节点/策略组名。
        public var policyTarget = ""
        /// 签发给这台设备的代理密码。**随机生成，每台不同**。
        public var password = ""
        public var firstSeen = Date()

        public var id: String { mac }

        /// 代理用户名：`dev-<去冒号小写 mac>`。
        /// 纯 ASCII，可直接写进 YAML 而不必加引号；也便于在核心日志里一眼认出是哪台。
        public var proxyUser: String { DeviceStore.proxyUser(for: mac) }

        public init(mac: String) { self.mac = mac }
    }

    /// 局域网代理入站端口。与主混合端口分开：**主混合口保持免认证且只监听本机**，
    /// Coast 自己的下载测速走它；对外的这个口恒带认证。
    public static let lanProxyPort = 7899

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
              password TEXT NOT NULL DEFAULT '',
              first_seen INTEGER NOT NULL DEFAULT 0)
            """)
        // 生成配置时只查「开着代理的」，给它一条索引。
        database.exec("CREATE INDEX IF NOT EXISTS device_proxy ON device(proxy_enabled)")
    }

    // MARK: - 读写

    public func all() -> [Device] {
        var result: [Device] = []
        database?.query("""
            SELECT mac, alias, proxy_enabled, policy_mode, policy_target, password, first_seen
            FROM device ORDER BY mac
            """) { row in
            var device = Device(mac: row.text(0))
            device.alias = row.text(1)
            device.proxyEnabled = row.int(2) != 0
            device.policyMode = PolicyMode(rawValue: row.text(3)) ?? .follow
            device.policyTarget = row.text(4)
            device.password = row.text(5)
            device.firstSeen = Date(timeIntervalSince1970: Double(row.int(6)))
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
            INSERT INTO device (mac, alias, proxy_enabled, policy_mode, policy_target, password, first_seen)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(mac) DO UPDATE SET
              alias = excluded.alias,
              proxy_enabled = excluded.proxy_enabled,
              policy_mode = excluded.policy_mode,
              policy_target = excluded.policy_target,
              password = excluded.password
            """, [
            .text(device.mac), .text(device.alias),
            .int(device.proxyEnabled ? 1 : 0),
            .text(device.policyMode.rawValue), .text(device.policyTarget),
            .text(device.password),
            .int(Int64(device.firstSeen.timeIntervalSince1970)),
        ])
    }

    @discardableResult
    public func remove(mac: String) -> Bool {
        database?.run("DELETE FROM device WHERE mac = ?", [.text(mac)]) ?? false
    }

    /// 为设备开启代理：没有密码就**当场签发一个随机的**。
    ///
    /// 密码只在这里生成一次并落库，之后一直复用 —— 每次重建配置都换密码的话，
    /// 用户已经配好的设备会在下一次热重载后集体掉线。
    @discardableResult
    public func setProxyEnabled(mac: String, _ enabled: Bool) -> Device? {
        var device = self.device(mac: mac) ?? Device(mac: mac)
        device.proxyEnabled = enabled
        if enabled, device.password.isEmpty {
            device.password = Self.generatePassword()
        }
        return save(device) ? device : nil
    }

    /// 生成配置时用的快照：只要开着代理的那些。
    public func proxiedDevices() -> [Device] {
        all().filter { $0.proxyEnabled && !$0.password.isEmpty }
    }

    // MARK: - 工具

    /// `dev-<去冒号小写 mac>`。MAC 非法时返回空串，调用方据此跳过 ——
    /// 写出一个坏用户名会让核心的 listener 配置整段失效。
    public static func proxyUser(for mac: String) -> String {
        let hex = mac.lowercased().filter { $0.isHexDigit }
        guard hex.count == 12 else { return "" }
        return "dev-" + hex
    }

    /// 20 位随机密码。用 `SystemRandomNumberGenerator`（底层 arc4random）——
    /// 这个口暴露在局域网上，要的是不可预测。
    static func generatePassword() -> String {
        let alphabet = Array("abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789")
        var generator = SystemRandomNumberGenerator()
        return String((0..<20).map { _ in alphabet[Int.random(in: 0..<alphabet.count, using: &generator)] })
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
