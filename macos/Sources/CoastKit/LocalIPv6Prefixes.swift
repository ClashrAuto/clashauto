import Darwin
import Foundation

extension ConfigBuilder {

    /// 本机各网卡的 IPv6 **全局单播**前缀，拼成 `IP-CIDR6,…,DIRECT,no-resolve` 规则行。
    ///
    /// 为什么必须动态取：IPv6 没有 RFC1918 那种固定私网段。家用 v6 内网用的就是运营商 RA 下发的
    /// 全局单播前缀（电信常见 `240e:…/64`），换网络、换 ISP 就变，静态列表根本写不出来。
    /// `fc00::/7`(ULA) 与 `fe80::/10`(链路本地) 一条都盖不住它 —— 不加这些前缀，
    /// 「访问自家 NAS 的 v6 地址」会被当成境外流量发到代理节点上。
    ///
    /// 只收 `2000::/3`（全局单播），且前缀长度限定 48…64：/128 是主机地址（把单个地址写成
    /// 直连规则没意义），短于 /48 的前缀大到会误伤真正的境外流量。
    static func localGlobal6Prefixes() -> [String] {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }

        var rules: [String] = []
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }

            let flags = Int32(entry.pointee.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_RUNNING != 0, flags & IFF_LOOPBACK == 0 else { continue }
            guard let rawAddress = entry.pointee.ifa_addr,
                  rawAddress.pointee.sa_family == UInt8(AF_INET6),
                  let rawMask = entry.pointee.ifa_netmask else { continue }

            let address = rawAddress.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) {
                $0.pointee.sin6_addr
            }
            let mask = rawMask.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) {
                $0.pointee.sin6_addr
            }

            var bytes = withUnsafeBytes(of: address) { Array($0) }
            let maskBytes = withUnsafeBytes(of: mask) { Array($0) }
            guard bytes.count == 16, maskBytes.count == 16 else { continue }

            // 只要 2000::/3 全局单播；LL/ULA/组播/回环都不是我们要的私网前缀
            guard bytes[0] & 0xE0 == 0x20 else { continue }

            let prefixLength = maskBytes.reduce(0) { $0 + $1.nonzeroBitCount }
            guard prefixLength >= 48, prefixLength <= 64 else { continue }

            // 掩到前缀边界：240e:…:bbc0:xxxx:xxxx:xxxx:xxxx/64 → 240e:…:bbc0::/64
            for bit in prefixLength..<128 {
                bytes[bit / 8] &= ~(UInt8(1) << (7 - UInt8(bit % 8)))
            }
            guard let text = presentation(bytes) else { continue }

            let rule = "IP-CIDR6,\(text)/\(prefixLength),DIRECT,no-resolve"
            // 同一前缀常同时挂在多张卡/多个地址上
            if !rules.contains(rule) { rules.append(rule) }
        }
        return rules
    }

    /// 16 字节 → `inet_ntop` 的规范写法（会做 `::` 压缩，和 Qt 的输出一致）。
    private static func presentation(_ bytes: [UInt8]) -> String? {
        var raw = in6_addr()
        withUnsafeMutableBytes(of: &raw) { destination in
            bytes.withUnsafeBytes { source in
                destination.copyMemory(from: source)
            }
        }
        var buffer = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
        guard inet_ntop(AF_INET6, &raw, &buffer, socklen_t(INET6_ADDRSTRLEN)) != nil else { return nil }
        return String(cString: buffer)
    }
}
