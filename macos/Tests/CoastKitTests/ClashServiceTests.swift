import Testing
@testable import CoastKit

// ClashService 里能脱离网络单测的三块：模式归一、节点排序、以及 /proxies 那张图的沿链解析。
// 这三块恰好也是最容易在改动中悄悄跑偏的 —— 排序顺序用户有肌肉记忆，沿链解析错了
// 整类节点会显示「无延迟」。

@Suite("模式归一")
struct ModeNormalizeTests {
    @Test("中英文与核心返回的小写串都要认")
    func normalize() {
        #expect(ClashService.normalizeMode("Rule") == "Rule")
        #expect(ClashService.normalizeMode("rule") == "Rule")
        #expect(ClashService.normalizeMode("规则") == "Rule")
        #expect(ClashService.normalizeMode("global") == "Global")
        #expect(ClashService.normalizeMode("全局") == "Global")
        #expect(ClashService.normalizeMode("全部") == "Global")
        #expect(ClashService.normalizeMode("direct") == "Direct")
        #expect(ClashService.normalizeMode("直连") == "Direct")
    }

    @Test("认不出来的串保持原样，不猜")
    func unknownStaysPut() {
        #expect(ClashService.normalizeMode("script") == "script")
    }
}

@Suite("节点排序")
struct NodeSortTests {
    private func node(_ name: String, delay: Int = 0, speed: Int64 = 0) -> NodeInfo {
        var n = NodeInfo(name: name)
        n.delay = delay
        n.speed = speed
        return n
    }

    @Test("当前节点永远置顶，哪怕又慢又没速度")
    func selectedFirst() {
        let list = [node("fast", delay: 50, speed: 9_000_000), node("current", delay: 900)]
        #expect(ClashService.sorted(list, selected: "current").map(\.name) == ["current", "fast"])
    }

    @Test("有实测速度的排在只有延迟的前面，速度降序")
    func speedBeatsDelay() {
        // 真实测速值是「字节/秒」，量级在几十万到几千万。
        let list = [node("slowdl", speed: 400_000), node("lowping", delay: 10), node("fastdl", speed: 9_000_000)]
        #expect(ClashService.sorted(list, selected: "").map(\.name) == ["fastdl", "slowdl", "lowping"])
    }

    @Test("权重公式的量纲怪癖：速度低于 ~10 KB/s 时会输给低延迟节点")
    func speedAndDelayShareOneScale() {
        // C++ 版的权重是 `speed > 0 ? speed : 10000 - delay` —— 字节/秒和毫秒挤在同一根标尺上，
        // 于是 1 KB/s 的实测速度（key=1000）排在 10ms 延迟（key=9990）之后。实测速度低到这个
        // 量级的节点本来就等于不可用，排在后面反而合理，所以**照搬不改**：换公式会让老用户的
        // 列表顺序无缘无故变样。这条用例把这个怪癖钉住，免得后来者当 bug「顺手修掉」。
        let list = [node("crawling", speed: 1000), node("lowping", delay: 10)]
        #expect(ClashService.sorted(list, selected: "").map(\.name) == ["lowping", "crawling"])
    }

    @Test("都只有延迟时按延迟升序，超时/无延迟垫底")
    func delayAscendingTimeoutsLast() {
        let list = [node("timeout"), node("far", delay: 300), node("near", delay: 30)]
        #expect(ClashService.sorted(list, selected: "").map(\.name) == ["near", "far", "timeout"])
    }

    @Test("同权重保持原始顺序，列表不抖")
    func stableForTies() {
        let list = [node("a"), node("b"), node("c")]   // 全是 0 延迟 0 速度
        #expect(ClashService.sorted(list, selected: "").map(\.name) == ["a", "b", "c"])
    }
}

@Suite("ProxyTree 沿 now 链解析")
struct ProxyTreeTests {
    // 节点选择 → 自动选择 → HK-6，延迟只挂在最末端的 HK-6 上
    private let tree = ProxyTree(proxies: [
        "节点选择": ["type": "Selector", "now": "自动选择", "all": ["自动选择", "HK-6"]],
        "自动选择": ["type": "URLTest", "now": "HK-6", "all": ["HK-6"]],
        "HK-6": ["type": "Trojan", "history": [["delay": 120, "speed": 3000]]],
        "无延迟节点": ["type": "Trojan", "history": [] as [[String: Any]]],
    ])

    @Test("组沿链拿到叶子的 history")
    func historyFollowsChain() {
        let history = tree.history(from: "节点选择")
        #expect((history.last?["delay"] as? Int) == 120)
    }

    @Test("叶子名一路走到底")
    func finalNameFollowsChain() {
        #expect(tree.finalName(from: "节点选择") == "HK-6")
        #expect(tree.finalName(from: "HK-6") == "HK-6")   // 叶子自身没有 now
    }

    @Test("没有 history 时返回空，不报错")
    func missingHistory() {
        #expect(tree.history(from: "无延迟节点").isEmpty)
        #expect(tree.history(from: "根本不存在").isEmpty)
    }

    @Test("配置写成环也不会卡死")
    func cycleTerminates() {
        let cyclic = ProxyTree(proxies: [
            "A": ["now": "B"],
            "B": ["now": "A"],
        ])
        #expect(cyclic.history(from: "A").isEmpty)
        // 成环时走到访问过的节点就停，返回当前那个，不死循环
        #expect(["A", "B"].contains(cyclic.finalName(from: "A")))
    }
}

@Suite("ProxyTree:畸形代理图健壮性")
struct ProxyTreeRobustnessTests {

    @Test("now 指向自己(自环) —— 不死循环")
    func selfLoop() {
        let tree = ProxyTree(proxies: ["A": ["now": "A", "history": [] as [[String: Any]]]])
        #expect(tree.finalName(from: "A") == "A")      // 走到访问过的就停
        #expect(tree.history(from: "A").isEmpty)
    }

    @Test("超长链(>16 步)—— 被步数上限截断,不无限递归")
    func veryLongChain() {
        var proxies: [String: [String: Any]] = [:]
        for i in 0..<100 { proxies["n\(i)"] = ["now": "n\(i+1)"] }  // n0→n1→…→n99→n100(不存在)
        let tree = ProxyTree(proxies: proxies)
        // 不崩、有返回(16 步上限内的某个节点)
        _ = tree.finalName(from: "n0")
        #expect(tree.history(from: "n0").isEmpty)
    }

    @Test("now 是数字/history 是字符串等错类型 —— 当作缺失,不崩")
    func wrongTypes() {
        let tree = ProxyTree(proxies: [
            "A": ["now": 42, "history": "not-an-array"],
            "B": ["now": ["nested": "array"], "history": ["也不是对象数组"]],
        ])
        #expect(tree.finalName(from: "A") == "A")   // now 非字符串 → 视为叶子
        #expect(tree.history(from: "A").isEmpty)
        #expect(tree.history(from: "B").isEmpty)
    }

    @Test("互指成环 A→B→A —— 不死循环")
    func mutualCycle() {
        let tree = ProxyTree(proxies: ["A": ["now": "B"], "B": ["now": "A"]])
        #expect(["A", "B"].contains(tree.finalName(from: "A")))
        #expect(tree.history(from: "A").isEmpty)
    }

    @Test("排序对含 NaN/极值 delay 不崩")
    func sortExtremes() {
        var a = NodeInfo(name: "a"); a.delay = Int.max
        var b = NodeInfo(name: "b"); b.delay = -999
        var c = NodeInfo(name: "c"); c.speed = Int64.max
        let sorted = ClashService.sorted([a, b, c], selected: "")
        #expect(sorted.count == 3)   // 不崩、不丢
    }
}
