import Foundation

/// 带进度回调的文件下载。
///
/// 为什么不用 `URLSession.bytes` 那个 AsyncSequence 逐字节读：内核包十几 MB，那是上千万次
/// await 恢复，纯属白烧 CPU（下载测速那边同样的理由，见 `SpeedProbe`）。这里用
/// `URLSessionDownloadTask` —— 系统直接写盘，进度由 `didWriteData` 回调给出。
final class FileDownloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {

    private let onProgress: @Sendable (Int) -> Void
    private var continuation: CheckedContinuation<Result<Data, Error>, Never>?
    private var session: URLSession?
    private var lastPercent = -1

    init(onProgress: @escaping @Sendable (Int) -> Void) {
        self.onProgress = onProgress
        super.init()
    }

    /// 下载完成后把内容读进内存返回。内核包只有十几 MB，读进来比在多处传临时文件路径省事；
    /// 真要下几百 MB 的东西时应该改成返回 URL。
    func download(request: URLRequest, bypassProxy: Bool) async -> Result<Data, Error> {
        await withCheckedContinuation { continuation in
            let queue = OperationQueue()
            queue.maxConcurrentOperationCount = 1
            let config = URLSessionConfiguration.ephemeral
            config.timeoutIntervalForRequest = 30
            // 走镜像时不套系统代理：镜像本来就是给「连不上 GitHub」的场景用的，再绕回代理没意义。
            if bypassProxy { config.connectionProxyDictionary = [:] }
            let session = URLSession(configuration: config, delegate: self, delegateQueue: queue)
            queue.addOperation {
                self.continuation = continuation
                self.session = session
                session.downloadTask(with: request).resume()
            }
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64, totalBytesWritten: Int64,
                    totalBytesExpectedToWrite: Int64) {
        guard totalBytesExpectedToWrite > 0 else { return }
        let percent = Int(totalBytesWritten * 100 / totalBytesExpectedToWrite)
        guard percent != lastPercent else { return }   // 只在百分比变化时回调，别每个包都刷 UI
        lastPercent = percent
        onProgress(percent)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didFinishDownloadingTo location: URL) {
        // 这个回调返回后系统就会删掉临时文件，必须在此**同步**读完。
        let result: Result<Data, Error>
        do {
            result = .success(try Data(contentsOf: location))
        } catch {
            result = .failure(error)
        }
        finish(result)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error else { return }   // 成功路径已在 didFinishDownloadingTo 收尾
        finish(.failure(error))
    }

    private func finish(_ result: Result<Data, Error>) {
        guard let continuation else { return }
        self.continuation = nil
        session?.finishTasksAndInvalidate()
        session = nil
        continuation.resume(returning: result)
    }
}
