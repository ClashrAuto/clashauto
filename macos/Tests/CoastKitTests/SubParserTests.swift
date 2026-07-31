import Testing
@testable import CoastKit

// 订阅解析是整条链路上最脏的一环：链接格式各家不一、编码不讲究、字段别名一堆。
// 解析错了的表现是「节点少了几个」或「核心加载整份配置失败」，都很难从现象倒推到这里，
// 所以每种协议至少钉一条。

@Suite("分享链接 → proxies")
struct SubParserURITests {

    @Test("ss SIP002：base64 的 userinfo + 中文备注")
    func ssSIP002() {
        let uri = "ss://YWVzLTI1Ni1nY206cGFzc3dvcmQ=@1.2.3.4:8388#香港01"
        let out = SubParser.parseSS(uri)
        #expect(out?.contains("name: \"香港01\"") == true)
        #expect(out?.contains("type: ss") == true)
        #expect(out?.contains("server: \"1.2.3.4\"") == true)
        #expect(out?.contains("port: 8388") == true)
        #expect(out?.contains("cipher: aes-256-gcm") == true)
        #expect(out?.contains("password: \"password\"") == true)
    }

    @Test("ss 旧式：整体 base64，无备注时用 host:port 兜底")
    func ssLegacy() {
        let out = SubParser.parseSS("ss://YWVzLTEyOC1nY206cHdAMS4yLjMuNDo4Mzg4")
        #expect(out?.contains("name: \"1.2.3.4:8388\"") == true)
        #expect(out?.contains("cipher: aes-128-gcm") == true)
    }

    @Test("ss 插件：obfs 与 v2ray-plugin 各转成对应的 plugin-opts")
    func ssPlugin() {
        let obfs = SubParser.parseSS("ss://YWVzLTI1Ni1nY206cGFzc3dvcmQ=@1.2.3.4:8388?plugin=obfs-local%3Bobfs%3Dhttp%3Bobfs-host%3Dbing.com#N")
        #expect(obfs?.contains("plugin: obfs") == true)
        #expect(obfs?.contains("plugin-opts: {mode: http, host: \"bing.com\"}") == true)

        let v2ray = SubParser.parseSS("ss://YWVzLTI1Ni1nY206cGFzc3dvcmQ=@1.2.3.4:8388?plugin=v2ray-plugin%3Bmode%3Dwebsocket%3Bhost%3Dcdn.com#N")
        #expect(v2ray?.contains("plugin: v2ray-plugin") == true)
        #expect(v2ray?.contains("host: \"cdn.com\"") == true)
    }

    @Test("ssr：各段都是 base64，remarks/obfsparam 要单独解")
    func ssr() {
        let uri = "ssr://MS4yLjMuNDo4Mzg4Om9yaWdpbjphZXMtMjU2LWNmYjpwbGFpbjpjMlZqY21WMC8_cmVtYXJrcz01cldMNkstVjZJcUM1NEs1Jm9iZnNwYXJhbT1ieTVqYjIw"
        let out = SubParser.parseSSR(uri)
        #expect(out?.contains("type: ssr") == true)
        #expect(out?.contains("name: \"测试节点\"") == true)
        #expect(out?.contains("password: \"secret\"") == true)
        #expect(out?.contains("protocol: origin") == true)
        #expect(out?.contains("cipher: aes-256-cfb") == true)
        #expect(out?.contains("obfs-param: \"o.com\"") == true)
    }

    @Test("vmess：base64(json)，ws + tls")
    func vmess() {
        let uri = "vmess://eyJ2IjogIjIiLCAicHMiOiAi5Lic5LqsIDAxIiwgImFkZCI6ICJhLmV4YW1wbGUuY29tIiwgInBvcnQiOiAiNDQzIiwgImlkIjogImI4MzEzODFkLTYzMjQtNGQ1My1hZDRmLThjZGE0OGIzMDgxMSIsICJhaWQiOiAiMCIsICJzY3kiOiAiYXV0byIsICJuZXQiOiAid3MiLCAiaG9zdCI6ICJjZG4uZXhhbXBsZS5jb20iLCAicGF0aCI6ICIvcmF5IiwgInRscyI6ICJ0bHMiLCAic25pIjogImNkbi5leGFtcGxlLmNvbSJ9"
        let out = SubParser.parseVMess(uri)
        #expect(out?.contains("name: \"东京 01\"") == true)
        #expect(out?.contains("uuid: \"b831381d-6324-4d53-ad4f-8cda48b30811\"") == true)
        #expect(out?.contains("alterId: 0") == true)
        #expect(out?.contains("tls: true") == true)
        #expect(out?.contains("servername: \"cdn.example.com\"") == true)
        #expect(out?.contains("network: ws") == true)
        #expect(out?.contains("ws-opts: {path: \"/ray\", headers: {Host: \"cdn.example.com\"}}") == true)
    }

    @Test("trojan：sni/peer 互为别名，insecure 转 skip-cert-verify")
    func trojan() {
        let out = SubParser.parseTrojan("trojan://mypass@t.example.com:443?peer=t.example.com&allowInsecure=1&alpn=h2,http/1.1#TR")
        #expect(out?.contains("type: trojan") == true)
        #expect(out?.contains("password: \"mypass\"") == true)
        #expect(out?.contains("sni: \"t.example.com\"") == true)
        #expect(out?.contains("skip-cert-verify: true") == true)
        #expect(out?.contains("alpn: [\"h2\", \"http/1.1\"]") == true)
    }

    @Test("vless reality：pbk/sid 转成 reality-opts")
    func vlessReality() {
        let out = SubParser.parseVLESS("vless://uuid-1@v.example.com:443?security=reality&sni=www.apple.com&fp=chrome&pbk=PUBKEY&sid=ab12&flow=xtls-rprx-vision#VL")
        #expect(out?.contains("uuid: \"uuid-1\"") == true)
        #expect(out?.contains("flow: xtls-rprx-vision") == true)
        #expect(out?.contains("tls: true") == true)
        #expect(out?.contains("servername: \"www.apple.com\"") == true)
        #expect(out?.contains("client-fingerprint: chrome") == true)
        #expect(out?.contains("reality-opts: {public-key: \"PUBKEY\", short-id: \"ab12\"}") == true)
    }

    @Test("hysteria2：密码在 userinfo 里，obfs 带独立口令")
    func hysteria2() {
        let out = SubParser.parseHysteria2("hysteria2://pw123@h.example.com:8443?sni=h.example.com&obfs=salamander&obfs-password=xyz&insecure=1#HY2")
        #expect(out?.contains("type: hysteria2") == true)
        #expect(out?.contains("password: \"pw123\"") == true)
        #expect(out?.contains("obfs: salamander") == true)
        #expect(out?.contains("obfs-password: \"xyz\"") == true)
        #expect(out?.contains("skip-cert-verify: true") == true)
    }

    @Test("tuic：有密码算 v5(uuid+password)，只有一段算 v4(token)")
    func tuicVersions() {
        let v5 = SubParser.parseTuic("tuic://uuid-x:pass-y@tu.example.com:443?congestion_control=bbr&udp_relay_mode=native#T5")
        #expect(v5?.contains("uuid: \"uuid-x\"") == true)
        #expect(v5?.contains("password: \"pass-y\"") == true)
        #expect(v5?.contains("congestion-controller: bbr") == true)

        let v4 = SubParser.parseTuic("tuic://token-only@tu.example.com:443#T4")
        #expect(v4?.contains("token: \"token-only\"") == true)
        #expect(v4?.contains("uuid:") == false)
    }

    @Test("端口非法/缺地址的条目直接丢掉，绝不写出坏配置")
    func rejectsBadEntries() {
        // 端口 0 与超范围：写进 full.yaml 会让核心整份配置加载失败
        #expect(SubParser.parseTrojan("trojan://pw@t.example.com:0#X") == nil)
        #expect(SubParser.parseTrojan("trojan://pw@t.example.com:70000#X") == nil)
        #expect(SubParser.parseOne("不是链接") == nil)
        #expect(SubParser.parseOne("ss://这不是base64也不是sip002") == nil)
    }
}

@Suite("订阅整体入口")
struct SubParserEntryTests {

    @Test("已是 Clash YAML → 原样返回")
    func passthroughYAML() {
        let yaml = "proxies:\n  - name: a\n    type: ss\n"
        #expect(SubParser.toClashProxies(yaml) == yaml)
    }

    @Test("整体 base64 的链接列表 → 逐条解析")
    func base64List() {
        let encoded = "dHJvamFuOi8vcHdAdC5leGFtcGxlLmNvbTo0NDM/c25pPXQuZXhhbXBsZS5jb20jVFIxCnZsZXNzOi8vdXVpZC0xQHYuZXhhbXBsZS5jb206NDQzP3NlY3VyaXR5PXRscyZ0eXBlPXdzJnBhdGg9L3cjVkwx"
        let out = SubParser.toClashProxies(encoded)
        #expect(out?.hasPrefix("proxies:\n") == true)
        #expect(out?.contains("name: \"TR1\"") == true)
        #expect(out?.contains("name: \"VL1\"") == true)
    }

    @Test("明文链接列表，夹杂解析不了的行也不影响其它节点")
    func plainListSkipsBadLines() {
        let text = """
        trojan://pw@t.example.com:443#TR1
        这一行是垃圾
        vless://uuid-1@v.example.com:443?security=tls#VL1
        """
        let out = SubParser.toClashProxies(text)
        #expect(out?.contains("TR1") == true)
        #expect(out?.contains("VL1") == true)
    }

    @Test("BOM 不能挡住 JSON 识别 —— Hiddify 返回的就带 BOM")
    func stripsBOM() {
        let json = """
        {"outbounds":[{"type":"trojan","tag":"SB1","server":"s.example.com","server_port":443,"password":"pw","tls":{"enabled":true,"server_name":"s.example.com"}}]}
        """
        let out = SubParser.toClashProxies("\u{FEFF}" + json)
        #expect(out?.contains("name: \"SB1\"") == true)
        #expect(out?.contains("type: trojan") == true)
        #expect(out?.contains("servername: \"s.example.com\"") == true)
    }

    @Test("什么都认不出来时返回 nil，而不是空的 proxies: 块")
    func unrecognized() {
        #expect(SubParser.toClashProxies("") == nil)
        #expect(SubParser.toClashProxies("just some text") == nil)
    }
}

@Suite("sing-box 出站转换")
struct SingBoxTests {

    @Test("策略组类出站要跳过，只留真节点")
    func skipsGroups() {
        let root: [String: Any] = ["outbounds": [
            ["type": "selector", "tag": "auto", "outbounds": ["a"]],
            ["type": "direct", "tag": "direct"],
            ["type": "shadowsocks", "tag": "SS1", "server": "s.example.com",
             "server_port": 8388, "method": "aes-256-gcm", "password": "pw"],
        ]]
        let out = SingBoxConverter.convert(root)
        #expect(out?.contains("SS1") == true)
        #expect(out?.contains("auto") == false)
        #expect(out?.contains("direct") == false)
    }

    @Test("headers.Host 写成数组时也要取到")
    func headerHostArray() {
        #expect(SingBoxConverter.headerHost(["Host": ["cdn.example.com"]]) == "cdn.example.com")
        #expect(SingBoxConverter.headerHost(["host": "lower.example.com"]) == "lower.example.com")
        #expect(SingBoxConverter.headerHost([:]) == "")
    }

    @Test("httpupgrade 按 ws 处理，http 按 h2 处理")
    func transportMapping() {
        let ws = SingBoxConverter.transportFields(["type": "httpupgrade", "path": "/up"])
        #expect(ws.first?.value == "ws")
        let h2 = SingBoxConverter.transportFields(["type": "http", "path": "/h"])
        #expect(h2.first?.value == "h2")
    }

    @Test("hy2 恒 TLS：没有 tls: 字段，sni 直接给出")
    func bareTLS() {
        let fields = SingBoxConverter.bareTLSFields(["server_name": "h.example.com", "insecure": true])
        #expect(fields.contains { $0.key == "sni" && $0.value == "\"h.example.com\"" })
        #expect(fields.contains { $0.key == "skip-cert-verify" })
        #expect(fields.contains { $0.key == "tls" } == false)
    }
}
