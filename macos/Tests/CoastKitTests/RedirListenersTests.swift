import Foundation
import Testing
@testable import CoastKit

private final class TempDir {
    let directory: URL
    let store: DeviceStore
    init() {
        directory = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("coast-redir-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        store = DeviceStore(configDir: directory)
    }
    deinit { try? FileManager.default.removeItem(at: directory) }
}

/// 每网卡一个 redir 入站（多网卡时「设备从它自己那条上行出去」的配置面）。
///
/// 为什么值得单测：这段一旦写坏，核心是**整份配置拒绝加载**（代理整个起不来），而 YAML 是
/// 手搓字符串、编译期一点都发现不了。另外「单网卡逐字节不变」是绝大多数用户走的那一档，
/// 必须钉住。
@Suite("每网卡一个 redir 入站")
struct RedirListenersTests {

    private let base = """
    proxies: []
    proxy-groups:
      - name: '🚀 节点选择'
        type: select
        proxies:
          - DIRECT
    rules:
      - 'MATCH,🚀 节点选择'
    """

    /// 两台设备分属两张卡：en0（主）与 en1（副）。
    ///
    /// ★ 网卡归属必须经 `recordSeen` 写：`save()` **故意不动身份列**（hostname/vendor/model/
    ///   interface/last_seen 归扫描侧管，见 DeviceStore.save 的注释）。第一版夹具直接
    ///   `d.interface = …; save(d)`，于是两台设备的 interface 都是空串、双双归到主网卡，
    ///   测试报的"少了一个 listener"其实是夹具没写进去。
    private func seed(_ temp: TempDir) {
        for (mac, ip, iface) in [
            ("aa:bb:cc:dd:ee:01", "192.168.20.11", "en0"),
            ("aa:bb:cc:dd:ee:02", "192.168.31.12", "en1"),
        ] {
            _ = temp.store.recordSeen([LanBrowser.Device(mac: mac, ip: ip, interface: iface)])
            _ = temp.store.setProxyEnabled(mac: mac, true, ip: ip)
        }
    }

    @Test("单网卡：一个 listener 都不发（这一档必须与改动前逐字节相同）")
    func singleNicUnchanged() {
        let temp = TempDir()
        seed(temp)
        var builder = ConfigBuilder(config: AppConfig(), directory: temp.directory)
        builder.egressNics = ["en0"]
        #expect(builder.applyRedirListeners(base) == base)
    }

    @Test("没有网卡表时也什么都不发（拓扑还没探到的窗口）")
    func noNicsNoListeners() {
        let temp = TempDir()
        seed(temp)
        let builder = ConfigBuilder(config: AppConfig(), directory: temp.directory)
        #expect(builder.applyRedirListeners(base) == base)
    }

    @Test("没有设备开代理时不发（两张卡也一样）")
    func noDevicesNoListeners() {
        let temp = TempDir()
        var builder = ConfigBuilder(config: AppConfig(), directory: temp.directory)
        builder.egressNics = ["en0", "en1"]
        #expect(builder.applyRedirListeners(base) == base)
    }

    @Test("两张卡：各发一个 redir 入站，端口向下编号，各绑各卡")
    func perNicListeners() {
        let temp = TempDir()
        seed(temp)
        var builder = ConfigBuilder(config: AppConfig(), directory: temp.directory)
        builder.egressNics = ["en0", "en1"]
        let out = builder.applyRedirListeners(base)
        #expect(out.contains("""
          - name: coast-redir-0
            type: redir
            listen: 127.0.0.1
            port: 7893
            interface-name: 'en0'
        """))
        #expect(out.contains("""
          - name: coast-redir-1
            type: redir
            listen: 127.0.0.1
            port: 7892
            interface-name: 'en1'
        """))
        // 原文一个字都不能少（我们是追加，不是重写）。
        #expect(out.hasPrefix(base))
    }

    // 台账里 interface 为空、或那张卡不在网卡表里 → 归主网卡。宁可走默认出口，也不能没人接。
    @Test("归不出网卡的设备算主网卡，不会凭空多出一个入站")
    func unknownInterfaceFallsBackToPrimary() {
        let temp = TempDir()
        // 一台台账里还没记到 interface（扫描没覆盖到），一台的 interface 不在网卡表里。
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:03", true, ip: "192.168.20.13")
        _ = temp.store.recordSeen([LanBrowser.Device(mac: "aa:bb:cc:dd:ee:04", ip: "10.0.0.4",
                                                     interface: "utun9")])
        _ = temp.store.setProxyEnabled(mac: "aa:bb:cc:dd:ee:04", true, ip: "10.0.0.4")
        var builder = ConfigBuilder(config: AppConfig(), directory: temp.directory)
        builder.egressNics = ["en0", "en1"]
        let out = builder.applyRedirListeners(base)
        #expect(out.contains("coast-redir-0"))
        #expect(!out.contains("coast-redir-1"))
    }
}
