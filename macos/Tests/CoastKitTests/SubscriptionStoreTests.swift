import Foundation
import Testing
@testable import CoastKit

/// 每个用例一个临时目录 —— `SubscriptionStore` 每个写操作都真的落盘，
/// 不隔离的话跑一次单测就把开发者自己的订阅覆盖了。
private final class TempStore {
    let directory: URL
    let store: SubscriptionStore

    init(config: AppConfig = AppConfig(), seed: String? = nil) {
        directory = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-subs-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        if let seed {
            try? seed.write(to: directory.appendingPathComponent("subscribe.yaml"),
                            atomically: true, encoding: .utf8)
        }
        store = SubscriptionStore(config: config, directory: directory)
    }

    var text: String {
        (try? String(contentsOf: directory.appendingPathComponent("subscribe.yaml"), encoding: .utf8)) ?? ""
    }

    deinit { try? FileManager.default.removeItem(at: directory) }
}

private let twoSubs = """
- name: 'A 机场'
  url: 'https://a.example.com/sub'
  type: 'sub'
  use: true
  list:
    - name: 'HK-1'
      type: trojan
      server: '1.1.1.1'
      port: 443
      use: true
    - name: 'JP-1'
      type: trojan
      server: '2.2.2.2'
      port: 443
      use: false
- name: 'B 机场'
  url: 'https://b.example.com/sub'
  type: 'sub'
  use: false
  list:
    - name: 'US-1'
      type: ss
      server: '3.3.3.3'
      port: 8388
      use: true
"""

@Suite("subscribe.yaml 读")
struct SubscriptionLoadTests {

    @Test("两条订阅的字段与节点计数")
    func loadsSummaries() {
        let temp = TempStore(seed: twoSubs)
        let subs = temp.store.load()
        #expect(subs.count == 2)
        #expect(subs[0].name == "A 机场")
        #expect(subs[0].url == "https://a.example.com/sub")
        #expect(subs[0].use == true)
        #expect(subs[0].nodeCount == 2)
        #expect(subs[0].enabledNodeCount == 1)   // JP-1 是 false
        #expect(subs[1].use == false)
        #expect(subs[1].nodeCount == 1)
    }

    @Test("节点明细")
    func loadsNodes() {
        let temp = TempStore(seed: twoSubs)
        let nodes = temp.store.nodes(at: 0)
        #expect(nodes.count == 2)
        #expect(nodes[0].name == "HK-1")
        #expect(nodes[0].server == "1.1.1.1")
        #expect(nodes[0].port == "443")
        #expect(nodes[1].use == false)
        // 第二条订阅的节点不能串进来
        #expect(temp.store.nodes(at: 1).map(\.name) == ["US-1"])
    }

    @Test("首次运行落地种子，读到空列表而不是崩")
    func seedsOnFirstRun() {
        let temp = TempStore()
        #expect(temp.store.load().isEmpty)
    }
}

@Suite("subscribe.yaml 写")
struct SubscriptionWriteTests {

    @Test("启停订阅只动那一行")
    func toggleSubscription() {
        let temp = TempStore(seed: twoSubs)
        #expect(temp.store.setSubscriptionEnabled(at: 1, true))
        #expect(temp.store.load()[1].use == true)
        #expect(temp.store.load()[0].use == true)   // 没串到别的订阅
    }

    @Test("启停单个节点")
    func toggleNode() {
        let temp = TempStore(seed: twoSubs)
        #expect(temp.store.setNodeEnabled(subscription: 0, node: 0, false))
        #expect(temp.store.nodes(at: 0)[0].use == false)
        #expect(temp.store.nodes(at: 0)[1].use == false)
        #expect(temp.store.nodes(at: 1)[0].use == true)
    }

    @Test("批量启停：缺 use 键的节点要补上，且不能因为插行错位")
    func toggleAllInsertsMissingKeys() {
        // 三个节点都没有 use 键 —— 从前往后改会让后面的区间整体位移
        let seed = """
        - name: 'S'
          url: 'u'
          list:
            - name: 'n1'
              server: '1.1.1.1'
              port: 1
            - name: 'n2'
              server: '2.2.2.2'
              port: 2
            - name: 'n3'
              server: '3.3.3.3'
              port: 3
        """
        let temp = TempStore(seed: seed)
        #expect(temp.store.setAllNodesEnabled(subscription: 0, false))
        let nodes = temp.store.nodes(at: 0)
        #expect(nodes.count == 3)
        #expect(nodes.allSatisfy { $0.use == false })
        #expect(nodes.map(\.name) == ["n1", "n2", "n3"])   // 名字没错位
    }

    @Test("新增订阅：种子的字面量 [] 要被清掉")
    func addClearsEmptyListLiteral() {
        let temp = TempStore(seed: "[]\n")
        #expect(temp.store.addSubscription(name: "新机场", url: "https://x.example.com", type: ""))
        // 留着 [] 会得到既是 flow 又是 block 的写法，YAML 解析器直接报错
        #expect(temp.text.contains("[]") == false)
        let subs = temp.store.load()
        #expect(subs.count == 1)
        #expect(subs[0].name == "新机场")
        #expect(subs[0].type == "sub")     // 空 type 记为 sub
        #expect(subs[0].use == true)
    }

    @Test("名字为空时用 URL 顶上，不留无名条目")
    func addFallsBackToURL() {
        let temp = TempStore(seed: "[]\n")
        #expect(temp.store.addSubscription(name: "", url: "https://y.example.com", type: "sub"))
        #expect(temp.store.load()[0].name == "https://y.example.com")
    }

    @Test("编辑订阅")
    func edit() {
        let temp = TempStore(seed: twoSubs)
        #expect(temp.store.editSubscription(at: 0, name: "改名了", url: "https://new.example.com", type: "sub"))
        let subs = temp.store.load()
        #expect(subs[0].name == "改名了")
        #expect(subs[0].url == "https://new.example.com")
        #expect(subs[0].nodeCount == 2)     // 节点没被动
        #expect(subs[1].name == "B 机场")
    }

    @Test("删除订阅；删光后写回 []，不能留空文件")
    func remove() {
        let temp = TempStore(seed: twoSubs)
        #expect(temp.store.removeSubscription(at: 0))
        #expect(temp.store.load().map(\.name) == ["B 机场"])
        #expect(temp.store.removeSubscription(at: 0))
        #expect(temp.store.load().isEmpty)
        // 空文件不是合法 YAML，核心和我们自己都读不了
        #expect(temp.text.trimmingCharacters(in: .whitespacesAndNewlines) == "[]")
    }

    @Test("带引号的名字里有单引号时能原样往返")
    func quotingRoundTrip() {
        let temp = TempStore(seed: "[]\n")
        #expect(temp.store.addSubscription(name: "it's mine", url: "https://z.example.com", type: "sub"))
        #expect(temp.store.load()[0].name == "it's mine")
    }
}

@Suite("订阅更新")
struct SubscriptionUpdateTests {

    private let newProxies = """
    proxies:
      - name: "HK-1 改名了"
        type: trojan
        server: 1.1.1.1
        port: 443
        password: pw
      - name: "SG-9"
        type: trojan
        server: 9.9.9.9
        port: 443
        password: pw
    """

    @Test("按 server:port 保留用户的启停，不按节点名 —— 机场经常改名")
    func preservesUseByEndpoint() {
        let temp = TempStore(seed: twoSubs)
        // 原 JP-1(2.2.2.2) 是 false，本次订阅里它消失了；HK-1(1.1.1.1) 改了名但地址没变
        let result = temp.store.updateSubscription(at: 0, fromText: newProxies)
        #expect(result.ok)
        #expect(result.changed)
        let nodes = temp.store.nodes(at: 0)
        #expect(nodes.map(\.name) == ["HK-1 改名了", "SG-9"])
        #expect(nodes[0].use == true)      // 1.1.1.1:443 原本是 true，改名后仍保留
        #expect(nodes[1].use == true)      // 新节点默认启用
    }

    @Test("更新中间那条订阅，不能把后面那条吃掉")
    func updatingMiddleKeepsFollowingSubscription() {
        // 替换的是 listLine..<block.upperBound，而中间块的 upperBound 正是下一块的起始行 ——
        // 这个边界错一格就会把后一条订阅整条吞掉，且现象要等用户发现「订阅少了一个」才暴露。
        let temp = TempStore(seed: twoSubs)
        _ = temp.store.updateSubscription(at: 0, fromText: newProxies)
        let subs = temp.store.load()
        #expect(subs.count == 2)
        #expect(subs[1].name == "B 机场")
        #expect(subs[1].url == "https://b.example.com/sub")
        #expect(subs[1].use == false)
        #expect(temp.store.nodes(at: 1).map(\.name) == ["US-1"])
    }

    @Test("禁用状态跨改名保留")
    func preservesDisabledAcrossRename() {
        let temp = TempStore(seed: twoSubs)
        #expect(temp.store.setNodeEnabled(subscription: 0, node: 0, false))
        _ = temp.store.updateSubscription(at: 0, fromText: newProxies)
        let nodes = temp.store.nodes(at: 0)
        #expect(nodes[0].name == "HK-1 改名了")
        #expect(nodes[0].use == false)     // 用户禁用过，改名不该让它复活
    }

    @Test("内容逐字节一致时 changed=false，好让调用方跳过热重载")
    func noChangeReportsUnchanged() {
        let temp = TempStore(seed: twoSubs)
        _ = temp.store.updateSubscription(at: 0, fromText: newProxies)
        let second = temp.store.updateSubscription(at: 0, fromText: newProxies)
        #expect(second.ok)
        #expect(second.changed == false)
    }

    @Test("increment 开启时保留本次没出现的老节点")
    func incrementKeepsMissingNodes() {
        var config = AppConfig()
        config.increment = true
        let temp = TempStore(config: config, seed: twoSubs)
        _ = temp.store.updateSubscription(at: 0, fromText: newProxies)
        let names = temp.store.nodes(at: 0).map(\.name)
        #expect(names.contains("JP-1"))    // 本次订阅里没有它，但增量模式留着
        #expect(names.contains("SG-9"))
    }

    @Test("allow 规则过滤节点名")
    func allowRuleFilters() {
        var config = AppConfig()
        config.allowRuleEnabled = true
        config.allowRule = "HK"
        let temp = TempStore(config: config, seed: twoSubs)
        _ = temp.store.updateSubscription(at: 0, fromText: newProxies)
        #expect(temp.store.nodes(at: 0).map(\.name) == ["HK-1 改名了"])
    }

    @Test("noallow 规则排除节点名")
    func noAllowRuleExcludes() {
        var config = AppConfig()
        config.noAllowRuleEnabled = true
        config.noAllowRule = "SG"
        let temp = TempStore(config: config, seed: twoSubs)
        _ = temp.store.updateSubscription(at: 0, fromText: newProxies)
        #expect(temp.store.nodes(at: 0).map(\.name) == ["HK-1 改名了"])
    }

    @Test("正则写错时当作没设，不能让所有节点消失")
    func invalidRegexIsIgnored() {
        var config = AppConfig()
        config.allowRuleEnabled = true
        config.allowRule = "([unclosed"      // 非法正则
        let temp = TempStore(config: config, seed: twoSubs)
        let result = temp.store.updateSubscription(at: 0, fromText: newProxies)
        #expect(result.ok)
        #expect(temp.store.nodes(at: 0).count == 2)
    }

    @Test("源里的运行时字段 use/delay/speed 不该被抄进来")
    func stripsRuntimeFields() {
        let temp = TempStore(seed: twoSubs)
        let withRuntime = """
        proxies:
          - name: "N1"
            type: trojan
            server: 5.5.5.5
            port: 443
            delay: 120
            speed: 999
            use: false
        """
        _ = temp.store.updateSubscription(at: 0, fromText: withRuntime)
        #expect(temp.text.contains("delay:") == false)
        #expect(temp.text.contains("speed:") == false)
        // use 由我们自己按 endpoint 决定，新节点默认 true —— 不是抄源里的 false
        #expect(temp.store.nodes(at: 0)[0].use == true)
    }

    @Test("一个节点都解析不出时报失败，且不动原有列表")
    func emptyResultKeepsOldList() {
        let temp = TempStore(seed: twoSubs)
        let result = temp.store.updateSubscription(at: 0, fromText: "这不是订阅内容")
        #expect(result.ok == false)
        #expect(temp.store.nodes(at: 0).count == 2)   // 原列表原封不动
    }

    @Test("分享链接订阅也能更新（走 SubParser）")
    func acceptsShareLinks() {
        let temp = TempStore(seed: twoSubs)
        let links = "trojan://pw@t.example.com:443#TR1\nvless://uuid-1@v.example.com:443?security=tls#VL1"
        let result = temp.store.updateSubscription(at: 0, fromText: links)
        #expect(result.ok)
        #expect(temp.store.nodes(at: 0).map(\.name) == ["TR1", "VL1"])
    }
}
