import Darwin
import Foundation

/// 构造以太网帧里的 IPv6 邻居发现（NDP）报文 —— `ARPPacket` 的 v6 对应物。
///
/// IPv6 没有 ARP：「谁在哪个 MAC」全靠 ICMPv6 的邻居发现（Neighbor Solicitation/Advertisement）。
/// 要把一台设备的 v6 上行引到本机，就得往它的**邻居缓存**里塞一条「默认路由器的链路本地地址
/// 在本机 MAC」——也就是发一条带 Override 的 **Neighbor Advertisement(NA, type 136)**。
///
/// 纯字节操作、可单测（收发那部分 BPF 只能在 root + 真网卡上跑，见 `Redirector`）。
/// 放在 `CoastHelperProtocol`：真正发包的是 **helper**，它只链接这个最小 target；
/// app 侧的测试也 `@testable import` 得到（与 `ARPPacket` 同理）。
///
/// 帧布局与 clashauto-c++ 的 `NdpSpoofer::buildNa` **逐字节一致**（那份在真机上验证过），
/// 见 `neighborAdvertisement`。
public enum NDPPacket {

    // MARK: - NA 标志位（报文第 4 字节的高 3 位）

    /// Router：发送方是路由器。投毒时置位 —— 我们冒充的就是默认路由器。
    public static let flagRouter: UInt8 = 0x80
    /// Solicited：这是对某条 NS 的应答。**只有单播 NA 才允许置位**（组播 solicited 是非法帧、
    /// 会被丢），所以周期性组播投毒 NA 不带它。
    public static let flagSolicited: UInt8 = 0x40
    /// Override：要求收方**覆盖**已有的邻居缓存条目。投毒与复原都靠它把网关 MAC 换掉。
    public static let flagOverride: UInt8 = 0x20

    // MARK: - 常量

    /// all-nodes 链路本地组播 `ff02::1`。设备恒为其成员，必处理发到这里的 NA。
    /// L3 目的用它、L2 目的用设备**单播** MAC —— 只此设备收得到，不污染全网（见 `poison`）。
    public static let allNodes: [UInt8] = {
        var a = [UInt8](repeating: 0, count: 16)
        a[0] = 0xFF
        a[1] = 0x02
        a[15] = 0x01
        return a
    }()

    // MARK: - 地址解析

    /// `"fe80::1"` / `"2408:...::5"` → 16 字节（网络序）。带 `%en0` 区标的会先去掉区标。
    /// 解析不了返回 nil。用 `inet_pton` 而不是手写，v6 的省略/压缩规则坑太多。
    public static func ipv6Bytes(_ text: String) -> [UInt8]? {
        let head = text.split(separator: "%", maxSplits: 1, omittingEmptySubsequences: false)
            .first.map(String.init) ?? text
        var addr = in6_addr()
        guard head.withCString({ inet_pton(AF_INET6, $0, &addr) }) == 1 else { return nil }
        return withUnsafeBytes(of: &addr) { Array($0.bindMemory(to: UInt8.self)) }
    }

    /// 16 字节 → 规范化 v6 串（`inet_ntop`，压缩形式）。长度不对返回 nil。
    /// 用于把从线上学到的设备 v6 源地址拼进 PF 规则 —— `inet_ntop` 的产物必为合法字面量，
    /// 天然无注入风险。
    public static func ipv6String(_ bytes: [UInt8]) -> String? {
        guard bytes.count == 16 else { return nil }
        var addr = in6_addr()
        withUnsafeMutableBytes(of: &addr) { raw in for i in 0..<16 { raw[i] = bytes[i] } }
        var buf = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
        guard inet_ntop(AF_INET6, &addr, &buf, socklen_t(INET6_ADDRSTRLEN)) != nil else { return nil }
        return String(cString: buf)
    }

    /// 可路由 v6：全局单播 `2000::/3` 或唯一本地 `fc00::/7`。链路本地、组播、未指定都不算 ——
    /// 只有可路由源地址才值得进 PF rdr（LL/组播不出网）。
    public static func isRoutableV6(_ bytes: [UInt8]) -> Bool {
        guard bytes.count == 16 else { return false }
        let global = (bytes[0] & 0xE0) == 0x20
        let ula = (bytes[0] & 0xFE) == 0xFC
        return global || ula
    }

    // MARK: - 便捷构造（投毒 / 复原）

    /// 周期性**投毒** NA：告诉设备「路由器链路本地地址 `routerLL6` 的 MAC 是本机（`selfMAC`）」。
    ///
    /// L3 目的 = `ff02::1`（组播，设备是成员必处理），L2 目的 = 设备单播 MAC（只此设备收得到）。
    /// target = 路由器 LL，TLLA = 本机 MAC，标志 = Router|Override（**不带 Solicited** —— 组播
    /// solicited 非法）。source = 路由器 LL（冒充路由器发出）。
    ///
    /// 语义：把设备**非 REACHABLE**（STALE 等）的网关条目持续压在本机 MAC 上。真正把 REACHABLE
    /// 条目翻过来的是「抢答设备 NS」那条单播 solicited NA（本 v1 未实现被动抢答，靠周期投毒 + 设备
    /// 主动 NS 时真路由器与本机的竞态兜底；说明见 `Redirector` 顶部）。
    public static func poison(deviceMAC: ARPPacket.MAC, selfMAC: ARPPacket.MAC,
                              routerLL6: [UInt8]) -> [UInt8] {
        neighborAdvertisement(ethDst: deviceMAC, ethSrc: selfMAC,
                              srcIP6: routerLL6, dstIP6: allNodes,
                              targetIP6: routerLL6, tlla: selfMAC,
                              flags: flagRouter | flagOverride)
    }

    /// **复原** NA：把设备网关条目从「本机 MAC」改回「真路由器 MAC(`routerMAC6`)」。
    ///
    /// 与 `poison` 唯一的差别是 TLLA 换成真路由器 MAC。带 Override 的 NA 会被内核当权威更新，
    /// 直接改回真路由器 MAC；发不出去设备就会一直把本机当 v6 网关、v6 断网。所以停机务必多发几遍
    /// （见 `Redirector.stopLocked`）。
    public static func restore(deviceMAC: ARPPacket.MAC, selfMAC: ARPPacket.MAC,
                               routerLL6: [UInt8], routerMAC6: ARPPacket.MAC) -> [UInt8] {
        neighborAdvertisement(ethDst: deviceMAC, ethSrc: selfMAC,
                              srcIP6: routerLL6, dstIP6: allNodes,
                              targetIP6: routerLL6, tlla: routerMAC6,
                              flags: flagRouter | flagOverride)
    }

    // MARK: - 被动抢答设备 NS（Neighbor Solicitation, type 135）

    /// 设备被投毒后，其网关邻居条目老化时会发 NUD **单播** NS 来复核可达性——目的正是本机 MAC
    /// （它缓存的「路由器 LL 在本机」），所以非混杂也收得到。抢在真路由器前回一条 solicited NA，
    /// 就能把条目从 STALE 直接翻成 REACHABLE、继续压在本机上。这是「唤醒沿」抢答的核心。
    ///
    /// 判据：以太类型 IPv6、next-header ICMPv6(58，NDP 无扩展头)、ICMPv6 type 135。
    /// 最短 78 字节 = 以太(14) + IPv6(40) + NS(8 头 + 16 target)。
    public static func isNeighborSolicitation(_ frame: [UInt8]) -> Bool {
        guard frame.count >= 78 else { return false }
        guard frame[12] == 0x86, frame[13] == 0xDD else { return false }
        guard frame[20] == 58 else { return false }   // IPv6 next header = ICMPv6
        return frame[54] == 135                        // ICMPv6 type = NS
    }

    /// NS 的 target 地址（要复核的那个地址）。偏移 = 以太(14) + IPv6(40) + NS头(8) = 62。
    public static func nsTarget(_ frame: [UInt8]) -> [UInt8]? {
        guard isNeighborSolicitation(frame) else { return nil }
        return Array(frame[62..<78])
    }

    /// 是否路由器通告(RA, type 134)或邻居通告(NA, type 136)。真路由器发的这两类会把设备的网关
    /// 邻居条目**解毒**回真 MAC —— 收到（且以太源是真路由器）就立刻重投盖回（见 `Redirector`）。
    /// RA 周期发到 ff02::1（本机是 all-nodes 成员，非混杂也收得到），是最主要的解毒来源。
    public static func isRouterAdvertOrNA(_ frame: [UInt8]) -> Bool {
        guard frame.count >= 55 else { return false }
        guard frame[12] == 0x86, frame[13] == 0xDD else { return false }
        guard frame[20] == 58 else { return false }
        let t = frame[54]
        return t == 134 || t == 136
    }

    /// 帧的 IPv6 源地址（NS 发起者的地址，抢答 NA 要单播回它）。`::`（DAD 阶段的 NS）返回 nil
    /// —— 无法单播回一个未指定地址，那种 NS 交给真路由器处理。
    public static func ipv6Source(_ frame: [UInt8]) -> [UInt8]? {
        guard frame.count >= 38 else { return nil }
        let src = Array(frame[22..<38])
        return src.allSatisfy { $0 == 0 } ? nil : src
    }

    /// 帧的以太源 MAC（NS 发起者的 MAC）。
    public static func ethSource(_ frame: [UInt8]) -> [UInt8]? {
        guard frame.count >= 12 else { return nil }
        return Array(frame[6..<12])
    }

    /// 抢答设备 NS 的 **solicited** NA：单播回设备、target = 路由器 LL、TLLA = 本机 MAC，
    /// 标志 Router|Solicited|Override（solicited 单播合法，直接把条目翻成 REACHABLE）。
    public static func solicitedNA(deviceMAC: ARPPacket.MAC, selfMAC: ARPPacket.MAC,
                                   deviceIP6: [UInt8], routerLL6: [UInt8]) -> [UInt8] {
        neighborAdvertisement(ethDst: deviceMAC, ethSrc: selfMAC,
                              srcIP6: routerLL6, dstIP6: deviceIP6,
                              targetIP6: routerLL6, tlla: selfMAC,
                              flags: flagRouter | flagSolicited | flagOverride)
    }

    /// **单播 solicited** 复原 NA：已知设备 LL 时用它把条目更强地翻回真路由器 MAC（比组播复原更快
    /// 落到 REACHABLE）。抢答过设备 NS 就会记下它的 LL（见 `Redirector`），复原时优先走这条。
    public static func restoreUnicast(deviceMAC: ARPPacket.MAC, selfMAC: ARPPacket.MAC,
                                      deviceIP6: [UInt8], routerLL6: [UInt8],
                                      routerMAC6: ARPPacket.MAC) -> [UInt8] {
        neighborAdvertisement(ethDst: deviceMAC, ethSrc: selfMAC,
                              srcIP6: routerLL6, dstIP6: deviceIP6,
                              targetIP6: routerLL6, tlla: routerMAC6,
                              flags: flagRouter | flagSolicited | flagOverride)
    }

    // MARK: - 底层帧构造

    /// 拼一帧完整的以太 + IPv6 + ICMPv6 Neighbor Advertisement(type 136)。
    ///
    /// 总长 14(以太) + 40(IPv6) + 32(ICMPv6 NA：8 头 + 16 target + 8 TLLA 选项) = 86 字节。
    /// hop limit 恒 255（NDP 强制：收方据此拒收被路由转发过来的伪造包，RFC 4861 §11.2）。
    /// 校验和含 IPv6 伪首部。
    public static func neighborAdvertisement(ethDst: ARPPacket.MAC, ethSrc: ARPPacket.MAC,
                                             srcIP6: [UInt8], dstIP6: [UInt8],
                                             targetIP6: [UInt8], tlla: ARPPacket.MAC,
                                             flags: UInt8) -> [UInt8] {
        precondition(srcIP6.count == 16 && dstIP6.count == 16 && targetIP6.count == 16)

        // —— ICMPv6 NA（先拼好再算校验和）——
        let icmpLen = 32
        var icmp = [UInt8](repeating: 0, count: icmpLen)
        icmp[0] = 136                 // type = Neighbor Advertisement
        icmp[1] = 0                   // code
        // icmp[2..3] = 校验和，占位
        icmp[4] = flags & 0xE0        // R/S/O，低 5 位保留为 0
        for i in 0..<16 { icmp[8 + i] = targetIP6[i] }
        icmp[24] = 2                  // 选项类型：Target Link-Layer Address
        icmp[25] = 1                  // 选项长度：1（单位 8 字节）
        for i in 0..<6 { icmp[26 + i] = tlla.bytes[i] }
        let checksum = icmp6Checksum(srcIP6: srcIP6, dstIP6: dstIP6, icmp: icmp)
        icmp[2] = UInt8(truncatingIfNeeded: checksum >> 8)
        icmp[3] = UInt8(truncatingIfNeeded: checksum)

        var frame = [UInt8]()
        frame.reserveCapacity(14 + 40 + icmpLen)
        // —— 以太头 ——
        frame += ethDst.bytes
        frame += ethSrc.bytes
        frame += [0x86, 0xDD]         // EtherType = IPv6
        // —— IPv6 头 ——
        var ip = [UInt8](repeating: 0, count: 40)
        ip[0] = 0x60                  // version = 6
        ip[4] = UInt8(truncatingIfNeeded: icmpLen >> 8)   // payload length
        ip[5] = UInt8(truncatingIfNeeded: icmpLen)
        ip[6] = 58                    // next header = ICMPv6
        ip[7] = 255                   // hop limit
        for i in 0..<16 { ip[8 + i] = srcIP6[i]; ip[24 + i] = dstIP6[i] }
        frame += ip
        frame += icmp
        return frame
    }

    /// ICMPv6 校验和：伪首部 src(16)+dst(16)+upper-layer-len(4,大端)+zero(3)+next-header(1=58) + 报文。
    static func icmp6Checksum(srcIP6: [UInt8], dstIP6: [UInt8], icmp: [UInt8]) -> UInt16 {
        var pseudo = [UInt8]()
        pseudo.reserveCapacity(40 + icmp.count)
        pseudo += srcIP6
        pseudo += dstIP6
        let len = icmp.count
        pseudo += [UInt8(truncatingIfNeeded: len >> 24), UInt8(truncatingIfNeeded: len >> 16),
                   UInt8(truncatingIfNeeded: len >> 8), UInt8(truncatingIfNeeded: len)]
        pseudo += [0, 0, 0, 58]
        pseudo += icmp
        return onesComplement(pseudo)
    }

    /// 16 位反码校验和（与 clashauto-c++ `NdpSpoofer::onesComplement` 同实现）。
    static func onesComplement(_ data: [UInt8]) -> UInt16 {
        var sum: UInt32 = 0
        var i = 0
        while i + 1 < data.count {
            sum += (UInt32(data[i]) << 8) | UInt32(data[i + 1])
            i += 2
        }
        if data.count & 1 == 1 {
            sum += UInt32(data[data.count - 1]) << 8
        }
        while sum >> 16 != 0 { sum = (sum & 0xFFFF) + (sum >> 16) }
        return UInt16(truncatingIfNeeded: ~sum)
    }
}
