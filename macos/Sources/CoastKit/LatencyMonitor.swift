import Foundation
import Observation

/// 「延迟」卡背后的四个数，定时刷新。
///
/// 与 Qt 一致：平时 10 秒一轮，也可手动点刷新立刻再来一轮。
/// 四项**各自独立并发**跑、互不阻塞 —— 串起来的话一项超时会把整轮拖到十几秒，
/// 而它们之间没有任何依赖关系。任何一项失败只把它自己置 `unknown`，不影响其余三项。
@MainActor
@Observable
public final class LatencyMonitor {

    public private(set) var reading = LatencyProbe.Reading()
    public private(set) var probing = false

    /// 网关 IP（「到路由」那一行的副标题，也是探测目标）。由外部按当前网络注入。
    public var gatewayIP: String = ""
    /// 当前选中节点的名字与延迟 —— 取自核心，不自己测。
    public var proxyName: String = ""
    public var proxyDelay: Int = LatencyProbe.untested

    private var timer: Task<Void, Never>?

    public init() {}

    public func start() {
        guard timer == nil else { return }
        timer = Task { [weak self] in
            while !Task.isCancelled {
                await self?.probe()
                try? await Task.sleep(for: .seconds(10))
            }
        }
    }

    public func stop() {
        timer?.cancel()
        timer = nil
    }

    /// 跑一轮。已在跑时直接返回 —— 手动连点不该叠出好几轮并发探测。
    public func probe() async {
        guard !probing else { return }
        probing = true
        defer { probing = false }

        let gateway = gatewayIP
        async let direct = LatencyProbe.directRTT()
        async let router = gateway.isEmpty
            ? LatencyProbe.unknown
            // 家用路由多半只开 80/443，甚至全关。443 被拒绝同样能测出 RTT
            // （见 LatencyProbe 里对 ECONNREFUSED 的说明）。
            : LatencyProbe.tcpRTT(host: gateway, port: 443)
        async let dns = LatencyProbe.dnsRTT()

        var next = LatencyProbe.Reading()
        next.directMs = await direct
        next.routerMs = await router
        next.dnsMs = await dns
        next.proxyMs = proxyDelay
        next.proxyName = proxyName
        reading = next
    }
}
