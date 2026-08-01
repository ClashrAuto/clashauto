import Foundation
import Network

/// 局域网设备的**型号**（`iPhone15,2` / `iMac21,1` / `AppleTV6,2` 这一类）。
///
/// Qt 版从原始 mDNS 报文里抠 TXT 的 `model=` / `md=`，再兼一路 UPnP 的 `<modelName>`
/// （`LanScanner.cpp:959` 与 `:1040`），详情窗的「型号」一行就是它。macOS 上不必自己
/// 拆 DNS 报文：Bonjour 就是 mDNS，`NWBrowser` 把 TXT 记录连着浏览结果一起给出来 ——
/// 同一件事的原生答案，几十行而不是几百行。
///
/// ## 认哪些服务
///
/// Qt 是**嗅探**，任何服务的 TXT 里带 `model=` 都会被它抠到；`NWBrowser` 只能按类型浏览，
/// 所以这里列举几个真正带型号的：`_device-info`（Apple 的「设备信息」）、
/// `_airplay` / `_raop`（AirPlay，`model=` / `am=`）、`_googlecast`（`md=`）。
/// 实测本机这台 iMac 没有广播 `_device-info`，但 `_airplay` 的 TXT 里是
/// `model=iMac21,1` —— 只认一个服务会漏掉一大片。
///
/// ## 怎么和台账里的设备对上号
///
/// Bonjour 给的是**服务名**（如「王超's iMac」），既不是 IP 也不一定是主机名。两把钥匙一起用：
///
/// 1. **`deviceid`** —— AirPlay 的 TXT 里带，多数设备就是它的网卡 MAC，而 MAC 正是
///    台账的主键，对上就是准的；
/// 2. **服务名** —— 退而求其次，和反查出来的主机名比。
///
/// 两把都对不上就显示「-」，与 Qt 那边没人广播时的表现一致。
public final class DeviceModelBrowser: @unchecked Sendable {

    public static let shared = DeviceModelBrowser()

    /// 带型号的服务类型。见类型注释：只认 `_device-info` 会漏掉一大片。
    static let serviceTypes = ["_device-info._tcp", "_airplay._tcp",
                               "_raop._tcp", "_googlecast._tcp"]

    /// MAC（小写冒号分隔）→ 型号。
    private var byMAC: [String: String] = [:]
    /// 名字（小写、去掉 `.local` 后缀）→ 型号。
    private var byName: [String: String] = [:]
    private let lock = NSLock()
    private var browsers: [NWBrowser] = []

    private init() {}

    /// 开始监听。反复调用只会启动一次。
    public func start() {
        lock.lock()
        defer { lock.unlock() }
        guard browsers.isEmpty else { return }

        for type in Self.serviceTypes {
            // `.bonjourWithTXTRecord` 而不是 `.bonjour`：后者只报「有这个服务」，
            // TXT 得再 resolve 一次才拿得到，而型号恰恰只在 TXT 里。
            let descriptor = NWBrowser.Descriptor.bonjourWithTXTRecord(type: type, domain: nil)
            let browser = NWBrowser(for: descriptor, using: .tcp)
            browser.browseResultsChangedHandler = { [weak self] results, _ in
                self?.absorb(results)
            }
            // 网络没起来或权限没给时 NWBrowser 会进 .failed，不重试 —— 型号只是锦上添花，
            // 拿不到就那一格显示 "-"，不该为它反复重连。
            browser.start(queue: .global(qos: .utility))
            browsers.append(browser)
        }
    }

    private func absorb(_ results: Set<NWBrowser.Result>) {
        var macs: [String: String] = [:]
        var names: [String: String] = [:]
        for result in results {
            guard case let .service(name, _, _, _) = result.endpoint,
                  case let .bonjour(record) = result.metadata,
                  let model = Self.model(in: record)
            else { continue }
            names[Self.key(name)] = model
            if let mac = Self.deviceMAC(in: record) { macs[mac] = model }
        }
        lock.lock()
        // 合并而不是覆盖：设备睡过去之后就不再广播，上一轮认到的型号不该跟着消失。
        for (key, value) in macs { byMAC[key] = value }
        for (key, value) in names { byName[key] = value }
        lock.unlock()
    }

    /// TXT 里的型号。`model` 是 Apple 的写法，`md` 是 Chromecast/HAP 那一路的简写
    /// （Qt 认的正是这两个），`am` 是 AirPlay 自己的字段。
    static func model(in record: NWTXTRecord) -> String? {
        for key in ["model", "md", "am"] {
            if let value = record[key], !value.trimmingCharacters(in: .whitespaces).isEmpty {
                return value
            }
        }
        return nil
    }

    /// TXT 里的 `deviceid`，规范成台账那套写法（小写、冒号分隔、每段补零）。
    static func deviceMAC(in record: NWTXTRecord) -> String? {
        guard let raw = record["deviceid"] else { return nil }
        let parts = raw.split(separator: ":")
        guard parts.count == 6 else { return nil }
        return parts.map { String(format: "%02x", UInt8($0, radix: 16) ?? 0) }.joined(separator: ":")
    }

    /// 连接键：小写、去掉 `.local`/`.local.` 后缀。
    static func key(_ name: String) -> String {
        var text = name.lowercased()
        for suffix in [".local.", ".local"] where text.hasSuffix(suffix) {
            text.removeLast(suffix.count)
            break
        }
        return text
    }

    /// 这台设备的型号。两把钥匙都对不上就是空串。
    public func model(mac: String, hostname: String) -> String {
        lock.lock()
        defer { lock.unlock() }
        if !mac.isEmpty, let model = byMAC[mac.lowercased()] { return model }
        if !hostname.isEmpty, let model = byName[Self.key(hostname)] { return model }
        return ""
    }
}
