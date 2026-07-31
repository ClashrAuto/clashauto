import Foundation
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
