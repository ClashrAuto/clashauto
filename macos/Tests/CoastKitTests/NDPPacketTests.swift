import Foundation
import Testing
@testable import CoastHelperProtocol

@Suite("NDP 报文构造（IPv6 邻居通告）")
struct NDPPacketTests {

    // 帧内偏移：以太(0..14) + IPv6(14..54) + ICMPv6 NA(54..86)。
    private static let ethEnd = 14
    private static let ip6Src = 22    // 14 + 8
    private static let ip6Dst = 38    // 14 + 24
    private static let icmp = 54      // 14 + 40
    private static let naTarget = 62  // icmp + 8
    private static let naOption = 78  // icmp + 24
    private static let naTLLA = 80    // icmp + 26

    @Test("IPv6 解析：压缩写法、区标、非法")
    func ipv6Parse() {
        #expect(NDPPacket.ipv6Bytes("::1") == [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1])
        #expect(NDPPacket.ipv6Bytes("fe80::1")?.prefix(2) == [0xFE, 0x80])
        // 带 %zone 的先去区标再解析
        #expect(NDPPacket.ipv6Bytes("fe80::1%en0") == NDPPacket.ipv6Bytes("fe80::1"))
        #expect(NDPPacket.ipv6Bytes("2408:8240:c000::1")?.prefix(2) == [0x24, 0x08])
        #expect(NDPPacket.ipv6Bytes("192.168.1.1") == nil)   // v4 不是 v6
        #expect(NDPPacket.ipv6Bytes("nonsense") == nil)
    }

    @Test("NA 帧总长 86，以太 + IPv6 + ICMPv6 结构正确")
    func naStructure() {
        let device = ARPPacket.MAC("dd:dd:dd:dd:dd:dd")!
        let selfMAC = ARPPacket.MAC("aa:aa:aa:aa:aa:aa")!
        let routerLL = NDPPacket.ipv6Bytes("fe80::1")!
        let frame = NDPPacket.poison(deviceMAC: device, selfMAC: selfMAC, routerLL6: routerLL)

        #expect(frame.count == 86)
        // 以太头：目的 = 设备本人，源 = 本机，EtherType = IPv6
        #expect(Array(frame[0..<6]) == device.bytes)
        #expect(Array(frame[6..<12]) == selfMAC.bytes)
        #expect(Array(frame[12..<14]) == [0x86, 0xDD])
        // IPv6：version=6、next-header=58(ICMPv6)、hop-limit=255、payload-length=32
        #expect(frame[Self.ethEnd] & 0xF0 == 0x60)
        #expect(frame[Self.ethEnd + 4] == 0 && frame[Self.ethEnd + 5] == 32)
        #expect(frame[Self.ethEnd + 6] == 58)
        #expect(frame[Self.ethEnd + 7] == 255)
        // ICMPv6：type=136(NA)、code=0
        #expect(frame[Self.icmp] == 136)
        #expect(frame[Self.icmp + 1] == 0)
        // TLLA 选项头：type=2、len=1
        #expect(Array(frame[Self.naOption..<Self.naOption + 2]) == [2, 1])
    }

    @Test("★ 单播：以太目的必须是设备本人，绝不是组播 33:33:*")
    func mustUnicastNotMulticast() {
        // 安全命门(同 v4 ARP)：L2 单播只影响目标那一台；发到 33:33:* 组播会污染全网 v6 邻居缓存。
        let device = ARPPacket.MAC("dd:dd:dd:dd:dd:dd")!
        let selfMAC = ARPPacket.MAC("aa:aa:aa:aa:aa:aa")!
        let routerLL = NDPPacket.ipv6Bytes("fe80::abcd")!
        let frame = NDPPacket.poison(deviceMAC: device, selfMAC: selfMAC, routerLL6: routerLL)
        #expect(Array(frame[0..<6]) == device.bytes)
        #expect(frame[0] != 0x33)   // 不是 IPv6 组播 MAC
        // L3 目的是 ff02::1（组播，设备是成员必处理）——单播由 L2 保证
        #expect(Array(frame[Self.ip6Dst..<Self.ip6Dst + 16]) == NDPPacket.allNodes)
    }

    @Test("★ 投毒：target = 路由器 LL，TLLA = 本机 MAC，标志含 Override")
    func poisonPointsAtSelf() {
        let device = ARPPacket.MAC("dd:dd:dd:dd:dd:dd")!
        let selfMAC = ARPPacket.MAC("aa:aa:aa:aa:aa:aa")!
        let routerLL = NDPPacket.ipv6Bytes("fe80::1")!
        let frame = NDPPacket.poison(deviceMAC: device, selfMAC: selfMAC, routerLL6: routerLL)
        // target address = 路由器 LL（设备缓存里要被覆盖的那个键）
        #expect(Array(frame[Self.naTarget..<Self.naTarget + 16]) == routerLL)
        // TLLA = 本机 MAC（把「路由器」指向本机）
        #expect(Array(frame[Self.naTLLA..<Self.naTLLA + 6]) == selfMAC.bytes)
        // 源地址冒充路由器 LL
        #expect(Array(frame[Self.ip6Src..<Self.ip6Src + 16]) == routerLL)
        // 标志：Router|Override，且**不带 Solicited**（组播 solicited 非法）
        #expect(frame[Self.icmp + 4] == NDPPacket.flagRouter | NDPPacket.flagOverride)
        #expect(frame[Self.icmp + 4] & NDPPacket.flagSolicited == 0)
    }

    @Test("★ 复原：TLLA = 真路由器 MAC，把设备缓存改回去")
    func restorePointsAtRealRouter() {
        // 命门：复原发的 TLLA 必须是真路由器 MAC，发错（比如还发本机）设备永久 v6 断网。
        let device = ARPPacket.MAC("dd:dd:dd:dd:dd:dd")!
        let selfMAC = ARPPacket.MAC("aa:aa:aa:aa:aa:aa")!
        let routerMAC = ARPPacket.MAC("cc:cc:cc:cc:cc:cc")!
        let routerLL = NDPPacket.ipv6Bytes("fe80::1")!
        let frame = NDPPacket.restore(deviceMAC: device, selfMAC: selfMAC,
                                      routerLL6: routerLL, routerMAC6: routerMAC)
        #expect(Array(frame[Self.naTLLA..<Self.naTLLA + 6]) == routerMAC.bytes)  // 真路由器
        #expect(Array(frame[Self.naTLLA..<Self.naTLLA + 6]) != selfMAC.bytes)    // 不是本机
        #expect(Array(frame[Self.naTarget..<Self.naTarget + 16]) == routerLL)
    }

    @Test("ICMPv6 校验和正确（含伪首部）：整包重算应为 0")
    func checksumValid() {
        // 反码校验和的性质：把已填好校验和字段的报文连同伪首部整体重算，结果为 0。
        let device = ARPPacket.MAC("dd:dd:dd:dd:dd:dd")!
        let selfMAC = ARPPacket.MAC("aa:aa:aa:aa:aa:aa")!
        let routerLL = NDPPacket.ipv6Bytes("fe80::1")!
        let frame = NDPPacket.poison(deviceMAC: device, selfMAC: selfMAC, routerLL6: routerLL)
        let src = Array(frame[Self.ip6Src..<Self.ip6Src + 16])
        let dst = Array(frame[Self.ip6Dst..<Self.ip6Dst + 16])
        let icmp = Array(frame[Self.icmp..<frame.count])
        // 校验和字段非全 0（真算过）
        #expect(!(icmp[2] == 0 && icmp[3] == 0))
        // 连校验和字段一起重算 → 0
        #expect(NDPPacket.icmp6Checksum(srcIP6: src, dstIP6: dst, icmp: icmp) == 0)
    }
}
