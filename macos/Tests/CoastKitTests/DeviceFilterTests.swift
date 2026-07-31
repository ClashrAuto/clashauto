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
