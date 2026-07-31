import Foundation
import Testing
@testable import CoastKit

/// 核心重启后，`ClashService` 能不能自己接回去。
///
/// `/traffic` 是**常开单流**，核心一死流就断。上一轮修好「核心崩溃后能重新启动」之后，
/// 这条路径才真正成为常态：用户点重启、或崩溃自愈之后，流量数字必须自己恢复跳动，
/// 否则界面会永远停在 0，看起来像「核心没起来」。
@Suite("核心重启后的自动重连")
struct ReconnectE2ETests {

    static let mihomo: String? = {
        let path = ProcessInfo.processInfo.environment["COAST_TEST_MIHOMO"]
            ?? FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent(".local/share/coast-devtools/mihomo").path
        return FileManager.default.isExecutableFile(atPath: path) ? path : nil
    }()

    /// 直接拉一个 mihomo 进程（不经 CoreProcess —— 这里要测的是 ClashService，
    /// 用最少的中间层，免得失败时分不清是谁的问题）。
    private func spawnCore(dir: URL, port: Int, secret: String) throws -> Process {
        let yaml = """
        mixed-port: \(port + 1)
        external-controller: '127.0.0.1:\(port)'
        secret: '\(secret)'
        proxies: []
        proxy-groups: []
        rules:
          - 'MATCH,DIRECT'
        """
        let path = dir.appendingPathComponent("full.yaml")
        try yaml.write(to: path, atomically: true, encoding: .utf8)
        let task = Process()
        task.executableURL = URL(fileURLWithPath: Self.mihomo!)
        task.arguments = ["-d", dir.path, "-f", path.path]
        task.standardOutput = FileHandle.nullDevice
        task.standardError = FileHandle.nullDevice
        try task.run()
        return task
    }

    @MainActor
    private func wait(for condition: @MainActor () -> Bool, seconds: Double) async -> Bool {
        let deadline = Date().addingTimeInterval(seconds)
        while Date() < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(150))
        }
        return condition()
    }

    @Test("★ 核心被杀再拉起:coreReachable 应 true → false → true(看门狗自己接回去)")
    @MainActor func reconnectsAfterCoreRestart() async throws {
        guard Self.mihomo != nil else {
            print("⏭  跳过重连测试:没有可用的 mihomo(设 COAST_TEST_MIHOMO)")
            return
        }
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-recon-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        var config = AppConfig()
        config.uiPort = 19421
        config.mixedPort = 19422
        config.secret = "recon"

        var core = try spawnCore(dir: dir, port: 19421, secret: "recon")
        let service = ClashService(config: config)
        service.start()
        defer { service.stop(); core.terminate() }

        #expect(await wait(for: { service.coreReachable }, seconds: 15),
                "核心在跑,却始终没收到 /traffic 数据")

        // 意外死亡
        kill(core.processIdentifier, SIGKILL)
        #expect(await wait(for: { !service.coreReachable }, seconds: 10),
                "核心已死,coreReachable 却还是 true —— 界面会显示一个不存在的「在线」")

        // 连续拿不到数据几轮之后，才该点亮「无响应」——刚断的一瞬间不算。
        #expect(await wait(for: { service.coreUnresponsive }, seconds: 15),
                "核心死了这么久还没判定为无响应 —— 界面会一直显示一个正常的假象")

        // 重新拉起（同一个端口，模拟用户点重启 / 崩溃自愈）
        core = try spawnCore(dir: dir, port: 19421, secret: "recon")
        // 看门狗是 2s 一轮，给足几轮的余量。
        #expect(await wait(for: { service.coreReachable }, seconds: 20),
                "核心已重新起来,看门狗没接回去 —— 流量数字会永远停在 0")
        #expect(await wait(for: { !service.coreUnresponsive }, seconds: 10),
                "已经重新连上了,「无响应」警告却没撤 —— 会一直吓唬用户")
    }

    @Test("★ 刚启动那一瞬不该误报「无响应」(coreReachable 天然先是 false)")
    @MainActor func noFalseAlarmAtStartup() async throws {
        guard Self.mihomo != nil else {
            print("⏭  跳过误报测试:没有可用的 mihomo(设 COAST_TEST_MIHOMO)")
            return
        }
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-recon2-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        var config = AppConfig()
        config.uiPort = 19431; config.mixedPort = 19432; config.secret = "recon2"
        let core = try spawnCore(dir: dir, port: 19431, secret: "recon2")
        let service = ClashService(config: config)
        service.start()
        defer { service.stop(); core.terminate() }

        // 核心从启动到 REST 就绪有个空窗，这期间 coreReachable 就是 false ——
        // 若警告直接照搬它，用户每次启动都会看到一闪而过的红字。
        for _ in 0..<12 {
            #expect(service.coreUnresponsive == false, "启动空窗期误报了「核心无响应」")
            try? await Task.sleep(for: .milliseconds(150))
        }
        #expect(await wait(for: { service.coreReachable }, seconds: 15), "核心最终没连上")
    }
}
