import Foundation

/// 本地订阅解析：把订阅原始内容转成 mihomo 的 `proxies:` YAML 块。
///
/// 三种入参形态（对齐 C++ `SubParser::toClashProxies`）：
///   1. 已是 Clash YAML（含 `proxies:`）→ **原样返回**，交给上层照常解析；
///   2. sing-box JSON（Hiddify 等）→ 走 `outbounds` 转换；
///   3. 分享链接列表（明文或整体 base64）→ 逐条解析。
///
/// 单条解析不了就跳过；一条都解析不出时返回 nil。输出刻意用「块式 + 单行 flow 值」
/// （嵌套项如 `ws-opts` 写成一行），因为下游 `ConfigBuilder` 的 `parseProxyList`
/// 是按行做的扁平化管线，多行嵌套它读不了。
public enum SubParser {

    public static func toClashProxies(_ rawContent: String) -> String? {
        // 去掉 UTF-8 BOM：Hiddify 返回的 JSON 带 BOM，会让 hasPrefix("{") 和 JSON 解析双双失败。
        var trimmed = rawContent.trimmingCharacters(in: .whitespacesAndNewlines)
        while trimmed.first == "\u{FEFF}" {
            trimmed = String(trimmed.dropFirst()).trimmingCharacters(in: .whitespacesAndNewlines)
        }
        guard !trimmed.isEmpty else { return nil }

        if trimmed.hasPrefix("{"),
           let data = trimmed.data(using: .utf8),
           let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let body = SingBoxConverter.convert(object), !body.isEmpty {
            return "proxies:\n" + body
        }

        if hasProxiesKey(trimmed) { return rawContent }

        let uriText: String
        if trimmed.contains("://") {
            uriText = trimmed
        } else {
            let decoded = String(data: base64Decode(trimmed) ?? Data(), encoding: .utf8) ?? ""
            if hasProxiesKey(decoded) { return decoded }   // base64 包着的 Clash YAML
            guard decoded.contains("://") else { return nil }
            uriText = decoded
        }

        var body = ""
        for line in uriText.split(whereSeparator: { $0 == "\n" || $0 == "\r" }) {
            if let node = parseOne(String(line)) { body += node }
        }
        return body.isEmpty ? nil : "proxies:\n" + body
    }

    static func hasProxiesKey(_ text: String) -> Bool {
        text.range(of: "(?m)^\\s*proxies\\s*:", options: .regularExpression) != nil
    }

    static func parseOne(_ raw: String) -> String? {
        let uri = raw.trimmingCharacters(in: .whitespaces)
        switch true {
        case uri.hasPrefix("ss://"):        return parseSS(uri)
        case uri.hasPrefix("ssr://"):       return parseSSR(uri)
        case uri.hasPrefix("vmess://"):     return parseVMess(uri)
        case uri.hasPrefix("trojan://"):    return parseTrojan(uri)
        case uri.hasPrefix("vless://"):     return parseVLESS(uri)
        case uri.hasPrefix("hysteria2://"), uri.hasPrefix("hy2://"): return parseHysteria2(uri)
        case uri.hasPrefix("hysteria://"),  uri.hasPrefix("hy://"):  return parseHysteria(uri)
        case uri.hasPrefix("tuic://"):      return parseTuic(uri)
        default: return nil
        }
    }

    // MARK: - ss / ssr

    /// `ss://` 两种形态：SIP002（`base64(method:pass)@host:port?plugin#name`）
    /// 与旧式（`base64(method:pass@host:port)#name`）。
    static func parseSS(_ uri: String) -> String? {
        var rest = String(uri.dropFirst("ss://".count))
        var name = ""
        if let hash = rest.firstIndex(of: "#") {
            name = ProxyURI.percentDecode(String(rest[rest.index(after: hash)...]))
            rest = String(rest[rest.startIndex..<hash])
        }

        var method = "", password = "", host = "", port = "", pluginRaw = ""
        if let at = rest.firstIndex(of: "@") {
            var userinfo = String(rest[rest.startIndex..<at])
            var hostPart = String(rest[rest.index(after: at)...])
            if let mark = hostPart.firstIndex(of: "?") {
                pluginRaw = String(hostPart[hostPart.index(after: mark)...])
                hostPart = String(hostPart[hostPart.startIndex..<mark])
            }
            hostPart = hostPart.replacingOccurrences(of: "/", with: "")
            guard let colon = hostPart.lastIndex(of: ":") else { return nil }
            host = String(hostPart[hostPart.startIndex..<colon])
            port = String(hostPart[hostPart.index(after: colon)...])
            // userinfo 可能是 base64(method:pass)，也可能是明文
            var decoded = String(data: base64Decode(userinfo) ?? Data(), encoding: .utf8) ?? ""
            if !decoded.contains(":") { decoded = ProxyURI.percentDecode(userinfo) }
            guard let c = decoded.firstIndex(of: ":") else { return nil }
            method = String(decoded[decoded.startIndex..<c])
            password = String(decoded[decoded.index(after: c)...])
            userinfo = ""
        } else {
            let decoded = String(data: base64Decode(rest) ?? Data(), encoding: .utf8) ?? ""
            guard let at2 = decoded.lastIndex(of: "@") else { return nil }
            let cred = String(decoded[decoded.startIndex..<at2])
            let hostPart = String(decoded[decoded.index(after: at2)...])
            guard let c = cred.firstIndex(of: ":") else { return nil }
            method = String(cred[cred.startIndex..<c])
            password = String(cred[cred.index(after: c)...])
            guard let colon = hostPart.lastIndex(of: ":") else { return nil }
            host = String(hostPart[hostPart.startIndex..<colon])
            port = String(hostPart[hostPart.index(after: colon)...])
        }
        if name.isEmpty { name = "\(host):\(port)" }

        var fields: [Field] = [
            ("cipher", method),
            ("password", yq(password)),
            ("udp", "true"),
        ]
        if !pluginRaw.isEmpty { fields += ssPluginFields(pluginRaw) }
        return buildProxy(name: name, type: "ss", server: host, port: port, extra: fields)
    }

    /// 插件串形如 `obfs-local;obfs=http;obfs-host=x.com` 或 `v2ray-plugin;mode=websocket;host=…`。
    private static func ssPluginFields(_ raw: String) -> [Field] {
        let parts = ProxyURI.percentDecode(raw).split(separator: ";", omittingEmptySubsequences: true).map(String.init)
        guard let pluginName = parts.first else { return [] }
        var mode = "", pluginHost = ""
        for kv in parts.dropFirst() {
            let key: String, value: String
            if let eq = kv.firstIndex(of: "=") {
                key = String(kv[kv.startIndex..<eq])
                value = String(kv[kv.index(after: eq)...])
            } else {
                key = kv; value = ""
            }
            if key == "obfs" || key == "mode" { mode = value }
            else if key == "obfs-host" || key == "host" { pluginHost = value }
        }
        if pluginName.contains("obfs") {
            var opts = "{mode: " + (mode.isEmpty ? "http" : mode)
            if !pluginHost.isEmpty { opts += ", host: " + yq(pluginHost) }
            opts += "}"
            return [("plugin", "obfs"), ("plugin-opts", opts)]
        }
        if pluginName.contains("v2ray") {
            var opts = "{mode: websocket"
            if !pluginHost.isEmpty { opts += ", host: " + yq(pluginHost) }
            opts += "}"
            return [("plugin", "v2ray-plugin"), ("plugin-opts", opts)]
        }
        return []
    }

    /// `ssr://base64(host:port:proto:method:obfs:base64(pass)/?params)`
    static func parseSSR(_ uri: String) -> String? {
        let decoded = String(data: base64Decode(String(uri.dropFirst("ssr://".count))) ?? Data(), encoding: .utf8) ?? ""
        let main: String, params: String
        if let slash = decoded.range(of: "/?") {
            main = String(decoded[decoded.startIndex..<slash.lowerBound])
            params = String(decoded[slash.upperBound...])
        } else {
            main = decoded; params = ""
        }
        let segments = main.split(separator: ":", omittingEmptySubsequences: false).map(String.init)
        guard segments.count >= 6 else { return nil }
        let n = segments.count
        // host 取前面所有段拼回来：极少数 host 自带冒号（IPv6）。
        let host = segments[0..<(n - 5)].joined(separator: ":")
        let port = segments[n - 5]
        let ssrProtocol = segments[n - 4]
        let method = segments[n - 3]
        let obfs = segments[n - 2]
        let password = String(data: base64Decode(segments[n - 1]) ?? Data(), encoding: .utf8) ?? ""

        let query = ProxyURI.parseQuery(params)
        func decodedParam(_ key: String) -> String {
            guard let raw = query[key], !raw.isEmpty else { return "" }
            return String(data: base64Decode(raw) ?? Data(), encoding: .utf8) ?? ""
        }
        var name = decodedParam("remarks")
        if name.isEmpty { name = "\(host):\(port)" }

        var fields: [Field] = [("cipher", method), ("password", yq(password)), ("protocol", ssrProtocol)]
        let protoParam = decodedParam("protoparam")
        if !protoParam.isEmpty { fields.append(("protocol-param", yq(protoParam))) }
        fields.append(("obfs", obfs))
        let obfsParam = decodedParam("obfsparam")
        if !obfsParam.isEmpty { fields.append(("obfs-param", yq(obfsParam))) }
        fields.append(("udp", "true"))
        return buildProxy(name: name, type: "ssr", server: host, port: port, extra: fields)
    }

    // MARK: - vmess

    /// `vmess://base64(json)`
    static func parseVMess(_ uri: String) -> String? {
        guard let data = base64Decode(String(uri.dropFirst("vmess://".count))),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }

        func str(_ key: String) -> String {
            if let value = object[key] as? String { return value }
            if let number = object[key] as? NSNumber { return String(number.int64Value) }
            return ""
        }
        let server = str("add"), port = str("port"), uuid = str("id")
        guard !server.isEmpty, !uuid.isEmpty else { return nil }
        var name = str("ps")
        if name.isEmpty { name = "\(server):\(port)" }

        let alterId = str("aid")
        let cipher = str("scy").isEmpty ? "auto" : str("scy")
        let network = str("net"), host = str("host"), path = str("path")
        let sni = str("sni"), fingerprint = str("fp")

        var fields: [Field] = [
            ("uuid", yq(uuid)),
            ("alterId", alterId.isEmpty ? "0" : alterId),
            ("cipher", cipher),
            ("udp", "true"),
        ]
        if str("tls") == "tls" {
            fields.append(("tls", "true"))
            let serverName = sni.isEmpty ? host : sni
            if !serverName.isEmpty { fields.append(("servername", yq(serverName))) }
            if !fingerprint.isEmpty { fields.append(("client-fingerprint", fingerprint)) }
        }
        let serviceName = str("serviceName").isEmpty ? path : str("serviceName")
        fields += transportFields(network: network, host: host, path: path, serviceName: serviceName)
        return buildProxy(name: name, type: "vmess", server: server, port: port, extra: fields)
    }

    // MARK: - trojan / vless

    static func parseTrojan(_ uri: String) -> String? {
        guard let u = ProxyURI(uri), let port = u.port else { return nil }
        let name = u.fragment.isEmpty ? "\(u.host):\(port)" : u.fragment

        var fields: [Field] = [("password", yq(u.userInfo)), ("udp", "true")]
        let sni = u.first("sni", "peer")
        if !sni.isEmpty { fields.append(("sni", yq(sni))) }
        if u.flag("allowInsecure", "insecure") { fields.append(("skip-cert-verify", "true")) }
        let alpn = alpnList(u.first("alpn"))
        if !alpn.isEmpty { fields.append(("alpn", alpn)) }
        let fingerprint = u.first("fp")
        if !fingerprint.isEmpty { fields.append(("client-fingerprint", fingerprint)) }
        fields += transportFields(network: u.first("type"), host: u.first("host"),
                                  path: u.first("path"), serviceName: u.first("serviceName"))
        return buildProxy(name: name, type: "trojan", server: u.host, port: String(port), extra: fields)
    }

    static func parseVLESS(_ uri: String) -> String? {
        guard let u = ProxyURI(uri), let port = u.port else { return nil }
        let name = u.fragment.isEmpty ? "\(u.host):\(port)" : u.fragment

        var fields: [Field] = [("uuid", yq(u.userInfo)), ("udp", "true")]
        let flow = u.first("flow")
        if !flow.isEmpty { fields.append(("flow", flow)) }

        let security = u.first("security")
        if security == "tls" || security == "reality" {
            fields.append(("tls", "true"))
            let sni = u.first("sni")
            if !sni.isEmpty { fields.append(("servername", yq(sni))) }
            let fingerprint = u.first("fp")
            if !fingerprint.isEmpty { fields.append(("client-fingerprint", fingerprint)) }
            let publicKey = u.first("pbk")
            if security == "reality", !publicKey.isEmpty {
                var opts = "{public-key: " + yq(publicKey)
                let shortID = u.first("sid")
                if !shortID.isEmpty { opts += ", short-id: " + yq(shortID) }
                opts += "}"
                fields.append(("reality-opts", opts))
            }
        }
        let alpn = alpnList(u.first("alpn"))
        if !alpn.isEmpty { fields.append(("alpn", alpn)) }
        fields += transportFields(network: u.first("type"), host: u.first("host"),
                                  path: u.first("path"), serviceName: u.first("serviceName"))
        return buildProxy(name: name, type: "vless", server: u.host, port: String(port), extra: fields)
    }

    // MARK: - hysteria / hysteria2 / tuic

    static func parseHysteria2(_ uri: String) -> String? {
        guard let u = ProxyURI(uri), let port = u.port else { return nil }
        let name = u.fragment.isEmpty ? "\(u.host):\(port)" : u.fragment
        let password = u.userInfo.isEmpty ? u.first("password") : u.userInfo

        var fields: [Field] = [("password", yq(password))]
        let sni = u.first("sni")
        if !sni.isEmpty { fields.append(("sni", yq(sni))) }
        if u.flag("insecure") { fields.append(("skip-cert-verify", "true")) }
        let obfs = u.first("obfs")
        if !obfs.isEmpty {
            fields.append(("obfs", obfs))
            let obfsPassword = u.first("obfs-password")
            if !obfsPassword.isEmpty { fields.append(("obfs-password", yq(obfsPassword))) }
        }
        let alpn = alpnList(u.first("alpn"))
        if !alpn.isEmpty { fields.append(("alpn", alpn)) }
        return buildProxy(name: name, type: "hysteria2", server: u.host, port: String(port), extra: fields)
    }

    static func parseHysteria(_ uri: String) -> String? {
        guard let u = ProxyURI(uri), let port = u.port else { return nil }
        let name = u.fragment.isEmpty ? "\(u.host):\(port)" : u.fragment

        var fields: [Field] = []
        let auth = u.first("auth")
        if !auth.isEmpty { fields.append(("auth-str", yq(auth))) }
        let hyProtocol = u.first("protocol")
        fields.append(("protocol", hyProtocol.isEmpty ? "udp" : hyProtocol))
        let up = u.first("upmbps", "up")
        if !up.isEmpty { fields.append(("up", yq(up))) }
        let down = u.first("downmbps", "down")
        if !down.isEmpty { fields.append(("down", yq(down))) }
        let peer = u.first("peer", "sni")
        if !peer.isEmpty { fields.append(("sni", yq(peer))) }
        if u.flag("insecure") { fields.append(("skip-cert-verify", "true")) }
        let alpn = alpnList(u.first("alpn"))
        if !alpn.isEmpty { fields.append(("alpn", alpn)) }
        let obfs = u.first("obfs")
        if !obfs.isEmpty { fields.append(("obfs", yq(obfs))) }
        return buildProxy(name: name, type: "hysteria", server: u.host, port: String(port), extra: fields)
    }

    static func parseTuic(_ uri: String) -> String? {
        guard let u = ProxyURI(uri), let port = u.port else { return nil }
        let name = u.fragment.isEmpty ? "\(u.host):\(port)" : u.fragment

        // 有密码 = v5（uuid:password）；只有一段 = v4（token）。
        var fields: [Field] = u.password.isEmpty
            ? [("token", yq(u.user))]
            : [("uuid", yq(u.user)), ("password", yq(u.password))]

        let sni = u.first("sni")
        if !sni.isEmpty { fields.append(("sni", yq(sni))) }
        let congestion = u.first("congestion_control", "congestion-controller")
        if !congestion.isEmpty { fields.append(("congestion-controller", congestion)) }
        let relayMode = u.first("udp_relay_mode", "udp-relay-mode")
        if !relayMode.isEmpty { fields.append(("udp-relay-mode", relayMode)) }
        let alpn = alpnList(u.first("alpn"))
        if !alpn.isEmpty { fields.append(("alpn", alpn)) }
        if u.flag("allow_insecure", "insecure") { fields.append(("skip-cert-verify", "true")) }
        return buildProxy(name: name, type: "tuic", server: u.host, port: String(port), extra: fields)
    }
}

// MARK: - 共用工具

typealias Field = (key: String, value: String)

extension SubParser {

    /// 输出一个块式代理条目。名字/地址缺失或端口非法时返回 nil —— 宁可少一个节点，
    /// 也不能写出一条让核心 **整份配置** 加载失败的记录。
    static func buildProxy(name: String, type: String, server: String, port: String,
                           extra: [Field]) -> String? {
        guard !name.isEmpty, !server.isEmpty, let portNumber = Int(port),
              portNumber > 0, portNumber <= 65535 else { return nil }
        var out = "  - name: \(yq(name))\n"
        out += "    type: \(type)\n"
        out += "    server: \(yq(server))\n"
        out += "    port: \(port)\n"
        for field in extra where !field.value.isEmpty {
            out += "    \(field.key): \(field.value)\n"
        }
        return out
    }

    /// YAML 双引号标量。字符串值一律加引号最安全 —— 节点名里的 `#`、`:`、前导 `*`
    /// 都会让不加引号的写法变成语法错误或被解析成别的类型。
    static func yq(_ input: String) -> String {
        // 剥控制字符(换行等)再引用 —— 同 YAMLSurgery.quote 的注入防线:节点名里的换行
        // 会让引用标量跨行,下游正则提取被骗。节点名无合法换行,折叠成空格。
        let raw = String(input.unicodeScalars.map { $0.properties.generalCategory == .control ? " " : Character($0) })
        var escaped = raw.replacingOccurrences(of: "\\", with: "\\\\")
        escaped = escaped.replacingOccurrences(of: "\"", with: "\\\"")
        escaped = escaped.replacingOccurrences(of: "\n", with: "\\n")
        escaped = escaped.replacingOccurrences(of: "\r", with: "")
        return "\"\(escaped)\""
    }

    /// 传输层（ws/grpc/h2/http）→ 单行 flow 值字段。vmess/vless/trojan 共用。
    static func transportFields(network: String, host: String, path: String, serviceName: String) -> [Field] {
        guard !network.isEmpty, network != "tcp" else { return [] }
        var fields: [Field] = [("network", network)]
        switch network {
        case "ws":
            var opts = "{path: " + yq(path.isEmpty ? "/" : path)
            if !host.isEmpty { opts += ", headers: {Host: " + yq(host) + "}" }
            opts += "}"
            fields.append(("ws-opts", opts))
        case "grpc":
            let service = serviceName.isEmpty ? path : serviceName
            if !service.isEmpty {
                fields.append(("grpc-opts", "{grpc-service-name: " + yq(service) + "}"))
            }
        case "h2", "http":
            var opts = "{path: " + yq(path.isEmpty ? "/" : path)
            if !host.isEmpty { opts += ", host: [" + yq(host) + "]" }
            opts += "}"
            fields.append(("h2-opts", opts))
        default:
            break
        }
        return fields
    }

    /// 逗号分隔的 alpn → flow list：`["h3", "h2"]`
    static func alpnList(_ raw: String) -> String {
        let items = raw.split(separator: ",", omittingEmptySubsequences: true)
            .map { yq($0.trimmingCharacters(in: .whitespaces)) }
        return items.isEmpty ? "" : "[" + items.joined(separator: ", ") + "]"
    }

    /// 容错 base64：兼容 url-safe（`-_`）、缺省填充、以及夹在中间的空白。
    /// 订阅源在这三点上非常不讲究，严格解码会大面积失败。
    static func base64Decode(_ raw: String) -> Data? {
        var text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        text = text.replacingOccurrences(of: "-", with: "+")
        text = text.replacingOccurrences(of: "_", with: "/")
        text = text.filter { !$0.isWhitespace }
        while text.count % 4 != 0 { text += "=" }
        return Data(base64Encoded: text)
    }
}
