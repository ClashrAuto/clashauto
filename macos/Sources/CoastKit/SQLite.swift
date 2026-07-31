import Foundation
import SQLite3

/// sqlite3 C API 的极薄封装。
///
/// 只包到「够用」为止：这个应用的 SQL 都是手写的固定语句，引 GRDB/SQLite.swift 那种全功能库
/// 换不来什么，却多一个要跟版本的依赖。
///
/// 库文件是全应用共用的一个：`configDir/coast.db`。
public final class SQLiteDatabase: @unchecked Sendable {

    public enum DBError: Error, Sendable {
        case openFailed(String)
        case prepareFailed(String, sql: String)
        case stepFailed(String, sql: String)
    }

    /// `SQLITE_TRANSIENT`：让 sqlite 自己拷一份绑定的字符串/数据。
    ///
    /// 不加这个而用默认的 STATIC，sqlite 会**只存指针**，等到 step 时 Swift 那边的临时
    /// String 早已释放 —— 写进去的是垃圾，而且不一定当场崩。
    private static let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)

    private var handle: OpaquePointer?
    private let queue = DispatchQueue(label: "coast.sqlite")

    public var isOpen: Bool { handle != nil }

    public init(path: URL) throws {
        try FileManager.default.createDirectory(at: path.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        var db: OpaquePointer?
        guard sqlite3_open_v2(path.path, &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                              nil) == SQLITE_OK, let db else {
            let message = db.map { String(cString: sqlite3_errmsg($0)) } ?? "sqlite3_open_v2 失败"
            sqlite3_close_v2(db)
            throw DBError.openFailed(message)
        }
        handle = db

        // WAL：读写互不阻塞。这个库同时被「每秒观察连接」和「界面查统计」用，
        // 默认的 rollback journal 下后者会被前者堵住。
        exec("PRAGMA journal_mode=WAL")
        // NORMAL：WAL 下仍然是崩溃安全的（丢的最多是最后几个事务），但省掉每次 commit 的 fsync。
        // 历史记录丢最后几条完全可以接受，拿它换磁盘寿命与写入延迟很划算。
        exec("PRAGMA synchronous=NORMAL")
        // 写与写之间排队而不是立刻报 SQLITE_BUSY。
        exec("PRAGMA busy_timeout=3000")
    }

    deinit {
        if let handle { sqlite3_close_v2(handle) }
    }

    public func close() {
        queue.sync {
            if let handle { sqlite3_close_v2(handle) }
            handle = nil
        }
    }

    // MARK: - 执行

    /// 无结果集语句。失败只返回 false，不抛 —— 建表这类语句的失败没有可行的补救动作，
    /// 调用方能做的只是整体降级为「没有历史」。
    @discardableResult
    public func exec(_ sql: String) -> Bool {
        queue.sync {
            guard let handle else { return false }
            return sqlite3_exec(handle, sql, nil, nil, nil) == SQLITE_OK
        }
    }

    public enum Value: Sendable {
        case text(String)
        case int(Int64)
        case null
    }

    /// 带参数的写语句。
    @discardableResult
    public func run(_ sql: String, _ parameters: [Value] = []) -> Bool {
        queue.sync { runLocked(sql, parameters) }
    }

    /// 批量写，**一个事务**。
    ///
    /// 逐条 commit 会把磁盘按在地上摩擦：每条一次 fsync，几万条历史记录写起来是分钟级。
    @discardableResult
    public func transaction(_ statements: [(String, [Value])]) -> Bool {
        queue.sync {
            guard handle != nil else { return false }
            guard sqlite3_exec(handle, "BEGIN IMMEDIATE", nil, nil, nil) == SQLITE_OK else { return false }
            for (sql, parameters) in statements where !runLocked(sql, parameters) {
                sqlite3_exec(handle, "ROLLBACK", nil, nil, nil)
                return false
            }
            return sqlite3_exec(handle, "COMMIT", nil, nil, nil) == SQLITE_OK
        }
    }

    /// 查询，逐行回调。行内用 `Row` 按列序号取值。
    public func query(_ sql: String, _ parameters: [Value] = [], row handler: (Row) -> Void) {
        queue.sync {
            guard let handle else { return }
            var statement: OpaquePointer?
            guard sqlite3_prepare_v2(handle, sql, -1, &statement, nil) == SQLITE_OK else { return }
            defer { sqlite3_finalize(statement) }
            bind(parameters, to: statement)
            while sqlite3_step(statement) == SQLITE_ROW {
                handler(Row(statement: statement))
            }
        }
    }

    public struct Row {
        let statement: OpaquePointer?

        public func text(_ index: Int32) -> String {
            guard let cString = sqlite3_column_text(statement, index) else { return "" }
            return String(cString: cString)
        }

        public func int(_ index: Int32) -> Int64 {
            sqlite3_column_int64(statement, index)
        }
    }

    // MARK: - 内部

    /// 调用方必须已持有 `queue`。
    private func runLocked(_ sql: String, _ parameters: [Value]) -> Bool {
        guard let handle else { return false }
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(handle, sql, -1, &statement, nil) == SQLITE_OK else { return false }
        defer { sqlite3_finalize(statement) }
        bind(parameters, to: statement)
        let code = sqlite3_step(statement)
        return code == SQLITE_DONE || code == SQLITE_ROW
    }

    private func bind(_ parameters: [Value], to statement: OpaquePointer?) {
        for (offset, value) in parameters.enumerated() {
            let index = Int32(offset + 1)
            switch value {
            case .text(let string):
                sqlite3_bind_text(statement, index, string, -1, Self.transient)
            case .int(let number):
                sqlite3_bind_int64(statement, index, number)
            case .null:
                sqlite3_bind_null(statement, index)
            }
        }
    }
}

public enum SQLitePaths {
    /// 库文件路径。顺带做一次性迁移：早期版本把连接历史写在 `history.db`，
    /// 只有它存在时整体改名过来（含 `-wal`/`-shm`），历史不丢。
    public static func databasePath(configDir: URL) -> URL {
        let target = configDir.appendingPathComponent("coast.db")
        let legacy = configDir.appendingPathComponent("history.db")
        let fm = FileManager.default
        if !fm.fileExists(atPath: target.path), fm.fileExists(atPath: legacy.path) {
            // -wal/-shm 必须跟着一起改名：只搬主文件会让 sqlite 读到一份「主库是旧的、
            // WAL 里还有没并回去的新事务」的组合，直接判定库损坏。
            for suffix in ["", "-wal", "-shm"] {
                let from = configDir.appendingPathComponent("history.db\(suffix)")
                let to = configDir.appendingPathComponent("coast.db\(suffix)")
                guard fm.fileExists(atPath: from.path) else { continue }
                try? fm.moveItem(at: from, to: to)
            }
        }
        return target
    }
}
