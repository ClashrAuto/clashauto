import Foundation
import Testing
@testable import CoastKit

private final class TempDevices {
    let directory: URL
    let store: DeviceStore

    init() {
        directory = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-dev-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        store = DeviceStore(configDir: directory)
    }
    deinit { try? FileManager.default.removeItem(at: directory) }
}

@Suite("设备台账")
struct DeviceStoreTests {

    @Test("往返读写")
    func roundTrip() {
        let temp = TempDevices()
        var device = DeviceStore.Device(mac: "aa:bb:cc:dd:ee:ff")
        device.alias = "客厅电视"
        device.proxyEnabled = true
        device.policyMode = .global
        device.policyTarget = "🚀 节点选择"
        device.lastIP = "192.168.1.5"
        #expect(temp.store.save(device))

        let loaded = temp.store.device(mac: "aa:bb:cc:dd:ee:ff")
        #expect(loaded?.alias == "客厅电视")
        #expect(loaded?.proxyEnabled == true)
        #expect(loaded?.policyMode == .global)
        #expect(loaded?.policyTarget == "🚀 节点选择")
    }

    @Test("★ 空 IP 不覆盖已有地址——设备这轮没扫到不代表它换了地址")
    func emptyIPDoesNotClobber() {
        let temp = TempDevices()
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:ff", true, ip: "192.168.1.5")
        // 再次开关时没带 IP（设备暂时不在邻居表里）——抹掉的话规则会凭空消失，
        // 那会让它在恢复的一瞬间直连出去
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:ff", true)
        #expect(temp.store.device(mac: "aa:bb:cc:dd:ee:ff")?.lastIP == "192.168.1.5")
    }

    @Test("IP 变了才算变——没变时返回 false，调用方据此跳过重建配置")
    func updateAddressDetectsChange() {
        let temp = TempDevices()
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:ff", true, ip: "192.168.1.5")
        #expect(temp.store.updateAddress(mac: "aa:bb:cc:dd:ee:ff", ip: "192.168.1.5") == false)
        #expect(temp.store.updateAddress(mac: "aa:bb:cc:dd:ee:ff", ip: "192.168.1.9"))
        #expect(temp.store.device(mac: "aa:bb:cc:dd:ee:ff")?.lastIP == "192.168.1.9")
    }

    @Test("proxiedDevices 只返回「开着且知道地址」的——不知道地址就写不出规则")
    func proxiedFilter() {
        let temp = TempDevices()
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true, ip: "192.168.1.5")
        var noAddress = DeviceStore.Device(mac: "aa:bb:cc:dd:ee:02")
        noAddress.proxyEnabled = true             // 开着但没地址（设备还没出现在邻居表里）
        _ = temp.store.save(noAddress)
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:03", false, ip: "192.168.1.7")

        #expect(temp.store.proxiedDevices().map(\.mac) == ["aa:bb:cc:dd:ee:01"])
    }

    @Test("删除")
    func remove() {
        let temp = TempDevices()
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:ff", true, ip: "1.2.3.4")
        #expect(temp.store.remove(mac: "aa:bb:cc:dd:ee:ff"))
        #expect(temp.store.all().isEmpty)
    }
}

@Suite("设备策略 → full.yaml")
struct DevicePolicyConfigTests {

    private let base = """
    proxies: []
    proxy-groups:
      - name: '🚀 节点选择'
        type: select
        proxies:
          - DIRECT
    rules:
      - 'MATCH,🚀 节点选择'
    """

    @Test("没有设备开代理时什么都不生成")
    func noDevicesNoRules() {
        let temp = TempDevices()
        let builder = ConfigBuilder(config: AppConfig(), directory: temp.directory)
        #expect(builder.applyDevicePolicies(base) == base)
    }

    @Test("★ 透明重定向看不到凭据，只能按源 IP 认设备 → SRC-IP-CIDR")
    func policyUsesSourceIP() {
        let temp = TempDevices()
        for (mac, ip, mode, target) in [
            ("aa:bb:cc:dd:ee:01", "192.168.1.11", DeviceStore.PolicyMode.direct, ""),
            ("aa:bb:cc:dd:ee:02", "192.168.1.12", .reject, ""),
            ("aa:bb:cc:dd:ee:03", "192.168.1.13", .global, "🚀 节点选择"),
            ("aa:bb:cc:dd:ee:04", "192.168.1.14", .follow, ""),
            ("aa:bb:cc:dd:ee:05", "192.168.1.15", .rule, ""),
        ] {
            var d = temp.store.setProxyEnabled(mac: mac, true, ip: ip)!
            d.policyMode = mode; d.policyTarget = target
            _ = temp.store.save(d)
        }
        let out = ConfigBuilder(config: AppConfig(), directory: temp.directory).applyDevicePolicies(base)
        #expect(out.contains("'SRC-IP-CIDR,192.168.1.11/32,DIRECT'"))
        #expect(out.contains("'SRC-IP-CIDR,192.168.1.12/32,REJECT'"))
        #expect(out.contains("SRC-IP-CIDR,192.168.1.13/32,🚀 节点选择"))
        // follow / rule 不生成专属规则——那正是「跟随全局 / 走默认规则表」的含义
        #expect(out.contains("192.168.1.14") == false)
        #expect(out.contains("192.168.1.15") == false)
        // 零配置：配置里不该出现任何凭据
        #expect(out.contains("username") == false)
        #expect(out.contains("password") == false)
    }

    @Test("规则前插到 rules: 顶部")
    func rulesPrepended() {
        let temp = TempDevices()
        var d = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true, ip: "192.168.1.11")!
        d.policyMode = .direct
        _ = temp.store.save(d)
        let out = ConfigBuilder(config: AppConfig(), directory: temp.directory).applyDevicePolicies(base)
        let lines = out.components(separatedBy: "\n")
        let rulesIndex = lines.firstIndex { $0 == "rules:" }!
        #expect(lines[rulesIndex + 1].contains("SRC-IP-CIDR,192.168.1.11/32,DIRECT"))
    }

    @Test("global 模式缺目标时跳过，不写出半条坏规则")
    func globalWithoutTarget() {
        let temp = TempDevices()
        var d = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true, ip: "192.168.1.11")!
        d.policyMode = .global; d.policyTarget = ""
        _ = temp.store.save(d)
        let out = ConfigBuilder(config: AppConfig(), directory: temp.directory).applyDevicePolicies(base)
        #expect(out.contains("SRC-IP-CIDR") == false)
    }
}
