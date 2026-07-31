import Foundation
import Testing
@testable import CoastKit

@Suite("连接行:时间戳与两个榜单")
struct ConnectionRowTests {

    @Test("★ 核心给的 RFC3339 带小数秒 —— 少了 withFractionalSeconds 会整条解析失败")
    func parsesFractionalSeconds() {
        // 这就是 mihomo /connections 里 start 的真实形态
        let withFraction = ConnectionRow.parseTimestamp("2026-07-31T21:33:33.878493+08:00")
        #expect(withFraction != .distantPast, "带小数秒的时间戳没解出来")
        // 不带小数秒的也要能解（不同核心版本格式略有出入）
        let plain = ConnectionRow.parseTimestamp("2026-07-31T21:33:33+08:00")
        #expect(plain != .distantPast, "不带小数秒的时间戳没解出来")
        // 两者相差不到一秒
        #expect(abs(withFraction.timeIntervalSince(plain)) < 1)
    }

    @Test("★ 解不出来退 distantPast 而不是 now")
    func badTimestampSinksToBottom() {
        // 退成 now 的话，解析失败的连接会永远霸占「最近」榜首，把真正新建的挤掉
        #expect(ConnectionRow.parseTimestamp("") == .distantPast)
        #expect(ConnectionRow.parseTimestamp("昨天下午") == .distantPast)
    }

    private func snapshot() -> [[String: Any]] {
        [
            ["id": "a", "chains": ["DIRECT"], "upload": 100, "download": 900,
             "start": "2026-07-31T10:00:00.000000+08:00",
             "metadata": ["host": "old-but-heavy.com", "sourceIP": "192.168.1.50",
                          "network": "tcp"]],
            ["id": "b", "chains": ["🚀 节点选择"], "upload": 1, "download": 2,
             "start": "2026-07-31T12:00:00.000000+08:00",
             "metadata": ["host": "brand-new.com", "sourceIP": "192.168.1.51",
                          "network": "tcp"]],
            ["id": "c", "chains": ["DIRECT"], "upload": 5, "download": 5,
             "start": "2026-07-31T11:00:00.000000+08:00",
             "metadata": ["destinationIP": "1.2.3.4", "sourceIP": "127.0.0.1",
                          "network": "udp"]],
        ]
    }

    @Test("解析:host 缺失时退回目标 IP;sourceIP 读得出来")
    func parsesFields() {
        let rows = ConnectionRow.parse(snapshot())
        #expect(rows.count == 3)
        let c = rows.first { $0.id == "c" }
        #expect(c?.host == "1.2.3.4", "host 为空时应退回 destinationIP")
        #expect(c?.sourceIP == "127.0.0.1")
        #expect(rows.first { $0.id == "b" }?.isProxied == true)
    }

    @Test("★ 「最近」与「用量最多」是两个不同的榜 —— 刚建立的连接往往还没跑量")
    func twoDistinctRankings() {
        let rows = ConnectionRow.parse(snapshot())
        let recent = ConnectionRow.recent(rows, limit: 2).map(\.host)
        let top = ConnectionRow.top(rows, limit: 2).map(\.host)
        #expect(recent.first == "brand-new.com", "最近榜首应是最新建立的:\(recent)")
        #expect(top.first == "old-but-heavy.com", "流量榜首应是跑量最大的:\(top)")
        #expect(recent != top, "两个榜出来一样,说明有一个没按自己的口径排")
    }

    @Test("limit 大于总数时不崩、不补空")
    func limitLargerThanCount() {
        let rows = ConnectionRow.parse(snapshot())
        #expect(ConnectionRow.recent(rows, limit: 99).count == 3)
        #expect(ConnectionRow.top(rows, limit: 99).count == 3)
        #expect(ConnectionRow.recent([], limit: 5).isEmpty)
    }
}

@Suite("连接行的设备标注")
struct ConnectionDeviceLabelTests {

    private func row(sourceIP: String) -> ConnectionRow {
        ConnectionRow.parse([[
            "id": "x", "chains": ["DIRECT"], "upload": 1, "download": 1,
            "start": "2026-07-31T12:00:00.000000+08:00",
            "metadata": ["host": "h", "sourceIP": sourceIP, "network": "tcp"],
        ]]).first!
    }

    private let proxied = [(ip: "192.168.1.50", alias: "客厅电视"),
                           (ip: "192.168.1.51", alias: "")]

    @Test("★ 本机发起的一律留空(每行都标「本机」只是噪音)")
    func localIsBlank() {
        #expect(ConnectionRow.deviceLabel(for: row(sourceIP: "127.0.0.1"), proxied: proxied) == "")
        #expect(ConnectionRow.deviceLabel(for: row(sourceIP: "::1"), proxied: proxied) == "")
        #expect(ConnectionRow.deviceLabel(for: row(sourceIP: ""), proxied: proxied) == "")
    }

    @Test("有别名就显示别名")
    func showsAlias() {
        #expect(ConnectionRow.deviceLabel(for: row(sourceIP: "192.168.1.50"),
                                          proxied: proxied) == "客厅电视")
    }

    @Test("★ 别名为空、或台账里查不到,都退回 IP —— 空着的话这行就没法对应到设备")
    func fallsBackToIP() {
        #expect(ConnectionRow.deviceLabel(for: row(sourceIP: "192.168.1.51"),
                                          proxied: proxied) == "192.168.1.51")
        #expect(ConnectionRow.deviceLabel(for: row(sourceIP: "192.168.1.99"),
                                          proxied: proxied) == "192.168.1.99")
    }
}
