import Foundation
import Testing
@testable import CoastKit

// 打包集成的内核(make_app.sh 放进 Contents/Resources/core)首次运行落位到用户目录。
// 三条都值得钉死:不落位 = 「默认集成」白集成;误覆盖 = 用户手动升级过的内核被一次
// 启动静默回滚;开发期不跳过 = swift run 每次启动都去拷一个不存在的文件。
@Suite("打包集成内核的落位")
struct CoreSeedTests {
    private func makeTempDir() throws -> URL {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("coast-seed-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
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

    @Test("已装过内核绝不覆盖 —— 集成只服务全新安装")
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
}
