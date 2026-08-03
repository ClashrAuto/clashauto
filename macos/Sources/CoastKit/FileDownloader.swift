import Foundation

/// 带进度回调的文件下载。
///
/// 为什么不用 `URLSession.bytes` 那个 AsyncSequence 逐字节读：内核包十几 MB，那是上千万次
/// await 恢复，纯属白烧 CPU（下载测速那边同样的理由，见 `SpeedProbe`）。这里用
/// `URLSessionDownloadTask` —— 系统直接写盘，进度由 `didWriteData` 回调给出。
final class FileDownloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {

    /// (百分比, 已下载字节, 总字节)。后两个是给界面显示「下载量/总量」与实时速度用的 ——
    /// 只有百分比的话，那两样都算不出来。
    private let onProgress: @Sendable (Int, Int64, Int64) -> Void
    private var continuation: CheckedContinuation<Result<Data, Error>, Never>?
    private var session: URLSession?
    private var lastPercent = -1

    init(onProgress: @escaping @Sendable (Int, Int64, Int64) -> Void) {
        self.onProgress = onProgress
        super.init()
    }

    /// 下载完成后把内容读进内存返回。内核包只有十几 MB，读进来比在多处传临时文件路径省事；
    /// 真要下几百 MB 的东西时应该改成返回 URL。
    /// ★ **必须响应任务取消**：界面上那颗「结束下载」按下时取消的是外层的 `Task`，而
    ///   `withCheckedContinuation` 本身不会因此恢复 —— 不接这一条的话，URLSession 会**照旧
    ///   把整个包下完**（几十 MB 白流），期间那条 async 函数一直挂着，用户点了「结束」却什么
    ///   都没停下。`withTaskCancellationHandler` 里把 session 作废，delegate 随即以
    ///   `NSURLErrorCancelled` 收尾，继续走正常的 `finish` 路径。
    func download(request: URLRequest, configuration: URLSessionConfiguration) async -> Result<Data, Error> {
        await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                let queue = OperationQueue()
                queue.maxConcurrentOperationCount = 1
                let session = URLSession(configuration: configuration, delegate: self, delegateQueue: queue)
                queue.addOperation {
                    self.continuation = continuation
                    self.session = session
                    session.downloadTask(with: request).resume()
                }
            }
        } onCancel: {
            cancel()
        }
    }

    /// 立刻中断在途下载。作废 session 会让 delegate 收到取消错误，`finish` 那条路照走，
    /// continuation 不会悬着。
    func cancel() {
        session?.invalidateAndCancel()
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64, totalBytesWritten: Int64,
                    totalBytesExpectedToWrite: Int64) {
        guard totalBytesExpectedToWrite > 0 else { return }
        let percent = Int(totalBytesWritten * 100 / totalBytesExpectedToWrite)
        guard percent != lastPercent else { return }   // 只在百分比变化时回调，别每个包都刷 UI
        lastPercent = percent
        onProgress(percent, totalBytesWritten, totalBytesExpectedToWrite)
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
