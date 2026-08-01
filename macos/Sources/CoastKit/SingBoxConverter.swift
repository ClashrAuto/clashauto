import Foundation

/// sing-box 配置（Hiddify 等客户端的订阅格式）→ mihomo `proxies:` 块。
///
/// 只转换能对上的出站类型；`selector` / `urltest` / `direct` / `block` / `dns`
/// 以及不认识的类型一律跳过 —— 策略组由 `ConfigBuilder` 自己生成，订阅里的组结构不采纳。
enum SingBoxConverter {

    static func convert(_ root: [String: Any]) -> String? {
        guard let outbounds = root["outbounds"] as? [[String: Any]], !outbounds.isEmpty else { return nil }
        var body = ""
        for outbound in outbounds {
            if let node = convertOutbound(outbound) { body += node }
        }
        return body.isEmpty ? nil : body
    }

    static func convertOutbound(_ o: [String: Any]) -> String? {
        let type = o["type"] as? String ?? ""
        let name = o["tag"] as? String ?? ""
        let server = o["server"] as? String ?? ""
        let portNumber = (o["server_port"] as? NSNumber)?.intValue ?? 0
        guard !name.isEmpty, !server.isEmpty, portNumber > 0 else { return nil }
        let port = String(portNumber)

        let tls = o["tls"] as? [String: Any] ?? [:]
        let tlsOn = (tls["enabled"] as? Bool) ?? false
        let transport = o["transport"] as? [String: Any] ?? [:]

        func str(_ key: String) -> String { o[key] as? String ?? "" }
        func int(_ key: String) -> Int { (o[key] as? NSNumber)?.intValue ?? 0 }

        switch type {
        case "shadowsocks":
            return SubParser.buildProxy(name: name, type: "ss", server: server, port: port, extra: [
                ("cipher", str("method")),
                ("password", SubParser.yq(str("password"))),
                ("udp", "true"),
            ])

        case "vmess":
            var fields: [Field] = [
                ("uuid", SubParser.yq(str("uuid"))),
                ("alterId", String(int("alter_id"))),
                ("cipher", str("security").isEmpty ? "auto" : str("security")),
                ("udp", "true"),
            ]
            if tlsOn { fields += tlsFields(tls) }
            fields += transportFields(transport)
            return SubParser.buildProxy(name: name, type: "vmess", server: server, port: port, extra: fields)

        case "vless":
            var fields: [Field] = [("uuid", SubParser.yq(str("uuid"))), ("udp", "true")]
            if !str("flow").isEmpty { fields.append(("flow", str("flow"))) }
            if !str("packet_encoding").isEmpty { fields.append(("packet-encoding", str("packet_encoding"))) }
            if tlsOn { fields += tlsFields(tls) }
            fields += transportFields(transport)
            return SubParser.buildProxy(name: name, type: "vless", server: server, port: port, extra: fields)

        case "trojan":
            var fields: [Field] = [("password", SubParser.yq(str("password"))), ("udp", "true")]
            if tlsOn { fields += tlsFields(tls) }
            fields += transportFields(transport)
            return SubParser.buildProxy(name: name, type: "trojan", server: server, port: port, extra: fields)

        case "hysteria2":
            var fields: [Field] = [("password", SubParser.yq(str("password")))]
            let obfs = o["obfs"] as? [String: Any] ?? [:]
            if let obfsType = obfs["type"] as? String, !obfsType.isEmpty {
                fields.append(("obfs", obfsType))
                if let obfsPassword = obfs["password"] as? String, !obfsPassword.isEmpty {
                    fields.append(("obfs-password", SubParser.yq(obfsPassword)))
                }
            }
            if int("up_mbps") > 0 { fields.append(("up", SubParser.yq(String(int("up_mbps"))))) }
            if int("down_mbps") > 0 { fields.append(("down", SubParser.yq(String(int("down_mbps"))))) }
            fields += bareTLSFields(tls)
            return SubParser.buildProxy(name: name, type: "hysteria2", server: server, port: port, extra: fields)

        case "hysteria":
            var fields: [Field] = [("auth-str", SubParser.yq(str("auth_str")))]
            if int("up_mbps") > 0 { fields.append(("up", SubParser.yq(String(int("up_mbps"))))) }
            if int("down_mbps") > 0 { fields.append(("down", SubParser.yq(String(int("down_mbps"))))) }
            if !str("obfs").isEmpty { fields.append(("obfs", SubParser.yq(str("obfs")))) }
            fields.append(("protocol", "udp"))
            fields += bareTLSFields(tls)
            return SubParser.buildProxy(name: name, type: "hysteria", server: server, port: port, extra: fields)

        case "tuic":
            var fields: [Field] = [("uuid", SubParser.yq(str("uuid")))]
            if !str("password").isEmpty { fields.append(("password", SubParser.yq(str("password")))) }
            if !str("congestion_control").isEmpty {
                fields.append(("congestion-controller", str("congestion_control")))
            }
            if !str("udp_relay_mode").isEmpty { fields.append(("udp-relay-mode", str("udp_relay_mode"))) }
            if (o["zero_rtt_handshake"] as? Bool) == true { fields.append(("reduce-rtt", "true")) }
            fields += bareTLSFields(tls)
            return SubParser.buildProxy(name: name, type: "tuic", server: server, port: port, extra: fields)

        default:
            return nil
        }
    }

    // MARK: - 片段

    /// 有独立 `tls:` 开关的协议（vmess/vless/trojan）用这一套。
    static func tlsFields(_ tls: [String: Any]) -> [Field] {
        var fields: [Field] = [("tls", "true")]
        if let serverName = tls["server_name"] as? String, !serverName.isEmpty {
            fields.append(("servername", SubParser.yq(serverName)))
        }
        if (tls["insecure"] as? Bool) == true { fields.append(("skip-cert-verify", "true")) }
        let alpn = alpnList(tls["alpn"] as? [String] ?? [])
        if !alpn.isEmpty { fields.append(("alpn", alpn)) }
        if let fingerprint = (tls["utls"] as? [String: Any])?["fingerprint"] as? String, !fingerprint.isEmpty {
            fields.append(("client-fingerprint", fingerprint))
        }
        if let reality = tls["reality"] as? [String: Any], (reality["enabled"] as? Bool) == true,
           let publicKey = reality["public_key"] as? String, !publicKey.isEmpty {
            var opts = "{public-key: " + SubParser.yq(publicKey)
            if let shortID = reality["short_id"] as? String, !shortID.isEmpty {
                opts += ", short-id: " + SubParser.yq(shortID)
            }
            opts += "}"
            fields.append(("reality-opts", opts))
        }
        return fields
    }

    /// hy2/hysteria/tuic **恒 TLS**，没有独立的 `tls:` 字段，只取 sni/skip-cert-verify/alpn。
    static func bareTLSFields(_ tls: [String: Any]) -> [Field] {
        guard !tls.isEmpty else { return [] }
        var fields: [Field] = []
        if let serverName = tls["server_name"] as? String, !serverName.isEmpty {
            fields.append(("sni", SubParser.yq(serverName)))
        }
        if (tls["insecure"] as? Bool) == true { fields.append(("skip-cert-verify", "true")) }
        let alpn = alpnList(tls["alpn"] as? [String] ?? [])
        if !alpn.isEmpty { fields.append(("alpn", alpn)) }
        return fields
    }

    static func transportFields(_ transport: [String: Any]) -> [Field] {
        let type = transport["type"] as? String ?? ""
        guard !type.isEmpty, type != "tcp" else { return [] }
        let path = transport["path"] as? String ?? ""
        let host = headerHost(transport["headers"] as? [String: Any] ?? [:])

        switch type {
        // httpupgrade 在 mihomo 侧没有对应类型，按 ws 处理 —— 两者的握手对服务端等价。
        case "ws", "httpupgrade":
            var opts = "{path: " + SubParser.yq(path.isEmpty ? "/" : path)
            if !host.isEmpty { opts += ", headers: {Host: " + SubParser.yq(host) + "}" }
            opts += "}"
            return [("network", "ws"), ("ws-opts", opts)]
        case "grpc":
            let service = transport["service_name"] as? String ?? ""
            guard !service.isEmpty else { return [("network", "grpc")] }
            return [("network", "grpc"),
                    ("grpc-opts", "{grpc-service-name: " + SubParser.yq(service) + "}")]
        case "http":
            // sing-box 的 http transport 就是 HTTP/2，对应 mihomo 的 h2。
            var opts = "{path: " + SubParser.yq(path.isEmpty ? "/" : path)
            if !host.isEmpty { opts += ", host: [" + SubParser.yq(host) + "]" }
            opts += "}"
            return [("network", "h2"), ("h2-opts", opts)]
        default:
            return []
        }
    }

    /// `headers.Host` 可能是字符串，也可能是字符串数组（sing-box 两种都写得出来）。
    static func headerHost(_ headers: [String: Any]) -> String {
        let raw = headers["Host"] ?? headers["host"]
        if let value = raw as? String { return value }
        if let list = raw as? [String] { return list.first ?? "" }
        return ""
    }

    static func alpnList(_ items: [String]) -> String {
        guard !items.isEmpty else { return "" }
        return "[" + items.map { SubParser.yq($0) }.joined(separator: ", ") + "]"
    }
}
