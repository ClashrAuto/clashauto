import Foundation
import Testing
@testable import CoastKit

@Suite("ARP 欺骗检测")
struct ArpWatchTests {

    private func snapshot(arp: [String: String],
                          proxied: [String: String] = [:],
                          local: Set<String> = ["aa:aa:aa:aa:aa:aa"]) -> ArpWatch.Snapshot {
        ArpWatch.Snapshot(arp: arp, gatewayIP: "192.168.1.1", gatewayMAC: "11:22:33:44:55:66",
                          localMACs: local, proxiedDevices: proxied)
    }

    @Test("一切正常时不报警")
    func quiet() {
        let alerts = ArpWatch().evaluate(snapshot(
            arp: ["192.168.1.1": "11:22:33:44:55:66", "192.168.1.50": "de:ad:be:ef:00:01"],
            proxied: ["192.168.1.50": "de:ad:be:ef:00:01"]))
        #expect(alerts.isEmpty, "误报:\(alerts)")
    }

    @Test("★ 网关 IP 指向了别的 MAC —— 教科书式中间人")
    func gatewaySpoofed() {
        let alerts = ArpWatch().evaluate(snapshot(
            arp: ["192.168.1.1": "66:66:66:66:66:66"]))
        #expect(alerts.count == 1)
        #expect(alerts.first?.kind == .gatewaySpoofed)
        #expect(alerts.first?.offenderMAC == "66:66:66:66:66:66")
        #expect(alerts.first?.expectedMAC == "11:22:33:44:55:66")
    }

    @Test("★ 我们代理中的设备被第三方抢走")
    func deviceContended() {
        let alerts = ArpWatch().evaluate(snapshot(
            arp: ["192.168.1.1": "11:22:33:44:55:66", "192.168.1.50": "66:66:66:66:66:66"],
            proxied: ["192.168.1.50": "de:ad:be:ef:00:01"]))
        #expect(alerts.count == 1)
        #expect(alerts.first?.kind == .deviceContended)
        #expect(alerts.first?.subjectIP == "192.168.1.50")
    }

    @Test("★ 本机自己的 MAC 占着某个 IP 不算欺骗(否则接管期间会自己报自己)")
    func ownMacIsNotAnAttack() {
        let alerts = ArpWatch().evaluate(snapshot(
            arp: ["192.168.1.1": "aa:aa:aa:aa:aa:aa", "192.168.1.50": "aa:aa:aa:aa:aa:aa"],
            proxied: ["192.168.1.50": "de:ad:be:ef:00:01"]))
        #expect(alerts.isEmpty, "把本机自己报成了攻击者:\(alerts)")
    }

    @Test("★ 大小写不一致不该被当成不同的 MAC(arp -an 与我们记录的大小写常常不同)")
    func caseInsensitive() {
        let alerts = ArpWatch().evaluate(snapshot(
            arp: ["192.168.1.1": "11:22:33:44:55:66".uppercased(),
                  "192.168.1.50": "DE:AD:BE:EF:00:01"],
            proxied: ["192.168.1.50": "de:ad:be:ef:00:01"]))
        #expect(alerts.isEmpty, "大小写差异被误判成欺骗:\(alerts)")
    }

    @Test("设备不在 ARP 表里（离线）不报警")
    func offlineDeviceIsQuiet() {
        let alerts = ArpWatch().evaluate(snapshot(
            arp: ["192.168.1.1": "11:22:33:44:55:66"],
            proxied: ["192.168.1.50": "de:ad:be:ef:00:01"]))
        #expect(alerts.isEmpty)
    }

    @Test("取不到网关真值时不猜(宁可漏报也不能误报)")
    func noGatewayTruth() {
        var snap = snapshot(arp: ["192.168.1.1": "66:66:66:66:66:66"])
        snap.gatewayMAC = ""
        #expect(ArpWatch().evaluate(snap).isEmpty)
    }

    @Test("★ 同一威胁 30 分钟内只通知一次;换了威胁立刻通知")
    func throttle() {
        let throttle = ArpAlertThrottle(interval: 30 * 60)
        let a = ArpWatch.Alert(kind: .gatewaySpoofed, offenderMAC: "66:66:66:66:66:66",
                               subjectIP: "192.168.1.1", expectedMAC: "11:22:33:44:55:66")
        let b = ArpWatch.Alert(kind: .deviceContended, offenderMAC: "77:77:77:77:77:77",
                               subjectIP: "192.168.1.50", expectedMAC: "de:ad:be:ef:00:01")
        let t0 = Date(timeIntervalSince1970: 1_000_000)
        #expect(throttle.filter([a], now: t0).count == 1)
        #expect(throttle.filter([a], now: t0.addingTimeInterval(60)).isEmpty, "60 秒后又弹了一次")
        #expect(throttle.filter([a, b], now: t0.addingTimeInterval(120)).map(\.id) == [b.id],
                "新威胁应当立刻通知,老威胁应当仍被压住")
        #expect(throttle.filter([a], now: t0.addingTimeInterval(31 * 60)).count == 1,
                "过了 30 分钟仍未再次提醒 —— 持续攻击会被永久静音")
    }
}

/// 数据源本身的真机验证。
///
/// `localMACs()` 手工算 `sockaddr_dl` 里的偏移 —— 算错了会**静默返回空集**，
/// 于是「本机自己占着某个 IP」的豁免失效，接管期间会把自己报成攻击者。
/// 这种错编译器不管、纯逻辑测试也照过，只能在真机上对着系统命令核。
@Suite("ARP 检测的数据源(真机)")
struct ArpWatchSourceTests {

    @Test("★ localMACs 与 ifconfig 报的 ether 地址对得上")
    func localMACsMatchIfconfig() throws {
        let mine = LanTopology.localMACs()
        #expect(!mine.isEmpty, "一个网卡 MAC 都没取到 —— sockaddr_dl 偏移多半算错了")

        // 用系统命令取一份真值来核对
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/sbin/ifconfig")
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = FileHandle.nullDevice
        try task.run()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()
        let text = String(data: data, encoding: .utf8) ?? ""
        let expected = Set(text.components(separatedBy: .newlines)
            .compactMap { line -> String? in
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                guard trimmed.hasPrefix("ether ") else { return nil }
                return String(trimmed.dropFirst(6)).trimmingCharacters(in: .whitespaces).lowercased()
            })
        guard !expected.isEmpty else { return }   // 无有线/无线网卡的环境，跳过核对
        #expect(expected.isSubset(of: mine),
                "ifconfig 报了而我们没取到:\(expected.subtracting(mine))")
    }

    @Test("arpMap 的 MAC 全部是规范化的十二位小写形式")
    func arpMapIsNormalized() {
        let map = LanBrowser.arpMap()
        for (ip, mac) in map {
            #expect(mac == mac.lowercased(), "\(ip) 的 MAC 没转小写:\(mac)")
            #expect(mac.split(separator: ":").count == 6, "\(ip) 的 MAC 段数不对:\(mac)")
            #expect(mac.split(separator: ":").allSatisfy { $0.count == 2 },
                    "\(ip) 的 MAC 没补前导零:\(mac) —— 补零缺失会让比对永远不相等")
        }
    }
}
