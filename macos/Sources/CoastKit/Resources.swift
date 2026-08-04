import Foundation

/// 只读种子资源的定位器。
///
/// Qt 端把种子塞进 qrc（`:/assets/bundle/*`）编进二进制；Swift 端不复制这一份 5.5 MB 的
/// `Country.mmdb` 到 `macos/`，而是**仍用仓库里唯一的那份** `clashauto-c++/assets`：
///   • 打包后 —— `scripts/make_app.sh` 把它拷进 `Coast.app/Contents/Resources/`，走分支 1；
///   • 开发期 —— `swift run` 没有 .app，沿可执行文件路径向上找到仓库根，走分支 2。
/// 两条路径都找不到时返回 nil，调用方按「没有种子」处理（首次运行会缺 config.yaml）。
public enum Resources {
    /// `assets/bundle/config/<name>`：config.yaml / default.yaml / plugin.yaml / subscribe.yaml / Country.mmdb
    public static func seed(_ name: String) -> URL? {
        locate("bundle/config/\(name)")
    }

    /// `assets/<relativePath>`：i18n/*.json、iconfont.ttf / remixicon.ttf、oui.txt、core。
    ///
    /// 注意 `assets/` 是三平台共用的池子，**mac 包里只有一个子集** ——
    /// `make_app.sh` 的 rsync 排掉了 MiSans、Windows 图标等 Swift 线读不到的东西。
    /// 想读新资源，先确认它没被排除（那份 exclude 列表旁边有配套的清单校验）。
    public static func asset(_ relativePath: String) -> URL? {
        locate(relativePath)
    }

    // MARK: - 查找

    private static func locate(_ relativePath: String) -> URL? {
        for root in roots {
            let candidate = root.appendingPathComponent(relativePath)
            if FileManager.default.fileExists(atPath: candidate.path) { return candidate }
        }
        return nil
    }

    /// 候选资源根，按优先级排列。惰性求值一次即可 —— 进程生命周期内不会变。
    private static let roots: [URL] = {
        var result: [URL] = []
        // 1) 打包后的 .app：Contents/Resources 直接就是 assets 的内容
        if let bundled = Bundle.main.resourceURL { result.append(bundled) }
        // 2) 开发期：从可执行文件往上找仓库根（`swift run` 走这条）
        result += assetRoots(from: CommandLine.arguments.first ?? "")
        // 3) 开发期兜底：从**本源文件**的位置往上找。
        //    `swift test` 时 argv[0] 是 Xcode 的 xctest 工具（在 /Applications 下），
        //    第 2 条一路往上什么都找不到 —— 单测里读种子会全部拿到 nil。
        //    打包后 .app 里这个路径不存在，所以不会误命中。
        result += assetRoots(from: #filePath)
        return result
    }()

    /// 从给定路径逐级向上，收集所有存在的 `clashauto-c++/assets`。
    private static func assetRoots(from path: String) -> [URL] {
        var result: [URL] = []
        var dir = URL(fileURLWithPath: path).resolvingSymlinksInPath().deletingLastPathComponent()
        for _ in 0..<8 {
            let assets = dir.appendingPathComponent("clashauto-c++/assets")
            if FileManager.default.fileExists(atPath: assets.path) { result.append(assets) }
            dir = dir.deletingLastPathComponent()
        }
        return result
    }
}
