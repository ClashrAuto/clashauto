import Foundation
import Testing
@testable import CoastKit

/// 多网卡拓扑解析（netstat 默认路由 + arp 按接口分组）。
///
/// 为什么值得单测：同时接两条上行时，「哪张卡的网关是谁」决定了投毒打向谁、复原时把哪个 MAC
/// 装回设备。判错了不会崩、不报错，只表现为「某张卡上的设备代理不了」或「关代理后断网到缓存
/// 老化」—— 全是静默故障。夹具用的是真机 `netstat -rn -f inet` / `arp -an` 原文。
@Suite("多网卡拓扑解析（netstat / arp）")
struct LanTopologyMultiNicTests {

    // 真机原文（macOS 26.5）：**第一条默认路由的下一跳是 `link#20`**（utun/VPN 那条），
    // 不是 IPv4 —— 必须跳过，否则会把 VPN 隧道当成局域网出口。
    private let netstatOneUplink = """
    Routing tables

    Internet:
    Destination        Gateway            Flags               Netif Expire
    default            link#20            UCSg                utun4
    default            192.168.20.1       UGScIg                en1
    127                127.0.0.1          UCS                   lo0
    """

    // 两条上行：有线 en0 接 A 路由器、Wi-Fi en1 接 B 路由器。
    private let netstatTwoUplinks = """
    Routing tables

    Internet:
    Destination        Gateway            Flags               Netif Expire
    default            192.168.20.1       UGScg                 en0
    default            192.168.31.1       UGScIg                en1
    127                127.0.0.1          UCS                   lo0
    """

    // 真机原文：MAC **不补前导零**，且带 `on <iface>`。
    private let arpTwoUplinks = """
    ? (192.168.20.1) at 70:a7:41:a4:19:7b on en0 ifscope [ethernet]
    ? (192.168.20.10) at b6:ac:7d:d6:91:94 on en0 ifscope [ethernet]
    ? (192.168.31.1) at cc:d8:43:9b:e3:b6 on en1 ifscope [ethernet]
    ? (192.168.31.238) at 7e:de:c2:a0:15:9a on en1 ifscope [ethernet]
    ? (169.254.69.9) at 1e:8:91:8f:25:1c on en1 [ethernet]
    """

    @Test("默认路由：跳过非 IPv4 下一跳（VPN 的 link#N），只留真正的上行")
    func skipsNonIPv4NextHop() {
        let routes = LanTopology.parseDefaultRoutes(netstatOneUplink)
        #expect(routes.count == 1)
        #expect(routes.first?.ip == "192.168.20.1")
        #expect(routes.first?.interface == "en1")
    }

    @Test("默认路由：两条上行都要认出来，顺序即出现顺序（index 0 = 主网卡）")
    func parsesBothUplinks() {
        let routes = LanTopology.parseDefaultRoutes(netstatTwoUplinks)
        #expect(routes.count == 2)
        #expect(routes[0].interface == "en0")
        #expect(routes[0].ip == "192.168.20.1")
        #expect(routes[1].interface == "en1")
        #expect(routes[1].ip == "192.168.31.1")
    }

    @Test("ARP 按接口分组：每张卡各一桶，不互相覆盖")
    func groupsArpByInterface() {
        let table = LanTopology.parseArpTableByInterface(arpTwoUplinks)
        #expect(table["en0"]?["192.168.20.1"] == "70:a7:41:a4:19:7b")
        #expect(table["en1"]?["192.168.31.1"] == "cc:d8:43:9b:e3:b6")
        // 各卡只看得到自己那一桶里的条目。
        #expect(table["en0"]?["192.168.31.1"] == nil)
        #expect(table["en1"]?["192.168.20.1"] == nil)
        // 不补零的 MAC 要被补齐（1e:8:… → 1e:08:…），这是 parseARPLine 的既有契约。
        #expect(table["en1"]?["169.254.69.9"] == "1e:08:91:8f:25:1c")
    }

    // ★ 这条是整个改动的理由：**两台路由器都用出厂默认 192.168.1.1**。
    //   拍平成一张全局 IP→MAC 表时，后读到的那条直接覆盖先读到的，于是一张卡拿到**另一台
    //   路由器**的 MAC —— 复原时就会把错的 MAC 装回设备，设备断网到缓存老化。
    @Test("两台路由器同 IP 时，各卡仍拿到各自的 MAC（拍平就会张冠李戴）")
    func sameGatewayIpOnBothUplinks() {
        // ⚠️ 用真实 MAC，别随手写 bb:bb:… —— **首字节最低位（I/G）为 1 的都是组播**，
        //   normalizeMAC 会按以太网标准把它们滤掉（0xBB & 1 == 1）。第一版夹具就写成了
        //   bb:bb:…，于是"测试失败"指的是夹具不合法，不是代码错。
        let arp = """
        ? (192.168.1.1) at 70:a7:41:a4:19:7b on en0 ifscope [ethernet]
        ? (192.168.1.1) at cc:d8:43:9b:e3:b6 on en1 ifscope [ethernet]
        """
        let table = LanTopology.parseArpTableByInterface(arp)
        #expect(table["en0"]?["192.168.1.1"] == "70:a7:41:a4:19:7b")
        #expect(table["en1"]?["192.168.1.1"] == "cc:d8:43:9b:e3:b6")
    }

    @Test("incomplete 条目不进表（没有 MAC 就等于没有这条）")
    func skipsIncomplete() {
        let arp = """
        ? (192.168.20.7) at (incomplete) on en0 ifscope [ethernet]
        ? (192.168.20.1) at 70:a7:41:a4:19:7b on en0 ifscope [ethernet]
        """
        let table = LanTopology.parseArpTableByInterface(arp)
        #expect(table["en0"]?.count == 1)
        #expect(table["en0"]?["192.168.20.7"] == nil)
    }
}
