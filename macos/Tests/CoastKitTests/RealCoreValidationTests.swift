import Foundation
import Testing
@testable import CoastKit

/// 用**真正的 mihomo 核心**(`mihomo -t`)校验我们生成的 full.yaml。
///
/// 此前只用 PyYAML 验过「是合法 YAML」,但核心有自己的 schema(规则类型、代理字段、组结构),
/// **那才是权威判据** —— 一份合法 YAML 完全可能被核心拒绝加载。
///
/// 核心二进制不在时自动跳过(CI/别人机器上未必有),不让它成为硬依赖。
/// 本机路径由 COAST_TEST_MIHOMO 指定。
@Suite("真核心校验 full.yaml")
struct RealCoreValidationTests {

    private var mihomo: String? {
        let p = ProcessInfo.processInfo.environment["COAST_TEST_MIHOMO"] ?? ""
        return FileManager.default.isExecutableFile(atPath: p) ? p : nil
    }

    /// 跑 `mihomo -t -d <dir> -f <config>`,返回 (通过?, 输出)
    private func validate(config: URL, dir: URL) -> (Bool, String) {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: mihomo!)
        task.arguments = ["-t", "-d", dir.path, "-f", config.path]
        let pipe = Pipe()
        task.standardOutput = pipe; task.standardError = pipe
        try? task.run()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()
        return (task.terminationStatus == 0, String(data: data, encoding: .utf8) ?? "")
    }

    /// 造一个完整场景:两条订阅(含各种协议)、自定义规则与区域组、一台被代理的设备。
    private func buildFullConfig(tun: Bool) throws -> (URL, URL) {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-realcore-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)

        // 订阅:用 SubParser 从真实分享链接生成(覆盖 trojan/vless/ss/hysteria2)
        let links = """
        trojan://pw123@t.example.com:443?sni=t.example.com#香港01
        vless://11111111-2222-3333-4444-555555555555@v.example.com:443?security=tls&type=ws&path=/w#日本02
        ss://YWVzLTI1Ni1nY206cGFzc3dvcmQ=@1.2.3.4:8388#美国03
        hysteria2://pw@h.example.com:8443?sni=h.example.com#新加坡04
        """
        let proxies = SubParser.toClashProxies(links) ?? ""
        var sub = "- name: '测试机场'\n  url: 'https://x'\n  use: true\n  list:\n"
        // 把 proxies 块转成 subscribe.yaml 的 list 缩进(4/6 空格)
        for line in proxies.components(separatedBy: "\n").dropFirst() where !line.isEmpty {
            sub += line.hasPrefix("  - ") ? "  " + line + "\n" : "  " + line + "\n"
        }
        sub += "      use: true\n"
        try sub.write(to: dir.appendingPathComponent("subscribe.yaml"), atomically: true, encoding: .utf8)

        // 自定义规则 + 区域组
        let rules = #"""
        {"area":[{"name":"我的港区","type":"url-test","rule":"香港"}],
         "rule":[{"type":"DOMAIN-SUFFIX","node":"DIRECT","value":"example.com"},
                 {"type":"IP-CIDR","node":"DIRECT","value":"10.8.0.0/16"},
                 {"type":"PROCESS-NAME","node":"🚀 节点选择","value":"curl"}]}
        """#
        try rules.write(to: dir.appendingPathComponent("rules.json"), atomically: true, encoding: .utf8)

        // 一台被代理的设备(生成 SRC-IP-CIDR 规则)
        let store = DeviceStore(configDir: dir)
        if var d = store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:01", true, ip: "192.168.31.50") {
            d.policyMode = .direct
            _ = store.save(d)
        }

        // ★ 把内置的 Country.mmdb 放进 `-d` 目录 —— 正是 app 首次运行时 `seedGeoIP()` 做的事。
        //
        //   不放的话 mihomo 会**联网下载** MMDB(生成的规则里有 GEOIP,CN),于是这个测试:
        //   一、依赖外网,二、下不动时要等满 90 秒超时才报错,三、报的错跟被测的配置毫无关系。
        //   实测就踩到过一次 “can't download MMDB: context deadline exceeded”,看起来像是
        //   我们生成的配置有问题,其实只是当时下不动。
        if let mmdb = Resources.seed("Country.mmdb") {
            try? FileManager.default.copyItem(
                at: mmdb, to: dir.appendingPathComponent("Country.mmdb"))
        }

        var config = AppConfig()
        config.secret = "testsecret"
        let builder = ConfigBuilder(config: config, directory: dir)
        guard let path = builder.ensureFullConfig(tunEnabled: tun) else {
            throw NSError(domain: "build", code: 1)
        }
        return (path, dir)
    }

    @Test("★ 真核心接受我们生成的 full.yaml(TUN 关)")
    func coreAcceptsConfig() throws {
        guard mihomo != nil else { return }   // 没核心就跳过
        let (config, dir) = try buildFullConfig(tun: false)
        defer { try? FileManager.default.removeItem(at: dir) }
        let (ok, output) = validate(config: config, dir: dir)
        #expect(ok, "mihomo -t 拒绝了配置:\n\(output)")
    }

    @Test("★ 真核心接受我们生成的 full.yaml(TUN 开)")
    func coreAcceptsConfigWithTun() throws {
        guard mihomo != nil else { return }
        let (config, dir) = try buildFullConfig(tun: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let (ok, output) = validate(config: config, dir: dir)
        #expect(ok, "mihomo -t 拒绝了 TUN 配置:\n\(output)")
    }
}
