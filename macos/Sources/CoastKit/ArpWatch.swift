import Foundation

/// ARP 欺骗检测 —— Qt 版 `DevicesController::onSecurityAlert` 的对应物。
///
/// 本程序自己就在做 ARP 欺骗（透明代理靠它），所以「有没有**别人**也在做」不是可选项：
///
/// - **设备被争抢**：另一台机器也在劫持我们正在代理的设备。两边每秒对着同一台设备发欺骗应答，
///   它的网关指向来回翻，表现为那台设备网络时好时坏 —— 而用户只会怪我们。
/// - **网关被冒充**：有人把网关 IP 绑到了自己的 MAC 上。这是教科书式的中间人：
///   本机出网流量会先经过它。我们做代理的，更没有理由对此视而不见。
///
/// 判据只用**本机的 ARP 表**（`arp -an`），不额外抓包：
/// 检测要在「没开接管」时也生效，而 BPF 嗅探要 root、要常驻，代价与收益不成比例。
/// 代价是粒度粗（只看得到本机学到的绑定），但两类最要紧的情形都覆盖得到。
public struct ArpWatch: Sendable {

    public enum Kind: Sendable, Equatable {
        /// 我们正在代理的设备，其 IP 被绑到了第三方 MAC 上。
        case deviceContended
        /// 网关（或本机 IP）被别的 MAC 冒充。
        case gatewaySpoofed
    }

    public struct Alert: Sendable, Equatable, Identifiable {
        public let kind: Kind
        /// 冒名者的 MAC —— 也就是现在占着这个 IP 的那个。
        public let offenderMAC: String
        /// 被冒充/被争抢的 IP。
        public let subjectIP: String
        /// 该 IP 本来应该对应的 MAC（我们记录的真值）。
        public let expectedMAC: String
        public var id: String { "\(kind)|\(offenderMAC)|\(subjectIP)" }
    }

    /// 一次检查所需的全部输入。**显式传进来而不是自己去取**：这样这份逻辑可以脱离
    /// 真实网络环境测试 —— 否则它只能在「恰好有人正在欺骗」的机器上被验证，等于验不了。
    public struct Snapshot: Sendable {
        /// 当前 ARP 表：IP → MAC（MAC 需已补零规范化）。
        public var arp: [String: String]
        /// 网关的真实 IP/MAC。
        public var gatewayIP: String
        public var gatewayMAC: String
        /// 本机所有网卡 MAC —— 我们自己占着某个 IP 不算欺骗。
        public var localMACs: Set<String>
        /// 正在代理的设备：IP → 其真实 MAC。
        public var proxiedDevices: [String: String]

        public init(arp: [String: String], gatewayIP: String, gatewayMAC: String,
                    localMACs: Set<String>, proxiedDevices: [String: String]) {
            self.arp = arp
            self.gatewayIP = gatewayIP
            self.gatewayMAC = gatewayMAC
            self.localMACs = localMACs
            self.proxiedDevices = proxiedDevices
        }
    }

    public init() {}

    /// 纯函数：给定一份快照，算出当下成立的告警。
    public func evaluate(_ snapshot: Snapshot) -> [Alert] {
        var alerts: [Alert] = []
        let normalizedLocal = Set(snapshot.localMACs.map { $0.lowercased() })

        // 1) 网关被冒充。
        //    注意**不能**把「MAC 是我们自己」当成欺骗：接管期间我们对**别的设备**冒充网关，
        //    但本机自己的 ARP 表里网关仍应指向真网关 —— 若这里变成了本机 MAC，那是我们
        //    自己的 bug（把欺骗包发给了自己），同样值得知道，但不该报成「有人攻击你」。
        if !snapshot.gatewayIP.isEmpty, !snapshot.gatewayMAC.isEmpty,
           let current = snapshot.arp[snapshot.gatewayIP]?.lowercased(),
           current != snapshot.gatewayMAC.lowercased(),
           !normalizedLocal.contains(current) {
            alerts.append(Alert(kind: .gatewaySpoofed, offenderMAC: current,
                                subjectIP: snapshot.gatewayIP,
                                expectedMAC: snapshot.gatewayMAC.lowercased()))
        }

        // 2) 我们代理中的设备被别人争抢。
        for (ip, trueMAC) in snapshot.proxiedDevices {
            guard let current = snapshot.arp[ip]?.lowercased() else { continue }
            let expected = trueMAC.lowercased()
            guard current != expected, !normalizedLocal.contains(current) else { continue }
            alerts.append(Alert(kind: .deviceContended, offenderMAC: current,
                                subjectIP: ip, expectedMAC: expected))
        }
        return alerts.sorted { $0.id < $1.id }
    }

    // MARK: - IPv6（NDP）监视

    /// v6 版的一次检查输入。判据同 v4，只是绑定来自**邻居表**（`ndp -an`）而非 ARP 表：
    ///
    /// - **路由器被冒充**：v6 默认路由器的链路本地地址被绑到了别的 MAC（v6 侧的中间人）。
    /// - **设备被争抢**：我们代理中的设备的某个 v6 地址被绑到了第三方 MAC。
    ///
    /// `proxiedV6` 是「设备 v6 → 设备真实 MAC」的**基线**：由调用方在**观察到干净绑定时**逐步
    /// 攒起来（见 AppState 的 v6Baseline），而不是每轮从当前邻居表现取 —— 否则地址一旦被投毒，
    /// 当前表里它已经指向攻击者，就再也判不出「本该是设备的」。这与 v4 用 DeviceStore 里存的
    /// lastIP 当锚点是同一个道理。
    public struct SnapshotV6: Sendable {
        public var neighbors: [String: String]   // v6 → MAC（规范化小写）
        public var routerLL: String
        public var routerMAC: String
        public var localMACs: Set<String>
        public var proxiedV6: [String: String]   // v6 → 设备真实 MAC（基线）

        public init(neighbors: [String: String], routerLL: String, routerMAC: String,
                    localMACs: Set<String>, proxiedV6: [String: String]) {
            self.neighbors = neighbors
            self.routerLL = routerLL
            self.routerMAC = routerMAC
            self.localMACs = localMACs
            self.proxiedV6 = proxiedV6
        }
    }

    /// 纯函数：给定一份 v6 快照，算出当下成立的告警（与 `evaluate` 同结构，共用 `Alert`/节流/留存）。
    public func evaluateV6(_ snapshot: SnapshotV6) -> [Alert] {
        var alerts: [Alert] = []
        let normalizedLocal = Set(snapshot.localMACs.map { $0.lowercased() })

        // 1) 路由器 LL 被冒充（不能把「是本机自己」当欺骗，同 v4 的理由）。
        if !snapshot.routerLL.isEmpty, !snapshot.routerMAC.isEmpty,
           let current = snapshot.neighbors[snapshot.routerLL]?.lowercased(),
           current != snapshot.routerMAC.lowercased(),
           !normalizedLocal.contains(current) {
            alerts.append(Alert(kind: .gatewaySpoofed, offenderMAC: current,
                                subjectIP: snapshot.routerLL,
                                expectedMAC: snapshot.routerMAC.lowercased()))
        }

        // 2) 代理设备的某个 v6 被别人争抢。
        for (v6, trueMAC) in snapshot.proxiedV6 {
            guard let current = snapshot.neighbors[v6]?.lowercased() else { continue }
            let expected = trueMAC.lowercased()
            guard current != expected, !normalizedLocal.contains(current) else { continue }
            alerts.append(Alert(kind: .deviceContended, offenderMAC: current,
                                subjectIP: v6, expectedMAC: expected))
        }
        return alerts.sorted { $0.id < $1.id }
    }
}

/// 告警的去重节流。
///
/// 同一条威胁不该每轮都弹一次通知 —— ARP 欺骗往往持续几十分钟，每 10 秒一条通知只会
/// 让用户关掉通知权限，然后连真正要紧的那条也看不到。与 Qt 版一致：同一威胁 30 分钟一次。
public final class ArpAlertThrottle: @unchecked Sendable {
    private var lastNotified: [String: Date] = [:]
    private let interval: TimeInterval
    private let lock = NSLock()

    public init(interval: TimeInterval = 30 * 60) { self.interval = interval }

    /// 返回本轮**应当通知**的告警（其余是重复的）。`now` 显式传入以便测试。
    public func filter(_ alerts: [ArpWatch.Alert], now: Date = Date()) -> [ArpWatch.Alert] {
        lock.lock()
        defer { lock.unlock() }
        var out: [ArpWatch.Alert] = []
        for alert in alerts {
            if let last = lastNotified[alert.id], now.timeIntervalSince(last) < interval { continue }
            lastNotified[alert.id] = now
            out.append(alert)
        }
        return out
    }
}

/// 告警的**留存**：一条威胁一旦出现，就在横幅里待到「连续 TTL 没再观察到」为止；
/// 展示次序按**首次出现时间**（先来的在上），与 Qt `DevicesController::m_secAlerts`
/// + `sweepSecurityAlerts()` 同一套。
///
/// 为什么不能「这一轮没检测到就立刻撤掉」（Swift 原来的做法）：
/// ARP 争抢的表现恰恰就是绑定来回翻 —— 这一拍是攻击者的 MAC、下一拍又翻回真 MAC。
/// 即时撤销会让横幅**每隔几秒闪一次**，用户看到的是界面在抽搐，而不是「你正被攻击」。
///
/// 次序按首次出现时间而不是 id：新告警插在**末尾**，已经在看的那几条不会往下跳。
public final class ArpAlertRetention: @unchecked Sendable {

    /// 与 Qt 的 `kSecTtlMs` 同值：150 秒没再观察到就判定已停止。
    public static let defaultTTL: TimeInterval = 150

    private struct Entry {
        var alert: ArpWatch.Alert
        var first: Date
        var last: Date
    }

    private var entries: [String: Entry] = [:]
    private let ttl: TimeInterval
    private let lock = NSLock()

    public init(ttl: TimeInterval = ArpAlertRetention.defaultTTL) { self.ttl = ttl }

    /// 喂进本轮实际观察到的告警，返回**当前应当展示**的那些（含仍在 TTL 内的旧告警），
    /// 按首次出现时间升序、同刻按 id 兜底。
    public func absorb(_ alerts: [ArpWatch.Alert], now: Date = Date()) -> [ArpWatch.Alert] {
        lock.lock()
        defer { lock.unlock() }
        for alert in alerts {
            if var entry = entries[alert.id] {
                entry.alert = alert
                entry.last = now
                entries[alert.id] = entry
            } else {
                entries[alert.id] = Entry(alert: alert, first: now, last: now)
            }
        }
        entries = entries.filter { now.timeIntervalSince($0.value.last) <= ttl }
        return entries.values
            .sorted { $0.first == $1.first ? $0.alert.id < $1.alert.id : $0.first < $1.first }
            .map(\.alert)
    }

    /// 用户点「忽略」时清空。威胁若仍在，下一轮会重新加入 —— 但通知去重表还在，不会又弹一次
    /// （与 Qt 的 `dismissSecurityAlerts()` 一致）。
    public func clear() {
        lock.lock()
        defer { lock.unlock() }
        entries.removeAll()
    }
}
