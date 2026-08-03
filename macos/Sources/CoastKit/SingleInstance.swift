import Foundation

/// 单实例守卫：保证同一用户同一时刻**只有一个 Coast 在跑**。
///
/// ── 为什么这不是"锦上添花的体验优化"，而是**必须的正确性保护** ──
///
/// Coast 把状态写进**内核/系统的全局命名空间**，而不是自己的进程里：
/// pf anchor `com.apple/coast.redirect`、`net.inet.ip.forwarding`、
/// 以及核心占用的 9191/7890 等固定端口。名字和端口都是**固定**的
/// （刻意如此：被 kill -9 打死后要靠固定名字清理残留），因而**分不清是谁装的**。
///
/// 本机实测（macOS 26.5.2，两个实例同一数据目录）：
///   · 两个 App 都活着，**各起了一个核心**（核心进程数 = 2）；
///   · 但 9191 只有**先启动那个**的核心绑上；
///   · 第二个实例的核心绑不上端口却**不退出** —— mihomo 只记一条
///     "address already in use" 然后照常运行，日志里**一条冲突提示都没有**。
///   于是"进程在跑、UI 一切正常、一个端口都没监听"，代理完全不通且毫无提示。
/// 更糟的是 helper 侧：`startRedirect` 会把 `redirectOwner` 覆盖成后来那个实例，
/// 后者退出时 `clientVanished` 就把接管撤掉 —— 而**第一个实例还活着、界面正常，
/// 它的整个数据面却已被撤销**。
///
/// 实现用 **文件锁（flock）** 而不是 `NSRunningApplication` 查同 bundle id：
/// 后者对 `swift run` / 直接跑可执行文件（没有 bundle identity）的进程无效，
/// 而开发和自动化测试恰恰都是那样跑的。锁文件放在用户数据目录下，
/// 进程退出（含被 SIGKILL）时内核自动释放，不会留下需要清理的残留。
public enum SingleInstance {
    private static var lockFD: Int32 = -1

    /// 尝试取得本机唯一实例锁。返回 `false` = 已经有一个 Coast 在跑。
    ///
    /// 拿到的锁**故意不释放** —— 靠进程退出时内核自动回收，
    /// 这样即使被 kill -9 也不会留下一把没人解的锁。
    public static func acquire() -> Bool {
        let dir = AppPaths.userDir
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let path = dir.appendingPathComponent("coast.lock").path
        let fd = open(path, O_CREAT | O_RDWR, 0o644)
        guard fd >= 0 else { return true }   // 建不了锁文件就放行，别为了守卫把人挡在门外
        if flock(fd, LOCK_EX | LOCK_NB) != 0 {
            close(fd)
            return false                     // 已被另一个实例持有
        }
        lockFD = fd                          // 存住 fd，别让它被回收导致锁提前释放
        return true
    }
}
