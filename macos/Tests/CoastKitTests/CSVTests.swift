import Foundation
import Testing
@testable import CoastKit

@Suite("CSV 转义")
struct CSVTests {

    @Test("普通字段原样输出")
    func plain() {
        #expect(CSV.escape("客厅电视") == "客厅电视")
        #expect(CSV.escape("192.168.1.50") == "192.168.1.50")
    }

    @Test("★ 含逗号要加引号 —— 设备别名里带逗号会把整行的列错开")
    func comma() {
        #expect(CSV.escape("电视, 客厅") == "\"电视, 客厅\"")
    }

    @Test("★ 含引号:整段加引号且内部引号翻倍")
    func quote() {
        #expect(CSV.escape("说\"你好\"") == "\"说\"\"你好\"\"\"")
    }

    @Test("★ 含换行也要加引号(否则一条记录会被拆成两行)")
    func newline() {
        #expect(CSV.escape("第一行\n第二行") == "\"第一行\n第二行\"")
        #expect(CSV.escape("回车\r") == "\"回车\r\"")
    }

    @Test("整表:表头 + 每行,末尾各带换行")
    func table() {
        let text = CSV.render(header: ["name", "ip"],
                              rows: [["电视, 客厅", "192.168.1.50"], ["手机", "192.168.1.51"]])
        #expect(text == """
        name,ip
        "电视, 客厅",192.168.1.50
        手机,192.168.1.51

        """)
    }

    @Test("空表也有表头")
    func emptyRows() {
        #expect(CSV.render(header: ["a", "b"], rows: []) == "a,b\n")
    }
}
