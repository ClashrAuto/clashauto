import Foundation
import Testing

@testable import CoastKit

/// 程序一键更新的可离线测的三段：产物挑选、运行形态早退、替换脚本本体。
/// 下载/校验那段要真网络，不在这里测；脚本的 dmg 分支与 Qt 逐行同源，只测 zip 分支
/// （测 dmg 要 hdiutil create，慢一个数量级，而两个分支的骨架完全一样）。
@Suite("程序一键更新")
struct AppUpdaterTests {

    @Test("产物挑选：优先 zip，退 dmg，.sha256 和别家平台一概不认")
    func pickAsset() {
        let dmgOnly: [(name: String, url: String)] = [
            ("ClashAuto-1.0.567-windows-x64-portable.zip", "u"),
            ("ClashAuto-1.0.567-macos-universal.dmg", "u"),
            ("ClashAuto-1.0.567-macos-universal.dmg.sha256", "u"),
        ]
        #expect(AppUpdater.pickAsset(from: dmgOnly)?.name == "ClashAuto-1.0.567-macos-universal.dmg")

        // 万一将来发布物加了 mac zip，要选它（解压比挂载快、无提权触点）
        let withZip = dmgOnly + [("ClashAuto-1.0.567-macos-universal.zip", "u")]
        #expect(AppUpdater.pickAsset(from: withZip)?.name == "ClashAuto-1.0.567-macos-universal.zip")

        let noMac: [(name: String, url: String)] = [("ClashAuto-1.0.567-linux-x64.deb", "u")]
        #expect(AppUpdater.pickAsset(from: noMac) == nil)
    }

    @Test("★ 不是 .app 形态直接抛 notInAppBundle——别浪费几十 MB 流量")
    func devBuildBailsOutEarly() async {
        let release = UpdateChecker.Release(tag: "v9.9.9", name: "", notes: "",
                                            isPrerelease: false,
                                            assets: [("ClashAuto-9.9.9-macos-universal.dmg", "http://127.0.0.1:1/x")])
        let updater = AppUpdater()
        await #expect(throws: AppUpdater.UpdateError.self) {
            _ = try await updater.stage(release: release,
                                        bundleURL: URL(fileURLWithPath: "/tmp/not-a-bundle"),
                                        onProgress: { _ in })
        }
    }

    @Test("★ 替换脚本 zip 分支：解压 → 覆盖旧 .app → 新内容真的落位")
    func replaceScriptSwapsTheBundle() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("coast-updtest-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: root) }

        // 旧 .app（要被换掉的）
        let oldApp = root.appendingPathComponent("Coast.app")
        try fm.createDirectory(at: oldApp.appendingPathComponent("Contents"), withIntermediateDirectories: true)
        try "old".write(to: oldApp.appendingPathComponent("Contents/marker.txt"), atomically: true, encoding: .utf8)

        // 新 .app → 压成 zip（ditto -ck，与发布物同一种 zip）
        let stage = root.appendingPathComponent("payload/Coast.app")
        try fm.createDirectory(at: stage.appendingPathComponent("Contents"), withIntermediateDirectories: true)
        try "new".write(to: stage.appendingPathComponent("Contents/marker.txt"), atomically: true, encoding: .utf8)
        let zip = root.appendingPathComponent("update-macos.zip")
        let pack = Process()
        pack.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        pack.arguments = ["-ck", "--keepParent", stage.path, zip.path]
        try pack.run(); pack.waitUntilExit()
        #expect(pack.terminationStatus == 0)

        // 脚本以 `replace` 参数直跑 do_replace（跳过等 PID 那段）
        let script = AppUpdater.relaunchScript(pid: 1, package: zip.path, appBundle: oldApp.path)
        let scriptPath = root.appendingPathComponent("update.sh")
        try script.write(to: scriptPath, atomically: true, encoding: .utf8)
        let run = Process()
        run.executableURL = URL(fileURLWithPath: "/bin/sh")
        run.arguments = [scriptPath.path, "replace"]
        try run.run(); run.waitUntilExit()

        #expect(run.terminationStatus == 0)
        let marker = try String(contentsOf: oldApp.appendingPathComponent("Contents/marker.txt"), encoding: .utf8)
        #expect(marker == "new")
    }
}
