import Foundation
import Network
import Testing
@testable import CoastKit

@Suite("MAC 规范化")
struct MACNormalizeTests {

    @Test("★ macOS 的 arp 不补前导零，必须补齐后才能查 OUI")
    func padsOctets() {
        // 真实 `arp -an` 输出里就是这样打印的。不补零的话 "1:2:3:4:5:6" 取前缀会取成 "1:2:3"，
        // 厂商永远查不到，而且不报错 —— 只是所有设备的厂商栏莫名其妙全空。
        #expect(LanBrowser.normalizeMAC("3c:84:6a:1:2:3") == "3c:84:6a:01:02:03")
        // 注意首字节要选**偶数**：奇数意味着 I/G 位置 1，那是组播 MAC，会被下面那组用例
        // 里的规则正当地拒掉。写这条用例时我第一版用了 "1:2:..."，自己撞上了这个规则。
        #expect(LanBrowser.normalizeMAC("2:3:4:5:6:7") == "02:03:04:05:06:07")
        #expect(LanBrowser.normalizeMAC("AA:BB:CC:DD:EE:FF") == "aa:bb:cc:dd:ee:ff")
    }

    @Test("段数不对/非十六进制/incomplete 一律丢掉")
    func rejectsInvalid() {
        #expect(LanBrowser.normalizeMAC("(incomplete)") == nil)
        #expect(LanBrowser.normalizeMAC("aa:bb:cc") == nil)
        #expect(LanBrowser.normalizeMAC("aa:bb:cc:dd:ee:ff:00") == nil)
        #expect(LanBrowser.normalizeMAC("zz:bb:cc:dd:ee:ff") == nil)
        #expect(LanBrowser.normalizeMAC("aaa:bb:cc:dd:ee:ff") == nil)
    }

    @Test("全零与广播地址不是设备")
    func rejectsSentinels() {
        #expect(LanBrowser.normalizeMAC("0:0:0:0:0:0") == nil)
        #expect(LanBrowser.normalizeMAC("ff:ff:ff:ff:ff:ff") == nil)
    }
}

@Suite("arp -an 解析")
struct ARPParseTests {

    @Test("标准行")
    func standardLine() {
        let device = LanBrowser.parseARPLine("? (192.168.1.1) at 3c:84:6a:1:2:3 on en0 ifscope [ethernet]")
        #expect(device?.ip == "192.168.1.1")
        #expect(device?.mac == "3c:84:6a:01:02:03")
        #expect(device?.interface == "en0")
    }

    @Test("带主机名的行（arp 有时会自己解析出来）")
    func namedLine() {
        let device = LanBrowser.parseARPLine("router.lan (192.168.1.1) at aa:bb:cc:dd:ee:ff on en0 [ethernet]")
        #expect(device?.ip == "192.168.1.1")
        #expect(device?.mac == "aa:bb:cc:dd:ee:ff")
    }

    @Test("incomplete 的条目要跳过 —— 那是没解析出来的占位")
    func skipsIncomplete() {
        #expect(LanBrowser.parseARPLine("? (192.168.1.9) at (incomplete) on en0 ifscope [ethernet]") == nil)
    }

    @Test("垃圾行不崩")
    func garbage() {
        #expect(LanBrowser.parseARPLine("") == nil)
        #expect(LanBrowser.parseARPLine("完全不相关的一行") == nil)
        #expect(LanBrowser.parseARPLine("? (999.1.1.1) at aa:bb:cc:dd:ee:ff on en0") == nil)
    }
}

@Suite("设备类型与排序")
struct DeviceClassifyTests {
    private func device(host: String = "", vendor: String = "", gateway: Bool = false) -> LanBrowser.Device {
        var d = LanBrowser.Device(mac: "aa:bb:cc:dd:ee:ff", ip: "192.168.1.2", interface: "en0")
        d.hostname = host; d.vendor = vendor; d.isGateway = gateway
        return d
    }

    @Test("网关恒判为路由器，压过其它关键词")
    func gatewayWins() {
        #expect(device(host: "my-iphone", gateway: true).typeKey == "router")
    }

    @Test("按主机名/厂商关键词分类")
    func keywords() {
        #expect(device(host: "Jacks-iPhone").typeKey == "phone")
        #expect(device(host: "Jacks-iPad").typeKey == "tablet")
        #expect(device(host: "MacBook-Pro").typeKey == "computer")
        #expect(device(vendor: "Synology Incorporated").typeKey == "nas")
        #expect(device(host: "living-room-tv").typeKey == "tvbox")
        #expect(device(vendor: "TP-LINK TECHNOLOGIES CO.,LTD.").typeKey == "router")
    }

    @Test("认不出来就是 unknown，不硬猜")
    func unknown() {
        #expect(device().typeKey == "unknown")
        #expect(device(host: "esp32-a3f1").typeKey == "unknown")
    }

    @Test("显示名回落顺序：主机名 > 厂商 > IP")
    func displayNameFallback() {
        #expect(device(host: "nas.local", vendor: "Synology").displayName == "nas.local")
        #expect(device(vendor: "Synology").displayName == "Synology")
        #expect(device().displayName == "192.168.1.2")
    }

    @Test("IP 排序按数值，不按字符串 —— 否则 .10 会排在 .2 前面")
    func ipSortIsNumeric() {
        #expect(LanBrowser.ipSortKey("192.168.1.2") < LanBrowser.ipSortKey("192.168.1.10"))
        #expect(LanBrowser.ipSortKey("192.168.1.255") < LanBrowser.ipSortKey("192.168.2.1"))
        #expect(LanBrowser.ipSortKey("坏数据") == 0)
    }

    @Test("IPv4 校验")
    func ipv4Validation() {
        #expect(LanBrowser.isIPv4("192.168.1.1"))
        #expect(LanBrowser.isIPv4("0.0.0.0"))
        #expect(LanBrowser.isIPv4("256.1.1.1") == false)
        #expect(LanBrowser.isIPv4("192.168.1") == false)
        #expect(LanBrowser.isIPv4("::1") == false)
    }
}

@Suite("OUI 厂商表")
struct OUITests {
    @Test("能从真实 oui.txt 查到厂商")
    func looksUpRealVendor() {
        // 000000 = XEROX，是表里第一条；用它验「表确实加载了」
        #expect(OUIDatabase.shared.vendor(for: "00:00:00:11:22:33") == "XEROX")
    }

    @Test("查不到返回空串而不是崩")
    func missingVendor() {
        #expect(OUIDatabase.shared.vendor(for: "ff:ff:ff:00:00:00") == "")
        #expect(OUIDatabase.shared.vendor(for: "") == "")
    }
}

@Suite("组播/广播过滤")
struct MulticastFilterTests {

    @Test("★ 组播 MAC 不是设备 —— 判首字节的 I/G 位，一条规则盖住三类")
    func rejectsMulticastMAC() {
        // 实测邻居表里就有 224.0.0.251 / 01:00:5e:00:00:fb（mDNS 组播组），
        // 不滤掉的话「mdns.mcast.net」会作为一台设备出现在列表里。
        #expect(LanBrowser.normalizeMAC("01:00:5e:00:00:fb") == nil)   // IPv4 组播
        #expect(LanBrowser.normalizeMAC("33:33:00:00:00:01") == nil)   // IPv6 组播
        #expect(LanBrowser.normalizeMAC("ff:ff:ff:ff:ff:ff") == nil)   // 广播
        // 单播不受影响
        #expect(LanBrowser.normalizeMAC("3c:84:6a:01:02:03") != nil)
        #expect(LanBrowser.normalizeMAC("aa:bb:cc:dd:ee:ff") != nil)
    }

    @Test("组播/广播 IP 也拦一道，两边互为双保险")
    func rejectsMulticastIP() {
        #expect(LanBrowser.isUnicastIPv4("192.168.1.1"))
        #expect(LanBrowser.isUnicastIPv4("224.0.0.251") == false)
        #expect(LanBrowser.isUnicastIPv4("239.255.255.250") == false)  // SSDP
        #expect(LanBrowser.isUnicastIPv4("255.255.255.255") == false)
    }

    @Test("整行级别：组播条目直接被丢掉")
    func rejectsMulticastLine() {
        #expect(LanBrowser.parseARPLine(
            "mdns.mcast.net (224.0.0.251) at 1:0:5e:0:0:fb on en1 ifscope permanent [ethernet]") == nil)
    }
}

@Suite("型号发现")
struct DeviceModelBrowserTests {

    @Test("连接键：小写 + 去掉 .local 后缀（Bonjour 服务名与反查主机名的写法对不上）")
    func hostnameKey() {
        #expect(DeviceModelBrowser.key("Wangchaos-iMac.local") == "wangchaos-imac")
        #expect(DeviceModelBrowser.key("Wangchaos-iMac.local.") == "wangchaos-imac")
        #expect(DeviceModelBrowser.key("Wangchaos-iMac") == "wangchaos-imac")
        // .local 只削尾巴，名字中间带 local 的不能动
        #expect(DeviceModelBrowser.key("local-nas") == "local-nas")
    }

    @Test("没广播过型号的设备返回空串，不返回 nil 也不崩")
    func unknownHostname() {
        #expect(DeviceModelBrowser.shared.model(mac: "", hostname: "") == "")
        #expect(DeviceModelBrowser.shared.model(mac: "aa:bb:cc:dd:ee:ff",
                                                hostname: "never-\(UUID().uuidString)") == "")
    }

    @Test("TXT 取型号：model / md / am 依次认（Qt 认前两个，am 是 AirPlay 自己的字段）")
    func modelKeys() {
        #expect(DeviceModelBrowser.model(in: NWTXTRecord(["model": "iMac21,1"])) == "iMac21,1")
        #expect(DeviceModelBrowser.model(in: NWTXTRecord(["md": "Chromecast"])) == "Chromecast")
        #expect(DeviceModelBrowser.model(in: NWTXTRecord(["am": "AppleTV6,2"])) == "AppleTV6,2")
        // 只有空白值等于没有 —— 否则详情窗那一格会显示成一片空白而不是 "-"
        #expect(DeviceModelBrowser.model(in: NWTXTRecord(["model": " "])) == nil)
        #expect(DeviceModelBrowser.model(in: NWTXTRecord(["deviceid": "aa:bb:cc:dd:ee:ff"])) == nil)
    }

    @Test("deviceid 规范成台账那套 MAC 写法（补零 + 小写），否则对不上主键")
    func deviceIDNormalisation() {
        #expect(DeviceModelBrowser.deviceMAC(in: NWTXTRecord(["deviceid": "B6:33:66:14:DA:00"]))
                == "b6:33:66:14:da:00")
        #expect(DeviceModelBrowser.deviceMAC(in: NWTXTRecord(["deviceid": "b6:3:66:4:da:0"]))
                == "b6:03:66:04:da:00")
        #expect(DeviceModelBrowser.deviceMAC(in: NWTXTRecord(["deviceid": "not-a-mac"])) == nil)
        #expect(DeviceModelBrowser.deviceMAC(in: NWTXTRecord([:])) == nil)
    }

    @Test("★ 只认 _device-info 会漏掉一大片——本机 iMac 就只在 _airplay 上广播型号")
    func browsesMoreThanDeviceInfo() {
        #expect(DeviceModelBrowser.serviceTypes.contains("_device-info._tcp"))
        #expect(DeviceModelBrowser.serviceTypes.contains("_airplay._tcp"))
    }
}

@Suite("按 MAC 去重")
struct DedupeByMACTests {

    private func device(_ mac: String, _ ip: String, interface: String = "en0",
                        gateway: Bool = false) -> LanBrowser.Device {
        var device = LanBrowser.Device(mac: mac, ip: ip, interface: interface)
        device.isGateway = gateway
        return device
    }

    /// ★ 这是设备列表「行内容串台/闪烁」的根因：`Device.id` 就是 MAC，
    /// 而邻居表里同一个 MAC 出现多次是常态，重复 id 交给 SwiftUI 的 ForEach 是未定义行为。
    @Test("同一个 MAC 只留一行")
    func collapsesDuplicates() {
        let result = LanBrowser.dedupeByMAC([
            device("aa:bb:cc:dd:ee:01", "192.168.1.2"),
            device("aa:bb:cc:dd:ee:01", "192.168.1.9"),
            device("aa:bb:cc:dd:ee:02", "192.168.1.3"),
        ])
        #expect(result.count == 2)
        #expect(Set(result.map(\.mac)).count == 2)
    }

    /// ★ 最要紧的一档：一台设备除了 DHCP 地址常常还挂着一个 169.254 链路本地地址
    /// （拿到租约之前自配的那个），两条都在邻居表里。留错了的话这台设备在界面上
    /// 顶着一个没用的地址，还会因为不在本机网段而被判成「其它网络」→ 不可代理。
    /// 实测这台开发 Mac 的 `arp -an` 里就有三台是这个样子。
    @Test("可路由地址胜过 169.254 链路本地，且与行序无关")
    func prefersRoutableOverLinkLocal() {
        let routable = device("aa:bb:cc:dd:ee:01", "192.168.20.42")
        let linkLocal = device("aa:bb:cc:dd:ee:01", "169.254.181.166")
        #expect(LanBrowser.dedupeByMAC([linkLocal, routable]).first?.ip == "192.168.20.42")
        #expect(LanBrowser.dedupeByMAC([routable, linkLocal]).first?.ip == "192.168.20.42")
    }

    /// 网关那一条优先级最高 —— 丢了 `isGateway` 的话，路由器那一行会变成一台普通设备，
    /// 于是「不可接管」的判据认不出它，用户能给网关自己开代理。
    @Test("网关那一条压过其它")
    func prefersGateway() {
        let plain = device("aa:bb:cc:dd:ee:01", "192.168.1.2")
        let gateway = device("aa:bb:cc:dd:ee:01", "192.168.1.250", gateway: true)
        #expect(LanBrowser.dedupeByMAC([plain, gateway]).first?.isGateway == true)
        #expect(LanBrowser.dedupeByMAC([gateway, plain]).first?.isGateway == true)
    }

    /// 全平票时按 IP 再按接口名，保证**任何输入顺序**都得到同一个结果 ——
    /// 否则邻居表行序一变，列表里那台设备的 IP 就会在两个值之间来回跳。
    @Test("判据是全序：输入顺序不影响结果")
    func totallyOrdered() {
        let devices = [
            device("aa:bb:cc:dd:ee:01", "192.168.1.5", interface: "en1"),
            device("aa:bb:cc:dd:ee:01", "192.168.1.5", interface: "en0"),
        ]
        #expect(LanBrowser.dedupeByMAC(devices).first?.interface == "en0")
        #expect(LanBrowser.dedupeByMAC(devices.reversed()).first?.interface == "en0")
    }

    @Test("没有重复时原样返回，且保持原次序")
    func keepsOrderWhenUnique() {
        let devices = [device("aa:bb:cc:dd:ee:01", "192.168.1.2"),
                       device("aa:bb:cc:dd:ee:02", "192.168.1.3"),
                       device("aa:bb:cc:dd:ee:03", "192.168.1.4")]
        #expect(LanBrowser.dedupeByMAC(devices).map(\.ip) == devices.map(\.ip))
    }
}
