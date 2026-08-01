import Foundation

/// 运行时数据目录。**必须与 Qt 端逐字一致** —— 两个版本共用同一份用户状态，
/// 换目录等于让老用户的订阅、设备台账、历史全部凭空消失。
///
/// Qt 端（`AppConfigLoader::load`）的算法是：取 `QStandardPaths::AppDataLocation`
/// （org == app == "Coast" 时得 `~/Library/Application Support/Coast/Coast`），再上跳一级
/// 回到品牌根。Swift 里直接写出那个品牌根即可，结果相同。
public enum AppPaths {
    /// 核心的家目录（mihomo `-d`）：`~/Library/Application Support/Coast`
    /// 内含 `logs/`、`Country.mmdb`、`cache`、`command/`（下载的核心）。
    /// `COAST_DATA_DIR` 可整体改写数据根。测试靠它保持无副作用 —— 否则任何摸到
    /// `ConfigBuilder`/`CoastController` 的测试都会往用户**真实**的 config 目录里写
    /// `full.yaml`。顺带也让本地联调能用独立 profile，不污染日常使用的那份。
    ///
    /// 写成计算属性而非 `static let`：`let` 只求值一次，测试进程里谁先碰到它谁定终身，
    /// 结果就是「单跑通过、全量跑失败」这类最难查的顺序依赖。
    public static var userDir: URL {
        if let override = ProcessInfo.processInfo.environment["COAST_DATA_DIR"], !override.isEmpty {
            return URL(fileURLWithPath: override)
        }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        return base.appendingPathComponent("Coast")
    }

    /// 配置目录：`<userDir>/config`。config.yaml / full.yaml / default.yaml /
    /// plugin.yaml / subscribe.yaml / rules.json / coast.db 都在这里。
    public static var configDir: URL { userDir.appendingPathComponent("config") }

    /// 下载到的 mihomo 核心。优先扁平的 `command/core`，回退早期版本的
    /// `command/clash/clash-darwin-<arch>`（兼容已经下过核心的老用户）。
    ///
    /// 必须在 .app **之外** —— 签名+公证的 .app 只读封存，往包内写核心会破坏封存，
    /// 使 SMAppService 特权 helper 被 Launch Constraint Violation SIGKILL（TUN 失效）。
    public static var coreExecutable: URL {
        // `COAST_CORE_PATH` 指定核心可执行文件。给测试与本地联调用 —— 否则任何摸到真核心的
        // 测试都得先把 mihomo 装进用户数据目录，等于让测试去改用户的真实数据。
        if let override = ProcessInfo.processInfo.environment["COAST_CORE_PATH"], !override.isEmpty {
            return URL(fileURLWithPath: override)
        }
        let flat = userDir.appendingPathComponent("command/core")
        if FileManager.default.fileExists(atPath: flat.path) { return flat }
        return userDir.appendingPathComponent("command/clash/clash-darwin-\(cpuArch)")
    }

    /// 生成的核心配置。启动核心前必先由 `ConfigBuilder` 写出。
    public static var fullConfig: URL { configDir.appendingPathComponent("full.yaml") }

    /// 用户配置（首次运行由种子落地）。
    public static var userConfig: URL { configDir.appendingPathComponent("config.yaml") }

    /// `arm64` / `amd64` —— 命名沿用核心发布物的约定（不是 uname 的 x86_64）。
    public static var cpuArch: String {
        #if arch(arm64)
        return "arm64"
        #else
        return "amd64"
        #endif
    }

    /// 建好 configDir（顺带建出父级 userDir）。加载配置前调用。
    public static func ensureDirectories() throws {
        try FileManager.default.createDirectory(at: configDir, withIntermediateDirectories: true)
    }

    /// qrc/只读种子拷出来后默认不可写，补回属主写权限。
    ///
    /// Qt 端 `AppConfig::makeWritable` 存在的理由在 Swift 这边一样成立：`FileManager.copyItem`
    /// 会连同源文件的权限位一起拷过来，种子在仓库里若是 0444，落地的 config.yaml 就写不进去，
    /// 而后续每一次「保存设置」都是静默失败。
    public static func makeWritable(_ url: URL) {
        try? FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: url.path)
    }
}
