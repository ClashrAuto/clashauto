import Testing
@testable import CoastKit

// YAML 是靠正则做的文本手术，编译器一点都保护不了 —— 这些用例就是那层保护。
// 取值口径逐条对齐 C++ 侧 AppConfigLoader 的五个 helper。

@Suite("YAMLText 读")
struct YAMLTextReadTests {
    let sample = """
    host: 127.0.0.1
    ui: 9191
    port: 7890
    web: true
    mini: false
    theme: 'black'
    note: yes            # 行内注释要被截掉
    use_rule:
      allow: 香港|HK
      allowUse: true
      noallow: ''
      noallowUse: false
    """

    @Test("顶层标量去引号、去行内注释")
    func scalars() {
        #expect(YAMLText.value(sample, key: "host") == "127.0.0.1")
        #expect(YAMLText.value(sample, key: "theme") == "black")
        #expect(YAMLText.value(sample, key: "missing", default: "fallback") == "fallback")
    }

    @Test("整数与布尔的各种写法")
    func intsAndBools() {
        #expect(YAMLText.int(sample, key: "ui", default: 0) == 9191)
        #expect(YAMLText.int(sample, key: "missing", default: 42) == 42)
        #expect(YAMLText.bool(sample, key: "web", default: false) == true)
        #expect(YAMLText.bool(sample, key: "mini", default: true) == false)
        #expect(YAMLText.bool(sample, key: "note", default: false) == true)   // yes
        #expect(YAMLText.bool(sample, key: "missing", default: true) == true)
    }

    @Test("两级标量只在自己的段里找")
    func nested() {
        #expect(YAMLText.nestedValue(sample, section: "use_rule", key: "allow") == "香港|HK")
        #expect(YAMLText.nestedBool(sample, section: "use_rule", key: "allowUse", default: false) == true)
        #expect(YAMLText.nestedBool(sample, section: "use_rule", key: "noallowUse", default: true) == false)
        // 顶层有 web，但 use_rule 段里没有：不能串段
        #expect(YAMLText.nestedValue(sample, section: "use_rule", key: "web", default: "none") == "none")
    }
}

@Suite("YAMLText 写")
struct YAMLTextWriteTests {
    @Test("改写已有键时只动那一行")
    func replaceInPlace() {
        let before = "# 头部注释\nweb: true\nport: 7890\n# 尾部注释\n"
        let after = YAMLText.setBool(before, key: "web", value: false)
        #expect(after == "# 头部注释\nweb: false\nport: 7890\n# 尾部注释\n")
    }

    @Test("键不存在时追加到末尾")
    func appendWhenMissing() {
        #expect(YAMLText.setInt("port: 7890\n", key: "ui", value: 9191) == "port: 7890\nui: 9191\n")
        // 末尾没换行也要补上，否则会和最后一行黏在一起
        #expect(YAMLText.setBool("port: 7890", key: "web", value: true) == "port: 7890\nweb: true\n")
    }

    @Test("替换内容里的 $ 不被当成正则反向引用")
    func dollarIsLiteral() {
        let after = YAMLText.setValue("secret: old\n", key: "secret", value: "a$1b")
        #expect(after == "secret: a$1b\n")
    }

    @Test("需要引号的自由文本才加引号")
    func quoting() {
        #expect(YAMLText.quoted("plain") == "plain")
        #expect(YAMLText.quoted("has space") == "\"has space\"")
        #expect(YAMLText.quoted("") == "\"\"")
        #expect(YAMLText.quoted("say \"hi\"") == "\"say \\\"hi\\\"\"")
    }
}
