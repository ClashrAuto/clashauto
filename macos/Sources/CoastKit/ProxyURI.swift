import Foundation

/// `scheme://[user[:password]@]host[:port][/path][?query][#fragment]` 的手写解析。
///
/// 为什么不用 `URLComponents`：订阅链接里的 fragment 常常是**未编码的中文节点名**、
/// 密码里带 `/` `?` `#` 也不罕见 —— 这些 `URLComponents(string:)` 直接返回 nil，
/// 于是整条节点被丢掉。Qt 那边用的是 `QUrl::TolerantMode`，这里等价地手写一个宽松版。
struct ProxyURI {
    var scheme = ""
    var user = ""
    var password = ""
    var host = ""
    var port: Int?
    var query: [String: String] = [:]
    var fragment = ""

    /// userinfo 整体（trojan 的密码、vless 的 uuid 都是不带冒号的单段）。
    var userInfo: String { password.isEmpty ? user : "\(user):\(password)" }

    init?(_ raw: String) {
        guard let schemeEnd = raw.range(of: "://") else { return nil }
        scheme = String(raw[raw.startIndex..<schemeEnd.lowerBound]).lowercased()
        var rest = String(raw[schemeEnd.upperBound...])

        // 先切 fragment：节点名里出现 ? & 都不该被当成 query 分隔符。
        if let hash = rest.firstIndex(of: "#") {
            fragment = Self.percentDecode(String(rest[rest.index(after: hash)...]))
            rest = String(rest[rest.startIndex..<hash])
        }
        if let mark = rest.firstIndex(of: "?") {
            query = Self.parseQuery(String(rest[rest.index(after: mark)...]))
            rest = String(rest[rest.startIndex..<mark])
        }
        // userinfo 用**最后一个** @ 切：密码里含 @ 时前面的都归 userinfo。
        if let at = rest.lastIndex(of: "@") {
            let info = String(rest[rest.startIndex..<at])
            rest = String(rest[rest.index(after: at)...])
            if let colon = info.firstIndex(of: ":") {
                user = Self.percentDecode(String(info[info.startIndex..<colon]))
                password = Self.percentDecode(String(info[info.index(after: colon)...]))
            } else {
                user = Self.percentDecode(info)
            }
        }
        // 剩下的是 host[:port][/path]，path 直接丢掉（这些协议都用不上）。
        if let slash = rest.firstIndex(of: "/") {
            rest = String(rest[rest.startIndex..<slash])
        }
        if rest.hasPrefix("[") {                 // IPv6 字面量：[::1]:443
            guard let close = rest.firstIndex(of: "]") else { return nil }
            host = String(rest[rest.index(after: rest.startIndex)..<close])
            let after = rest[rest.index(after: close)...]
            if after.hasPrefix(":") { port = Int(after.dropFirst()) }
        } else if let colon = rest.lastIndex(of: ":") {
            host = String(rest[rest.startIndex..<colon])
            port = Int(rest[rest.index(after: colon)...])
        } else {
            host = rest
        }
        guard !host.isEmpty else { return nil }
    }

    /// 查询参数，按给定的几个别名依次取第一个非空值。
    /// 同一个字段在不同客户端里叫法不同（`sni`/`peer`、`congestion_control`/`congestion-controller`…）。
    func first(_ keys: String...) -> String {
        for key in keys {
            if let value = query[key], !value.isEmpty { return value }
        }
        return ""
    }

    func flag(_ keys: String...) -> Bool {
        for key in keys {
            guard let value = query[key]?.trimmingCharacters(in: .whitespaces).lowercased() else { continue }
            if value == "1" || value == "true" { return true }
        }
        return false
    }

    // MARK: - 工具

    static func parseQuery(_ raw: String) -> [String: String] {
        var result: [String: String] = [:]
        for pair in raw.split(separator: "&", omittingEmptySubsequences: true) {
            guard let eq = pair.firstIndex(of: "=") else {
                result[percentDecode(String(pair))] = ""
                continue
            }
            let key = percentDecode(String(pair[pair.startIndex..<eq]))
            let value = percentDecode(String(pair[pair.index(after: eq)...]))
            result[key] = value
        }
        return result
    }

    /// `+` 不当空格处理 —— 这些链接用的是 RFC 3986 的百分号编码，不是表单编码；
    /// 把 `+` 换成空格会破坏 base64 密码。
    static func percentDecode(_ raw: String) -> String {
        raw.removingPercentEncoding ?? raw
    }
}
