import Testing
@testable import CoastHelperProtocol

// helper 以 root 跑,这两个校验是纵深防御。用例把「什么该拒」钉死 ——
// 放松它们就等于放松一道 root 服务的防线,得有测试拦着。

@Suite("网卡名校验(防 PF 注入)")
struct InterfaceValidationTests {

    @Test("正常网卡名都过")
    func acceptsRealInterfaces() {
        for name in ["en0", "en1", "utun3", "bridge0", "lo0", "awdl0"] {
            #expect(InputValidation.isValidInterface(name), "应接受 \(name)")
        }
    }

    @Test("★ 能注入 PF 规则的字符一律拒")
    func rejectsInjection() {
        // interface 拼进 PF 规则文本;换行=另起一条规则,空格改变语义,这些都必须拒
        #expect(InputValidation.isValidInterface("en0 to any -> 127.0.0.1") == false)  // 空格
        #expect(InputValidation.isValidInterface("en0\npass out all") == false)        // 换行注入
        #expect(InputValidation.isValidInterface("en0;pfctl") == false)                // 分号
        #expect(InputValidation.isValidInterface("../en0") == false)                   // 斜杠
        #expect(InputValidation.isValidInterface("") == false)                         // 空
        #expect(InputValidation.isValidInterface("0en") == false)                      // 数字开头
        #expect(InputValidation.isValidInterface(String(repeating: "e", count: 16)) == false)  // 超长
    }
}

@Suite("IPv6 字面量校验(防 PF 注入)")
struct IPv6ValidationTests {

    @Test("合法 v6 字面量都过")
    func acceptsValid() {
        for v6 in ["::1", "fe80::1", "2408:8240:c000::1",
                   "2408:8240:c000:abcd:1122:3344:5566:7788", "fd00:abcd::1"] {
            #expect(InputValidation.isValidIPv6(v6), "应接受 \(v6)")
        }
    }

    @Test("★ 注入字符、区标、非 v6 一律拒")
    func rejectsInjection() {
        // 设备 v6 拼进 PF `inet6 ... from <v6>` 规则文本,注入字符必须拒
        #expect(InputValidation.isValidIPv6("2408::1 to any -> ::1") == false)  // 空格
        #expect(InputValidation.isValidIPv6("2408::1\nrdr pass") == false)      // 换行
        #expect(InputValidation.isValidIPv6("fe80::1%en0") == false)            // 区标(PF from 不接受)
        #expect(InputValidation.isValidIPv6("192.168.1.1") == false)           // v4 不是 v6
        #expect(InputValidation.isValidIPv6("nonsense") == false)
        #expect(InputValidation.isValidIPv6("") == false)
    }
}

@Suite("路径卫生(防遍历)")
struct PathValidationTests {

    @Test("正常绝对路径过")
    func acceptsAbsolute() {
        #expect(InputValidation.isSanePath("/Users/x/Library/Application Support/Coast/command/core"))
        #expect(InputValidation.isSanePath("/tmp/a.yaml"))
    }

    @Test("★ 相对路径、.. 遍历、空一律拒")
    func rejectsTraversal() {
        #expect(InputValidation.isSanePath("../../etc/passwd") == false)          // 相对 + 遍历
        #expect(InputValidation.isSanePath("/Users/x/../../etc/passwd") == false) // 绝对但含 ..
        #expect(InputValidation.isSanePath("relative/path") == false)             // 非绝对
        #expect(InputValidation.isSanePath("") == false)                          // 空
        // 文件名里含 .. 但不是独立段的,不拦(那是合法文件名)
        #expect(InputValidation.isSanePath("/tmp/a..b"))
    }
}
