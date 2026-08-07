import Foundation
import Testing
@testable import CoastKit

// TIDE 节点必须能一字不差地穿过订阅解析 + full.yaml 生成。
//
// ★ 这一组存在的理由是那个 **1622 字符的公钥**：X25519(32) + ML-KEM-768(1184) = 1216 字节，
// base64（RawURL 无填充）之后就是 1622 个字符。它必须完整、单行、不被折行也不被转义——
// 折了行或少一个字符，核心拒绝的不是这一个节点，而是**整份配置**，
// 于是代理根本起不来，而 YAML 全是手搓字符串拼的，编译期一点提示都没有。
//
// Qt 那边早就有对应的自测（COAST_TIDE_SELFTEST，8 条断言）；Swift 这边一条都没有，
// 而两条产品线用的是同一个核心、同一份配置格式。

private let tideBaseYAML = """
mixed-port: 7890
allow-lan: false
proxies: null
proxy-groups:
  - name: '🚀 节点选择'
    type: select
    proxies:
      - DIRECT
rules:
  - 'MATCH,🚀 节点选择'
"""

/// 真实长度的 TIDE 公钥。**必须用真实长度**——这组测试的全部意义就在长标量上，
/// 拿一个短占位串来测等于什么都没测。
private let tidePublicKey: String = {
    let alphabet = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
    return String((0..<1622).map { alphabet[($0 * 7 + 13) % 64] })
}()

private var tideSubscribeYAML: String {
    """
    - name: 'tide-sub'
      url: ''
      type: 'clash'
      use: true
      list:
        - name: '自建-TIDE'
          type: tide
          server: 'tide.example.com'
          port: 8443
          password: 'sup3r-s3cret'
          public-key: '\(tidePublicKey)'
          sni: 'www.example.com'
          udp: true
          quic: true
          redundancy: true
          use: true
    """
}

/// 独立的临时 configDir（ConfigBuilderTests 里那个 TempBuilder 是 file-private，用不了）。
private final class TideTempBuilder {
    let directory: URL
    let builder: ConfigBuilder

    init(files: [String: String]) {
        directory = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-tide-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for (name, content) in files {
            try? content.write(to: directory.appendingPathComponent(name),
                               atomically: true, encoding: .utf8)
        }
        builder = ConfigBuilder(config: AppConfig(), directory: directory)
    }

    deinit { try? FileManager.default.removeItem(at: directory) }
}

@Suite("TIDE 节点生成")
struct TideNodeTests {

    private func buildFullYAML() -> String {
        let t = TideTempBuilder(files: [
            "default.yaml": tideBaseYAML,
            "subscribe.yaml": tideSubscribeYAML,
        ])
        guard let url = t.builder.ensureFullConfig(tunEnabled: false),
              let text = try? String(contentsOf: url, encoding: .utf8) else { return "" }
        return text
    }

    @Test("type: tide 落进 full.yaml")
    func typeSurvives() {
        #expect(buildFullYAML().contains("type: tide"))
    }

    @Test("1622 字符公钥一字不差")
    func publicKeyIntact() {
        let yaml = buildFullYAML()
        #expect(yaml.contains(tidePublicKey),
                "公钥没有完整穿过来——核心会拒绝整份配置，代理直接起不来")
    }

    @Test("公钥没有被折行")
    func publicKeyNotWrapped() {
        let yaml = buildFullYAML()
        // 折行的表现：前缀出现在某一行末尾。核心对折行的 base64 一律当成语法错。
        #expect(yaml.contains(String(tidePublicKey.prefix(200)) + "\n") == false,
                "公钥被折行了，核心会拒绝整份配置")
    }

    @Test("password / quic / redundancy 保留")
    func optionsSurvive() {
        let yaml = buildFullYAML()
        #expect(yaml.contains("password: 'sup3r-s3cret'") || yaml.contains("password: sup3r-s3cret"))
        #expect(yaml.contains("quic: true"))
        #expect(yaml.contains("redundancy: true"))
    }

    @Test("节点挂进了策略组（否则界面上选不到）")
    func nodeInGroup() {
        let yaml = buildFullYAML()
        guard let groupsAt = yaml.range(of: "proxy-groups:") else {
            Issue.record("产物里没有 proxy-groups")
            return
        }
        let after = yaml[groupsAt.upperBound...]
        #expect(after.contains("自建-TIDE"),
                "节点没出现在任何策略组里——生成是对的，但用户在界面上选不到它")
    }
}

// ★ 订阅**导入**这条路（updateSubscription(at:fromText:)）—— 上面那几条测试是直接
// 写好 subscribe.yaml 再生成，绕过了导入。而用户实际是「添加订阅 → 拉取 → 解析」，
// 失败就发生在这一步：那台 Mac 上 subscribe.yaml 根本不存在，说明订阅从未成功建立。
@Suite("TIDE 订阅导入")
struct TideSubscriptionImportTests {

    /// 用户手里那种 clash 配置：顶层 proxies: 里放一个 tide 节点。
    private var clashConfigWithTide: String {
        """
        mixed-port: 7890
        proxies:
          - name: '自建-TIDE-SG'
            type: tide
            server: 47.0.0.1
            port: 8443
            password: 'pw'
            public-key: '\(tidePublicKey)'
            sni: tide.local
            skip-cert-verify: true
            udp: true
            quic: true
            redundancy: true
        rules:
          - MATCH,DIRECT
        """
    }

    @Test("导入含 tide 的 clash 配置能解析出节点")
    func importClashWithTide() {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-imp-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        let store = SubscriptionStore(config: AppConfig(), directory: dir)
        // addSubscription 要求 url 非空——自建单节点的用户填的是**本地文件路径**
        // （Qt 那边输入框写着"URL 或本地路径"，Swift 侧的取回逻辑也支持本地文件）。
        let localFile = dir.appendingPathComponent("tide.yaml").path
        try? clashConfigWithTide.write(toFile: localFile, atomically: true, encoding: .utf8)
        #expect(store.addSubscription(name: "tide", url: localFile, type: "clash"))

        let result = store.updateSubscription(at: 0, fromText: clashConfigWithTide)
        #expect(result.ok, "导入失败：\(result.message) —— 用户看到的就是这一步不成功")

        let nodes = store.nodes(at: 0)
        #expect(nodes.count == 1, "解析出 \(nodes.count) 个节点，期望 1 个")
    }

    @Test("导入之后公钥仍然一字不差")
    func importKeepsPublicKey() {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-imp2-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        let store = SubscriptionStore(config: AppConfig(), directory: dir)
        let localFile2 = dir.appendingPathComponent("tide.yaml").path
        try? clashConfigWithTide.write(toFile: localFile2, atomically: true, encoding: .utf8)
        _ = store.addSubscription(name: "tide", url: localFile2, type: "clash")
        _ = store.updateSubscription(at: 0, fromText: clashConfigWithTide)

        let text = (try? String(contentsOf: dir.appendingPathComponent("subscribe.yaml"),
                                encoding: .utf8)) ?? ""
        #expect(text.contains(tidePublicKey),
                "导入之后公钥就已经不完整了——后面生成 full.yaml 再对也没用")
    }
}
