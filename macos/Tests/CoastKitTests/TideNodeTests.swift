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
