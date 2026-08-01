import Darwin
import Foundation

/// helper 收到的外部输入的校验。放共享层(而非 helper 内部)的两个理由:
///   1. 可单测 —— 可执行的 CoastHelper target 不能被 `@testable import`;
///   2. app 侧下发前也能先用同一套挡一道(fail fast),不必等 XPC 往返才发现非法。
///
/// helper 以 root 跑,codesign 门是主防线;这些校验是**纵深防御** —— root 服务不信任
/// 任何输入,即便来自"自己的 app"(app 一旦被注入,这些字符串就是直通 root 的路)。
public enum InputValidation {

    /// 网卡名白名单:字母开头、只含字母数字、长度 ≤ IFNAMSIZ-1(15)。
    ///
    /// 为什么严:interface 会被原样拼进 PF 规则文本喂给 pfctl。带换行的串能**另起一条规则**、
    /// 带空格能改变规则语义 —— 一个精心构造的 interface 就是任意 PF 规则注入。
    /// `en0`/`en1`/`utun3`/`bridge0` 都过。
    public static func isValidInterface(_ name: String) -> Bool {
        guard !name.isEmpty, name.count <= 15, let first = name.first, first.isLetter else { return false }
        return name.allSatisfy { $0.isLetter || $0.isNumber }
    }

    /// 路径卫生:绝对路径、非空、不含 `..` 段(防路径遍历)。
    ///
    /// 不检查文件是否存在(那由调用点各自 fileExists/isExecutable 处理),只挡明显滥用:
    /// helper 以 root 起 executable、按 userDir 拼日志路径,`..` 能写/执行到约定之外的地方。
    public static func isSanePath(_ path: String) -> Bool {
        guard path.hasPrefix("/"), !path.isEmpty else { return false }
        return !path.split(separator: "/").contains("..")
    }

    /// 严格的 IPv6 字面量校验（**不接受区标 `%en0`**）。
    ///
    /// 为什么严:设备 v6 源地址会被原样拼进 PF `rdr ... inet6 ... from <v6>` 规则文本喂给 pfctl
    /// （helper 以 root 跑）。和 `isValidInterface` 同理，带空格/换行的串能改写或另起 PF 规则,
    /// 这里是那条注入路径的纵深防御。`inet_pton` 通过 = 是合法字面量,同时 `%`/空白等注入字符
    /// 必然让它失败。区标另挡:PF 的 `from` 不接受区标,带 `%` 的地址会让整条规则语法错。
    public static func isValidIPv6(_ text: String) -> Bool {
        guard !text.isEmpty, text.count <= 45, !text.contains("%") else { return false }
        var addr = in6_addr()
        return text.withCString { inet_pton(AF_INET6, $0, &addr) } == 1
    }
}
