import Foundation
import Testing
@testable import CoastHelperProtocol

@Suite("ARP 报文构造")
struct ARPPacketTests {

    @Test("MAC 解析与回写")
    func macRoundTrip() {
        let mac = ARPPacket.MAC("3c:84:6a:01:02:03")
        #expect(mac?.bytes == [0x3c, 0x84, 0x6a, 0x01, 0x02, 0x03])
        #expect(mac?.text == "3c:84:6a:01:02:03")
        #expect(ARPPacket.MAC("bad") == nil)
        #expect(ARPPacket.MAC("3c:84:6a:01:02") == nil)
    }

    @Test("IPv4 解析")
    func ipv4() {
        #expect(ARPPacket.ipv4Bytes("192.168.1.1") == [192, 168, 1, 1])
        #expect(ARPPacket.ipv4Bytes("10.0.0.255") == [10, 0, 0, 255])
        #expect(ARPPacket.ipv4Bytes("1.2.3") == nil)
        #expect(ARPPacket.ipv4Bytes("256.1.1.1") == nil)
    }

    @Test("应答帧总长 42 字节，以太头 + ARP 结构正确")
    func replyStructure() {
        let sender = ARPPacket.MAC("aa:aa:aa:aa:aa:aa")!
        let target = ARPPacket.MAC("bb:bb:bb:bb:bb:bb")!
        let frame = ARPPacket.reply(senderMAC: sender, senderIP: [192, 168, 1, 1],
                                    targetMAC: target, targetIP: [192, 168, 1, 50])
        #expect(frame.count == 42)
        // 以太头：目的 MAC(0..6) = target，源 MAC(6..12) = sender，EtherType(12..14) = 0x0806
        #expect(Array(frame[0..<6]) == target.bytes)
        #expect(Array(frame[6..<12]) == sender.bytes)
        #expect(Array(frame[12..<14]) == [0x08, 0x06])
        // ARP 操作码在偏移 20..22，应答 = 0x0002
        #expect(Array(frame[20..<22]) == [0x00, 0x02])
        // 发送方 IP 在偏移 28..32
        #expect(Array(frame[28..<32]) == [192, 168, 1, 1])
        // 目标 IP 在偏移 38..42
        #expect(Array(frame[38..<42]) == [192, 168, 1, 50])
    }

    @Test("★ 单播：目的 MAC 必须是目标设备本人，绝不是广播")
    func mustUnicastNotBroadcast() {
        // 这是安全命门。发广播 ARP 会被整个局域网上缓存里有网关条目的设备处理 ——
        // 用户只选了一台，却把全网流量都引过来。目的 MAC(以太头 0..6) 必须是目标设备。
        let self_ = ARPPacket.MAC("aa:aa:aa:aa:aa:aa")!
        let device = ARPPacket.MAC("dd:dd:dd:dd:dd:dd")!
        let frame = ARPPacket.reply(senderMAC: self_, senderIP: [192, 168, 1, 1],
                                    targetMAC: device, targetIP: [192, 168, 1, 50])
        #expect(Array(frame[0..<6]) == device.bytes)                 // 目的 = 设备本人
        #expect(Array(frame[0..<6]) != ARPPacket.MAC.broadcast.bytes) // 不是广播
        // ARP 载荷里的 target hardware address(偏移 32..38) 也是设备本人
        #expect(Array(frame[32..<38]) == device.bytes)
    }

    // 手拼一帧 ARP 请求（who-has targetIP，tell senderIP/senderMAC），供解析测试。
    private func makeRequest(senderMAC: ARPPacket.MAC, senderIP: [UInt8], targetIP: [UInt8]) -> [UInt8] {
        var f = [UInt8](repeating: 0, count: 42)
        for i in 0..<6 { f[i] = 0xff; f[6 + i] = senderMAC.bytes[i] }   // 以太：dst 广播、src 设备
        f[12] = 0x08; f[13] = 0x06                                       // EtherType ARP
        f[14] = 0x00; f[15] = 0x01; f[16] = 0x08; f[17] = 0x00           // htype 以太 / ptype IPv4
        f[18] = 6; f[19] = 4; f[20] = 0x00; f[21] = 0x01                 // hlen/plen / op=request
        for i in 0..<6 { f[22 + i] = senderMAC.bytes[i] }               // sha
        for i in 0..<4 { f[28 + i] = senderIP[i] }                      // spa
        for i in 0..<4 { f[38 + i] = targetIP[i] }                      // tpa
        return f
    }

    @Test("★ 解析 who-has 网关的 ARP 请求（v4 抢答用）")
    func parseRequest() {
        let device = ARPPacket.MAC("dd:dd:dd:dd:dd:dd")!
        let req = makeRequest(senderMAC: device, senderIP: [192, 168, 31, 168], targetIP: [192, 168, 31, 1])
        // 问的正是网关 → 解析出请求方 IP/MAC
        let r = ARPPacket.parseRequest(req, targetIP: [192, 168, 31, 1])
        #expect(r?.senderIP == [192, 168, 31, 168])
        #expect(r?.senderMAC == device)
        // 问的不是网关 → nil
        #expect(ARPPacket.parseRequest(req, targetIP: [192, 168, 31, 9]) == nil)
        // 一帧 ARP 应答（op=2）不是请求 → nil
        let reply = ARPPacket.reply(senderMAC: device, senderIP: [192, 168, 31, 1],
                                    targetMAC: device, targetIP: [192, 168, 31, 168])
        #expect(ARPPacket.parseRequest(reply, targetIP: [192, 168, 31, 1]) == nil)
    }

    @Test("★ 复原帧：sender = 真网关 MAC，把设备的 ARP 缓存改回去")
    func restoreFrameUsesRealGateway() {
        // 这是整个功能的命门：复原发的是「网关 IP → 真网关 MAC」，
        // 发错了（比如还发本机 MAC）设备就永久断网。
        let realGateway = ARPPacket.MAC("cc:cc:cc:cc:cc:cc")!
        let device = ARPPacket.MAC("dd:dd:dd:dd:dd:dd")!
        let frame = ARPPacket.reply(senderMAC: realGateway, senderIP: [192, 168, 1, 1],
                                    targetMAC: device, targetIP: [192, 168, 1, 50])
        // 发送方 MAC（ARP 载荷偏移 22..28）必须是真网关，不是本机
        #expect(Array(frame[22..<28]) == realGateway.bytes)
        // 发送方 IP 是网关 IP
        #expect(Array(frame[28..<32]) == [192, 168, 1, 1])
    }
}
