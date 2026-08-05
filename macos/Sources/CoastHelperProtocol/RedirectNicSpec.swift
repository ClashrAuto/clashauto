import Foundation

/// **一张网卡上的接管任务。**
///
/// 同时接两条上行（有线一台路由器、Wi-Fi 另一台）时，每张卡各有自己的网关、自己的一批设备、
/// 以及自己的 redir 入站端口 —— 因为「设备从哪条上行出去」在核心里是**入站**的属性
/// （listener 的 `interface-name`，见 core 的 `component/dialer/egress.go`）。
///
/// ★ **一张卡就是长度 1 的数组**，没有「单网卡模式」这个特例，也就没有什么可回退的。
///   helper 侧对应 `[Redirector]`，一个实例管一张卡（各自的 PF anchor 与崩溃记录）。
///
/// 走 JSON 串过 XPC，与协议里既有的「用逗号串而不是数组，避免 NSXPC 的容器类白名单样板」
/// 同一个理由：嵌套结构用白名单表达要写一大堆样板，而这里的内容全是自己人生成、
/// helper 侧逐字段做字符集校验（网卡名/IP/MAC 都会拼进 PF 规则文本，注入面必须自己守）。
public struct RedirectNicSpec: Codable, Sendable, Equatable {
    /// BSD 网卡名（`en0`）。会被拼进 PF 规则 → helper 侧必须白名单校验。
    public let interface: String
    public let gatewayIP: String
    public let gatewayMAC: String
    /// 这张卡对应的核心 redir 入站端口（`DeviceStore.redirPort(forNic:)`）。
    public let redirPort: Int
    /// v6 路由器（链路本地 + MAC）。两者都空 = 这张卡不接管 v6。
    public let routerLL6: String
    public let routerMAC6: String
    /// 这张卡上被接管的设备，`deviceIPs[i]` 与 `deviceMACs[i]` **一一对应**。
    public let deviceIPs: [String]
    public let deviceMACs: [String]
    /// 这些设备的可路由 v6 源地址（扁平集合，PF 按源匹配，与 MAC 无需对齐）。
    public let deviceV6s: [String]

    public init(interface: String, gatewayIP: String, gatewayMAC: String, redirPort: Int,
                routerLL6: String = "", routerMAC6: String = "",
                deviceIPs: [String], deviceMACs: [String], deviceV6s: [String] = []) {
        self.interface = interface
        self.gatewayIP = gatewayIP
        self.gatewayMAC = gatewayMAC
        self.redirPort = redirPort
        self.routerLL6 = routerLL6
        self.routerMAC6 = routerMAC6
        self.deviceIPs = deviceIPs
        self.deviceMACs = deviceMACs
        self.deviceV6s = deviceV6s
    }

    /// 编码成过 XPC 的 JSON 串。编不出来（正常情况下不可能）返回 `"[]"` ——
    /// 那会让 helper 认为「没有要接管的卡」而原地失败，比传一个半截串安全。
    public static func encode(_ nics: [RedirectNicSpec]) -> String {
        guard let data = try? JSONEncoder().encode(nics),
              let text = String(data: data, encoding: .utf8) else { return "[]" }
        return text
    }

    /// 解码。串坏了返回空数组 —— 调用方据此报错，不会拿着半截数据去接管。
    public static func decode(_ json: String) -> [RedirectNicSpec] {
        guard let data = json.data(using: .utf8),
              let nics = try? JSONDecoder().decode([RedirectNicSpec].self, from: data)
        else { return [] }
        return nics
    }
}
