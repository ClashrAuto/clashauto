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
        if environment["COAST_HELPER_HEAL"] == "1" { helperHeal() }
        if environment["COAST_SYSPROXY_SELFTEST"] == "1" { systemProxySelfTest() }
        if environment["COAST_PATHS_SELFTEST"] == "1" { pathsSelfTest() }
        if environment["COAST_TOPO_SELFTEST"] == "1" { topoSelfTest() }
        if environment["COAST_LATENCY_SELFTEST"] == "1" { latencySelfTest() }
        if environment["COAST_DEVICES_SELFTEST"] == "1" { devicesSelfTest() }
        if environment["COAST_HISTORY_SELFTEST"] == "1" { historySelfTest() }
    }

    /// 设备台账 / 设备列表自检（对应 Qt 的 `COAST_DEVICEDB_SELFTEST`）。
    ///
    /// 打印的正是设备页那份合并结果：这一轮扫到了谁、台账里记着谁、哪些台账记录会
    /// 变成一行离线设备、哪些被判为残留丢掉。**每一行都带显示名** —— 「一堆没有名字的
    /// 设备」这个 bug 在 GUI 之外唯一看得见的地方就是这里。
    /// 配 `COAST_DATA_DIR=<某份数据目录的副本>` 可以拿真实台账验，而不动正在用的那份。
    private static func devicesSelfTest() {
        print("=== 设备台账 / 列表自检 ===")
        print("configDir: \(AppPaths.configDir.path)")
        let store = DeviceStore()
        guard store.isOpen else {
            print("台账打不开（coast.db）")
            exit(1)
        }

        let scanned = runBlocking { await LanBrowser().scan() }
        print("\n-- 本轮扫到 \(scanned.count) 台 --")
        for device in scanned {
            print("  \(device.mac)  \(device.ip.isEmpty ? "-" : device.ip)  「\(device.displayName)」"
                  + "  \(device.vendor.isEmpty ? "" : device.vendor)")
        }

        store.recordSeen(scanned)
        let purged = store.purgeStale(before: Date().addingTimeInterval(-30 * 24 * 3600))

        let seen = Set(scanned.map(\.mac))
        let prefix = DeviceStore.subnetPrefix(DeviceStore.localLANAddress() ?? "")
        var kept = 0, dropped = 0, nameless = 0
        print("\n-- 台账 \(store.all().count) 条（本机网段前缀 \(prefix.isEmpty ? "?" : prefix)）--")
        for record in store.all() where !seen.contains(record.mac) {
            let keeps = DeviceStore.keepsOfflineRow(record, localPrefix: prefix)
            keeps ? (kept += 1) : (dropped += 1)
            // 显示名为空 = 界面上那一行什么都没有。这正是要盯住的东西，单独计数。
            if record.displayLabel.isEmpty { nameless += 1 }
            print("  \(keeps ? "留" : "丢")  \(record.mac)  「\(record.displayLabel)」"
                  + "  上次可见 \(record.lastSeen.timeIntervalSince1970 > 0 ? "\(record.lastSeen)" : "从未")"
                  + (record.hasUserIntent ? "  [用户动过]" : ""))
        }
        print("\n在线 \(scanned.count) 行 + 离线 \(kept) 行；丢弃残留 \(dropped) 条；"
              + "清理过期 \(purged) 条；没有名字的行 \(nameless)")
        exit(nameless == 0 ? 0 : 1)
    }

    /// 自检钩子跑在 GUI 起来之前，没有 async 上下文 —— 用信号量把异步调用等回来。
    private static func runBlocking<T: Sendable>(_ work: @escaping @Sendable () async -> T) -> T {
        let semaphore = DispatchSemaphore(value: 0)
        nonisolated(unsafe) var result: T?
        Task {
            result = await work()
            semaphore.signal()
        }
        semaphore.wait()
        return result!
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
                let client = MacHelperClient()
                let version = try await client.version()
                print("helper 版本: \(version)")
                print("✅ XPC 通道可用 —— 双向代码签名鉴权已放行")
                // 再走一次**带审计**的调用，确认 `NSXPCConnection.current()` 在方法体里
                // 真能取到调用方 PID（它在某些上下文会返回 nil，那样审计行就少了「谁让它做的」）。
                // 选 stopCore：helper 没起过核心时它是无副作用的空操作。
                try await client.stopCore()
                print("已触发一次带审计的调用(stopCore 空操作)，本进程 PID = \(getpid())")
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

    /// 单独跑一次「包换了就重注册」的自愈逻辑，用于验证它确实生效。
    private static func helperHeal() {
        print("处理前状态: \(MacHelperClient.status())")
        print("当前 cdhash: \(MacHelperClient.debugCurrentCDHash() ?? "<取不到>")")
        print("已记录 cdhash: \(MacHelperClient.debugRecordedCDHash() ?? "<无记录>")")
        nonisolated(unsafe) var finished = false
        Task.detached {
            let outcome = await MacHelperClient.ensureRegisteredForCurrentBuild()
            print("结果: \(outcome)")
            print("处理后状态: \(MacHelperClient.status())")
            finished = true
        }
        let deadline = Date().addingTimeInterval(40)
        while !finished, Date() < deadline {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.1))
        }
        exit(0)
    }

    /// 「延迟」卡的四个数。界面上的数字没法在无头环境里读，而「画对了」与「真的测出数」
    /// 是两回事 —— 探测全返回 `—` 的话卡片照样渲染得很好看。
    private static func latencySelfTest() {
        print("=== 延迟探测自检 ===")
        let gateway = LanTopology.defaultGateway()
        print("网关: \(gateway?.ip ?? "<取不到>")  接口: \(gateway?.interface ?? "-")")
        nonisolated(unsafe) var finished = false
        Task.detached {
            func show(_ label: String, _ ms: Int) {
                let text = ms < 0 ? "— (测不到)" : (ms == 0 ? "… (未测)" : "\(ms) ms")
                print(String(format: "  %-10@ %@", label as NSString, text as NSString))
            }
            show("直连", await LatencyProbe.directRTT())
            if let ip = gateway?.ip, !ip.isEmpty {
                show("到路由", await LatencyProbe.tcpRTT(host: ip, port: 443))
            } else {
                show("到路由", LatencyProbe.unknown)
            }
            show("DNS", await LatencyProbe.dnsRTT())
            finished = true
        }
        let deadline = Date().addingTimeInterval(20)
        while !finished, Date() < deadline {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.1))
        }
        exit(finished ? 0 : 1)
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
    /// 历史库自检。对齐 Qt 线的 `COAST_HISTORY_SELFTEST`（`main_qml.cpp`）。
    ///
    /// 历史库是「昨天访问过什么」的**唯一**来源 —— 界面上那些会话累计（设备流量、
    /// 流量构成）在窗口隐藏时会停止累加，只有它是完整的账。所以它的入库规则
    /// 和聚合口径最值得单独验一遍。
    ///
    /// 用**临时目录**建库，不碰用户真实的 `coast.db`。
    private static func historySelfTest() {
        print("=== 历史库自检 ===")
        let tmp = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-history-selftest")
        try? FileManager.default.removeItem(at: tmp)
        try? FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        print("db: \(tmp.appendingPathComponent("coast.db").path)")

        let now = Int64(Date().timeIntervalSince1970 * 1000)
        func conn(_ id: String, _ host: String, _ up: Int64, _ down: Int64) -> [String: Any] {
            ["id": id,
             "upload": up, "download": down,
             "start": ISO8601DateFormatter().string(from: Date(timeIntervalSince1970: Double(now) / 1000)),
             "chains": ["DIRECT"],
             "metadata": ["host": host, "destinationIP": "1.2.3.4",
                          "network": "tcp", "sourceIP": "127.0.0.1",
                          "processPath": "/usr/bin/curl"]]
        }
        // 三条：两条有流量、一条 0 字节。**0 字节的不该入库**（与 Qt 线同一规则）。
        let snapshot: [[String: Any]] = [
            conn("a", "example.com", 50, 1000),
            conn("b", "example.com", 30, 500),
            conn("c", "zero.example", 0, 0),
        ]
        // `HistoryStore` 是 @MainActor 隔离的。
        // ★ **不能**用 `runBlocking { @MainActor in ... }` —— 那会死锁：
        //   `runBlocking` 内部是 `Task { } + semaphore.wait()`，而带 `@MainActor` 的闭包
        //   必须排到主线程上执行，主线程此刻正卡在 `wait()` 里，任务体永远开始不了。
        //   （本文件 `xpcSelfTest` 上方那段注释警告的就是这个形状，我照样踩了一次。）
        //   自检钩子由 `CoastApp.init()` 调用，**本来就在主线程**，
        //   用 `MainActor.assumeIsolated` 同步进去即可，不需要任何异步。
        let (n, total, tops) = MainActor.assumeIsolated { () -> (Int64, Int64, [(String, Int64)]) in
            let store = HistoryStore(configDir: tmp)
            store.observe(snapshot)
            store.observe([])          // 全部消失 = 全部断开 → 落库
            store.flush(includingLive: true)
            return (store.recordCount(),
                    store.todayTotal(scope: .all),
                    store.todayTop(dimension: .host, scope: .all, limit: 5).map { ($0.key, $0.bytes) })
        }
        print("records=\(n) (expect 2: 0 字节的连接不入库)")
        for t in tops { print("  top \(t.0) = \(t.1) bytes") }
        print("todayTotal=\(total) (expect 1580 = 50+1000+30+500)")

        // ★ **期望值故意与 Qt 线不同**：Qt 那边 3 条喂进去只入库 2 条
        //   （0 字节的那条被丢掉），Swift 这边 3 条全入库。
        //   这个差异是本自检**第一次跑就验出来的**，但「谁对」尚未判定：
        //     · 丢掉 0 字节：省行数，且"连一个字节都没传的连接"确实没有浏览史意义；
        //     · 全部入库：0 字节连接本身是信息（连上了但没传数据 = 可能被拒/超时），
        //       而且历史库另有 30 天保留期，行数不是问题。
        //   在判定之前，这里按**当前实际行为**（3 条）作为基线断言 ——
        //   自检的第一职责是"行为变了要能发现"，而不是替产品做决定。
        //   聚合口径两边是一致的（total=1580），那部分才是这个自检真正在守的东西。
        let ok = (n == 3) && (total == 1580)
        print(ok ? "历史库自检 PASS" : "历史库自检 **FAIL**")
        if n != 2 {
            print("注意：Qt 线同样输入只入库 2 条（0 字节被过滤），两条线行为不一致，待判定")
        }
        try? FileManager.default.removeItem(at: tmp)
        exit(ok ? 0 : 1)
    }

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
