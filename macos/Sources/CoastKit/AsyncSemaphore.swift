import Foundation

/// 计数信号量的 async 版。`DispatchSemaphore.wait()` 会**阻塞**线程，在 Swift 并发里
/// 那等于占死一个协作线程池的线程，几个下载测速就能把池饿光；这里改成挂起任务。
///
/// 用途见 `ClashService` 的下载测速：一把 1 计数的锁串行化「选组 + 建连」，
/// 一把 5 计数的闸门限制同时在跑的下载数。
actor AsyncSemaphore {
    private var available: Int
    private var waiters: [CheckedContinuation<Void, Never>] = []

    init(_ value: Int) { available = value }

    func wait() async {
        if available > 0 {
            available -= 1
            return
        }
        await withCheckedContinuation { waiters.append($0) }
    }

    func signal() {
        if waiters.isEmpty {
            available += 1
        } else {
            waiters.removeFirst().resume()
        }
    }
}
