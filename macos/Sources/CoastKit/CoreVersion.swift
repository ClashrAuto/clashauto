import Foundation

/// 已安装内核的版本号。对齐 Qt `AboutController::checkCore` 的前半段：
/// 跑一次 `<core> -v`，从输出里抠出第一个 `vN…` 形状的串。
///
/// 存在的理由是侧栏那颗 **"core" 角标** —— 「内核有没有新版」必须先知道本地是哪一版，
/// 而 mihomo 不在任何配置文件里写自己的版本，只能问它本人。
public enum CoreVersion {

    /// 从 `-v` 的输出里解析版本。**纯函数**，可以直接喂各版本 mihomo 的真实输出来测 ——
    /// 这才是这段代码唯一容易写错的地方（正则），起进程那半段没什么可测的。
    ///
    /// 与 Qt 用的是同一条正则 `\bv\d+[0-9A-Za-z.\-]*`：mihomo 的输出形如
    /// `Mihomo Meta v1.19.29 linux amd64 with go1.24.0 …`，要的是 `v1.19.29`。
    /// 注意**不能**放宽成「任意 v 开头」—— 输出里还有 `with go1.24.0`、编译日期之类，
    /// 放宽了会抠到别的东西，而错的版本号会让角标常亮或常灭，两种都没法排查。
    public static func parse(_ output: String) -> String? {
        let pattern = #"\bv\d+[0-9A-Za-z.\-]*"#
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(in: output,
                                           range: NSRange(output.startIndex..., in: output)),
              let range = Range(match.range, in: output)
        else { return nil }
        return String(output[range])
    }

    /// 问一次本地内核。核心不存在、跑不起来、或输出里没有版本形状的串时一律返回 nil ——
    /// 调用方据此**不置角标**。Qt 的注释写得很清楚：「版本探测失败，宁可不误报」。
    public static func local(executable: URL = AppPaths.coreExecutable) -> String? {
        guard FileManager.default.isExecutableFile(atPath: executable.path) else { return nil }
        let task = Process()
        task.executableURL = executable
        task.arguments = ["-v"]
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = FileHandle.nullDevice
        guard (try? task.run()) != nil else { return nil }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()
        guard let text = String(data: data, encoding: .utf8) else { return nil }
        return parse(text)
    }
}
