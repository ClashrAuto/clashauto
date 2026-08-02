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

    // —— 被动抢答设备 NS（仅 v6 启用时）——
    // 设备网关邻居条目老化后会发 NUD **单播** NS 来本机复核（它缓存的是「路由器 LL 在本机」）。
    // 收到就回一条 solicited NA 抢在真路由器前把条目翻回 REACHABLE。收包全在 `queue` 上，与
    // start/stop/timer 串行，无需额外加锁。**不开混杂**：单播 NS 目的就是本机 MAC，非混杂也收得到，
    // 而混杂会让本机自己的协议栈把我们发的欺骗帧学进缓存 → 自投毒（见 Qt L2Endpoint_mac 的论证）。
    private var bpfReadSource: DispatchSourceRead?
    private var bpfReadBuf: UnsafeMutableRawBufferPointer?
    // 抢答时记下每台设备的链路本地地址（NS 源）：复原时优先单播 solicited 回它（更快落 REACHABLE）。
    private var deviceLLByMAC: [String: [UInt8]] = [:]
    // 反制真网关/真路由器解毒帧（v4 ARP + v6 RA/NA）的**统一**重投节流（uptime 纳秒；0=没反制过）。
    private var lastReassertNanos: UInt64 = 0

    // —— 唤醒沿 boost（v4+v6 统一）——
    // 某设备空闲一阵后又发帧（ARP/IPv6）→ 它的网关 ARP/邻居 条目多半刚老化、正在重解析 → 进一段
    // 50ms×8 的高频重投窗口，抢在真网关/真路由器的应答前把「网关在本机」钉回。治「空闲后首次访问
    // 先漏到真网关」。
    private var boostTimer: DispatchSourceTimer?
    private var boostRemaining = 0
    private var lastSeenByMAC: [String: UInt64] = [:]   // 设备 MAC 串 → 上次见到其帧的 uptime 纳秒
    private static let kBoostTicks = 8
    private static let kWakeIdleNanos: UInt64 = 10_000_000_000   // 空闲 >10s 判为唤醒沿

    // —— PF 规则的可重建状态 ——
    // v6 设备源地址常在接管**之后**才出现在链路上（`ndp -an` 那一刻手机往往还没在本机邻居表里，
    // 真机实测确认过）。所以除了启动时那份，收包路径还会从被投毒设备**转发来的 v6 帧**里现学
    // 全局/ULA 源地址，动态补进 `inet6 from <v6>` 规则。存下重建 anchor 所需的一切。
    private var pfDeviceIP4s: [String] = []
    private var pfRedirPort = 0
    private var pfDnsPort = 0
    private var pfV6set: Set<String> = []   // 当前所有 v6 源（启动发现 ∪ 线上现学），去重

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

    /// ★ **必须挂在 `com.apple/` 之下**。macOS 的主规则集（`/etc/pf.conf`）只有
    ///   `rdr-anchor "com.apple/*"` / `anchor "com.apple/*"` 这几条通配引用 —— 一个平级的
    ///   `coast.redirect` anchor 装得进去、`pfctl -s Anchors` 也看得见，却**从来不会被求值**，
    ///   规则等于没装。真机实测（iMac 网关 + Android 设备）：平级 anchor 下 PF 状态表
    ///   `inserts=0`、设备的包被原样转发给真路由器，核心一条连接都收不到；换成
    ///   `com.apple/coast.redirect` 后同一条规则立刻开始命中。
    ///
    ///   只在这个通配命名空间下**新开我们自己的子 anchor**，不碰 Apple 已有的任何规则；
    ///   卸载时只 flush 这一个子 anchor（见 `uninstallPF`）。
    static let anchorName = "com.apple/coast.redirect"

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
            // 挂收包源做抢答/反制/学习（v4 的 ARP、v6 的 NDP 都要）。放在 active=true 之后、发第一轮
            // 欺骗之前都行 —— 它盯着同一个 bpfFD。预筛只放 ARP/IPv6，IPv4 数据帧不进用户态。
            startBPFReader()
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
            teardownBPFReader()
            if bpfFD >= 0 { close(bpfFD); bpfFD = -1 }
            return
        }
        timer?.cancel(); timer = nil
        boostTimer?.cancel(); boostTimer = nil; boostRemaining = 0
        // 先停收包源，再动 bpfFD —— 收包源盯着这个 fd，必须在 close 前取消，否则会对已关的 fd 触发。
        teardownBPFReader()

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
                        // 抢答过该设备 NS → 知道它的 LL → 单播 solicited 复原（更快落 REACHABLE）；
                        // 否则退回组播 override 复原。两者都把 TLLA 换回真路由器 MAC。
                        let frame: [UInt8]
                        if let ll = deviceLLByMAC[mac.text] {
                            frame = NDPPacket.restoreUnicast(deviceMAC: mac, selfMAC: selfMAC,
                                                             deviceIP6: ll, routerLL6: routerLL6,
                                                             routerMAC6: routerMAC6)
                        } else {
                            frame = NDPPacket.restore(deviceMAC: mac, selfMAC: selfMAC,
                                                      routerLL6: routerLL6, routerMAC6: routerMAC6)
                        }
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
        deviceLLByMAC = [:]
        lastSeenByMAC = [:]
        lastReassertNanos = 0
        pfDeviceIP4s = []; pfV6set = []; pfRedirPort = 0; pfDnsPort = 0
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

    /// 进入一段高频重投窗口（50ms × kBoostTicks），v4+v6 一起投，抢在真网关/真路由器前钉回。
    /// 幂等：已在 boost 时只续期。全在 `queue` 上，与收包/1s tick 串行。
    private func boostAll() {
        guard bpfFD >= 0 else { return }
        sendSpoof(); sendNDPSpoof()    // 立刻先投一轮（v6 未启用时后者 no-op）
        boostRemaining = Self.kBoostTicks
        guard boostTimer == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + .milliseconds(50), repeating: .milliseconds(50))
        t.setEventHandler { [weak self] in
            guard let self else { return }
            self.sendSpoof(); self.sendNDPSpoof()
            self.boostRemaining -= 1
            if self.boostRemaining <= 0 { self.boostTimer?.cancel(); self.boostTimer = nil }
        }
        t.resume()
        boostTimer = t
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
            var disable: UInt32 = 0
            _ = ioctl(fd, COAST_BIOCSHDRCMPLT, &enable)  // 我们自己填以太源地址，别让内核覆盖
            _ = ioctl(fd, COAST_BIOCIMMEDIATE, &enable)  // 立即返回，不等缓冲填满（抢答要快）
            _ = ioctl(fd, COAST_BIOCSSEESENT, &disable)  // 别回显自己发出的帧（省得处理自己的 NA）
            _ = fcntl(fd, F_SETFL, O_NONBLOCK)           // 非阻塞：收包源误触时 read 不挂住
            bpfFD = fd
            return true
        }
        return false
    }

    // MARK: - 被动抢答/反制/学习（收包路径，v4+v6 共用）

    /// 起收包源：按 BIOCGBLEN 分配读缓冲，挂一个读事件源到 `queue`。有设备接管就挂（v4 也要）。
    private func startBPFReader() {
        guard bpfFD >= 0, bpfReadSource == nil else { return }
        var blen: Int32 = 0
        if ioctl(bpfFD, COAST_BIOCGBLEN, &blen) < 0 || blen <= 0 { blen = 32768 }
        bpfReadBuf = UnsafeMutableRawBufferPointer.allocate(byteCount: Int(blen),
                                                            alignment: MemoryLayout<UInt64>.alignment)
        let source = DispatchSource.makeReadSource(fileDescriptor: bpfFD, queue: queue)
        source.setEventHandler { [weak self] in self?.drainBPF() }
        source.resume()
        bpfReadSource = source
    }

    /// 停收包源、释放读缓冲。必须在 `close(bpfFD)` **之前**调（源盯着这个 fd）。幂等。
    private func teardownBPFReader() {
        bpfReadSource?.cancel()
        bpfReadSource = nil
        bpfReadBuf?.deallocate()
        bpfReadBuf = nil
    }

    /// 排空一次 BPF 读缓冲：按 bpf_hdr + BPF_WORDALIGN 逐帧走，交给 `handleCapturedFrame`。
    private func drainBPF() {
        guard bpfFD >= 0, let buf = bpfReadBuf, let base = buf.baseAddress else { return }
        let n = read(bpfFD, base, buf.count)
        guard n > 0 else { return }
        var p = 0
        while p + 18 <= n {   // 至少放得下一个 bpf_hdr（mac 上约 18 字节）
            let rec = base.advanced(by: p)
            let caplen = Int(coast_bpf_caplen(rec))
            let hdrlen = Int(coast_bpf_hdrlen(rec))
            let frameStart = p + hdrlen
            guard hdrlen > 0, caplen > 0, frameStart + caplen <= n else { break }
            // 预筛：只放行 **ARP(0x0806) 与 IPv6(0x86DD)** —— 抢答/反制/学习要看的都在这两类里。
            // IPv4 数据帧（流量大头）不拷贝、不处理。裸指针看两字节，避开每包一次堆分配；没有内核
            // 源 MAC 过滤时，这层用户态预筛把无关帧成本降到近零。设备唤醒时必会 ARP 重解析网关，
            // 所以「唤醒沿」这个信号靠 ARP 就够，不必抓 IPv4 数据帧。
            let ethBase = base.advanced(by: frameStart).assumingMemoryBound(to: UInt8.self)
            let isARP = ethBase[12] == 0x08 && ethBase[13] == 0x06
            let isV6 = ethBase[12] == 0x86 && ethBase[13] == 0xDD
            if caplen >= 14, isARP || isV6 {
                let frame = Array(UnsafeRawBufferPointer(start: base.advanced(by: frameStart), count: caplen)
                                    .bindMemory(to: UInt8.self))
                handleCapturedFrame(frame)
            }
            let advance = Int(coast_bpf_wordalign(Int32(hdrlen + caplen)))
            guard advance > 0 else { break }   // 防呆：对齐算出 0 会死循环
            p += advance
        }
    }

    /// 一帧 ARP 或 IPv6（drainBPF 已按以太类型预筛）。v4 与 v6 统一处理，关心的情况——
    ///   ① 真网关/真路由器发的解毒帧（v4 的 ARP、v6 的 RA/NA）→ 立刻重投盖回（统一节流）；
    ///   ② 唤醒沿：被接管设备空闲后又发帧 → 进 boost（v4+v6 一起）；
    ///   ③ 抢答：设备问「网关在哪」（v4 who-has / v6 NS）→ 抢先应答本机；
    ///   ④ 现学：设备转发来的普通 v6 帧 → 学它的可路由源地址补 PF rdr。
    private func handleCapturedFrame(_ frame: [UInt8]) {
        guard bpfFD >= 0, frame.count >= 14 else { return }
        let isARP = frame[12] == 0x08 && frame[13] == 0x06
        guard let srcMACBytes = NDPPacket.ethSource(frame) else { return }

        // ① 反制解毒帧（不是设备流量，先于 victim 判定处理）。
        //    v4：真网关自己广播的 who-has / 免费 ARP，携带「网关在真 MAC」→ 设备一收就解毒。
        //    v6：真路由器的 RA/NA。两者都走统一的 reassertAll（含 50ms 节流）。
        if isARP {
            if srcMACBytes == gatewayMAC.bytes { reassertAll(); return }
        } else if NDPPacket.isRouterAdvertOrNA(frame), v6Active, srcMACBytes == routerMAC6.bytes {
            reassertAll(); return
        }

        // 以下都只认**我们正在接管的设备**发来的帧。
        guard deviceMACs.contains(where: { $0.bytes == srcMACBytes }) else { return }
        let devMAC = ARPPacket.MAC(bytes: srcMACBytes)

        // ② 统一唤醒沿：设备空闲 >10s 后又发**任一**帧（ARP/IPv6）→ 邻居/ARP 条目多半刚老化、
        //    正在重解析网关 → 进 boost（v4+v6 一起），抢在真网关/真路由器前钉回。
        let now = DispatchTime.now().uptimeNanoseconds
        if let last = lastSeenByMAC[devMAC.text], now &- last > Self.kWakeIdleNanos { boostAll() }
        lastSeenByMAC[devMAC.text] = now

        // ③ 抢答设备的网关解析。
        if isARP {
            answerDeviceArp(frame, deviceMAC: devMAC)   // v4：who-has 网关 → 回「网关在本机」
            return
        }
        // v6 NS（复核路由器 LL）。NS 源是设备 LL（不可路由），不会落到 ④ 的学习里。
        if v6Active, NDPPacket.isNeighborSolicitation(frame),
           let target = NDPPacket.nsTarget(frame), target == routerLL6,
           let deviceIP6 = NDPPacket.ipv6Source(frame) {
            deviceLLByMAC[devMAC.text] = deviceIP6   // NS 源即设备 LL，复原时单播回它
            let na = NDPPacket.solicitedNA(deviceMAC: devMAC, selfMAC: selfMAC,
                                           deviceIP6: deviceIP6, routerLL6: routerLL6)
            // 连发两帧压过真路由器对同一 NS 的应答（同 Qt 抢答的短促连发，成本可忽略）。
            _ = na.withUnsafeBytes { write(bpfFD, $0.baseAddress, $0.count) }
            _ = na.withUnsafeBytes { write(bpfFD, $0.baseAddress, $0.count) }
            return
        }

        // ④ 现学：普通 v6 帧的**可路由**源地址进 rdr，让它的 v6 也走代理而非退回普通转发。
        //    （真机实测：ndp 那一刻常查不到设备 v6，只有等它经本机发帧才学得到——见 pfV6set 说明。）
        if v6Active, let s6 = NDPPacket.ipv6Source(frame), NDPPacket.isRoutableV6(s6),
           let str = NDPPacket.ipv6String(s6) {
            learnDeviceV6(str)   // 幂等，只有新地址才真的重灌 anchor
        }
    }

    /// v4 抢答：设备广播「who-has 网关 IP」→ 抢先回一条「网关 IP 在本机 MAC」单播给它，
    /// 压过真网关的应答。只对**问的正是我们冒充的网关**的请求应答。
    private func answerDeviceArp(_ frame: [UInt8], deviceMAC: ARPPacket.MAC) {
        guard let req = ARPPacket.parseRequest(frame, targetIP: gatewayIP) else { return }
        // 单播回**已校验的**设备（eth 源），target IP 用请求里的 spa。
        let reply = ARPPacket.reply(senderMAC: selfMAC, senderIP: gatewayIP,
                                    targetMAC: deviceMAC, targetIP: req.senderIP)
        _ = reply.withUnsafeBytes { write(bpfFD, $0.baseAddress, $0.count) }
        _ = reply.withUnsafeBytes { write(bpfFD, $0.baseAddress, $0.count) }
    }

    /// 统一反制：真网关/真路由器解毒 → 立刻给所有设备重投一轮（v4 + v6），把「网关在本机」盖回。
    /// 单一 50ms 节流，防真网关短时连发多条时放大成风暴。
    private func reassertAll() {
        let now = DispatchTime.now().uptimeNanoseconds
        guard now &- lastReassertNanos > 50_000_000 else { return }
        lastReassertNanos = now
        sendSpoof()        // v4 ARP
        sendNDPSpoof()     // v6 NDP（v6Active 时才实际发）
    }

    // MARK: - PF

    /// 把规则写进一个专用 anchor，只影响这些源 IP，绝不动用户既有的 PF 配置。
    ///
    /// `deviceV6s`：被接管设备的**全局/ULA** v6 源地址（可空 = 无 v6 或没发现任何 v6 地址）。
    /// 现在只记着备用：**v6 一条 rdr 都不装**，因为 macOS 的 pf 投递不了转发流量的 inet6 rdr
    /// （真机实证与取舍见 `anchorRulesText()`）。设备的 v6 走普通内核转发，能上网但不经核心。
    private func installPF(deviceIPs: [String], deviceV6s: [String],
                          redirPort: Int, dnsPort: Int) -> String? {
        // 存下重建 anchor 需要的一切（收包路径现学到新 v6 源时要照这份重灌）。
        pfDeviceIP4s = deviceIPs
        pfRedirPort = redirPort
        pfDnsPort = dnsPort
        pfV6set = Set(deviceV6s)
        guard runPfctl(["-a", Self.anchorName, "-f", "-"], stdin: anchorRulesText()) else {
            return "pfctl 装规则失败"
        }
        // 确保 PF 本身是开的（-E 引用计数，卸载时 -X 对应）。已开时 -E 也安全。
        _ = runPfctl(["-E"], stdin: nil)
        // 每次装规则都重设一遍：别的服务（互联网共享等）重载主规则集会把它打回默认 10s。
        tightenPurgeInterval()
        return nil
    }

    /// 按当前 PF 状态（v4 设备 + v6 源集合 + 端口）生成 anchor 规则文本。
    ///
    /// ★ **每条 rdr 都要配一条 `route-to (lo0 …)` 的 pass**，而且 rdr **不能带 `pass`**。
    ///   被接管设备的包对本机而言是**转发流量**：rdr 把目的改写成环回地址之后，内核仍按
    ///   「这个包要转发出去」处理，于是它既不出网也不本地投递，直接消失 —— 设备侧表现为
    ///   SYN 重传到超时。`route-to (lo0 …)` 明确把它交给环回口，才真正送进核心的监听套接字。
    ///   而 `rdr pass` 的 `pass` 会让这个包**跳过整个过滤规则集**，那条 route-to 永远轮不上，
    ///   所以必须拆成「rdr 只做翻译」+「filter 规则负责投递」两条。
    ///   真机实测（iMac 网关 + Android 设备）：`rdr pass` 单条 → 本地监听 0 连接；
    ///   拆成两条后同一请求立刻被核心收下并按规则选中节点。
    private func anchorRulesText() -> String {
        var rules = ""
        for ip in pfDeviceIP4s {
            // TCP 全部重定向到 redir 口；UDP :53 单独重定向到 DNS 口（域名要走 fake-ip）
            rules += "rdr on \(interface) inet proto tcp from \(ip) to any -> 127.0.0.1 port \(pfRedirPort)\n"
            rules += "rdr on \(interface) inet proto udp from \(ip) to any port 53 -> 127.0.0.1 port \(pfDnsPort)\n"
        }
        // ★ **v6 不装 rdr** —— macOS 的 pf 不会把「转发流量的 inet6 rdr」投递到本机监听。
        //
        //   真机实测（iMac 网关 + Android 设备，macOS 26.5，NDP 投毒正常、设备 v6 帧确实到达本机）：
        //   inet6 rdr 规则**命中并建了状态**（Packets>0、States=2），配套的 route-to 过滤规则也命中，
        //   可目标端口上的监听**一条连接都收不到**；设备侧是 SYN 一直重传到超时。
        //   两种目的地址都试过 —— `::1`（核心的 redir 确认在 `[::1]` 上 LISTEN）和本机 en1 的全局
        //   v6 地址 —— **都不投递**。同一时刻、同一份 anchor 里，等价的 inet **v4** 规则工作正常。
        //   即 v4 能走通不是运气，v6 走不通也不是配置问题，是 pf 这条路本身在 macOS 上不通。
        //
        //   于是装 v6 rdr 的后果是**设备 IPv6 直接断网**：包被 pf 吃掉，既不投递也不再转发 ——
        //   比不接管还糟。去掉 rdr 之后（投毒照旧，本机仍是它的 v6 网关），设备 v6 走普通内核转发
        //   正常出网（同一台设备实测 `https://ipv6.baidu.com` 200）。
        //
        //   **代价**：设备的 v6 流量不经核心，不受规则/节点选择约束。这是当下两害相权 ——
        //   「v6 能用但不代理」优于「v6 直接断」。要真正代理 v6 得换机制（不是 rdr），
        //   在此之前 v6 的接管只用于「本机当网关 + 可靠复原」，不做重定向。
        //   注意 `learnDeviceV6` 现学到的地址仍然记着，换机制时可直接复用。
        //
        // 投递规则必须排在全部 rdr 之后：pfctl 要求 anchor 里翻译规则先于过滤规则。
        //
        // ★ `rdr` 之外还要这条 `route-to`：见上方方法注释 —— 转发流量被 rdr 改写目的之后，
        //   内核仍按「要转发出去」处理，只有显式 route-to 环回才真正送进本机监听。
        // ★ **必须给这两条规则单独设状态超时**，否则被接管设备一忙就把 PF 的状态表打爆。
        //
        //   PF 的状态表是**全系统一张**、硬上限默认 10000 条（`pfctl -s memory`），而每条被重定向的
        //   连接都要占一条。macOS 的默认超时又长得离谱：`tcp.closing 900s`、`tcp.closed 90s`、
        //   `tcp.finwait 45s` —— 短命 HTTP 连接关完之后还要在表里躺一分钟以上。
        //   真机实测（iMac 当网关、被接管设备 64 并发背靠背打）：状态数一路涨到 **10001 触顶**，
        //   吞吐随之从 857 崩到 161 conn/s，而且**一轮比一轮低**（表来不及回收）。
        //   打爆的后果还不止于网关：这张表是全机共用的，撑满之后 Mac 上**所有**新连接都会受影响。
        //
        //   只给我们自己 anchor 里的这两条规则设短超时（per-rule，不动 `set timeout` 那套全局值，
        //   免得影响系统其它流量）。取值按「连接已经结束、只是等迟到的 FIN/RST」来定：
        //   closed/finwait 5s、closing 10s 都远大于局域网 RTT。**established 一个字不动**
        //   （默认 24h）—— 长连接（SSH、WebSocket、下载）必须留着。
        //   同一台子改完复测：930 / 1006 / 966 conn/s，**不再逐轮劣化**，状态峰值 9673 不再触顶，
        //   压力一停就迅速回落。
        // ★ **别再往下调这几个值了** —— 试过，没用，原因见下。
        //
        //   直觉是「状态数 ≈ 连接速率 × 停留时长」，那把 closed/finwait 从 5s 压到 2s
        //   就该减半。**实测完全没动**：4 台设备各 16 并发、合计 ~1000 conn/s、持续 60s，
        //   5/5/10 与 2/2/5 两组的状态峰值都是 **10001**（正好顶死硬上限），
        //   `pfctl -s info` 的 `memory` 丢包分别 388 / 469，设备侧失败数与之一一对应
        //   （390 / 471 —— 每个失败正好对应一次 PF 丢包）。
        //
        //   把状态表抓出来看才明白：8000 多条全是 `FIN_WAIT_2:FIN_WAIT_2`，
        //   而且 **`expires in 00:00:00`** —— 超时**早就到了**，只是**还没被回收**。
        //   pf 的清扫是一个按 `set timeout interval`（默认 **10 秒**）分摊扫全表的后台线程，
        //   所以一条到期状态最长要在表里再躺 10 秒。真正的停留时长是
        //   `max(超时值, 清扫周期)` —— 只要超时值低于 10s，改它就是一个字都不影响。
        //   平衡点因此固定在 1000/s × 10s = 1 万，正好撞上限，这才是那 0.5% 失败的来源。
        //
        //   真修法是把清扫周期调下来（见 `tightenPurgeInterval()`，实测 interval=1 之后
        //   同一负载 **四台全 100.0%、零失败**，状态峰值 10001 → 6108，`memory` 丢包归零）。
        //   这里保持 5/5/10 就够：它们只覆盖「设备 ↔ 本机环回」这一段（LAN + loopback，
        //   RTT 不到 1ms），到**目标服务器**的那条连接是核心自己拨的本机 socket、**不占 PF 状态**。
        //   `tcp.first`/`tcp.opening` 不设：默认值再大也被清扫周期盖住，压短只是白担
        //   「慢握手被误杀」的风险。**`tcp.established` 一个字不动**（默认 24h）—— 长连接必须留着。
        let tcpTimeouts = "(tcp.closed 5, tcp.finwait 5, tcp.closing 10)"
        // DNS 是一来一回的短交互，UDP 状态没必要留满默认的 60s。
        let udpTimeouts = "(udp.first 10, udp.single 10, udp.multiple 20)"
        for ip in pfDeviceIP4s {
            rules += "pass in quick on \(interface) route-to (lo0 127.0.0.1) inet proto tcp"
                + " from \(ip) to 127.0.0.1 port \(pfRedirPort) keep state \(tcpTimeouts)\n"
            rules += "pass in quick on \(interface) route-to (lo0 127.0.0.1) inet proto udp"
                + " from \(ip) to 127.0.0.1 port \(pfDnsPort) keep state \(udpTimeouts)\n"
        }
        // ★ **挡掉被接管设备的 QUIC（UDP/443）**，否则「已代理」是句假话。
        //
        //   macOS 这条腿只重定向 TCP 和 UDP:53 —— 核心的 `redir` 入站本来就只有 TCP，
        //   而管 UDP 的 `tproxy` 入站是 Linux 专属（darwin 上直接返回 "not supported"）。
        //   于是设备其余的 UDP 走的是**普通内核转发**：真机实测（被接管设备发 UDP:9000）
        //   包在 en1 上原样进出、源地址还是设备自己，而核心的 /connections **一条都没有**。
        //   后果不是"慢一点"，是两件实打实的事：
        //     · **每设备策略对这些流量完全失效** —— 设成「禁网」的设备，它的 QUIC 照跑；
        //     · 流量带着设备的真实源地址直接出网，绕过了用户以为已经生效的代理。
        //   而今天的 Web 有很大一块是 HTTP/3（QUIC）—— 这不是边角流量。
        //
        //   在核心能接管 darwin 的透明 UDP 之前，正确做法是**不让它悄悄漏过去**：挡掉之后
        //   客户端判定 QUIC 不可用并**回落到 TCP**（浏览器的既定行为），而 TCP 那条是真正
        //   经代理的 —— 于是「已代理」重新成立。
        //
        //   ⚠️ **回落不是立即的，别照抄 `return` 的字面语义**：本想用 `block return` 回一个
        //   ICMP 端口不可达让客户端秒判，真机实测**没有这回事** —— 规则确实命中
        //   （`pfctl -s rules -v` 的 Packets 在涨），但 en1 上抓不到任何 ICMP，设备侧的
        //   connected UDP socket 等满 2002ms 超时而不是 ECONNREFUSED。macOS 的 pf 对
        //   **转发**流量不生成 ICMP 不可达（只对发给本机的才会）。`return` 留着无害
        //   （目的地真是本机时它有用），但实际效果等同 `drop`。
        //   ⇒ 代价是**每个新目的主机首次 QUIC 要多等一次客户端超时**（浏览器随后会把该主机
        //   记成「QUIC 不可用」，不会每条连接都付），换来的是策略真正生效、流量不再裸奔。
        //
        //   **只挡 443，不挡其它 UDP**：QUIC 有明确的 TCP 回落路径，挡了只损失一点首连延迟；
        //   而 NTP/WebRTC/游戏那些没有回落，一刀切会直接弄坏它们。它们仍走转发（仍不受策略
        //   约束，这个洞留着 —— 要补得靠真正的 UDP 接管，见上）。
        for ip in pfDeviceIP4s {
            rules += "block return in quick on \(interface) inet proto udp"
                + " from \(ip) to any port 443\n"
        }
        return rules
    }

    /// 现学到一个新的设备 v6 源 → 重灌 anchor（幂等：只在集合真变了时才动 pfctl）。在 `queue` 上跑。
    private func learnDeviceV6(_ v6: String) {
        guard pfInstalled, pfV6set.insert(v6).inserted else { return }
        _ = runPfctl(["-a", Self.anchorName, "-f", "-"], stdin: anchorRulesText())
    }

    private func uninstallPF() {
        _ = runPfctl(["-a", Self.anchorName, "-F", "all"], stdin: nil)  // 清空我们的 anchor
        restorePurgeInterval()
    }

    /// 干净收尾时把清扫周期还回 10s。**只在这条路上做** —— 崩溃恢复的静态 `restore()` 不碰它：
    /// 那条路上没有改动前的快照，再写一次主规则集只是白白多一次弄坏用户防火墙的机会。
    /// 而且这个选项**本来就不跨重启**（开机会重载 `/etc/pf.conf`），漏还的代价有上限。
    private func restorePurgeInterval() {
        guard !purgeTuneAbandoned, let snap = mainRulesetSnapshot() else { return }
        _ = runPfctl(["-m", "-f", "-"], stdin: "set timeout interval 10\n" + snap.text)
    }

    // MARK: - PF 清扫周期

    /// PF 到期状态的回收周期（`set timeout interval`）。macOS 默认 **10 秒**。
    ///
    /// 为什么非动不可：pf 的状态表是**全系统一张、硬上限 10000 条**，而清扫是一个按这个周期
    /// 分摊扫全表的后台线程 —— 一条状态即使超时早到了，也要在表里再躺最多一个周期。
    /// 于是真正的停留时长是 `max(per-rule 超时, interval)`，我们 anchor 里那几个 5s/10s
    /// 全被 10s 盖住，**调它们一点用都没有**（实测见 `anchorRulesText()` 里的注释）。
    ///
    /// 真机实测（iMac 网关、4 台设备各 16 并发、合计 ~1000 conn/s、持续 60s）：
    ///   · interval=10（默认）：状态峰值 **10001**（顶死上限），`memory` 丢包 388，设备侧 99.3~99.5%
    ///   · interval=1        ：状态峰值 **6108**，`memory` 丢包 **0**，四台设备**全 100.0%**
    ///
    /// ★ **为什么不能用 `pfctl -m -f` 只喂这一行**（这是个会把网关整死的坑，别踩）：
    ///   `-m` 只保证「没写到的**选项**保持原值」，**规则集照样按文件内容整体替换**。
    ///   喂一个只有 `set timeout interval 1` 的文件 = 主规则集被清空 ——
    ///   `anchor "com.apple/*"` / `rdr-anchor "com.apple/*"` 这些**锚点引用全没了**。
    ///   我们的 `com.apple/coast.redirect` 规则还在、`pfctl -s Anchors` 也看得见，
    ///   但主规则集里再没有东西去引用它，于是**永远不会被求值**，网关静默失效。
    ///   `/etc/pf.conf` 开头那段注释就是在说这件事（"the nested anchors rely on the
    ///   anchor point defined here"）。真机上验证过：确实被清空了。
    ///
    ///   所以这里**先把当前主规则集原样读回来**（四个 dump 拼起来就是完整的一份，
    ///   而且本身就是合法的 pf.conf 语法），把选项加在最前面，再整份灌回去。
    ///   不读 `/etc/pf.conf` 而读实时状态，是为了保住**系统服务动态插进来的锚点**
    ///   （互联网共享等，`/etc/pf.conf` 里没有）—— 直接重载 pf.conf 会把它们丢掉。
    ///
    /// 装完会**逐条核对**主规则集是否与改动前完全一致；只要少了一行就立刻从
    /// `/etc/pf.conf` 复原并放弃（`purgeTuneAbandoned` 置位，之后不再尝试）——
    /// 宁可回到 10001 顶格的老样子，也不能让用户的防火墙缺一条规则。
    private var purgeTuneAbandoned = false

    /// 读回当前主规则集。返回 (给 pfctl 重灌用的文本, 用于比对的行集合)。
    /// 顺序照 `/etc/pf.conf` 的既定顺序：scrub → nat/rdr → dummynet → filter。
    private func mainRulesetSnapshot() -> (text: String, lines: [String])? {
        func dump(_ what: String) -> [String]? {
            guard let out = pfctlOutput(["-s", what]) else { return nil }
            return out.split(separator: "\n").map(String.init)
                .map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }
        }
        guard let rules = dump("rules"), let nat = dump("nat"),
              let dn = dump("dummynet") else { return nil }
        let scrub = rules.filter { $0.hasPrefix("scrub") }
        let filt = rules.filter { !$0.hasPrefix("scrub") }
        let ordered = scrub + nat + dn + filt
        return (ordered.joined(separator: "\n") + "\n", ordered)
    }

    /// 把 `set timeout interval` 压到 1 秒。幂等；失败或校验不过就自动复原并永久放弃。
    private func tightenPurgeInterval() {
        guard !purgeTuneAbandoned else { return }
        // 已经是 1s 就别动 —— 每写一次主规则集就多一次弄坏用户防火墙的机会，能省则省。
        if let t = pfctlOutput(["-s", "timeouts"]),
           t.split(separator: "\n").contains(where: {
               $0.hasPrefix("interval") && $0.contains(" 1s") }) {
            return
        }
        guard let before = mainRulesetSnapshot(), !before.lines.isEmpty else {
            Audit.log("PF: 读不回主规则集，跳过清扫周期调优（保持默认 interval=10s）")
            purgeTuneAbandoned = true
            return
        }
        guard runPfctl(["-m", "-f", "-"],
                       stdin: "set timeout interval 1\n" + before.text) else {
            Audit.log("PF: 设置 interval 失败，尝试复原主规则集")
            _ = runPfctl(["-f", "/etc/pf.conf"], stdin: nil)
            purgeTuneAbandoned = true
            return
        }
        // 核对：主规则集必须一行不差。少一行就说明我们把用户的防火墙改坏了。
        guard let after = mainRulesetSnapshot(), after.lines == before.lines else {
            Audit.log("PF: 主规则集在设置 interval 后发生变化，立即从 /etc/pf.conf 复原并放弃调优")
            _ = runPfctl(["-f", "/etc/pf.conf"], stdin: nil)
            purgeTuneAbandoned = true
            return
        }
        Audit.log("PF: 状态清扫周期 interval=1s（默认 10s）—— 主规则集 \(after.lines.count) 条已原样保留")
    }

    /// 跑 pfctl 并拿回 stdout（拿不到就返回 nil）。
    private func pfctlOutput(_ arguments: [String]) -> String? {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/sbin/pfctl")
        task.arguments = arguments
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = FileHandle.nullDevice
        guard (try? task.run()) != nil else { return nil }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()
        guard task.terminationStatus == 0 else { return nil }
        return String(data: data, encoding: .utf8)
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
