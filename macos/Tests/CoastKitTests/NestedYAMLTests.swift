import Foundation
import Testing
@testable import CoastKit

/// `use_rule:` 段的读写往返。
///
/// 这四个设置（允许/排除节点的正则及其开关）此前**有界面、无落盘** —— 因为 `YAMLText`
/// 只有嵌套读、没有嵌套写。用户填完正则、点应用、节点当场被过滤，重启后全丢。
@Suite("嵌套 YAML 读写往返")
struct NestedYAMLTests {

    private func roundTrip(_ value: String, into text: String = "use_rule:\n  allowUse: false\n")
        -> String? {
        let written = YAMLText.setNestedValue(text, section: "use_rule", key: "allow", value: value)
        return YAMLText.nestedValue(written, section: "use_rule", key: "allow")
    }

    @Test("普通正则原样往返")
    func plain() {
        #expect(roundTrip("香港|HK") == "香港|HK")
    }

    @Test("★ 含空格 / 冒号 / # 的正则 —— 读侧遇到 # 就截断，必须靠引号兜住")
    func specialCharacters() {
        // 读侧的捕获是 `[^'"\r\n#]*`：# 会截断，引号会终止。写侧不加引号的话这些值读回来是残的。
        for raw in ["HK 01", "节点: 香港", "机场#1", "a|b c"] {
            let quoted = YAMLText.quoted(raw)
            let text = YAMLText.setNestedValue("use_rule:\n  allowUse: true\n",
                                               section: "use_rule", key: "allow", value: quoted)
            let back = YAMLText.nestedValue(text, section: "use_rule", key: "allow")
            #expect(back == raw, "『\(raw)』往返后成了『\(back ?? "nil")』(写入的是 \(quoted))")
        }
    }

    @Test("段落不存在时整段追加，且不动原有内容")
    func createsSection() {
        let original = "ui: 9191\nport: 7890\n"
        let out = YAMLText.setNestedValue(original, section: "use_rule", key: "allow", value: "'HK'")
        #expect(out.hasPrefix(original), "原有内容被改动了")
        #expect(YAMLText.nestedValue(out, section: "use_rule", key: "allow") == "HK")
    }

    @Test("段在、键不在时插到段首，不碰同段其它键")
    func insertsKeyIntoExistingSection() {
        let original = "use_rule:\n  allowUse: true\n  noallow: 'x'\nui: 9191\n"
        let out = YAMLText.setNestedValue(original, section: "use_rule", key: "allow", value: "'HK'")
        #expect(YAMLText.nestedValue(out, section: "use_rule", key: "allow") == "HK")
        #expect(YAMLText.nestedBool(out, section: "use_rule", key: "allowUse", default: false))
        #expect(YAMLText.nestedValue(out, section: "use_rule", key: "noallow") == "x")
        #expect(YAMLText.int(out, key: "ui", default: 0) == 9191, "段外的键被牵连了")
    }

    @Test("重复写同一个键不产生第二行")
    func idempotent() {
        var text = "use_rule:\n  allow: 'a'\n"
        for value in ["'b'", "'c'", "'d'"] {
            text = YAMLText.setNestedValue(text, section: "use_rule", key: "allow", value: value)
        }
        #expect(text.components(separatedBy: "allow:").count - 1 == 1, "写重复了:\n\(text)")
        #expect(YAMLText.nestedValue(text, section: "use_rule", key: "allow") == "d")
    }
}

/// 「设置改完重启还在吗」的端到端锁。
@Suite("允许/排除规则的持久化")
struct AllowRulePersistenceTests {

    @Test("★ 写盘再重新加载，四个字段原样还在")
    func survivesReload() throws {
        // 隔离数据根：这几个 API 直接写 AppPaths.userConfig。
        guard AppPaths.userDir.path.contains("coast-test-") else {
            print("⏭  跳过持久化测试:需要隔离数据根(跑 scripts/regression.sh 或设 COAST_DATA_DIR)")
            return
        }
        try FileManager.default.createDirectory(at: AppPaths.configDir,
                                                withIntermediateDirectories: true)
        // 故意挑会咬人的值：空格、竖线、冒号、# —— 早先的读侧遇 # 就截断。
        let allow = "香港|HK 01|机场#1"
        let noAllow = "过期: 勿用"

        AppConfigLoader.persist(section: "use_rule", key: "allow", raw: YAMLText.quoted(allow))
        AppConfigLoader.persist(section: "use_rule", key: "noallow", raw: YAMLText.quoted(noAllow))
        AppConfigLoader.persist(section: "use_rule", key: "allowUse", bool: true)
        AppConfigLoader.persist(section: "use_rule", key: "noallowUse", bool: true)

        let reloaded = try AppConfigLoader.load()
        #expect(reloaded.allowRule == allow, "允许规则没存住:『\(reloaded.allowRule)』")
        #expect(reloaded.noAllowRule == noAllow, "排除规则没存住:『\(reloaded.noAllowRule)』")
        #expect(reloaded.allowRuleEnabled, "允许开关没存住")
        #expect(reloaded.noAllowRuleEnabled, "排除开关没存住")
    }
}

@Suite("新设备提醒的持久化")
struct NewDeviceAlertPersistenceTests {

    @Test("★ 开关写盘后重新加载还在")
    func survivesReload() throws {
        guard AppPaths.userDir.path.contains("coast-test-") else {
            print("⏭  跳过:需要隔离数据根(跑 scripts/regression.sh)")
            return
        }
        try FileManager.default.createDirectory(at: AppPaths.configDir,
                                                withIntermediateDirectories: true)
        #expect(AppConfig().newDeviceAlert == false, "默认应当是关 —— 热闹的网络里默认开会被通知淹没")
        AppConfigLoader.persist(key: "newDevice", bool: true)
        #expect(try AppConfigLoader.load().newDeviceAlert, "开关没存住")
        AppConfigLoader.persist(key: "newDevice", bool: false)
        #expect(try AppConfigLoader.load().newDeviceAlert == false, "关不掉")
    }
}
