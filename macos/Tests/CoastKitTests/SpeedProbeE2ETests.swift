import Foundation
import Testing
@testable import CoastKit

/// `SpeedProbe` 对**真实 HTTP 下载 + 真核心代理**的验证。
///
/// 此前它一行都没被真跑过 —— 而它恰恰是最容易算错的地方:
/// **计时从首字节开始**(排除建连+TLS,否则慢握手会把速度算低),且到字节/时间上限就中止。
///
/// 用本地 HTTP 服务器当下载目标,不依赖外网。核心不在时跳过。
@Suite(.serialized)
struct SpeedProbeE2ETests {

    private static var mihomo: String? {
        let p = ProcessInfo.processInfo.environment["COAST_TEST_MIHOMO"] ?? ""
        return FileManager.default.isExecutableFile(atPath: p) ? p : nil
    }

    /// 起本地 HTTP 服务器(python3),返回 (进程, 目录)。
    private func startHTTP(port: Int, megabytes: Int) throws -> (Process, URL) {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-dl-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        // 造下载目标
        let blob = Data(count: megabytes * 1024 * 1024)
        try blob.write(to: dir.appendingPathComponent("blob.bin"))

        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        task.arguments = ["-m", "http.server", String(port)]
        task.currentDirectoryURL = dir
        task.standardOutput = FileHandle.nullDevice
        task.standardError = FileHandle.nullDevice
        try task.run()
        return (task, dir)
    }

    private func startCore(restPort: Int, mixedPort: Int, dir: URL) throws -> Process {
        let config = """
        mixed-port: \(mixedPort)
        allow-lan: false
        external-controller: '127.0.0.1:\(restPort)'
        secret: 'dl'
        proxies: []
        proxy-groups:
          - name: '🚀 节点选择'
            type: select
            proxies: [DIRECT]
        rules:
          - 'MATCH,DIRECT'
        """
        let path = dir.appendingPathComponent("full.yaml")
        try config.write(to: path, atomically: true, encoding: .utf8)
        let task = Process()
        task.executableURL = URL(fileURLWithPath: Self.mihomo!)
        task.arguments = ["-d", dir.path, "-f", path.path]
        task.standardOutput = FileHandle.nullDevice
        task.standardError = FileHandle.nullDevice
        try task.run()
        return task
    }

    /// 等某个端口能连上
    private func waitPort(_ port: Int, path: String = "/") async -> Bool {
        for _ in 0..<50 {
            var req = URLRequest(url: URL(string: "http://127.0.0.1:\(port)\(path)")!)
            req.timeoutInterval = 1
            if (try? await URLSession.shared.data(for: req)) != nil { return true }
            try? await Task.sleep(for: .milliseconds(100))
        }
        return false
    }

    @Test("★ SpeedProbe 经真核心代理下载:测到速度、首字节回调恰好触发一次")
    func liveSpeedProbe() async throws {
        guard Self.mihomo != nil else { return }
        let (http, dir) = try startHTTP(port: 18081, megabytes: 8)
        defer { http.terminate() }
        let core = try startCore(restPort: 19321, mixedPort: 17821, dir: dir)
        defer { core.terminate(); try? FileManager.default.removeItem(at: dir) }

        #expect(await waitPort(18081, path: "/blob.bin"), "本地 HTTP 没起来")
        try await Task.sleep(for: .seconds(2))   // 等核心

        // 首字节回调的触发次数 —— SpeedProbe 承诺「恰好一次」(它用来释放选组串行锁,
        // 多触发会让锁提前放、少触发会让整轮测速卡死)
        let counter = FirstByteCounter()
        let probe = SpeedProbe(host: "127.0.0.1", mixedPort: 17821,
                               maxBytes: 4 * 1024 * 1024, maxMs: 3000)
        let result = await probe.run(url: URL(string: "http://127.0.0.1:18081/blob.bin")!) {
            counter.bump()
        }

        #expect(result.bytes > 0, "没测到任何字节")
        #expect(result.ms > 0, "耗时应 > 0")
        #expect(counter.count == 1, "首字节回调应恰好一次,实得 \(counter.count)")
        // 计时从首字节开始,所以速度应该是个合理的正数(本地回环会很快)
        let bps = result.bytes * 1000 / result.ms
        #expect(bps > 0, "算出的速度应 > 0")
    }

    @Test("★ 目标不可达时:不崩、返回 0 字节、首字节回调仍触发(否则锁泄漏)")
    func liveSpeedProbeUnreachable() async throws {
        guard Self.mihomo != nil else { return }
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-dl-dead-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let core = try startCore(restPort: 19322, mixedPort: 17822, dir: dir)
        defer { core.terminate(); try? FileManager.default.removeItem(at: dir) }
        try await Task.sleep(for: .seconds(2))

        let counter = FirstByteCounter()
        let probe = SpeedProbe(host: "127.0.0.1", mixedPort: 17822, maxBytes: 1024, maxMs: 2000)
        // 指向没人监听的端口
        let result = await probe.run(url: URL(string: "http://127.0.0.1:19999/nope")!) {
            counter.bump()
        }
        #expect(result.bytes == 0)
        // ★ 关键:即使一个字节都没收到,也必须触发首字节回调 —— 它负责释放「选组+建连」
        //   的串行锁。不触发的话整轮测速会永远卡在这个节点上。
        #expect(counter.count == 1, "连不通时也必须恰好触发一次(释放串行锁),实得 \(counter.count)")
    }
}

extension SpeedProbeE2ETests {

    @Test("★ 整轮测速(并发闸门 + 选组串行锁)在真核心下能跑完、不卡死")
    @MainActor func liveFullSpeedTest() async throws {
        guard Self.mihomo != nil else { return }
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-full-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        // 多个节点的组:让 startSpeedTestForValidNodes 有活干。节点都是死的,
        // 于是每个 probe 都走「连不通」路径 —— 正是最容易让串行锁泄漏、整轮卡死的场景。
        let config = """
        mixed-port: 17823
        allow-lan: false
        external-controller: '127.0.0.1:19323'
        secret: 'full'
        proxies:
          - {name: N1, type: trojan, server: 127.0.0.1, port: 44301, password: p, skip-cert-verify: true}
          - {name: N2, type: trojan, server: 127.0.0.1, port: 44302, password: p, skip-cert-verify: true}
          - {name: N3, type: trojan, server: 127.0.0.1, port: 44303, password: p, skip-cert-verify: true}
        proxy-groups:
          - name: '🚀 节点选择'
            type: select
            proxies: [N1, N2, N3, DIRECT]
        rules:
          - 'MATCH,DIRECT'
        """
        let path = dir.appendingPathComponent("full.yaml")
        try config.write(to: path, atomically: true, encoding: .utf8)
        let core = Process()
        core.executableURL = URL(fileURLWithPath: Self.mihomo!)
        core.arguments = ["-d", dir.path, "-f", path.path]
        core.standardOutput = FileHandle.nullDevice; core.standardError = FileHandle.nullDevice
        try core.run()
        defer { core.terminate(); try? FileManager.default.removeItem(at: dir) }
        try await Task.sleep(for: .seconds(2))

        var appConfig = AppConfig()
        appConfig.uiPort = 19323; appConfig.mixedPort = 17823; appConfig.secret = "full"
        let service = ClashService(config: appConfig)
        service.start()
        defer { service.stop() }
        try await Task.sleep(for: .seconds(2))

        // 先测延迟(DIRECT 会有效),再对有效节点跑下载测速
        await service.testDelays(thenSpeed: true)

        // 整轮测速必须在合理时间内结束(不卡死)。给足余量:每个 probe 最多 6s 超时。
        let deadline = Date().addingTimeInterval(45)
        while service.speedTesting, Date() < deadline {
            try await Task.sleep(for: .milliseconds(200))
        }
        #expect(service.speedTesting == false, "整轮测速没在 45s 内结束 —— 疑似串行锁泄漏/卡死")
    }
}

/// 线程安全的计数器(SpeedProbe 的回调在它自己的串行队列上)。
private final class FirstByteCounter: @unchecked Sendable {
    private var value = 0
    private let lock = NSLock()
    func bump() { lock.lock(); value += 1; lock.unlock() }
    var count: Int { lock.lock(); defer { lock.unlock() }; return value }
}
