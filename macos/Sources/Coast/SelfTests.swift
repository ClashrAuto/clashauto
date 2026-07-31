import CoastKit
import Foundation
import ServiceManagement

/// 无头自检钩子。沿用 Qt 版 `COAST_*_SELFTEST` 的做法：**不建 GUI、不起核心**，
/// 跑完打印结果并退出。
///
/// 存在的理由是有些路径**只在打包后的真实 .app 里才走得到** —— helper 注册要求 bundle 布局
/// 与代码签名都对，系统代理要求真实桌面会话。这些在单测里一行都验不了，而出错的后果
/// （TUN 不生效 / 本机断网）又很重。给一个几秒钟就能跑的钩子，比让用户拿 GUI 去试划算得多。
enum SelfTests {

    static func runIfRequested() {
        let environment = ProcessInfo.processInfo.environment
        if environment["COAST_HELPER_SELFTEST"] == "1" { helperSelfTest() }
        if environment["COAST_XPC_SELFTEST"] == "1" { xpcSelfTest() }
        if environment["COAST_HELPER_UNREGISTER"] == "1" { helperUnregister() }
        if environment["COAST_SYSPROXY_SELFTEST"] == "1" { systemProxySelfTest() }
        if environment["COAST_PATHS_SELFTEST"] == "1" { pathsSelfTest() }
        if environment["COAST_TOPO_SELFTEST"] == "1" { topoSelfTest() }
    }

    /// helper 自检：报告 bundle 布局与 SMAppService 状态。
    ///
    /// 重点是把**「包没打对」**和**「签名不满足要求」**区分开 —— 两者都表现为 helper 不可用，
    /// 但前者是我们的 bug，后者是 ad-hoc 签名的固有限制（本地开发必然如此）。
    private static func helperSelfTest() {
        print("=== helper 自检 ===")
        let bundle = Bundle.main.bundleURL
        print("bundle: \(bundle.path)")

        let plist = bundle.appendingPathComponent(
            "Contents/Library/LaunchDaemons/com.yuehongsun.coast.helper.plist")
        let executable = bundle.appendingPathComponent(
            "Contents/MacOS/com.yuehongsun.coast.helper")
        let fm = FileManager.default
        print("daemon plist 存在: \(fm.fileExists(atPath: plist.path))  \(plist.lastPathComponent)")
        print("helper 可执行:     \(fm.isExecutableFile(atPath: executable.path))")

        // ★ 位置会影响判定，而且影响方式极具误导性：同一个包，放在构建目录里跑 SMAppService
        //   报 **notFound**（看起来像「plist 没打进去」），拷到 ~/Applications 再跑就变成
        //   **requiresApproval**（说明它其实找到并认了这份 plist）。在构建目录里排查 notFound
        //   会让人去找一个根本不存在的打包 bug，所以这里先把位置讲清楚。
        let path = bundle.path
        let inStandardLocation = path.hasPrefix("/Applications/")
            || path.hasPrefix(NSHomeDirectory() + "/Applications/")
        if !inStandardLocation {
            print("!! 当前不在标准位置（/Applications 或 ~/Applications）。")
            print("   此时 SMAppService 常报 notFound —— 那是**位置**问题，不是包没打对。")
            print("   要判断包本身，请先把 .app 拷到 ~/Applications 再跑本自检。")
        }

        print("SMAppService 状态: \(MacHelperClient.status())")
        do {
            let status = try MacHelperClient.register()
            print("注册结果: \(status)")
        } catch {
            print("注册失败: \(error.localizedDescription)")
            print("（ad-hoc 签名下这是**预期**的：SMAppService 要求 helper 与主程序同属一个 Team ID，")
            print("  而 ad-hoc 签名的 TeamIdentifier 是 not set。真正可用的 helper 必须用正式开发者")
            print("  证书签，走外部仓库 integemjack/schat.build。")
            print("  判断包本身对不对，看上面两行：plist 与可执行文件在位、且状态不是 notFound。）")
        }
        exit(0)
    }

    /// XPC 往返自检：真的连上已安装的 root helper 并调一次**只读**方法。
    ///
    /// 这是 `COAST_HELPER_SELFTEST` 之后的下一格：那个只证明 helper 被 launchd 认下了，
    /// **不证明 XPC 通道能用**。两端的 `setCodeSigningRequirement` 是双向的 ——
    /// 签名对不上时连接会在第一次调用时才断（XPC 是懒建连的），所以必须真发一次请求。
    /// 选 `version()` 是因为它不改任何系统状态：连通性失败与「改配置失败」不会混在一起。
    private static func xpcSelfTest() {
        print("=== XPC 往返自检 ===")
        print("SMAppService 状态: \(MacHelperClient.status())")
        // ★ 这里**不能**用 `Task { }` + 信号量阻塞主线程：`SelfTests` 是 @MainActor 隔离的，
        //   `Task { }` 会继承主 actor，任务体因此排在主线程上，而主线程正卡在 wait —— 任务
        //   永远不开始，连 MacHelperClient 自己的 15s 超时都不会触发（第一版就是这么假死的，
        //   现象酷似「XPC 通道挂住」，极具误导性）。用 detached 让它真的跑在协作线程池上，
        //   主线程则跑 runloop 而不是阻塞。
        nonisolated(unsafe) var exitCode: Int32 = 1
        nonisolated(unsafe) var finished = false
        Task.detached {
            do {
                let version = try await MacHelperClient().version()
                print("helper 版本: \(version)")
                print("✅ XPC 通道可用 —— 双向代码签名鉴权已放行")
                exitCode = 0
            } catch {
                print("❌ XPC 调用失败: \(error)")
                print("   状态是 enabled 却调不通时，几乎总是两端 requirement 与实际签名不符；")
                print("   用 codesign -v -R=<requirement> 分别验主程序与 helper 可定位是哪一端。")
            }
            finished = true
        }
        // 必须**长于** MacHelperClient 自身的 15s 超时，否则先超时的是本探针，
        // 会屏蔽掉客户端本来会抛出的真实错误。
        let deadline = Date().addingTimeInterval(25)
        while !finished, Date() < deadline {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.1))
        }
        if !finished { print("❌ 25s 内无响应") }
        exit(exitCode)
    }

    /// 注销 helper。**重打包后必须做这一步**：launchd 把注册与注册时那份包的代码签名绑在一起，
    /// 原地替换 .app 之后旧注册既不失效也拉不起来 —— `status()` 仍报 enabled，XPC 却永远无人应答。
    private static func helperUnregister() {
        do {
            try MacHelperClient.unregister()
            print("已注销。当前状态: \(MacHelperClient.status())")
            exit(0)
        } catch {
            print("注销失败: \(error.localizedDescription)")
            exit(1)
        }
    }

    private static func topoSelfTest() {
        print("=== 默认网关自检 ===")
        if let gw = LanTopology.defaultGateway() {
            print("IP: \(gw.ip)  MAC: \(gw.mac)  接口: \(gw.interface)")
            print("接管设备所需的三要素齐全 ✅")
            exit(0)
        }
        print("取不到默认网关（三要素缺一即无法安全接管，会拒绝开始）")
        exit(1)
    }

    private static func systemProxySelfTest() {
        print("=== 系统代理自检 ===")
        let result = SystemProxy.selfTest()
        print(result.ok ? "PASS: \(result.message)" : "FAIL: \(result.message)")
        exit(result.ok ? 0 : 1)
    }

    /// 路径与种子资源自检：确认打包后能从 `Contents/Resources` 找到种子，
    /// 而不是悄悄回退到开发期那条仓库相对路径（那条在用户机器上根本不存在）。
    private static func pathsSelfTest() {
        print("=== 路径 / 资源自检 ===")
        print("userDir:   \(AppPaths.userDir.path)")
        print("configDir: \(AppPaths.configDir.path)")
        print("core:      \(AppPaths.coreExecutable.path)")
        var ok = true
        for name in ["config.yaml", "default.yaml", "plugin.yaml", "subscribe.yaml", "Country.mmdb"] {
            if let url = Resources.seed(name) {
                let inBundle = url.path.hasPrefix(Bundle.main.bundleURL.path)
                print("种子 \(name): \(inBundle ? "来自 .app" : "来自仓库(开发期回退)")  \(url.path)")
            } else {
                print("种子 \(name): **找不到**")
                ok = false
            }
        }
        exit(ok ? 0 : 1)
    }
}
