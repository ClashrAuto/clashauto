import Foundation
import Network
import Testing
@testable import CoastKit

/// TCP 握手 RTT 的边界。全部在本机完成，不依赖外网。
@Suite("延迟探测")
struct LatencyProbeTests {

    /// 起一个只 accept 不说话的监听器，返回它的端口。
    @MainActor
    private func listen() async throws -> (NWListener, UInt16) {
        let listener = try NWListener(using: .tcp)
        listener.newConnectionHandler = { $0.cancel() }
        listener.start(queue: .global())
        for _ in 0..<50 {
            if let port = listener.port?.rawValue, port != 0 { return (listener, port) }
            try? await Task.sleep(for: .milliseconds(50))
        }
        throw NSError(domain: "listen", code: 1)
    }

    @Test("★ 端口开着:测到一个非负的 RTT")
    @MainActor func measuresOpenPort() async throws {
        let (listener, port) = try await listen()
        defer { listener.cancel() }
        let ms = await LatencyProbe.tcpRTT(host: "127.0.0.1", port: port)
        #expect(ms >= 0, "本机监听端口都没测到:\(ms)")
        #expect(ms < 1000, "本机往返不该超过 1 秒:\(ms)")
    }

    @Test("★ 端口关着(ECONNREFUSED)同样算测到 —— 家用路由多半全端口关闭")
    @MainActor func refusedCountsAsMeasurement() async throws {
        // 先占一个端口再放掉，拿到一个几乎肯定没人监听的号
        let (listener, port) = try await listen()
        listener.cancel()
        try? await Task.sleep(for: .milliseconds(200))

        let ms = await LatencyProbe.tcpRTT(host: "127.0.0.1", port: port)
        #expect(ms >= 0, """
            连接被拒绝被当成了失败(\(ms))。RST 是对端内核直接回的，与 SYN-ACK 走同一条路，
            往返时间同样成立；按失败处理的话「到路由」这一项在多数家用网络里永远是「—」。
            """)
    }

    @Test("★ 超时分支:到点就返回 unknown,不挂住")
    @MainActor func timesOut() async {
        // ★ 这里**不能**用「不可达地址」来触发超时。第一版拿 192.0.2.1（RFC5737 文档地址段）
        //   试，结果测到了 7ms —— 很多网络对不可达目的地**直接回 RST**，按本探测的设计
        //   那就是一次有效往返。判据落在网络行为上，测试就成了对环境的赌博。
        //   改成把超时设为 0：睡眠任务立刻胜出，确定性地走到超时分支。
        let started = Date()
        let ms = await LatencyProbe.tcpRTT(host: "127.0.0.1", port: 9, timeout: .zero)
        #expect(ms == LatencyProbe.unknown, "超时分支没返回 unknown:\(ms)")
        #expect(Date().timeIntervalSince(started) < 2, "超时没生效,拖了太久")
    }

    @Test("空主机名 / 非法端口:直接 unknown,不去建连接")
    @MainActor func rejectsBadInput() async {
        #expect(await LatencyProbe.tcpRTT(host: "", port: 80) == LatencyProbe.unknown)
        #expect(await LatencyProbe.tcpRTT(host: "127.0.0.1", port: 0) == LatencyProbe.unknown)
    }

    @Test("★ DNS:解析得了的域名有耗时;解析失败返回 unknown")
    @MainActor func dnsResolution() async {
        // localhost 一定解析得出来，且不需要外网
        let ok = await LatencyProbe.dnsRTT(host: "localhost", timeout: .seconds(3))
        #expect(ok >= 0, "localhost 都解析不出来:\(ok)")

        // ★ **不能**拿一个「不存在的域名」当失败样本。第一版用 `*.invalid` 试，结果解析成功 ——
        //   这条网络的 DNS 劫持了 NXDOMAIN（运营商通配符解析很常见）。那样测的是网络的脾气，
        //   不是我们的代码。用空主机名：getaddrinfo 对它必然失败，与网络无关。
        //   （顺带说明：被劫持的应答对**测延迟**这件事无害，我们要的就是「解析一次要多久」。）
        let bad = await LatencyProbe.dnsRTT(host: "", timeout: .seconds(3))
        #expect(bad == LatencyProbe.unknown, "空主机名居然解析成功了:\(bad)")
    }
}
