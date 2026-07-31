import Foundation
import Testing
@testable import CoastKit

/// 核心**意外死亡**时的状态传播。
///
/// 这是真实会发生的事：mihomo 被 OOM killer 收走、配置热重载踩到 panic、用户手动 kill。
/// 而它的后果不对称得厉害 —— 系统代理指向一个已经没人监听的端口，等于**彻底断网**，
/// 界面却还显示「运行中」。所以「核心死了 app 知不知道」必须有测试兜着。
@Suite("核心意外退出")
struct CoreCrashE2ETests {

    static let mihomo: String? = {
        let path = ProcessInfo.processInfo.environment["COAST_TEST_MIHOMO"]
            ?? FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent(".local/share/coast-devtools/mihomo").path
        return FileManager.default.isExecutableFile(atPath: path) ? path : nil
    }()

    /// 等 REST 起来。核心 `task.run()` 返回时它还没开始监听。
    static func waitForREST(port: Int, secret: String) async -> Bool {
        let url = URL(string: "http://127.0.0.1:\(port)/version")!
        for _ in 0..<40 {
            var request = URLRequest(url: url)
            request.setValue("Bearer \(secret)", forHTTPHeaderField: "Authorization")
            if let (_, response) = try? await URLSession.shared.data(for: request),
               (response as? HTTPURLResponse)?.statusCode == 200 { return true }
            try? await Task.sleep(for: .milliseconds(150))
        }
        return false
    }

    /// 起一个真核心，返回 (进程封装, 临时目录)。
    @MainActor
    private func startRealCore(port: Int, secret: String) async throws
        -> (process: CoreProcess, dir: URL)? {
        guard let mihomo = Self.mihomo else { return nil }
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-crash-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
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

        let process = CoreProcess(config: AppConfig())
        let result = await process.start(tunEnabled: false, fullConfigPath: path)
        guard case .success = result else {
            Issue.record("起核心失败: \(result)")
            return nil
        }
        return (process, dir)
    }

    @Test("★ 核心被 kill -9 后,CoreProcess.isRunning 必须变 false")
    @MainActor func processNoticesCrash() async throws {
        // CoreProcess 走的是 `AppPaths.coreExecutable`。裸跑时那个位置通常没有核心 ——
        // 缺的是运行器配置(COAST_CORE_PATH),不是被测代码有问题,所以跳过而不是判失败。
        guard FileManager.default.isExecutableFile(atPath: AppPaths.coreExecutable.path) else {
            print("⏭  跳过「核心 kill -9」测试:\(AppPaths.coreExecutable.path) 没有核心。"
                  + "跑 bash scripts/regression.sh,或设 COAST_CORE_PATH=<mihomo 路径>。")
            return
        }
        guard let (process, dir) = try await startRealCore(port: 19411, secret: "c1") else {
            Issue.record("核心就在那儿却起不来 —— 这才是真问题")
            return
        }
        defer { try? FileManager.default.removeItem(at: dir) }
        #expect(process.isRunning)
        // ★ 必须先确认核心**真的在服务**。否则「起了个立刻退出的进程」也能让下面全绿 ——
        //   那样这个测试证明的是「死进程会被发现是死的」,毫无意义。
        #expect(await Self.waitForREST(port: 19411, secret: "c1"), "核心没起来,后面的断言没有意义")

        // SIGKILL 而不是 SIGTERM：模拟的是**意外**死亡，没有优雅退出的机会。
        guard let pid = process.coreProcessIdentifier else {
            Issue.record("取不到核心 PID")
            return
        }
        kill(pid, SIGKILL)

        // terminationHandler 是异步派发到主 actor 的，给它一点时间。
        let deadline = Date().addingTimeInterval(5)
        while process.isRunning, Date() < deadline {
            try? await Task.sleep(for: .milliseconds(100))
        }
        #expect(process.isRunning == false, "核心已死,CoreProcess 却还认为在跑")
        await process.stop()
    }

    @Test("★ 核心崩溃后 CoastController 必须知道:否则永远起不起来,且系统代理挂在死端口上")
    @MainActor func controllerNoticesCrash() async throws {
        guard let mihomo = Self.mihomo else {
            Issue.record("没有可用的 mihomo,本测试无法证明任何事")
            return
        }
        _ = mihomo
        // ★ **不在测试里 setenv**。环境变量是进程级共享可变状态,而 swift-testing 默认并行跑
        //   套件 —— 一个测试改掉 COAST_CORE_PATH,另一个套件恰好在这期间读它,就会莫名其妙地
        //   失败。这不是假设:第一版就是这么写的,结果 RealCoreE2ETests 单跑全过、全量跑必挂。
        //   隔离根由**运行器**在进程启动时设好(见 scripts/regression.sh)。
        // 跳过而不是判失败:缺的是**运行器配置**,不是被测代码有问题。裸跑 `swift test`
        // 是完全正当的用法,不该因此变红。打一行说明,让跳过是看得见的而不是静默的。
        // `scripts/regression.sh` 始终会设隔离根,所以回归跑里它一定真跑。
        guard AppPaths.userDir.path.contains("coast-test-") else {
            print("⏭  跳过「核心崩溃」控制器测试:需要隔离数据根,"
                  + "否则会写用户真实 config。跑 bash scripts/regression.sh,"
                  + "或设 COAST_DATA_DIR=<临时目录>。")
            return
        }
        try FileManager.default.createDirectory(
            at: AppPaths.configDir, withIntermediateDirectories: true)

        var config = AppConfig()
        config.uiPort = 19412
        config.mixedPort = 19413
        config.webProxy = false     // 绝不在测试里真去改系统代理
        config.tun = false
        let controller = CoastController(config: config)

        nonisolated(unsafe) var notified = false
        controller.onCoreUnexpectedlyExited = { notified = true }

        await controller.startCore()
        guard controller.isCoreRunning else {
            Issue.record("核心没起来,后面的断言没有意义")
            return
        }

        // 模拟意外死亡
        guard let pid = controller.coreProcessIdentifierForDiagnostics else {
            Issue.record("取不到核心 PID")
            return
        }
        kill(pid, SIGKILL)

        let deadline = Date().addingTimeInterval(8)
        while controller.isCoreRunning, Date() < deadline {
            try? await Task.sleep(for: .milliseconds(100))
        }

        #expect(controller.isCoreRunning == false,
                "核心已死,controller 仍认为在跑 —— startCore 的 guard 会让它永远起不起来")
        #expect(notified, "没有通知上层 —— 用户看不到任何提示,只会觉得网突然坏了")

        // 修复的实质检验:此刻必须**能重新启动**。
        await controller.startCore()
        #expect(controller.isCoreRunning, "崩溃之后起不起来 —— 用户只能靠先点一次「停止」")
        await controller.stopCore()
    }
}
