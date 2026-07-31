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
        device.password = "secret123"
        #expect(temp.store.save(device))

        let loaded = temp.store.device(mac: "aa:bb:cc:dd:ee:ff")
        #expect(loaded?.alias == "客厅电视")
        #expect(loaded?.proxyEnabled == true)
        #expect(loaded?.policyMode == .global)
        #expect(loaded?.policyTarget == "🚀 节点选择")
    }

    @Test("★ 开启代理时签发随机密码，且**再次开启不换密码**")
    func passwordIssuedOnceAndStable() {
        let temp = TempDevices()
        let first = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:ff", true)
        #expect(first?.password.isEmpty == false)

        // 关掉再开：密码必须还是原来那个 —— 每次重建配置都换密码的话，
        // 用户已经配好的设备会在下一次热重载后集体掉线。
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:ff", false)
        let second = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:ff", true)
        #expect(second?.password == first?.password)
    }

    @Test("每台设备的密码互不相同")
    func passwordsDiffer() {
        let temp = TempDevices()
        let a = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true)?.password
        let b = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:02", true)?.password
        #expect(a != b)
        #expect((a?.count ?? 0) >= 16)
    }

    @Test("代理用户名：dev- + 去冒号小写 mac；非法 mac 返回空串")
    func proxyUserFormat() {
        #expect(DeviceStore.proxyUser(for: "AA:BB:CC:DD:EE:FF") == "dev-aabbccddeeff")
        // 非法 mac 若不拦住，会写出一个坏用户名，让整段 listener 配置失效
        #expect(DeviceStore.proxyUser(for: "aa:bb:cc") == "")
        #expect(DeviceStore.proxyUser(for: "") == "")
    }

    @Test("proxiedDevices 只返回「开着且有密码」的")
    func proxiedFilter() {
        let temp = TempDevices()
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true)
        var noPassword = DeviceStore.Device(mac: "aa:bb:cc:dd:ee:02")
        noPassword.proxyEnabled = true            // 开着但没密码（不该发生，但配置里不能出现）
        _ = temp.store.save(noPassword)
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:03", false)

        #expect(temp.store.proxiedDevices().map(\.mac) == ["aa:bb:cc:dd:ee:01"])
    }

    @Test("删除")
    func remove() {
        let temp = TempDevices()
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:ff", true)
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
    func noDevicesNoListener() {
        let temp = TempDevices()
        let builder = ConfigBuilder(config: AppConfig(), directory: temp.directory)
        #expect(builder.applyDevicePolicies(base) == base)
    }

    @Test("★ listener 必须绑 0.0.0.0 —— 设备是直接连过来的，绑回环谁也连不上")
    func listenerBindsAllInterfaces() {
        let temp = TempDevices()
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true)
        let out = ConfigBuilder(config: AppConfig(), directory: temp.directory).applyDevicePolicies(base)
        #expect(out.contains("listen: 0.0.0.0"))
        #expect(out.contains("port: 7899"))
        #expect(out.contains("type: mixed"))       // 同口收 HTTP 与 SOCKS
        #expect(out.contains("- username: dev-aabbccddee01"))
    }

    @Test("★ 密码必须是每设备随机的，不能是固定字面量")
    func perDevicePassword() {
        let temp = TempDevices()
        let a = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true)!
        let b = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:02", true)!
        let out = ConfigBuilder(config: AppConfig(), directory: temp.directory).applyDevicePolicies(base)
        // 这个口暴露在局域网上，固定密码等于开放代理
        #expect(out.contains(a.password))
        #expect(out.contains(b.password))
        #expect(out.contains("password: coast") == false)
    }

    @Test("策略 → IN-USER 规则；follow/rule 不生成专属规则")
    func policyRules() {
        let temp = TempDevices()
        for (mac, mode, target) in [
            ("aa:bb:cc:dd:ee:01", DeviceStore.PolicyMode.direct, ""),
            ("aa:bb:cc:dd:ee:02", .reject, ""),
            ("aa:bb:cc:dd:ee:03", .global, "🚀 节点选择"),
            ("aa:bb:cc:dd:ee:04", .follow, ""),
            ("aa:bb:cc:dd:ee:05", .rule, ""),
        ] {
            var d = temp.store.setProxyEnabled(mac: mac, true)!
            d.policyMode = mode; d.policyTarget = target
            _ = temp.store.save(d)
        }
        let out = ConfigBuilder(config: AppConfig(), directory: temp.directory).applyDevicePolicies(base)
        #expect(out.contains("'IN-USER,dev-aabbccddee01,DIRECT'"))
        #expect(out.contains("'IN-USER,dev-aabbccddee02,REJECT'"))
        #expect(out.contains("IN-USER,dev-aabbccddee03,🚀 节点选择"))
        // follow / rule 不生成专属规则 —— 那正是「跟随全局 / 走默认规则表」的含义
        #expect(out.contains("dev-aabbccddee04,") == false)
        #expect(out.contains("dev-aabbccddee05,") == false)
        // 但它们的凭据仍在 listener 里（否则设备连不上）
        #expect(out.contains("dev-aabbccddee04"))
    }

    @Test("global 模式缺目标时跳过规则，不写出半条坏规则")
    func globalWithoutTarget() {
        let temp = TempDevices()
        var d = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true)!
        d.policyMode = .global; d.policyTarget = ""
        _ = temp.store.save(d)
        let out = ConfigBuilder(config: AppConfig(), directory: temp.directory).applyDevicePolicies(base)
        #expect(out.contains("IN-USER,dev-aabbccddee01,") == false)
    }

    @Test("重复生成不叠加 listener 块")
    func idempotentListener() {
        let temp = TempDevices()
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true)
        let builder = ConfigBuilder(config: AppConfig(), directory: temp.directory)
        let once = builder.applyDevicePolicies(base)
        let twice = builder.applyDevicePolicies(once)
        #expect(twice.components(separatedBy: "listeners:\n").count == 2)
    }
}
