import Foundation

/// 「哪些设备可以被透明接管」的判据。
///
/// 这是一道**安全闸门**，不是界面装饰：
///
/// - **网关不可接管。** 接管的做法是向设备发「网关在我这儿」的 ARP 应答。设备如果**就是**
///   网关，等于告诉路由器「你自己的地址在我这儿」—— 污染它对自身地址的 ARP，
///   整个局域网都可能被打瘫。Qt 那边写得很直白：「劫持它会把整个网络打瘫，因此不可代理」。
/// - **本机不可接管。** 本机流量已经由 Coast 自己处理，再 ARP 欺骗自己没有意义，
///   只会把自己的路由搅乱。
///
/// ★ 光在界面上禁用开关**不够**：台账是按 MAC 存的、开关会一直留着，而网络是会变的 ——
///   在 A 网把 192.168.1.50 设成代理，换到 B 网时那个地址可能正是路由器。
///   所以真正生效的过滤必须在**下发给 helper 之前**再做一次。
public enum RedirectTargets {

    public enum Rejection: Sendable, Equatable {
        case isGateway
        case isLocalMachine
        case noAddress

        public var reason: String {
            switch self {
            case .isGateway:
                return "这是当前网络的路由器（网关），劫持它会把整个网络打瘫，因此不可代理。"
            case .isLocalMachine:
                return "这是本机——它的流量已经由 Coast 自己接管，无需（也不能）代理。"
            case .noAddress:
                return "设备当前离线：拿不到它的 IP/ARP，无法开启代理；等它上线后再开。"
            }
        }
    }

    /// 这台设备能不能被接管。返回 nil 表示可以。
    public static func rejection(ip: String, mac: String,
                                 gatewayIP: String, gatewayMAC: String,
                                 localMACs: Set<String>) -> Rejection? {
        guard !ip.isEmpty, !mac.isEmpty else { return .noAddress }
        let lowerMAC = mac.lowercased()
        // IP 与 MAC 任一命中网关都算 —— 换了网络之后台账里的 IP 可能已经指向路由器，
        // 而 MAC 没变；反过来路由器换了硬件时 MAC 变了、IP 没变。两条都要挡。
        if ip == gatewayIP || (!gatewayMAC.isEmpty && lowerMAC == gatewayMAC.lowercased()) {
            return .isGateway
        }
        if localMACs.contains(where: { $0.lowercased() == lowerMAC }) { return .isLocalMachine }
        return nil
    }

    /// 过滤出真正可以下发给 helper 的目标。
    public static func allowed(_ devices: [(ip: String, mac: String)],
                               gatewayIP: String, gatewayMAC: String,
                               localMACs: Set<String>) -> [(ip: String, mac: String)] {
        devices.filter {
            rejection(ip: $0.ip, mac: $0.mac, gatewayIP: gatewayIP,
                      gatewayMAC: gatewayMAC, localMACs: localMACs) == nil
        }
    }
}
