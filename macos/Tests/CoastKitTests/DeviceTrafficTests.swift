import Foundation
import Testing

@testable import CoastKit

/// 每设备实时速率 / 会话累计。攒增量这类代码的错法（重复计总数、断线倒扣、
/// 核心重启后回退）在真实网络上极难复现，只能靠喂两拍快照钉死。
struct DeviceTrafficTests {

    private func row(_ id: String, ip: String, up: Int64, down: Int64) -> ConnectionRow {
        ConnectionRow(id: id, host: "example.com", network: "tcp", type: "HTTP", process: "",
                      chain: "🚀", upload: up, download: down,
                      start: .distantPast, sourceIP: ip)
    }

    /// 首次见到的连接，它此前的量一次性计入（就是它的全部）。
    @Test func firstSightCountsEverything() {
        var traffic = DeviceTraffic()
        traffic.observe([row("a", ip: "10.0.0.2", up: 100, down: 900)])
        let sample = traffic.sample(ip: "10.0.0.2")
        #expect(sample.rateUp == 100)
        #expect(sample.rateDown == 900)
        #expect(sample.sessionUp == 100)
        #expect(sample.sessionDown == 900)
    }

    /// ★ 攒的是**增量**，不是每拍重复计一次总数。
    @Test func accumulatesDeltasNotTotals() {
        var traffic = DeviceTraffic()
        traffic.observe([row("a", ip: "10.0.0.2", up: 10, down: 100)])
        traffic.observe([row("a", ip: "10.0.0.2", up: 15, down: 250)])

        let sample = traffic.sample(ip: "10.0.0.2")
        #expect(sample.rateUp == 5)
        #expect(sample.rateDown == 150)
        #expect(sample.sessionUp == 15)      // 不是 10 + 15
        #expect(sample.sessionDown == 250)   // 不是 100 + 250
    }

    /// 同一台设备的多条连接要合到一个桶里。
    @Test func sumsAcrossConnectionsOfTheSameDevice() {
        var traffic = DeviceTraffic()
        traffic.observe([row("a", ip: "10.0.0.2", up: 1, down: 2),
                         row("b", ip: "10.0.0.2", up: 3, down: 4),
                         row("c", ip: "10.0.0.9", up: 100, down: 100)])
        #expect(traffic.sample(ip: "10.0.0.2").sessionDown == 6)
        #expect(traffic.sample(ip: "10.0.0.9").sessionDown == 100)
    }

    /// ★ 核心把计数清零重来（重启后 id 撞车）时按新连接算，**绝不倒扣** ——
    /// 倒扣会算出负速率，界面上就是「↓ -3.00 MB/s」。
    @Test func counterResetIsTreatedAsNewNotAsNegative() {
        var traffic = DeviceTraffic()
        traffic.observe([row("a", ip: "10.0.0.2", up: 0, down: 1000)])
        traffic.observe([row("a", ip: "10.0.0.2", up: 0, down: 5)])
        let sample = traffic.sample(ip: "10.0.0.2")
        #expect(sample.rateDown == 5)
        #expect(sample.sessionDown == 1005)
        #expect(sample.rateDown >= 0)
    }

    /// 这一拍没有流量的设备速率**归零**，而不是维持上一拍的数字 ——
    /// 否则一台早就静默的设备会永远显示着「正在以 3MB/s 下载」。
    @Test func rateDropsToZeroWhenTheDeviceGoesQuiet() {
        var traffic = DeviceTraffic()
        traffic.observe([row("a", ip: "10.0.0.2", up: 0, down: 1000)])
        traffic.observe([])
        let sample = traffic.sample(ip: "10.0.0.2")
        #expect(sample.rateDown == 0)
        #expect(sample.sessionDown == 1000)   // 会话累计保留
    }

    /// 没有 sourceIP 的连接（核心没填）直接跳过，不能归到某个空 IP 的桶里。
    @Test func rowsWithoutSourceIPAreIgnored() {
        var traffic = DeviceTraffic()
        traffic.observe([row("a", ip: "", up: 10, down: 10)])
        #expect(traffic.byIP.isEmpty)
    }
}

extension DeviceTrafficTests {
    /// 历史**每拍给每台已知设备都推一个点**（没流量的推 0）。只给有流量的推的话，
    /// 曲线的横轴就不是时间了 —— 一台间歇跑量的设备会画出一条时间被压缩的假曲线。
    @Test func historyGetsAPointEveryTickEvenWhenIdle() {
        var traffic = DeviceTraffic()
        traffic.observe([row("a", ip: "10.0.0.2", up: 0, down: 100)])
        traffic.observe([])
        traffic.observe([])

        let sample = traffic.sample(ip: "10.0.0.2")
        #expect(sample.downHistory == [100, 0, 0])
        #expect(sample.upHistory.count == 3)
    }

    /// 历史**必须有上限** —— 一台设备挂一晚上就是几万个点，而行里只画得下几十个。
    @Test func historyIsCappedAtHistoryLength() {
        var traffic = DeviceTraffic()
        for _ in 0..<(DeviceTraffic.historyLength + 25) {
            traffic.observe([row("a", ip: "10.0.0.2", up: 0, down: 1)])
        }
        #expect(traffic.sample(ip: "10.0.0.2").downHistory.count == DeviceTraffic.historyLength)
    }
}

@Suite("本机流量的归属")
struct LocalAttributionTests {

    private func row(_ id: String, source: String, up: Int64, down: Int64) -> ConnectionRow {
        ConnectionRow(id: id, host: "x.com", network: "tcp", type: "HTTP", process: "Safari",
                      chain: "US1-HY2", upload: up, download: down,
                      start: .distantPast, sourceIP: source)
    }

    @Test("★ 回环发出的连接归到本机那一行——否则全机器最忙的一台恒显示 0")
    func loopbackFoldsIntoLocalRow() {
        var traffic = DeviceTraffic()
        traffic.observe([row("1", source: "127.0.0.1", up: 10, down: 90)], localIP: "192.168.1.5")
        #expect(traffic.sample(ip: "192.168.1.5").rateDown == 90)
        #expect(traffic.sample(ip: "127.0.0.1").rateDown == 0)
    }

    @Test("★ 开增强模式后本机流量的 sourceIP 是 TUN 的 198.18.x，一样要归到本机")
    func tunAddressFoldsIntoLocalRow() {
        var traffic = DeviceTraffic()
        traffic.observe([row("1", source: "198.18.0.1", up: 5, down: 15)], localIP: "192.168.1.5")
        #expect(traffic.sample(ip: "192.168.1.5").rateUp == 5)
    }

    @Test("局域网设备的流量不受影响，仍按各自的源 IP 归")
    func lanDevicesUnaffected() {
        var traffic = DeviceTraffic()
        traffic.observe([row("1", source: "192.168.1.9", up: 1, down: 2)], localIP: "192.168.1.5")
        #expect(traffic.sample(ip: "192.168.1.9").rateDown == 2)
        #expect(traffic.sample(ip: "192.168.1.5").rateDown == 0)
    }

    @Test("不知道本机 IP 时按原样归（不猜）")
    func withoutLocalIPUnchanged() {
        var traffic = DeviceTraffic()
        traffic.observe([row("1", source: "127.0.0.1", up: 0, down: 7)])
        #expect(traffic.sample(ip: "127.0.0.1").rateDown == 7)
    }
}

@Suite("速率按实测间隔归一化")
struct RateNormalisationTests {

    private func row(_ id: String, up: Int64, down: Int64) -> ConnectionRow {
        ConnectionRow(id: id, host: "x.com", network: "tcp", type: "HTTP", process: "",
                      chain: "US1-HY2", upload: up, download: down,
                      start: .distantPast, sourceIP: "192.168.1.9")
    }

    @Test("★ 轮询晚到时不把 3 秒的量报成 3 倍速率")
    func lateTickDoesNotInflate() {
        var traffic = DeviceTraffic()
        let t0 = Date(timeIntervalSince1970: 1_000)
        traffic.observe([row("1", up: 0, down: 0)], now: t0)
        // 隔了 3 秒才回来，期间跑了 3000 字节 → 1000 B/s，不是 3000
        traffic.observe([row("1", up: 0, down: 3_000)], now: t0.addingTimeInterval(3))
        #expect(traffic.sample(ip: "192.168.1.9").rateDown == 1_000)
    }

    @Test("累计量不除时间——那是「一共跑了多少」，与间隔无关")
    func sessionTotalsAreRaw() {
        var traffic = DeviceTraffic()
        let t0 = Date(timeIntervalSince1970: 1_000)
        traffic.observe([row("1", up: 0, down: 0)], now: t0)
        traffic.observe([row("1", up: 0, down: 3_000)], now: t0.addingTimeInterval(3))
        #expect(traffic.sample(ip: "192.168.1.9").sessionDown == 3_000)
    }

    @Test("正常一秒一拍时与原来一致")
    func oneSecondTickUnchanged() {
        var traffic = DeviceTraffic()
        let t0 = Date(timeIntervalSince1970: 1_000)
        traffic.observe([row("1", up: 0, down: 0)], now: t0)
        traffic.observe([row("1", up: 0, down: 500)], now: t0.addingTimeInterval(1))
        #expect(traffic.sample(ip: "192.168.1.9").rateDown == 500)
    }

    @Test("同一拍被喂两次不会除出天文数字")
    func zeroIntervalIsSafe() {
        var traffic = DeviceTraffic()
        let t0 = Date(timeIntervalSince1970: 1_000)
        traffic.observe([row("1", up: 0, down: 0)], now: t0)
        traffic.observe([row("1", up: 0, down: 100)], now: t0)
        #expect(traffic.sample(ip: "192.168.1.9").rateDown == 100)
    }
}

@Suite("曲线的点数上限")
struct HistoryLengthTests {

    @Test("★ 42 = 40 可见 + 2 富余，与 QML 的 maxPointer 同值——少一个横轴跨度就对不上")
    func matchesQtPointCount() {
        #expect(DeviceTraffic.historyLength == 42)
    }

    @Test("超过上限时丢最旧的，长度封死")
    func trimsOldest() {
        var traffic = DeviceTraffic()
        let t0 = Date(timeIntervalSince1970: 1_000)
        for tick in 0..<60 {
            traffic.observe([ConnectionRow(id: "1", host: "x", network: "tcp", type: "",
                                           process: "", chain: "P",
                                           upload: 0, download: Int64(tick) * 100,
                                           start: .distantPast, sourceIP: "192.168.1.9")],
                            now: t0.addingTimeInterval(Double(tick)))
        }
        #expect(traffic.sample(ip: "192.168.1.9").downHistory.count == 42)
    }
}
