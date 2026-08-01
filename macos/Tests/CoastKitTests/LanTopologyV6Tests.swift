import Foundation
import Testing
@testable import CoastKit

@Suite("IPv6 拓扑解析（netstat / ndp）")
struct LanTopologyV6Tests {

    private let netstat = """
    Routing tables

    Internet6:
    Destination                             Gateway                         Flags           Netif Expire
    default                                 fe80::1%en0                     UGcg            en0
    ::1                                     ::1                             UHL             lo0
    fe80::%en0/64                           link#4                          UCI             en0
    """

    // macOS `ndp -an`：注意 MAC 不补前导零（a4:2b:8c:1:2e:3f），且组播条目(33:33:*)混在里头。
    private let ndp = """
    Neighbor                             Linklayer Address  Netif Expire    St Flgs Prbs
    fe80::1%en0                          a4:2b:8c:1:2e:3f   en0 23s         R
    2408:8240:c000::1%en0                a4:2b:8c:1:2e:3f   en0 permanent   R
    fe80::c0a:beef%en0                   aa:bb:cc:dd:ee:1   en0 5s          R
    2408:8240:c000:abcd:1122:3344:5566:7788%en0  aa:bb:cc:dd:ee:1  en0 permanent R
    (incomplete)线也可能出现:
    fe80::dead%en0                       (incomplete)       en0 (none)      I
    ff02::1%en0                          33:33:0:0:0:1      en0 permanent   R
    """

    @Test("默认路由：取链路本地下一跳，去 zone，出口网卡取 zone")
    func defaultRoute() {
        let route = LanTopology.parseDefaultRouteV6(netstat)
        #expect(route?.gateway == "fe80::1")
        #expect(route?.interface == "en0")
    }

    @Test("默认路由缺失时返回 nil")
    func noDefaultRoute() {
        #expect(LanTopology.parseDefaultRouteV6("Internet6:\n::1  ::1  UHL  lo0") == nil)
    }

    @Test("邻居表解析：跳过表头/incomplete/组播，MAC 补零")
    func ndpParse() {
        let neighbors = LanTopology.parseNdpNeighbors(ndp)
        // 组播 33:33:* 与 (incomplete) 都被 normalizeMAC 滤掉
        #expect(neighbors.count == 4)
        #expect(neighbors.contains { $0.ip == "fe80::1" && $0.mac == "a4:2b:8c:01:2e:3f" })
        #expect(neighbors.contains { $0.ip == "2408:8240:c000::1" && $0.mac == "a4:2b:8c:01:2e:3f" })
        #expect(!neighbors.contains { $0.mac.hasPrefix("33:33") })
    }

    @Test("路由器 MAC：按规范化字节匹配 LL")
    func routerMAC() {
        let neighbors = LanTopology.parseNdpNeighbors(ndp)
        #expect(LanTopology.macOf(routerLL: "fe80::1", in: neighbors) == "a4:2b:8c:01:2e:3f")
        // 压缩写法不同也能配上（都归一成字节比较）
        #expect(LanTopology.macOf(routerLL: "fe80:0:0:0:0:0:0:1", in: neighbors) == "a4:2b:8c:01:2e:3f")
        #expect(LanTopology.macOf(routerLL: "fe80::9999", in: neighbors) == nil)
    }

    @Test("设备 v6：按 MAC 反查，只留可路由地址（丢链路本地）")
    func deviceV6ByMAC() {
        let neighbors = LanTopology.parseNdpNeighbors(ndp)
        let deviceMAC = "aa:bb:cc:dd:ee:01"   // 规范化后
        let routable = neighbors.filter { $0.mac == deviceMAC && LanTopology.isRoutableV6($0.ip) }
        #expect(routable.count == 1)
        #expect(routable.first?.ip == "2408:8240:c000:abcd:1122:3344:5566:7788")
        // 链路本地 fe80::c0a:beef 被排除
        #expect(!routable.contains { $0.ip.hasPrefix("fe80") })
    }

    @Test("地址分类：链路本地 / 可路由")
    func classification() {
        #expect(LanTopology.isLinkLocalV6("fe80::1"))
        #expect(!LanTopology.isLinkLocalV6("2408::1"))
        #expect(LanTopology.isRoutableV6("2408:8240::5"))    // 全局单播 2000::/3
        #expect(LanTopology.isRoutableV6("fd00:abcd::1"))    // ULA fc00::/7
        #expect(!LanTopology.isRoutableV6("fe80::1"))         // 链路本地
        #expect(!LanTopology.isRoutableV6("ff02::1"))         // 组播
    }
}
