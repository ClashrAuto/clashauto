import Foundation

/// GeoIP 数据库（MaxMind DB / `Country.mmdb`）的**校验 + 落盘**。
///
/// 这不是防御性编程的洁癖，是 Qt 版真机上栽过的一次事故（见 `clashauto-c++/src/MmdbFile.h`）：
/// 下载路径只判 `error == nil && !data.isEmpty` 就原地截断覆盖线上库，于是半截 body、
/// 代理返回的错误页、CDN 截断全都会被当成新数据库写下去。而**核心不报错** ——
/// maxminddb 打得开、不 fatal，只是每次查询返回空。
///
/// `GEOIP,CN` 是 `MATCH` 之前最后一条、负责把国内流量留在直连的规则。它静默失效之后，
/// 所有嗅探不出域名的裸 IP 目的地（推送 / IM / 游戏 / 部分 CDN）全部落进 `MATCH` → 出海。
/// 表现是「国内国外都很慢」，日志里一个字都没有。
///
/// 所以这里做三件事，缺一不可：
///
/// 1. `validate` —— 写之前先确认这坨字节确实是一份**结构自洽**的 MaxMind DB；
/// 2. `stage` —— 校验通过也**不碰**线上文件，只写到 `<target>.new`。核心把它 mmap 着，
///    而且 mmdb 在核心里是加载一次就不再重读的：原地覆盖从来就没有「立刻生效」过，
///    它唯一的效果就是有概率毁掉好文件；
/// 3. `validateFile` + `applyStaged` —— 起核心前给线上那份体检，坏了退回内置种子；
///    有暂存的新库就在此刻换上。
public enum MmdbFile {

    /// MaxMind DB 的元数据段起始标记：裸字节 `AB CD EF` + `"MaxMind.com"`。
    ///
    /// **必须逐字节写**。`"\u{AB}…".utf8` 得到的是 UTF-8 编码后的 `C2 AB …` ——
    /// 那是两个字节，标记永远匹配不上，于是真库被判成「找不到 metadata」。
    /// 而所有坏文件测试照样绿（它们本来就该被拒），所以只测坏文件是发现不了这个的。
    private static let metadataMarker: [UInt8] = [0xAB, 0xCD, 0xEF] + Array("MaxMind.com".utf8)
    /// 体积下限：挡掉 HTML 错误页 / 空响应这类明显不是数据库的东西。
    private static let minimumSize = 512 * 1024

    public struct Metadata: Sendable, Equatable {
        public var nodeCount: Int
        public var recordSize: Int
        public var ipVersion: Int
        public var majorVersion: Int
        /// 库的**构建时刻**（Unix 秒），上游写在 metadata 里。缺这个键时为 0。
        public var buildEpoch: Int = 0
        /// 每个节点的字节数 = 两条记录。
        public var nodeSize: Int { recordSize * 2 / 8 }
        public var treeSize: Int { nodeCount * nodeSize }
    }

    /// 结构性校验。只查规范里的硬不变量，不解数据段内容。
    public static func validate(_ data: Data) -> (ok: Bool, why: String) {
        guard data.count >= minimumSize else {
            return (false, "体积只有 \(data.count / 1024) KB，不像是数据库")
        }
        guard let metadata = parseMetadata(data) else {
            return (false, "找不到或解不出 metadata 段")
        }
        guard metadata.majorVersion == 2 else {
            return (false, "不支持的格式主版本 \(metadata.majorVersion)")
        }
        guard metadata.recordSize == 24 || metadata.recordSize == 28 || metadata.recordSize == 32 else {
            return (false, "record_size 异常：\(metadata.recordSize)")
        }
        guard metadata.nodeCount > 0 else { return (false, "node_count 为 0") }

        let separatorEnd = metadata.treeSize + 16
        guard data.count > separatorEnd else {
            return (false, "文件长度 \(data.count) 装不下由 metadata 推算的搜索树 \(metadata.treeSize)")
        }
        // ★ 真机那个坏文件正是栽在这一条：搜索树末尾那 16 字节的数据段分隔符必须全零。
        let separator = data[data.startIndex.advanced(by: metadata.treeSize)
                             ..< data.startIndex.advanced(by: separatorEnd)]
        guard separator.allSatisfy({ $0 == 0 }) else {
            return (false, "搜索树末尾的 16 字节分隔符不是全零 —— 树与数据段错位")
        }
        // 拿几个知名 IP 走一遍树，终止记录必须落在合法的数据指针区间内。
        for probe in ["114.114.114.114", "8.8.8.8", "1.1.1.1"] {
            guard let reason = walkFailureReason(data, metadata: metadata, ipv4: probe) else { continue }
            return (false, "查 \(probe) 时 \(reason)")
        }
        return (true, "")
    }

    public static func validateFile(_ url: URL) -> (ok: Bool, why: String) {
        guard let data = try? Data(contentsOf: url, options: .mappedIfSafe) else {
            return (false, "读不出文件")
        }
        return validate(data)
    }

    /// 暂存路径。`applyStaged` 之前，线上那份原封不动。
    public static func stagedURL(for target: URL) -> URL {
        target.appendingPathExtension("new")
    }

    /// 校验并暂存。**不碰** `target`。
    @discardableResult
    public static func stage(_ data: Data, target: URL) -> (ok: Bool, why: String) {
        let result = validate(data)
        guard result.ok else { return result }
        let staged = stagedURL(for: target)
        do {
            try FileManager.default.createDirectory(
                at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: staged, options: .atomic)
        } catch {
            return (false, "写暂存文件失败：\(error.localizedDescription)")
        }
        return (true, "")
    }

    /// 起核心前把暂存的新库换上。没有暂存文件时什么都不做。
    /// 换之前再校验一次 —— 暂存到现在之间磁盘可能出过问题，而这一步之后就没人再看了。
    @discardableResult
    public static func applyStaged(target: URL) -> Bool {
        let staged = stagedURL(for: target)
        guard FileManager.default.fileExists(atPath: staged.path) else { return false }
        guard validateFile(staged).ok else {
            try? FileManager.default.removeItem(at: staged)
            return false
        }
        _ = try? FileManager.default.replaceItemAt(target, withItemAt: staged)
        return true
    }

    // MARK: - MaxMind 格式解析（只解校验需要的那部分）

    static func parseMetadata(_ data: Data) -> Metadata? {
        let bytes = [UInt8](data)
        // metadata 在文件末尾，从后往前找标记（规范只保证在最后 128KB 内）。
        let searchFrom = max(0, bytes.count - 128 * 1024)
        var markerAt: Int?
        var index = bytes.count - metadataMarker.count
        while index >= searchFrom {
            if Array(bytes[index..<index + metadataMarker.count]) == metadataMarker {
                markerAt = index
                break
            }
            index -= 1
        }
        guard let start = markerAt else { return nil }
        var cursor = start + metadataMarker.count
        guard let map = decodeMap(bytes, &cursor) else { return nil }
        guard let nodeCount = map["node_count"] as? Int,
              let recordSize = map["record_size"] as? Int,
              let ipVersion = map["ip_version"] as? Int,
              let major = map["binary_format_major_version"] as? Int else { return nil }
        return Metadata(nodeCount: nodeCount, recordSize: recordSize,
                        ipVersion: ipVersion, majorVersion: major,
                        buildEpoch: map["build_epoch"] as? Int ?? 0)
    }

    /// 这份库的**构建日期**。读不出来返回 nil。
    ///
    /// ★ 不要拿文件 mtime 当这个用：mtime 说的是我们哪天把它写到盘上的，重装、换机、
    ///   甚至一次失败重下都会把它刷成今天，而库本身还是原来那份。GeoIP 没有版本号，
    ///   build_epoch 是「你手上这份是哪天的」唯一可信的答案。
    ///
    /// 只读末尾 128 KiB —— metadata 按规范就在那儿，为一个时间戳把五六 MB 全读进来没道理
    /// （设置页每次进页都会叫到它）。
    public static func buildDate(of url: URL) -> Date? {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        let total = (try? handle.seekToEnd()).map(Int64.init) ?? 0
        let window: Int64 = 128 * 1024
        try? handle.seek(toOffset: UInt64(max(0, total - window)))
        guard let tail = try? handle.readToEnd(), !tail.isEmpty else { return nil }
        guard let meta = parseMetadata(tail), meta.buildEpoch > 0 else { return nil }
        return Date(timeIntervalSince1970: TimeInterval(meta.buildEpoch))
    }

    /// 解一个 MaxMind 数据段的 map（只支持校验用得到的类型）。
    private static func decodeMap(_ bytes: [UInt8], _ cursor: inout Int) -> [String: Any]? {
        guard let (type, size) = decodeControl(bytes, &cursor), type == 7 else { return nil }
        var result: [String: Any] = [:]
        for _ in 0..<size {
            guard let key = decodeValue(bytes, &cursor) as? String,
                  let value = decodeValue(bytes, &cursor) else { return nil }
            result[key] = value
        }
        return result
    }

    private static func decodeControl(_ bytes: [UInt8], _ cursor: inout Int) -> (Int, Int)? {
        guard cursor < bytes.count else { return nil }
        let control = bytes[cursor]; cursor += 1
        var type = Int(control >> 5)
        if type == 0 {   // 扩展类型
            guard cursor < bytes.count else { return nil }
            type = Int(bytes[cursor]) + 7; cursor += 1
        }
        var size = Int(control & 0x1F)
        if size >= 29 {
            let extra = size - 28
            guard cursor + extra <= bytes.count else { return nil }
            var value = 0
            for offset in 0..<extra { value = value << 8 | Int(bytes[cursor + offset]) }
            cursor += extra
            size = [29: 29 + value, 30: 285 + value, 31: 65_821 + value][size] ?? value
        }
        return (type, size)
    }

    private static func decodeValue(_ bytes: [UInt8], _ cursor: inout Int) -> Any? {
        guard let (type, size) = decodeControl(bytes, &cursor) else { return nil }
        switch type {
        case 2:   // UTF-8 字符串
            guard cursor + size <= bytes.count else { return nil }
            let slice = bytes[cursor..<cursor + size]; cursor += size
            return String(decoding: slice, as: UTF8.self)
        case 5, 6, 9, 10:   // uint16 / uint32 / uint64 / uint128 —— 都当整数读
            guard cursor + size <= bytes.count else { return nil }
            var value = 0
            for offset in 0..<size { value = value << 8 | Int(bytes[cursor + offset]) }
            cursor += size
            return value
        case 7:   // map
            var result: [String: Any] = [:]
            for _ in 0..<size {
                guard let key = decodeValue(bytes, &cursor) as? String,
                      let value = decodeValue(bytes, &cursor) else { return nil }
                result[key] = value
            }
            return result
        case 11:  // array
            var result: [Any] = []
            for _ in 0..<size {
                guard let value = decodeValue(bytes, &cursor) else { return nil }
                result.append(value)
            }
            return result
        case 14:  // bool（size 即取值）
            return size != 0
        default:
            guard cursor + size <= bytes.count else { return nil }
            cursor += size
            return 0
        }
    }

    /// 走一遍搜索树。返回 nil 表示这条路径正常；否则返回人话原因。
    private static func walkFailureReason(_ data: Data, metadata: Metadata,
                                          ipv4 address: String) -> String? {
        let parts = address.split(separator: ".").compactMap { UInt8($0) }
        guard parts.count == 4 else { return nil }
        let bytes = [UInt8](data)
        // ip_version == 6 的库里，IPv4 从走完 96 个 0 比特之后的节点开始。
        var node = 0
        if metadata.ipVersion == 6 {
            for _ in 0..<96 {
                guard let next = record(bytes, metadata: metadata, node: node, bit: 0) else {
                    return "IPv4 起始节点定位失败"
                }
                if next >= metadata.nodeCount { return nil }   // 提前终止是合法的
                node = next
            }
        }
        for byte in parts {
            for shift in (0..<8).reversed() {
                let bit = Int((byte >> UInt8(shift)) & 1)
                guard node < metadata.nodeCount else { break }
                guard let next = record(bytes, metadata: metadata, node: node, bit: bit) else {
                    return "读节点越界"
                }
                node = next
            }
        }
        if node == metadata.nodeCount { return nil }           // 「查无此项」，合法
        guard node > metadata.nodeCount else { return "终止记录仍指向节点，树未收敛" }
        let dataOffset = node - metadata.nodeCount - 16 + metadata.treeSize + 16
        guard dataOffset < bytes.count else {
            return "数据指针 \(dataOffset) 越出文件末尾 \(bytes.count)"
        }
        return nil
    }

    private static func record(_ bytes: [UInt8], metadata: Metadata,
                               node: Int, bit: Int) -> Int? {
        let base = node * metadata.nodeSize
        guard base + metadata.nodeSize <= bytes.count else { return nil }
        switch metadata.recordSize {
        case 24:
            let offset = base + bit * 3
            return Int(bytes[offset]) << 16 | Int(bytes[offset + 1]) << 8 | Int(bytes[offset + 2])
        case 28:
            // 中间那个字节的高/低四位分别是左右记录的第 25–28 位。
            let middle = bytes[base + 3]
            if bit == 0 {
                return Int(middle >> 4) << 24 | Int(bytes[base]) << 16
                     | Int(bytes[base + 1]) << 8 | Int(bytes[base + 2])
            }
            return Int(middle & 0x0F) << 24 | Int(bytes[base + 4]) << 16
                 | Int(bytes[base + 5]) << 8 | Int(bytes[base + 6])
        case 32:
            let offset = base + bit * 4
            return Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16
                 | Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
        default:
            return nil
        }
    }
}
