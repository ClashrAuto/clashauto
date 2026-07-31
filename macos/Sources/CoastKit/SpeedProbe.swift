import Foundation

/// 单次下载测速。一次一个 `URLSession` —— 因为 delegate 只能在建 session 时挂上，
/// 而每次测速需要自己的一套计数与首字节回调。并发上限 5，多开 5 个 ephemeral session
/// 的代价远小于让它们共享 delegate 后互相串账。
///
/// 结束条件三选一：到字节上限、到时间上限、或服务端/代理先断（慢节点常见）。
/// 三种都算「测到多少算多少」，不当失败 —— 拿不到速度的节点显示 0 即可。
final class SpeedProbe: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    private let maxBytes: Int64
    private let maxMs: Int64
    private let config: URLSessionConfiguration

    // 下面这组状态只在 delegate 队列（下方 makeQueue 的串行队列）上访问，故无需再加锁。
    private var total: Int64 = 0
    private var started: DispatchTime?
    private var onFirstByte: (@Sendable () -> Void)?
    private var continuation: CheckedContinuation<(bytes: Int64, ms: Int64), Never>?
    private var session: URLSession?

    init(host: String, mixedPort: Int, maxBytes: Int64, maxMs: Int64) {
        self.maxBytes = maxBytes
        self.maxMs = maxMs
        self.config = ClashAPI.speedSessionConfig(host: host, mixedPort: mixedPort)
        super.init()
    }

    func run(url: URL, onFirstByte: @escaping @Sendable () -> Void) async -> (bytes: Int64, ms: Int64) {
        await withCheckedContinuation { continuation in
            let queue = OperationQueue()
            queue.maxConcurrentOperationCount = 1   // 串行：delegate 回调不并发，省掉一把锁
            let session = URLSession(configuration: config, delegate: self, delegateQueue: queue)
            queue.addOperation {
                self.continuation = continuation
                self.onFirstByte = onFirstByte
                self.session = session
                session.dataTask(with: url).resume()
            }
        }
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        if started == nil {
            // 首字节 = 建连+TLS 完成。从这里归零重新计时，并放行下一个节点的握手。
            started = .now()
            onFirstByte?()
            onFirstByte = nil
            return
        }
        total += Int64(data.count)
        let elapsedMs = Int64((DispatchTime.now().uptimeNanoseconds - started!.uptimeNanoseconds) / 1_000_000)
        if total >= maxBytes || elapsedMs >= maxMs {
            dataTask.cancel()
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        finish()
    }

    private func finish() {
        guard let continuation else { return }
        self.continuation = nil
        // 从未收到过数据：也要放行串行锁，否则整轮测速会卡在这个节点上永不推进。
        if let onFirstByte { onFirstByte(); self.onFirstByte = nil }
        let ms: Int64 = started.map {
            Int64((DispatchTime.now().uptimeNanoseconds - $0.uptimeNanoseconds) / 1_000_000)
        } ?? 0
        session?.invalidateAndCancel()
        session = nil
        continuation.resume(returning: (total, max(ms, 1)))
    }
}
