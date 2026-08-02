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

    @Test("★ 旧库（列还叫 password）也得能用——改名没写迁移，台账曾整个哑掉")
    func migratesRenamedColumn() {
        let directory = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-dev-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        // 8e3463a 之前的表结构：第六列叫 `password`，没有 `last_ip`、没有 `type_override`。
        let old = try! SQLiteDatabase(path: SQLitePaths.databasePath(configDir: directory))
        old.exec("""
            CREATE TABLE device (
              mac TEXT PRIMARY KEY,
              alias TEXT NOT NULL DEFAULT '',
              proxy_enabled INTEGER NOT NULL DEFAULT 0,
              policy_mode TEXT NOT NULL DEFAULT 'follow',
              policy_target TEXT NOT NULL DEFAULT '',
              password TEXT NOT NULL DEFAULT '',
              first_seen INTEGER NOT NULL DEFAULT 0)
            """)
        old.close()

        // 补列没做的话，下面每一句 SQL 都会以「no such column: last_ip」失败，
        // 而 `save()` 只返回 false —— 用户看到的是「改了没反应」。
        let store = DeviceStore(configDir: directory)
        var device = DeviceStore.Device(mac: "aa:bb:cc:dd:ee:ff")
        device.alias = "打印机"
        device.lastIP = "192.168.1.9"
        device.typeOverride = "printer"
        #expect(store.save(device))
        let loaded = store.device(mac: "aa:bb:cc:dd:ee:ff")
        #expect(loaded?.alias == "打印机")
        #expect(loaded?.lastIP == "192.168.1.9")
        #expect(loaded?.typeOverride == "printer")
    }

    @Test("类型覆盖往返——空串=自动识别")
    func typeOverrideRoundTrip() {
        let temp = TempDevices()
        var device = DeviceStore.Device(mac: "11:22:33:44:55:66")
        device.typeOverride = "nas"
        #expect(temp.store.save(device))
        #expect(temp.store.device(mac: "11:22:33:44:55:66")?.typeOverride == "nas")
        device.typeOverride = ""
        #expect(temp.store.save(device))
        #expect(temp.store.device(mac: "11:22:33:44:55:66")?.typeOverride == "")
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

    // MARK: - 设备列表持久化

    private func scanned(mac: String, ip: String, hostname: String = "",
                         vendor: String = "") -> LanBrowser.Device {
        var device = LanBrowser.Device(mac: mac, ip: ip, interface: "en0")
        device.hostname = hostname
        device.vendor = vendor
        return device
    }

    @Test("★ 扫到的设备要落库——不落的话列表只是邻居表的一次投影，重启后一片空白")
    func recordSeenPersistsIdentity() {
        let temp = TempDevices()
        temp.store.recordSeen([scanned(mac: "aa:bb:cc:dd:ee:01", ip: "192.168.1.11",
                                       hostname: "living-room-tv", vendor: "Sony")])

        // 换一个 store 实例 = 重启一次进程，读的是同一个库文件。
        let reopened = DeviceStore(configDir: temp.directory)
        let loaded = reopened.device(mac: "aa:bb:cc:dd:ee:01")
        #expect(loaded?.hostname == "living-room-tv")
        #expect(loaded?.vendor == "Sony")
        #expect(loaded?.lastIP == "192.168.1.11")
        #expect(loaded?.interface == "en0")
    }

    @Test("★ 扫描不覆盖用户设置，用户保存也不抹掉扫描存的身份——两半边分开写")
    func scanAndUserWritesDoNotClobberEachOther() {
        let temp = TempDevices()
        temp.store.recordSeen([scanned(mac: "aa:bb:cc:dd:ee:01", ip: "192.168.1.11",
                                       hostname: "tv", vendor: "Sony")])
        _ = temp.store.setAlias(mac: "aa:bb:cc:dd:ee:01", "客厅电视")

        // 详情窗里选策略 / 选类型走的是 store 自己的读-改-写入口 ——
        // 它既不该把身份列写没，也不该把用户刚起的备注名冲掉。
        #expect(temp.store.setPolicy(mac: "aa:bb:cc:dd:ee:01", mode: .direct, target: "") != nil)
        #expect(temp.store.setTypeOverride(mac: "aa:bb:cc:dd:ee:01", "tvbox") != nil)
        #expect(temp.store.device(mac: "aa:bb:cc:dd:ee:01")?.alias == "客厅电视")

        var loaded = temp.store.device(mac: "aa:bb:cc:dd:ee:01")
        #expect(loaded?.hostname == "tv")      // 身份还在
        #expect(loaded?.vendor == "Sony")
        #expect(loaded?.policyMode == .direct) // 用户的改动生效了

        // 反过来：下一轮扫描不能把备注名/策略洗掉。
        temp.store.recordSeen([scanned(mac: "aa:bb:cc:dd:ee:01", ip: "192.168.1.12",
                                       hostname: "tv", vendor: "Sony")])
        loaded = temp.store.device(mac: "aa:bb:cc:dd:ee:01")
        #expect(loaded?.alias == "客厅电视")
        #expect(loaded?.policyMode == .direct)
        #expect(loaded?.lastIP == "192.168.1.12")   // 地址跟着走
    }

    @Test("★ 给没开过代理的设备起名字不能静默失效——原来 setAlias 找不到记录就返回 nil")
    func aliasCreatesRecordForUnknownDevice() {
        let temp = TempDevices()
        #expect(temp.store.setAlias(mac: "aa:bb:cc:dd:ee:09", "打印机") != nil)
        #expect(temp.store.device(mac: "aa:bb:cc:dd:ee:09")?.alias == "打印机")
    }

    @Test("用户动过的记录一律留着，没动过的按最近可见时间清")
    func purgeKeepsUserConfigured() {
        let temp = TempDevices()
        let old = Date(timeIntervalSince1970: 1_000_000)
        // 只是被扫到过的两台：一台旧、一台新
        temp.store.recordSeen([scanned(mac: "aa:bb:cc:dd:ee:01", ip: "192.168.1.11")], at: old)
        temp.store.recordSeen([scanned(mac: "aa:bb:cc:dd:ee:02", ip: "192.168.1.12")])
        // 同样旧、但用户开过代理 / 起过名的两台
        temp.store.recordSeen([scanned(mac: "aa:bb:cc:dd:ee:03", ip: "192.168.1.13")], at: old)
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:03", true, ip: "192.168.1.13")
        temp.store.recordSeen([scanned(mac: "aa:bb:cc:dd:ee:04", ip: "192.168.1.14")], at: old)
        _ = temp.store.setAlias(mac: "aa:bb:cc:dd:ee:04", "打印机")

        #expect(temp.store.purgeStale(before: Date(timeIntervalSince1970: 2_000_000)) == 1)
        #expect(temp.store.all().map(\.mac) == ["aa:bb:cc:dd:ee:02",
                                                "aa:bb:cc:dd:ee:03",
                                                "aa:bb:cc:dd:ee:04"])
    }

    @Test("★ 老库升级后 last_seen 不能是 0——那等于「1970 年见过」，一升级全被清掉")
    func legacyRowsGetLastSeenBackfilled() {
        let temp = TempDevices()
        // 升级前的写法：只有用户那半边，没有 last_seen 这一列。
        var device = DeviceStore.Device(mac: "aa:bb:cc:dd:ee:ff")
        device.firstSeen = Date()
        #expect(temp.store.save(device))
        let raw = try! SQLiteDatabase(path: SQLitePaths.databasePath(configDir: temp.directory))
        raw.exec("UPDATE device SET last_seen = 0")
        raw.close()

        let reopened = DeviceStore(configDir: temp.directory)   // 建表/补列会顺手回填
        #expect(reopened.purgeStale(before: Date().addingTimeInterval(-3600)) == 0)
        #expect(reopened.device(mac: "aa:bb:cc:dd:ee:ff") != nil)
    }
}

@Suite("离线行的去留")
struct OfflineRowTests {

    private func record(mac: String, ip: String, seenSecondsAgo: TimeInterval)
        -> DeviceStore.Device {
        var device = DeviceStore.Device(mac: mac)
        device.lastIP = ip
        device.lastSeen = Date().addingTimeInterval(-seenSecondsAgo)
        return device
    }

    @Test("★ 没有任何用户配置、又不在当前网络/太久没见的，不该变成一行灰设备")
    func dropsStrayLedgerRows() {
        // 真实场景（本机实测）：台账里 10 条记录、当前网络上一台都不在，
        // 设备页于是凭空多出 10 行离线设备，其中 3 行连名字都没有。
        let stale = record(mac: "aa:bb:cc:dd:ee:01", ip: "192.168.20.173", seenSecondsAgo: 3 * 86400)
        #expect(DeviceStore.keepsOfflineRow(stale, localPrefix: "192.168.20.") == false)

        let otherNetwork = record(mac: "aa:bb:cc:dd:ee:02", ip: "10.0.0.5", seenSecondsAgo: 60)
        #expect(DeviceStore.keepsOfflineRow(otherNetwork, localPrefix: "192.168.20.") == false)
    }

    @Test("刚才还在的同网段设备留着——睡一觉掉出邻居表不等于这台设备没了")
    func keepsRecentSameSubnet() {
        let recent = record(mac: "aa:bb:cc:dd:ee:03", ip: "192.168.20.42", seenSecondsAgo: 600)
        #expect(DeviceStore.keepsOfflineRow(recent, localPrefix: "192.168.20."))
    }

    @Test("★ 用户动过的一律留——开着代理的设备离线时更得看得见，否则撤销不了")
    func keepsUserConfiguredForever() {
        for mutate in [{ (d: inout DeviceStore.Device) in d.proxyEnabled = true },
                       { d in d.alias = "客厅电视" },
                       { d in d.policyMode = .direct },
                       { d in d.typeOverride = "printer" }] {
            var device = record(mac: "aa:bb:cc:dd:ee:04", ip: "10.0.0.9",
                                seenSecondsAgo: 400 * 86400)
            mutate(&device)
            #expect(DeviceStore.keepsOfflineRow(device, localPrefix: "192.168.20."))
        }
    }

    @Test("拿不到本机地址时不做同网段判定——宁可多显示一行，也不要凭错判据抹掉设备")
    func noLocalAddressSkipsSubnetCheck() {
        let recent = record(mac: "aa:bb:cc:dd:ee:05", ip: "10.0.0.5", seenSecondsAgo: 60)
        #expect(DeviceStore.keepsOfflineRow(recent, localPrefix: ""))
    }

    @Test("/24 前缀")
    func subnetPrefix() {
        #expect(DeviceStore.subnetPrefix("192.168.20.7") == "192.168.20.")
        #expect(DeviceStore.subnetPrefix("") == "")
        #expect(DeviceStore.subnetPrefix("fe80::1") == "")
    }

    @Test("★ 显示名永远非空——三档全空时回落到 MAC，不能留一行没有名字的设备")
    func displayNameNeverEmpty() {
        let nameless = LanBrowser.Device(mac: "aa:bb:cc:dd:ee:06", ip: "", interface: "")
        #expect(nameless.displayName == "aa:bb:cc:dd:ee:06")

        var withVendor = nameless
        withVendor.vendor = "Sony"
        #expect(withVendor.displayName == "Sony")

        var blank = DeviceStore.Device(mac: "aa:bb:cc:dd:ee:07")
        #expect(blank.displayLabel == "aa:bb:cc:dd:ee:07")
        blank.hostname = "tv"
        #expect(blank.displayLabel == "tv")
        blank.alias = "客厅电视"
        #expect(blank.displayLabel == "客厅电视")
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
