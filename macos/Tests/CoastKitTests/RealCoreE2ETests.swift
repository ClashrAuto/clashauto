import Foundation
import Testing
@testable import CoastKit

/// 端到端:让 `ClashService` 对着**真实运行的 mihomo 核心**说话。
///
/// 此前 ClashService 只对假数据验过(ProxyTree/排序/模式归一那些纯函数)。真核心才能验:
/// REST 鉴权(secret)、`/proxies` 的真实结构、模式切换真的落到核心、选节点真的生效。
///
/// 核心不在时跳过(COAST_TEST_MIHOMO 未设)。用非常规端口 + allow-lan:false,
/// **不碰系统代理、不碰用户配置**。
@Suite(.serialized)   // 起真核心,别并发抢端口
struct RealCoreE2ETests {

    private static var mihomo: String? {
        let p = ProcessInfo.processInfo.environment["COAST_TEST_MIHOMO"] ?? ""
        return FileManager.default.isExecutableFile(atPath: p) ? p : nil
    }

    /// 起一个隔离的核心,返回 (进程, 目录)。用完必须 stop。
    private func startCore(restPort: Int, mixedPort: Int, secret: String) throws -> (Process, URL) {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-e2e-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let config = """
        mixed-port: \(mixedPort)
        allow-lan: false
        mode: rule
        log-level: warning
        external-controller: '127.0.0.1:\(restPort)'
        secret: '\(secret)'
        proxies:
          - name: 'NodeA'
            type: trojan
            server: 127.0.0.1
            port: 44300
            password: pw
            skip-cert-verify: true
          - name: 'NodeB'
            type: trojan
            server: 127.0.0.1
            port: 44301
            password: pw
            skip-cert-verify: true
        proxy-groups:
          - name: '🚀 节点选择'
            type: select
            proxies:
              - NodeA
              - NodeB
              - DIRECT
        rules:
          - 'MATCH,🚀 节点选择'
        """
        let path = dir.appendingPathComponent("full.yaml")
        try config.write(to: path, atomically: true, encoding: .utf8)

        let task = Process()
        task.executableURL = URL(fileURLWithPath: Self.mihomo!)
        task.arguments = ["-d", dir.path, "-f", path.path]
        task.standardOutput = FileHandle.nullDevice
        task.standardError = FileHandle.nullDevice
        try task.run()
        return (task, dir)
    }

    /// 等 REST 起来(最多 ~5s)。核心启动是异步的,不等会偶发连不上。
    private func waitForREST(port: Int, secret: String) async -> Bool {
        for _ in 0..<50 {
            let api = ClashAPI(host: "127.0.0.1", port: port, mixedPort: 0, secret: secret)
            if (try? await api.configs()) != nil { return true }
            try? await Task.sleep(for: .milliseconds(100))
        }
        return false
    }

    @Test("★ ClashService 对真核心:轮询拿到节点、模式、组")
    func liveService() async throws {
        guard Self.mihomo != nil else { return }
        let (core, dir) = try startCore(restPort: 19301, mixedPort: 17801, secret: "s1")
        defer { core.terminate(); try? FileManager.default.removeItem(at: dir) }
        #expect(await waitForREST(port: 19301, secret: "s1"), "核心 REST 没起来")

        var config = AppConfig()
        config.uiPort = 19301; config.mixedPort = 17801; config.secret = "s1"
        let service = await ClashService(config: config)
        await service.start()
        defer { Task { await service.stop() } }

        // 等轮询跑一轮
        try await Task.sleep(for: .seconds(2))

        let nodes = await service.nodes
        let groups = await service.groups
        let mode = await service.mode
        // 真核心返回的组里应有我们定义的选择组;节点里应有 NodeA/NodeB
        #expect(groups.contains("🚀 节点选择"), "组没解析到:\(groups)")
        #expect(nodes.contains { $0.name == "NodeA" }, "节点没解析到:\(nodes.map(\.name))")
        #expect(mode == "Rule", "模式应从核心读回 Rule,实得 \(mode)")
    }

    @Test("★ 模式切换真的落到核心并读回")
    func liveModeSwitch() async throws {
        guard Self.mihomo != nil else { return }
        let (core, dir) = try startCore(restPort: 19302, mixedPort: 17802, secret: "s2")
        defer { core.terminate(); try? FileManager.default.removeItem(at: dir) }
        #expect(await waitForREST(port: 19302, secret: "s2"))

        let api = ClashAPI(host: "127.0.0.1", port: 19302, mixedPort: 0, secret: "s2")
        try await api.setMode("Global")
        let configs = try await api.configs()
        #expect((configs["mode"] as? String)?.lowercased() == "global", "模式没落到核心:\(configs["mode"] ?? "nil")")
    }

    @Test("★ 选节点真的生效(核心的 now 变了)")
    func liveSelectNode() async throws {
        guard Self.mihomo != nil else { return }
        let (core, dir) = try startCore(restPort: 19303, mixedPort: 17803, secret: "s3")
        defer { core.terminate(); try? FileManager.default.removeItem(at: dir) }
        #expect(await waitForREST(port: 19303, secret: "s3"))

        let api = ClashAPI(host: "127.0.0.1", port: 19303, mixedPort: 0, secret: "s3")
        try await api.selectNode(group: "🚀 节点选择", name: "NodeB")
        let proxies = try await api.proxies()
        #expect(proxies["🚀 节点选择"]?["now"] as? String == "NodeB", "选节点没生效")
    }

    @Test("★ secret 错误时被核心拒绝(鉴权真的在起作用)")
    func liveAuthRequired() async throws {
        guard Self.mihomo != nil else { return }
        let (core, dir) = try startCore(restPort: 19304, mixedPort: 17804, secret: "correct")
        defer { core.terminate(); try? FileManager.default.removeItem(at: dir) }
        #expect(await waitForREST(port: 19304, secret: "correct"))

        // 错 secret 应拿不到数据 —— 证明我们发的 Bearer 头确实被核心校验
        let bad = ClashAPI(host: "127.0.0.1", port: 19304, mixedPort: 0, secret: "wrong")
        let result = try? await bad.configs()
        #expect(result == nil || (result?["mode"] == nil), "错 secret 竟然拿到了数据")
    }

    @Test("★ CoreProcess 起停真核心:能起来、REST 通、能干净停掉")
    @MainActor func liveCoreProcess() async throws {
        guard let bin = Self.mihomo else { return }
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-cp-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        // CoreProcess 从 AppPaths.coreExecutable 找核心,这里用真实二进制的**副本**放到
        // 它期望的位置不现实(那是用户目录)。改为直接验 CoreProcess 的进程管理语义:
        // 用 launchPlain 那条路 —— 通过公开 API start(tunEnabled:fullConfigPath:)。
        // 核心路径由 AppPaths 决定,所以这里把真核心装到用户目录的 command/core。
        let target = AppPaths.userDir.appendingPathComponent("command/core")
        let hadCore = FileManager.default.fileExists(atPath: target.path)
        guard !hadCore else { return }   // 用户已装核心,不覆盖他的,跳过

        try? FileManager.default.createDirectory(at: target.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        try FileManager.default.copyItem(atPath: bin, toPath: target.path)
        defer { try? FileManager.default.removeItem(at: target) }   // 用完删掉,不污染用户环境

        let config = """
        mixed-port: 17805
        allow-lan: false
        external-controller: '127.0.0.1:19305'
        secret: 's5'
        proxies: []
        proxy-groups: []
        rules:
          - 'MATCH,DIRECT'
        """
        let path = dir.appendingPathComponent("full.yaml")
        try config.write(to: path, atomically: true, encoding: .utf8)

        let process = CoreProcess(config: AppConfig())
        let result = await process.start(tunEnabled: false, fullConfigPath: path)
        guard case .success = result else {
            Issue.record("CoreProcess 起核心失败: \(result)")
            return
        }
        #expect(process.isRunning)
        #expect(await waitForREST(port: 19305, secret: "s5"), "CoreProcess 起的核心 REST 没通")

        await process.stop()
        #expect(process.isRunning == false, "停不干净")
    }
}
