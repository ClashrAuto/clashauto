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
        if environment["COAST_LOOKUP_SELFTEST"] == "1" { lookupSelfTest() }
        if environment["COAST_CONNSTATS_SELFTEST"] == "1" { connStatsSelfTest() }
        if environment["COAST_SETTINGS_SELFTEST"] == "1" { settingsSelfTest() }
    }

    /// 设置写入自检 —— 把「保存一个设置」这条路径变成**可以从外部驱动**的。
    ///
    /// 存在的理由：`config.yaml` 是两条产品线共用的，一条线保存设置时**会不会把另一条线
    /// 认识、自己不认识的键抹掉**，是个只有真写一次才答得上来的问题。而在 GUI 里它只能
    /// 靠点开关触发 —— 实测 `System Events click at` 与 `cliclick` 合成的点击都驱动不了
    /// QML 控件，于是这条路径在自动化里**根本走不到**。给它一个入口，一秒钟就能验完。
    ///
    /// 只翻 `clearConnections` 这个无副作用的开关（不动网络、不动系统代理），翻完打印前后值。
    private static func settingsSelfTest() {
        print("=== 设置写入自检 ===")
        print("configDir: \(AppPaths.configDir.path)")
        guard let before = try? AppConfigLoader.load() else {
            print("[settings] FAIL 读不到 config.yaml"); exit(1)
        }
        let flipped = !before.clearConnections
        AppConfigLoader.persist(key: "clearConnections", bool: flipped)
        guard let after = try? AppConfigLoader.load() else {
            print("[settings] FAIL 写完读不回来"); exit(1)
        }
        print("clearConnections: \(before.clearConnections) -> 写入 \(flipped) -> 读回 \(after.clearConnections)")
        print(after.clearConnections == flipped ? "[settings] OK" : "[settings] FAIL 写进去又读不回来")
        exit(after.clearConnections == flipped ? 0 : 1)
    }

    /// 流量构成自检 —— 与 Qt 的 `COAST_CONNSTATS_SELFTEST` **同一份 fixture、同一组期望**。
    ///
    /// 这块是纯算术（逐连接取增量 + 直连/代理分桶 + 按 host 累计排序），没有 UI 能验。
    /// 两条线各自实现了一遍，用同一份输入跑出不同的数，就是其中一条错了 ——
    /// 这正是单看一条线永远发现不了的那类 bug。
    /// 关键点：第二拍只能计**增量**，否则同一份流量每拍重复累加，总量随挂机时间线性虚涨。
    private static func connStatsSelfTest() {
        print("=== 流量构成自检（fixture 与 Qt 对齐） ===")
        func snapshot(_ json: String) -> [[String: Any]] {
            let data = Data(json.utf8)
            return (try? JSONSerialization.jsonObject(with: data)) as? [[String: Any]] ?? []
        }
        // 第一拍：代理 2 条（一条来自局域网设备）、直连 1 条、REJECT 1 条（两桶都不该记）
        let tick0 = snapshot("""
            [{"id":"a","start":"2026-07-28T10:00:01+08:00","chains":["HK-01","节点选择"],
              "download":1000,"upload":100,
              "metadata":{"host":"youtube.com","sourceIP":"192.168.20.140"}},
             {"id":"b","start":"2026-07-28T10:00:02+08:00","chains":["DIRECT"],
              "download":2000,"upload":200,
              "metadata":{"host":"mirrors.aliyun.com","sourceIP":"127.0.0.1"}},
             {"id":"c","start":"2026-07-28T10:00:03+08:00","chains":["JP-03"],
              "download":50,"upload":5,
              "metadata":{"host":"api.github.com","sourceIP":"127.0.0.1"}},
             {"id":"d","start":"2026-07-28T10:00:04+08:00","chains":["REJECT"],
              "download":9999,"upload":9999,
              "metadata":{"host":"ads.example.com","sourceIP":"127.0.0.1"}}]
            """)
        // 第二拍：a/b 累计值变大（只该记增量）、c 断开消失、e 新建
        let tick1 = snapshot("""
            [{"id":"a","start":"2026-07-28T10:00:01+08:00","chains":["HK-01","节点选择"],
              "download":1500,"upload":150,
              "metadata":{"host":"youtube.com","sourceIP":"192.168.20.140"}},
             {"id":"b","start":"2026-07-28T10:00:02+08:00","chains":["DIRECT"],
              "download":2200,"upload":220,
              "metadata":{"host":"mirrors.aliyun.com","sourceIP":"127.0.0.1"}},
             {"id":"d","start":"2026-07-28T10:00:04+08:00","chains":["REJECT"],
              "download":9999,"upload":9999,
              "metadata":{"host":"ads.example.com","sourceIP":"127.0.0.1"}},
             {"id":"e","start":"2026-07-28T10:00:09+08:00","chains":["SG-02"],
              "download":300,"upload":30,
              "metadata":{"host":"cdn.jsdelivr.net","sourceIP":"192.168.20.140"}}]
            """)

        var composition = TrafficComposition()
        // 「sourceIP → 设备名」这段：台账里那台设备叫 Xiaomi-Phone，本机连接显示「本机」。
        let naming: (String, String) -> String = { sourceIP, _ in
            if sourceIP == "192.168.20.140" { return "Xiaomi-Phone" }
            return DeviceStore.isLocalMachineIP(sourceIP) ? "本机" : sourceIP
        }
        func dump(_ tag: String) {
            print(String(format: "[connstats] %@ direct=%.0f proxy=%.0f total=%@", tag,
                         Double(composition.directBytes), Double(composition.proxyBytes),
                         Formatting.bytes(composition.totalBytes)))
            for item in composition.topHosts(limit: 5) {
                print(String(format: "[connstats]   top    %-20@ dev=%-14@ bytes=%.0f",
                             item.host as NSString, item.stat.device as NSString,
                             Double(item.stat.bytes)))
            }
        }
        composition.observe(tick0, deviceName: naming)
        dump("tick1")   // 期望 direct=2200 proxy=1155（REJECT 的 9999+9999 不计）
        composition.observe(tick1, deviceName: naming)
        dump("tick2")   // 期望 direct=2420 proxy=2035（只加增量；c 断开不回退）
        exit(0)
    }

    /// 单台设备查询的耗时自检。
    ///
    /// 存在的理由：`DeviceDetailView` 里 `record` 是 computed property，body 每求值一次
    /// 就查一次台账，而 body 上引用它（含 `proxyEnabled`/`canToggle` 这些派生量）有十几处。
    /// 于是「打开一台设备的详情」这个动作的开销，直接由单台查询的耗时 × 十几倍决定。
    /// 这里把它单独拎出来计时，好让「改成主键查找」这类优化有个可复现的前后对照。
    private static func lookupSelfTest() {
        print("=== 单台查询耗时自检 ===")
        let store = DeviceStore()
        guard store.isOpen else { print("台账打不开"); exit(1) }
        let macs = store.all().map(\.mac)
        guard let target = macs.last else { print("台账是空的，测不了"); exit(1) }
        print("台账 \(macs.count) 台，取末尾一台作目标（线性扫描的最坏情况）：\(target)")

        let rounds = 500
        // 先热身，把 SQLite 的页缓存与首次语句准备的开销排掉，免得算进结果。
        for _ in 0..<50 { _ = store.device(mac: target) }
        let began = Date()
        for _ in 0..<rounds { _ = store.device(mac: target) }
        let elapsed = Date().timeIntervalSince(began)
        let per = elapsed / Double(rounds) * 1000
        print(String(format: "%d 次查询共 %.3f s，单次 %.3f ms", rounds, elapsed, per))
        // 详情页一次 body 大约十几次查询，按 14 次估一帧的台账开销。
        print(String(format: "按详情页每帧约 14 次算：%.2f ms/帧（16.7 ms 预算的 %.0f%%）",
                     per * 14, per * 14 / 16.7 * 100))

        // 设备页 `lastHost(for:)` 是 per-row 调的，里面每行问一次本机地址，
        // 而这函数走 getifaddrs + getnameinfo（系统调用 + 枚举全部网卡）。
        // 旁边的 localMachineIPs 已经因为「每条连接都要问一次」加了 30 秒缓存，这个漏了。
        for _ in 0..<20 { _ = DeviceStore.localLANAddress() }
        let addrBegan = Date()
        for _ in 0..<200 { _ = DeviceStore.localLANAddress() }
        let addrPer = Date().timeIntervalSince(addrBegan) / 200 * 1000
        print(String(format: "\nlocalLANAddress 单次 %.3f ms；设备页每行调一次，"
                     + "%d 台一帧 %.2f ms（16.7 ms 预算的 %.0f%%）",
                     addrPer, macs.count, addrPer * Double(macs.count),
                     addrPer * Double(macs.count) / 16.7 * 100))

        // 设备页 `lastHost(for:)` 每行都全量扫一遍 connections，页面整体是 设备数 × 连接数。
        // 本机 Surge 才是主代理、Coast 连接数很少，实测量不出网关规模，
        // 所以这里**建模**：行数据用本地同形结构，归属判定调真的 connectionBelongs。
        struct Conn { let host: String; let sourceIP: String; let start: Date }
        let connCount = 500
        let now = Date()
        let conns = (0..<connCount).map {
            Conn(host: "h\($0).example.com",
                 sourceIP: "192.168.20.\($0 % 254 + 1)",
                 start: now.addingTimeInterval(Double($0)))
        }
        let ips = (0..<macs.count).map { "192.168.20.\($0 % 254 + 1)" }

        let nowBegan = Date()
        for ip in ips {                       // 现状：每行一次全量 filter + max
            _ = conns.filter { !$0.host.isEmpty
                    && DeviceStore.connectionBelongs(sourceIP: $0.sourceIP,
                                                     deviceIP: ip, isLocalMachine: false) }
                .max { $0.start < $1.start }?.host ?? ""
        }
        let nowCost = Date().timeIntervalSince(nowBegan) * 1000

        let fixBegan = Date()               // 提议：整页先归一次索引，每行 O(1)
        var latest: [String: Conn] = [:]
        for c in conns where !c.host.isEmpty {
            if let had = latest[c.sourceIP], had.start >= c.start { continue }
            latest[c.sourceIP] = c
        }
        for ip in ips { _ = latest[ip]?.host ?? "" }
        let fixCost = Date().timeIntervalSince(fixBegan) * 1000

        print(String(format: "\nlastHost 建模（%d 台 × %d 连接）：现状 %.2f ms/帧（预算 %.0f%%）"
                     + "→ 预建索引 %.2f ms（%.0f%%），快 %.1f 倍",
                     macs.count, connCount, nowCost, nowCost / 16.7 * 100,
                     fixCost, fixCost / 16.7 * 100, nowCost / max(fixCost, 0.0001)))

        // 状态页 CompositionCard：`topHosts` 是 computed property，ForEach 的 5 次迭代里
        // 各引用 2 次（.count 与 [index]）= **每次渲染 10 遍**，而它每遍都 filter + 全量排序
        // 512 条 hostBytes。状态页每秒随 pollTick 重绘。
        var composition = TrafficComposition()
        var synthetic: [[String: Any]] = []
        for i in 0..<TrafficComposition.maxHostStatsForTests {
            synthetic.append(["id": "c\(i)", "chains": ["🚀 节点选择"],
                              "upload": NSNumber(value: i * 7 + 1),
                              "download": NSNumber(value: i * 13 + 1),
                              "metadata": ["host": "h\(i).example.com", "sourceIP": "192.168.20.9"]])
        }
        composition.observe(synthetic)
        let hostsBegan = Date()
        for _ in 0..<100 { _ = composition.topHosts(limit: 5) }
        let hostsPer = Date().timeIntervalSince(hostsBegan) / 100 * 1000
        print(String(format: "\ntopHosts 单次 %.3f ms（%d 条）；状态页每帧调 10 遍 = %.2f ms"
                     + "（16.7 ms 预算的 %.0f%%）",
                     hostsPer, TrafficComposition.maxHostStatsForTests,
                     hostsPer * 10, hostsPer * 10 / 16.7 * 100))
        exit(0)
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

        // 期望 2 条：0 字节的那条不入库（失败的连接尝试/探测，没有浏览史意义）。
        // 两条线现已对齐 —— 这个差异正是本自检补上后第一次跑验出来的，
        // 判定与修复过程见 HistoryStore.swift 里那段注释。
        let ok = (n == 2) && (total == 1580)
        print(ok ? "历史库自检 PASS" : "历史库自检 **FAIL**")
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
