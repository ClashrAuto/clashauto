import Foundation
import Testing
@testable import CoastKit

@Suite("接管目标的安全闸门")
struct RedirectTargetsTests {

    private let gatewayIP = "192.168.1.1"
    private let gatewayMAC = "cc:d8:43:9b:e3:b6"
    private let localMACs: Set<String> = ["a4:83:e7:aa:bb:cc"]

    private func check(ip: String, mac: String) -> RedirectTargets.Rejection? {
        RedirectTargets.rejection(ip: ip, mac: mac, gatewayIP: gatewayIP,
                                  gatewayMAC: gatewayMAC, localMACs: localMACs)
    }

    @Test("普通设备放行")
    func ordinaryDeviceAllowed() {
        #expect(check(ip: "192.168.1.50", mac: "de:ad:be:ef:00:01") == nil)
    }

    @Test("★ 网关不可接管 —— 向路由器发「你自己在我这儿」会打瘫整个局域网")
    func gatewayRejected() {
        #expect(check(ip: gatewayIP, mac: "de:ad:be:ef:00:01") == .isGateway)
        #expect(check(ip: "192.168.1.77", mac: gatewayMAC) == .isGateway,
                "MAC 命中网关也要挡 —— 路由器换了 IP 但 MAC 没变时就是这种情形")
        #expect(check(ip: gatewayIP, mac: gatewayMAC.uppercased()) == .isGateway,
                "MAC 大小写不同就漏过去了")
    }

    @Test("★ 本机不可接管(流量已由 Coast 自己处理,再欺骗自己只会搅乱路由)")
    func localRejected() {
        #expect(check(ip: "192.168.1.9", mac: "A4:83:E7:AA:BB:CC") == .isLocalMachine)
    }

    @Test("★ 离线设备(拿不到 IP)不可接管 —— ARP 欺骗需要目标地址")
    func offlineRejected() {
        #expect(check(ip: "", mac: "de:ad:be:ef:00:01") == .noAddress)
        #expect(check(ip: "192.168.1.50", mac: "") == .noAddress)
    }

    @Test("★ 换网络后台账里的老记录变成了网关 —— 下发前必须再过滤一次")
    func staleLedgerEntryBecomesGateway() {
        // 用户在 A 网把 192.168.1.50 设成代理；换到 B 网后这个地址正是路由器
        let ledger = [(ip: "192.168.1.50", mac: "de:ad:be:ef:00:01"),
                      (ip: "192.168.1.60", mac: "de:ad:be:ef:00:02")]
        let allowed = RedirectTargets.allowed(ledger, gatewayIP: "192.168.1.50",
                                              gatewayMAC: "", localMACs: localMACs)
        #expect(allowed.map(\.ip) == ["192.168.1.60"],
                "老记录指向了新网络的路由器,却还是被下发了 —— 界面那道闸门拦不住这种情况")
    }
}
