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
}
