import Foundation
import Testing
@testable import CoastKit

// 打包集成的内核(make_app.sh 放进 Contents/Resources/core)首次运行落位到用户目录。
// 三条都值得钉死:不落位 = 「默认集成」白集成;误降级 = 用户手动升级过的内核被一次
// 启动静默回滚;开发期不跳过 = swift run 每次启动都去拷一个不存在的文件。
//
// ★ 第四条是 2026-08-08 加的:**集成内核更新时必须顶掉旧的**。原来是「缺了才补、绝不
//   覆盖」,于是内核更新永远到不了老用户 —— 一台 App 已是最新的 Mac,command/core 还停在
//   v1.10.4394(早于 tide 支持),核心甩 `unsupport proxy type: tide`、REST 400,而 App
//   版本、配置、订阅全都是对的,从哪儿都查不出来。
@Suite("打包集成内核的落位")
struct CoreSeedTests {
    private func makeTempDir() throws -> URL {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("coast-seed-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// 造一个「会报版本号的内核」—— 落位要按版本比大小,不会打印版本就测不到升级。
    private func makeFakeCore(at url: URL, version: String) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try Data("#!/bin/sh\necho \"Coast Meta \(version) selftest\"\n".utf8).write(to: url)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path)
    }

    @Test("缺内核时从打包资源拷入,并补上执行位")
    func seedsWhenMissing() throws {
        let dir = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let bundled = dir.appendingPathComponent("bundled-core")
        try Data("bundled".utf8).write(to: bundled)
        let target = dir.appendingPathComponent("command/core")

        CoreProcess.seedCoreIfMissing(from: bundled, installedAt: target, to: target)

        #expect(FileManager.default.isExecutableFile(atPath: target.path))
        #expect(try Data(contentsOf: target) == Data("bundled".utf8))
    }

    @Test("版本读不出来时不覆盖已装内核 —— 宁可不升级也不要降级")
    func neverOverwrites() throws {
        let dir = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let bundled = dir.appendingPathComponent("bundled-core")
        try Data("bundled".utf8).write(to: bundled)
        let target = dir.appendingPathComponent("command/core")
        try FileManager.default.createDirectory(at: target.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try Data("user-updated".utf8).write(to: target)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: target.path)

        CoreProcess.seedCoreIfMissing(from: bundled, installedAt: target, to: target)

        #expect(try Data(contentsOf: target) == Data("user-updated".utf8))
    }

    @Test("老路径已有内核时同样不落位 —— installedAt 与 target 可以不同")
    func respectsLegacyInstall() throws {
        let dir = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let bundled = dir.appendingPathComponent("bundled-core")
        try Data("bundled".utf8).write(to: bundled)
        let legacy = dir.appendingPathComponent("command/clash/clash-darwin-arm64")
        try FileManager.default.createDirectory(at: legacy.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try Data("legacy".utf8).write(to: legacy)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: legacy.path)
        let target = dir.appendingPathComponent("command/core")

        CoreProcess.seedCoreIfMissing(from: bundled, installedAt: legacy, to: target)

        #expect(!FileManager.default.fileExists(atPath: target.path))
    }

    @Test("没有打包资源时安静跳过(开发期 swift run 走这条)")
    func skipsWithoutBundledCore() throws {
        let dir = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let target = dir.appendingPathComponent("command/core")

        CoreProcess.seedCoreIfMissing(from: nil, installedAt: target, to: target)

        #expect(!FileManager.default.fileExists(atPath: target.path))
    }

    // MARK: - 按版本升级

    @Test("集成内核更新时顶掉旧内核")
    func upgradesStaleCore() throws {
        let dir = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let bundled = dir.appendingPathComponent("bundled-core")
        try makeFakeCore(at: bundled, version: "v1.10.4420")
        let target = dir.appendingPathComponent("command/core")
        try makeFakeCore(at: target, version: "v1.10.4394")   // 早于 tide 支持

        CoreProcess.seedCoreIfMissing(from: bundled, installedAt: target, to: target)

        let now = try String(contentsOf: target, encoding: .utf8)
        #expect(now.contains("v1.10.4420"),
                "旧内核没被顶掉 —— 装过一次 Coast 的机器会永远停在当初那个内核")
    }

    @Test("已装内核更新时绝不降级")
    func neverDowngrades() throws {
        let dir = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let bundled = dir.appendingPathComponent("bundled-core")
        try makeFakeCore(at: bundled, version: "v1.10.4420")
        let target = dir.appendingPathComponent("command/core")
        try makeFakeCore(at: target, version: "v2.0.1")       // 用户手动装的更新版

        CoreProcess.seedCoreIfMissing(from: bundled, installedAt: target, to: target)

        let now = try String(contentsOf: target, encoding: .utf8)
        #expect(now.contains("v2.0.1"),
                "把用户手动升级的内核换成了旧的 —— 这是最难查的那种回滚")
    }

    @Test("同版本不重复拷贝")
    func skipsSameVersion() throws {
        let dir = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let bundled = dir.appendingPathComponent("bundled-core")
        try makeFakeCore(at: bundled, version: "v1.10.4420")
        let target = dir.appendingPathComponent("command/core")
        try makeFakeCore(at: target, version: "v1.10.4420")
        let before = try FileManager.default.attributesOfItem(atPath: target.path)[.modificationDate] as? Date

        CoreProcess.seedCoreIfMissing(from: bundled, installedAt: target, to: target)

        let after = try FileManager.default.attributesOfItem(atPath: target.path)[.modificationDate] as? Date
        #expect(before == after, "版本相同却重拷了一遍")
    }

    @Test("版本号比较:次版本号优先于构建号,读不出来返回 nil")
    func versionRanking() {
        #expect(CoreProcess.coreVersionRank("Coast Meta v1.10.4420 darwin arm64")!
                > CoreProcess.coreVersionRank("Coast Meta v1.10.4394 darwin arm64")!)
        #expect(CoreProcess.coreVersionRank("Coast Meta v1.11.1 x")!
                > CoreProcess.coreVersionRank("Coast Meta v1.10.9999 x")!)
        #expect(CoreProcess.coreVersionRank("这里没有版本号") == nil)
    }
}
