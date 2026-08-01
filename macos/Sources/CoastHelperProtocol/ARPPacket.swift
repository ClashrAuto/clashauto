import Foundation

/// 构造以太网帧里的 ARP 报文。纯字节操作，不碰网络 —— 单独放出来就是为了能单测，
/// 而收发那部分（BPF）只能在 root + 真网卡上跑。
///
/// 放在 `CoastHelperProtocol` 而不是 CoastKit：真正发包的是 **helper**（`/dev/bpf*` 需 root），
/// 它只链接这个最小 target；app 侧的测试也 `@testable import` 得到。
public enum ARPPacket {

    /// 6 字节 MAC。用定长数组而不是 String，避免每次发包都要解析冒号分隔串。
    public struct MAC: Equatable, Sendable {
        public var bytes: [UInt8]   // 恒为 6

        public init?(_ text: String) {
            let parts = text.split(separator: ":", omittingEmptySubsequences: false)
            guard parts.count == 6 else { return nil }
            var out = [UInt8]()
            for part in parts {
                guard let value = UInt8(part, radix: 16) else { return nil }
                out.append(value)
            }
            bytes = out
        }

        public init(bytes: [UInt8]) { self.bytes = bytes }

        public static let broadcast = MAC(bytes: [0xff, 0xff, 0xff, 0xff, 0xff, 0xff])

        public var text: String { bytes.map { String(format: "%02x", $0) }.joined(separator: ":") }
    }

    /// 4 字节 IPv4。
    public static func ipv4Bytes(_ text: String) -> [UInt8]? {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return nil }
        var out = [UInt8]()
        for part in parts {
            guard let value = UInt8(part) else { return nil }
            out.append(value)
        }
        return out
    }

    /// 解析一帧 ARP **请求**：若是「who-has `targetIP`」的请求，返回请求方的 (senderIP, senderMAC)。
    /// 不是 ARP 请求、或问的不是 `targetIP` 就返回 nil。用于 v4 抢答（设备问「网关在哪」→ 抢先回本机）。
    ///
    /// 偏移：以太(14) + ARP。op 在 20（请求=1），sha 在 22，spa 在 28，tha 在 32，tpa 在 38。
    public static func parseRequest(_ frame: [UInt8], targetIP: [UInt8]) -> (senderIP: [UInt8], senderMAC: MAC)? {
        guard frame.count >= 42, targetIP.count == 4 else { return nil }
        guard frame[12] == 0x08, frame[13] == 0x06 else { return nil }   // EtherType = ARP
        guard frame[20] == 0x00, frame[21] == 0x01 else { return nil }   // op == request(1)
        guard Array(frame[38..<42]) == targetIP else { return nil }      // 目标协议地址 == targetIP
        return (senderIP: Array(frame[28..<32]), senderMAC: MAC(bytes: Array(frame[22..<28])))
    }

    /// 构造一个**ARP 应答**的完整以太网帧（14 字节以太头 + 28 字节 ARP）。
    ///
    /// 用途是**主动 ARP**（gratuitous-style reply）：不等对方问，直接告诉目标设备
    /// 「`senderIP` 的 MAC 是 `senderMAC`」。
    ///   • 欺骗时：senderIP = 网关 IP，senderMAC = 本机 —— 让设备把流量发给我们；
    ///   • 复原时：senderIP = 网关 IP，senderMAC = **真网关** —— 把它改回去。
    ///
    /// 复原用的这一份是整个功能里最要命的东西：发不出去，被欺骗的设备会一直把本机当网关，
    /// 而它的 ARP 缓存要十几分钟才自然过期 —— 期间那台设备完全断网。
    public static func reply(senderMAC: MAC, senderIP: [UInt8],
                             targetMAC: MAC, targetIP: [UInt8]) -> [UInt8] {
        precondition(senderIP.count == 4 && targetIP.count == 4)
        var frame = [UInt8]()
        frame.reserveCapacity(42)

        // —— 以太网头 ——
        frame += targetMAC.bytes         // 目的 MAC（发给这台设备）
        frame += senderMAC.bytes         // 源 MAC
        frame += [0x08, 0x06]            // EtherType = ARP

        // —— ARP ——
        frame += [0x00, 0x01]            // 硬件类型 = 以太网
        frame += [0x08, 0x00]            // 协议类型 = IPv4
        frame += [0x06]                  // 硬件地址长度 = 6
        frame += [0x04]                  // 协议地址长度 = 4
        frame += [0x00, 0x02]            // 操作 = 应答(2)
        frame += senderMAC.bytes         // 发送方 MAC
        frame += senderIP                // 发送方 IP
        frame += targetMAC.bytes         // 目标 MAC
        frame += targetIP                // 目标 IP
        return frame
    }
}
