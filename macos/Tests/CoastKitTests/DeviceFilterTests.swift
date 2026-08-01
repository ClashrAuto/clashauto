import Foundation
import Testing
@testable import CoastKit

@Suite("设备搜索")
struct DeviceFilterTests {

    private let tv = DeviceFilter.haystack(alias: "客厅电视", hostname: "living-tv.lan",
                                           vendor: "Xiaomi", ip: "192.168.1.50",
                                           mac: "a4:83:e7:11:22:33")

    @Test("空关键词一律命中(等于不过滤)")
    func emptyMatchesAll() {
        #expect(DeviceFilter.matches(keyword: "", fields: tv))
        #expect(DeviceFilter.matches(keyword: "   ", fields: tv))
    }

    @Test("别名 / 主机名 / 厂商 / IP 都能搜")
    func searchesAllFields() {
        for keyword in ["客厅", "living", "Xiaomi", "192.168.1.5"] {
            #expect(DeviceFilter.matches(keyword: keyword, fields: tv), "搜不到「\(keyword)」")
        }
    }

    @Test("★ 不区分大小写(厂商名的大小写各家不一)")
    func caseInsensitive() {
        #expect(DeviceFilter.matches(keyword: "xiaomi", fields: tv))
        #expect(DeviceFilter.matches(keyword: "XIAOMI", fields: tv))
    }

    @Test("★ MAC 带不带分隔符都要搜得到 —— 从路由器后台复制过来的写法有三种")
    func macSeparatorAgnostic() {
        for keyword in ["a4:83:e7", "a4-83-e7", "a483e7", "A483E7"] {
            #expect(DeviceFilter.matches(keyword: keyword, fields: tv),
                    "MAC 写成「\(keyword)」就搜不到了")
        }
    }

    @Test("不相干的关键词不命中")
    func missesUnrelated() {
        #expect(DeviceFilter.matches(keyword: "卧室", fields: tv) == false)
        #expect(DeviceFilter.matches(keyword: "10.0.0.1", fields: tv) == false)
    }

    @Test("★ 空字段不参与匹配(否则空别名会让任意单字符关键词命中所有设备)")
    func emptyFieldsIgnored() {
        let bare = DeviceFilter.haystack(alias: "", hostname: "", vendor: "",
                                         ip: "192.168.1.7", mac: "")
        #expect(DeviceFilter.matches(keyword: "x", fields: bare) == false)
        #expect(DeviceFilter.matches(keyword: "192.168.1.7", fields: bare))
    }
}

@Suite("设备列表次序")
struct DeviceOrderingTests {

    private func sorted(_ items: [(online: Bool, ip: String, mac: String, bytes: Int64)])
        -> [String] {
        items.sorted {
            DeviceOrdering.key(online: $0.online, ip: $0.ip, mac: $0.mac, todayBytes: $0.bytes)
                < DeviceOrdering.key(online: $1.online, ip: $1.ip, mac: $1.mac, todayBytes: $1.bytes)
        }.map(\.mac)
    }

    @Test("在线的一律排在离线的前面——哪怕离线那台今天跑得更多")
    func onlineFirst() {
        let order = sorted([
            (false, "", "bb", 900_000_000),
            (true, "192.168.1.9", "aa", 0),
        ])
        #expect(order == ["aa", "bb"])
    }

    @Test("★ 今日流量按 MB 取档降序——同档位内不因为差几百字节就互换")
    func bucketedByMegabyte() {
        // 两台差 100 字节：同一个 MB 档 → 不比流量，改按 IP 定序
        let sameBucket = sorted([
            (true, "192.168.1.20", "bb", 5_000_100),
            (true, "192.168.1.10", "aa", 5_000_000),
        ])
        #expect(sameBucket == ["aa", "bb"])
        // 差一个 MB 档：流量大的在前，IP 再大也压不过
        let differentBucket = sorted([
            (true, "192.168.1.10", "aa", 1_000_000),
            (true, "192.168.1.20", "bb", 9_000_000),
        ])
        #expect(differentBucket == ["bb", "aa"])
    }

    @Test("IP 按数值排，不是按字符串——.2 要排在 .10 前面")
    func ipIsNumeric() {
        #expect(sorted([
            (true, "192.168.1.10", "bb", 0),
            (true, "192.168.1.2", "aa", 0),
        ]) == ["aa", "bb"])
    }

    @Test("★ 全部相同时用 MAC 兜底——次序必须是确定的（离线行原来是字典遍历顺序）")
    func macBreaksTies() {
        #expect(sorted([
            (false, "", "cc", 0),
            (false, "", "aa", 0),
            (false, "", "bb", 0),
        ]) == ["aa", "bb", "cc"])
    }
}

@Suite("连接归属到哪台设备")
struct ConnectionBelongsTests {

    @Test("普通设备：源 IP 相同才算")
    func plainMatch() {
        #expect(DeviceStore.connectionBelongs(sourceIP: "192.168.1.9", deviceIP: "192.168.1.9",
                                              isLocalMachine: false))
        #expect(!DeviceStore.connectionBelongs(sourceIP: "192.168.1.8", deviceIP: "192.168.1.9",
                                               isLocalMachine: false))
    }

    @Test("★ 本机那一行要认回环与 TUN 地址——只比局域网 IP 的话它永远匹配不到任何连接")
    func localMachineAcceptsLoopbackAndTUN() {
        #expect(DeviceStore.connectionBelongs(sourceIP: "127.0.0.1", deviceIP: "192.168.1.5",
                                              isLocalMachine: true))
        #expect(DeviceStore.connectionBelongs(sourceIP: "198.18.0.1", deviceIP: "192.168.1.5",
                                              isLocalMachine: true))
        // 不是本机那一行就不能借这条路认领别人的流量
        #expect(!DeviceStore.connectionBelongs(sourceIP: "127.0.0.1", deviceIP: "192.168.1.9",
                                               isLocalMachine: false))
    }

    @Test("源地址为空一律不算")
    func emptySourceNeverMatches() {
        #expect(!DeviceStore.connectionBelongs(sourceIP: "", deviceIP: "192.168.1.5",
                                               isLocalMachine: true))
    }
}
