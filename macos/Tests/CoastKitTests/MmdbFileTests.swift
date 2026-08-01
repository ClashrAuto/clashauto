import Foundation
import Testing
@testable import CoastKit

/// GeoIP 数据库的校验。
///
/// 判据来自 Qt 版真机上的一次事故（2026-07-29）：写坏的 mmdb 让核心**不报错**地
/// 每次查询返回空，`GEOIP,CN` 静默失效，国内流量全部出海。所以「好文件必须过、
/// 坏文件必须拦」这两侧都要有实证 —— 只测前者的话，一个永远返回 true 的实现也能全绿。
@Suite("GeoIP 数据库校验")
struct MmdbFileTests {

    private var seed: Data? {
        guard let url = Resources.seed("Country.mmdb") else { return nil }
        return try? Data(contentsOf: url)
    }

    @Test("★ 真的内置 Country.mmdb 必须通过")
    func realDatabasePasses() throws {
        guard let data = seed else {
            Issue.record("找不到内置 Country.mmdb —— 后面的坏文件测试也就失去了参照")
            return
        }
        let result = MmdbFile.validate(data)
        #expect(result.ok, "真库被判为不合法：\(result.why)")
    }

    @Test("能解出 metadata 且数值合理")
    func metadataParses() throws {
        guard let data = seed else { return }
        let meta = try #require(MmdbFile.parseMetadata(data))
        #expect(meta.majorVersion == 2)
        #expect([24, 28, 32].contains(meta.recordSize), "record_size=\(meta.recordSize)")
        #expect(meta.nodeCount > 1000)
        #expect(meta.treeSize < data.count, "搜索树比整个文件还大")
    }

    @Test("★ HTML 错误页当成数据库写下去 —— 必须拦住")
    func rejectsHTML() {
        let html = Data(String(repeating: "<html>error</html>", count: 100).utf8)
        #expect(MmdbFile.validate(html).ok == false)
    }

    @Test("★ 半截 body(CDN 截断)—— 必须拦住")
    func rejectsTruncated() throws {
        guard let data = seed else { return }
        let half = data.prefix(data.count / 2)
        let result = MmdbFile.validate(Data(half))
        #expect(result.ok == false, "截断一半的库被判为合法了")
    }

    @Test("★ 分隔符被写脏 —— 真机那次事故正是栽在这一条")
    func rejectsDirtySeparator() throws {
        guard let data = seed, let meta = MmdbFile.parseMetadata(data) else { return }
        var broken = data
        // 把搜索树末尾那 16 字节全零分隔符里的一个字节改掉
        broken[broken.startIndex.advanced(by: meta.treeSize + 4)] = 0x7F
        let result = MmdbFile.validate(broken)
        #expect(result.ok == false, "分隔符被写脏却判为合法 —— 这正是那次静默失效的形态")
        #expect(result.why.contains("分隔符"), "原因没说到点上：\(result.why)")
    }

    @Test("★ metadata 段被抹掉 —— 必须拦住")
    func rejectsMissingMetadata() throws {
        guard let data = seed else { return }
        var broken = data
        // 抹掉末尾 64KB，metadata 标记就没了
        broken.replaceSubrange(broken.index(broken.endIndex, offsetBy: -65_536)..<broken.endIndex,
                               with: Data(repeating: 0x41, count: 65_536))
        #expect(MmdbFile.validate(broken).ok == false)
    }

    @Test("★ stage 不碰线上文件;applyStaged 之后才生效")
    func stagingDoesNotTouchLive() throws {
        guard let data = seed else { return }
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-mmdb-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let live = dir.appendingPathComponent("Country.mmdb")
        let sentinel = Data("我是线上那份，不许动".utf8)
        try sentinel.write(to: live)

        #expect(MmdbFile.stage(data, target: live).ok)
        #expect(try Data(contentsOf: live) == sentinel, "stage 动了线上文件")
        #expect(FileManager.default.fileExists(atPath: MmdbFile.stagedURL(for: live).path))

        #expect(MmdbFile.applyStaged(target: live))
        #expect(try Data(contentsOf: live).count == data.count, "applyStaged 之后没换上")
        #expect(FileManager.default.fileExists(
            atPath: MmdbFile.stagedURL(for: live).path) == false, "暂存文件没清掉")
    }

    @Test("★ 坏数据连暂存都不该产生")
    func badDataNeverStages() throws {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-mmdb2-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let live = dir.appendingPathComponent("Country.mmdb")
        let result = MmdbFile.stage(Data(repeating: 0x41, count: 1024), target: live)
        #expect(result.ok == false)
        #expect(FileManager.default.fileExists(
            atPath: MmdbFile.stagedURL(for: live).path) == false, "坏数据也写出了暂存文件")
    }
}

/// 起核心前的自愈：坏库退回内置种子。
///
/// Qt 那次事故里**没有**这一步 —— 写坏之后版本戳已经记成最新，
/// `checkGeoip` 每次都在「已是最新」处直接 return，坏库会一直用到上游发下一个 release。
@Suite("GeoIP 起核心前的体检")
struct GeoIPHealthTests {

    @Test("★ 线上库坏掉时,起核心前应换回内置种子")
    @MainActor func fallsBackToSeed() async throws {
        guard AppPaths.userDir.path.contains("coast-test-") else {
            print("⏭  跳过 GeoIP 自愈测试:需要隔离数据根(跑 scripts/regression.sh)")
            return
        }
        // ★ 还得有**核心**。体检那一步在 `CoreProcess.start()` 里,而 `start()` 的第一道 guard
        //   就是「核心在不在」—— 没有核心它在那儿就 return 了,压根走不到 GeoIP 收尾,
        //   坏库自然还是坏的。只判隔离根的话,这条测试在「设了 COAST_DATA_DIR、
        //   没设 COAST_CORE_PATH」时就会跑起来然后必挂(而它挂的其实是运行器配置)。
        guard FileManager.default.isExecutableFile(atPath: AppPaths.coreExecutable.path) else {
            print("⏭  跳过 GeoIP 自愈测试:\(AppPaths.coreExecutable.path) 没有核心。"
                  + "跑 bash scripts/regression.sh,或设 COAST_CORE_PATH=<mihomo 路径>。")
            return
        }
        guard let seedURL = Resources.seed("Country.mmdb"),
              let good = try? Data(contentsOf: seedURL) else { return }
        try FileManager.default.createDirectory(at: AppPaths.userDir,
                                                withIntermediateDirectories: true)
        let live = AppPaths.userDir.appendingPathComponent("Country.mmdb")
        // 放一份「打得开但查询全空」的坏库 —— 就是那次事故的形态
        var broken = good
        if let meta = MmdbFile.parseMetadata(good) {
            broken[broken.startIndex.advanced(by: meta.treeSize + 2)] = 0x5A
        }
        try broken.write(to: live)
        #expect(MmdbFile.validateFile(live).ok == false, "构造的坏库居然通过了校验")

        // 必须给一份**存在**的配置：`start()` 在做 GeoIP 收尾之前先 guard 核心与配置是否在位，
        // 传个不存在的路径会在那里就返回，根本走不到体检那一步（第一版就这么假失败了）。
        let configPath = AppPaths.userDir.appendingPathComponent("full.yaml")
        try """
        mixed-port: 19441
        external-controller: '127.0.0.1:19440'
        proxies: []
        proxy-groups: []
        rules: ['MATCH,DIRECT']
        """.write(to: configPath, atomically: true, encoding: .utf8)
        let process = CoreProcess(config: AppConfig())
        _ = await process.start(tunEnabled: false, fullConfigPath: configPath)
        await process.stop()

        #expect(MmdbFile.validateFile(live).ok, "坏库没有被换回内置种子 —— GEOIP 会静默失效")
        try? FileManager.default.removeItem(at: live)
    }
}
