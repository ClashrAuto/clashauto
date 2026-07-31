import Foundation
import Network

/// 状态页「延迟」卡的四个数：直连 / 到路由 / DNS / 到当前代理。
///
/// 移植自 Qt 的 `LatencyProbe.h`，三条决定实现方式的理由一并搬过来：
///
/// 1. **不用 ICMP。** ping 在 macOS 要 root 或 raw socket 权限，还可能被防火墙整段丢掉。
///    这里一律用 **TCP 握手 RTT**：从发起连接到连上的耗时就是一个 RTT。
///    ★ 端口关着**也算测到**——`ECONNREFUSED` 是对端内核直接回的 RST，和 SYN-ACK 走同样的路，
///    所以它同样是一次有效测量。这对路由器尤其要紧：家用路由多半只开 80/443，甚至全关，
///    把「拒绝」当失败的话「到路由」永远是「—」。只有**超时**才算失败。
///
/// 2. **直连必须真的直连。** 这里用 `NWConnection` 直接建 TCP，不经 URLSession，
///    因此天然不受系统 HTTP 代理影响 —— 换成 URLSession 的话，开着系统代理时测到的
///    是「到代理服务器」的延迟，这张卡就没意义了。
///
/// 3. **到代理的延迟不自己测。** 核心已经在测（`/proxies` 的 history），自己再连一遍节点
///    既慢又可能触发对端风控。直接取 `ClashService` 里当前选中节点的 delay。
public enum LatencyProbe {

    /// 未知/失败。界面显示「—」。
    public static let unknown = -1
    /// 还没测过。界面显示「…」。
    public static let untested = 0

    public struct Reading: Sendable, Equatable {
        public var directMs = untested
        public var routerMs = untested
        public var dnsMs = untested
        public var proxyMs = untested
        public var proxyName = ""

        public init() {}
    }

    /// TCP 握手 RTT，毫秒。失败返回 `unknown`。
    ///
    /// - Parameter timeout: 超时。默认 3s —— 再长的话一次探测会把整轮拖住，
    ///   而一条 3 秒都握不上手的链路，用户体感上早就是「不通」了。
    public static func tcpRTT(host: String, port: UInt16,
                              timeout: Duration = .seconds(3)) async -> Int {
        guard !host.isEmpty, let nwPort = NWEndpoint.Port(rawValue: port) else { return unknown }
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host), port: nwPort)
        let parameters = NWParameters.tcp
        // 不等 TLS、不做任何应用层握手：要测的就是三次握手本身。
        parameters.prohibitExpensivePaths = false

        let connection = NWConnection(to: endpoint, using: parameters)
        let started = DispatchTime.now()

        return await withTaskGroup(of: Int.self) { group in
            group.addTask {
                await withCheckedContinuation { continuation in
                    // 只 resume 一次：状态可能连续跳（preparing → ready），也可能直接 failed。
                    let once = OnceFlag()
                    connection.stateUpdateHandler = { state in
                        switch state {
                        case .ready:
                            if once.claim() { continuation.resume(returning: elapsedMs(since: started)) }
                        case .waiting(let error), .failed(let error):
                            // ★ 连接被拒绝**是**一次有效测量：RST 由对端内核直接回，
                            //   和 SYN-ACK 走同一条路，往返时间同样成立。
                            //
                            // ★★ 必须同时接 `.waiting`：`NWConnection` 默认会**自动重试**，
                            //    被拒绝时它进的是 `.waiting(ECONNREFUSED)` 并在那儿等着再试，
                            //    根本不会走到 `.failed`。只处理 `.failed` 的话，「端口关着」
                            //    这条路径永远测不到 —— 而家用路由多半就是全端口关闭。
                            //    （实测确认：state 依次是 preparing → waiting(61 Connection refused)。）
                            let refused = (error == .posix(.ECONNREFUSED))
                            if once.claim() {
                                continuation.resume(returning: refused ? elapsedMs(since: started) : unknown)
                            }
                        case .cancelled:
                            if once.claim() { continuation.resume(returning: unknown) }
                        default:
                            break
                        }
                    }
                    connection.start(queue: .global(qos: .utility))
                }
            }
            group.addTask {
                try? await Task.sleep(for: timeout)
                return unknown
            }
            let first = await group.next() ?? unknown
            group.cancelAll()
            connection.cancel()
            return first
        }
    }

    /// 一次真实域名解析的耗时，毫秒。失败返回 `unknown`。
    ///
    /// 用 `getaddrinfo` 而不是发 DNS 包：要测的是「这台机器解析一个域名要多久」，
    /// 那就该走系统解析器的完整路径（含缓存、search domain、mDNS 回退），
    /// 自己拼 DNS 查询包测到的是另一个东西。
    public static func dnsRTT(host: String = "www.apple.com",
                              timeout: Duration = .seconds(3)) async -> Int {
        await withTaskGroup(of: Int.self) { group in
            group.addTask {
                await withCheckedContinuation { continuation in
                    DispatchQueue.global(qos: .utility).async {
                        var hints = addrinfo(ai_flags: 0, ai_family: AF_UNSPEC,
                                             ai_socktype: SOCK_STREAM, ai_protocol: 0,
                                             ai_addrlen: 0, ai_canonname: nil,
                                             ai_addr: nil, ai_next: nil)
                        var result: UnsafeMutablePointer<addrinfo>?
                        let started = DispatchTime.now()
                        let status = getaddrinfo(host, nil, &hints, &result)
                        let elapsed = elapsedMs(since: started)
                        if let result { freeaddrinfo(result) }
                        continuation.resume(returning: status == 0 ? elapsed : unknown)
                    }
                }
            }
            group.addTask {
                try? await Task.sleep(for: timeout)
                return unknown
            }
            let first = await group.next() ?? unknown
            group.cancelAll()
            return first
        }
    }

    /// 直连公网：TCP 握手到公共 DNS 的 53 口。
    ///
    /// 挑 53 而不是 80/443：这个口几乎不会被中间设备劫持成透明代理，
    /// 测到的更接近真实的网络往返，而不是「到某个透明缓存的距离」。
    public static func directRTT(timeout: Duration = .seconds(3)) async -> Int {
        await tcpRTT(host: "1.1.1.1", port: 53, timeout: timeout)
    }

    private static func elapsedMs(since started: DispatchTime) -> Int {
        Int((DispatchTime.now().uptimeNanoseconds &- started.uptimeNanoseconds) / 1_000_000)
    }
}
