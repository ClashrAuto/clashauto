import Foundation

/// 应用配置。字段与 YAML 键的映射**沿用 Qt 端**（`AppConfig.h` + `AppConfigLoader::load`），
/// 键名多数是 Electron 时代的历史遗留（`ui`/`port`/`web`/`use`/`node`/`sys`/`mini`…），
/// 看着不直观，但改名会让老用户的 config.yaml 全部失效，所以原样保留。
public struct AppConfig: Sendable, Equatable {
    // MARK: 网络
    /// REST API 地址。9191 而非上游默认的 9090 —— 刻意避开，免得与用户自己装的核心打架。
    public var host = "127.0.0.1"
    public var uiPort = 9191            // YAML: ui
    public var mixedPort = 7890         // YAML: port
    /// external-controller 访问密钥。为空时首次加载随机生成 32 位十六进制并落盘。
    public var secret = ""

    // MARK: 开关
    public var webProxy = true          // YAML: web —— 系统代理
    public var tun = false              // YAML: use
    public var nodeOnlyAvailable = true // YAML: node —— 只显示可用节点
    public var clearConnections = true  // YAML: clearConnections —— 切节点时断掉旧连接
    public var increment = false        // YAML: increment —— 订阅增量更新
    public var closeToTray = false      // YAML: mini —— 见下方 seed 归一化注释
    public var autoStart = false        // YAML: sys
    public var nodeSwitchNote = true    // YAML: note —— 切节点弹通知
    public var autoUpdateMinutes = 0    // YAML: autoUpdate —— 0 = 不自动更新订阅

    // MARK: 节点过滤（use_rule 段）
    public var allowRule = ""
    public var noAllowRule = ""
    public var allowRuleEnabled = false
    public var noAllowRuleEnabled = false

    // MARK: 外观与语言
    public var theme = "black"
    public var autoTheme = false
    public var autoLanguage = true      // 跟随系统语言，忽略下面的 language 手选
    public var language = "zh-CN"

    // MARK: 更新
    public var mirror = false           // 下载走国内镜像（ghfast.top）
    public var receiveBeta = false      // YAML: beta —— 更新检查是否看 prerelease

    public init() {}
}

// MARK: - 加载 / 保存

public enum AppConfigLoader {

    /// 读用户 config.yaml；首次运行先从种子落地一份。
    ///
    /// 与 Qt 端 `AppConfigLoader::load()` 的三个副作用完全一致，缺一个都会让两版行为分叉：
    ///   1. 种子落地后补写权限（种子只读，不补则后续保存静默失败）；
    ///   2. 首次落地时把 `mini` 归一为 false —— 种子来自 Electron 版发布，那边固定写
    ///      `mini: true`，而 C++/Swift 版的默认是「正常显示窗口 + ✕ 退出」；
    ///   3. `secret` 为空时随机生成并**追加**回用户配置，之后固定复用。
    @discardableResult
    public static func load() throws -> AppConfig {
        try AppPaths.ensureDirectories()

        let userConfig = AppPaths.userConfig
        let fm = FileManager.default
        if !fm.fileExists(atPath: userConfig.path), let seed = Resources.seed("config.yaml") {
            try? fm.copyItem(at: seed, to: userConfig)
            AppPaths.makeWritable(userConfig)
            if var text = try? String(contentsOf: userConfig, encoding: .utf8) {
                text = YAMLText.setBool(text, key: "mini", value: false)
                try? text.write(to: userConfig, atomically: true, encoding: .utf8)
            }
        }

        let source = fm.fileExists(atPath: userConfig.path) ? userConfig : Resources.seed("config.yaml")
        guard let source, let yaml = try? String(contentsOf: source, encoding: .utf8) else {
            return AppConfig()   // 种子也没有：全默认值，应用仍可启动并引导用户配置
        }

        var config = AppConfig()
        config.host = YAMLText.value(yaml, key: "host", default: config.host)!
        config.uiPort = YAMLText.int(yaml, key: "ui", default: config.uiPort)
        config.mixedPort = YAMLText.int(yaml, key: "port", default: config.mixedPort)
        config.webProxy = YAMLText.bool(yaml, key: "web", default: config.webProxy)
        config.tun = YAMLText.bool(yaml, key: "use", default: config.tun)
        config.nodeOnlyAvailable = YAMLText.bool(yaml, key: "node", default: config.nodeOnlyAvailable)
        config.clearConnections = YAMLText.bool(yaml, key: "clearConnections", default: config.clearConnections)
        config.increment = YAMLText.bool(yaml, key: "increment", default: config.increment)
        config.closeToTray = YAMLText.bool(yaml, key: "mini", default: config.closeToTray)
        config.autoStart = YAMLText.bool(yaml, key: "sys", default: config.autoStart)
        config.nodeSwitchNote = YAMLText.bool(yaml, key: "note", default: config.nodeSwitchNote)
        config.autoUpdateMinutes = YAMLText.int(yaml, key: "autoUpdate", default: config.autoUpdateMinutes)
        config.allowRule = YAMLText.nestedValue(yaml, section: "use_rule", key: "allow", default: config.allowRule)!
        config.noAllowRule = YAMLText.nestedValue(yaml, section: "use_rule", key: "noallow", default: config.noAllowRule)!
        config.allowRuleEnabled = YAMLText.nestedBool(yaml, section: "use_rule", key: "allowUse", default: false)
        config.noAllowRuleEnabled = YAMLText.nestedBool(yaml, section: "use_rule", key: "noallowUse", default: false)
        config.theme = YAMLText.value(yaml, key: "theme", default: config.theme)!
        config.autoTheme = YAMLText.bool(yaml, key: "autoTheme", default: config.autoTheme)
        config.autoLanguage = YAMLText.bool(yaml, key: "autoLanguage", default: config.autoLanguage)
        config.mirror = YAMLText.bool(yaml, key: "mirror", default: config.mirror)
        config.receiveBeta = YAMLText.bool(yaml, key: "beta", default: config.receiveBeta)
        config.language = YAMLText.value(yaml, key: "language", default: config.language)!
        config.secret = YAMLText.value(yaml, key: "secret", default: "")!

        if config.secret.isEmpty {
            config.secret = randomSecret()
            if fm.fileExists(atPath: userConfig.path) {
                let separator = (yaml.isEmpty || yaml.hasSuffix("\n")) ? "" : "\n"
                let appended = yaml + separator + "secret: \(config.secret)\n"
                try? appended.write(to: userConfig, atomically: true, encoding: .utf8)
            }
        }
        return config
    }

    /// 把单个布尔键写回用户 config.yaml（设置页每个开关即时落盘，对齐 `QmlBridge::persistConfigBool`）。
    public static func persist(key: String, bool value: Bool) {
        persist(key: key, raw: value ? "true" : "false")
    }

    public static func persist(key: String, int value: Int) {
        persist(key: key, raw: String(value))
    }

    public static func persist(key: String, raw value: String) {
        let url = AppPaths.userConfig
        let existing = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
        let updated = YAMLText.setValue(existing, key: key, value: value)
        AppPaths.makeWritable(url)
        try? updated.write(to: url, atomically: true, encoding: .utf8)
    }

    /// 32 位十六进制。用 `SystemRandomNumberGenerator`（底层 arc4random）而不是 `Int.random`
    /// 的默认种子路径，密钥要的是不可预测，不是可复现。
    private static func randomSecret() -> String {
        var generator = SystemRandomNumberGenerator()
        return (0..<16).map { _ in String(format: "%02x", UInt8.random(in: 0...255, using: &generator)) }.joined()
    }
}
