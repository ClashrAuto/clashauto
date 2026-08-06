// catch-all 端口改写 —— smoltcp 没有「通配任意目的端口」的监听。
//
// smoltcp 的 IpListenEndpoint 能 addr:None（收任意目的 IP，等价于 lwIP 的 accept-all 补丁），
// 但 port 必须是具体值，没有 lwIP 那种 `lpcb->local_port == 0` 的通配分支
// （third_party/lwip/src/core/tcp_in.c:367 那处 Coast 补丁）。而透明网关必须接住设备发往
// **任意 (IP, 端口)** 的连接。
//
// 做法（bench/stackbench 里已验证，见 docs/lwip-alternatives.md R8/R9）：
//   RX（设备→栈）：把 TCP 目的端口改写成固定端口 FIXED_PORT，并记下
//                  (客户端IP, 客户端端口, 服务IP) → 真实目的端口。
//                  栈那边只挂一个 addr:None, port=FIXED_PORT 的监听就全接住了。
//   查表：连接建立后 local_port 恒为 FIXED_PORT，用上面的映射查回真实目的端口交给 C++ 拨 SOCKS。
//   TX（栈→设备）：把 TCP 源端口从 FIXED_PORT 改回真实目的端口，设备看到的回包就来自真实 (IP,端口)。
//
// 校验和只动了 16 位端口，用 RFC 1624 增量修正，不做全量重算。
//
// ★ 相对 bench 版的两处改动：
//   ① 支持 IPv6（bench 版只有 v4）—— 头文件承诺双栈，且「双栈设备 v6 漏代理」是本项目
//      已知的真实风险（见 NdpSpoofer）。
//   ② 去掉 Arc<Mutex>（单线程，见 coaststack.h 的线程模型），也不再由 phy 层持有 ——
//      改写发生在 FFI 入口/出口，Device 只搬字节。省掉一层共享。

use alloc::collections::BTreeMap;

/// 所有被改写的连接都落到这个本地端口上。取 1 是因为它不可能与真实服务端口冲突。
pub const FIXED_PORT: u16 = 1;

/// (客户端IP, 客户端端口, 服务IP) —— v4 地址左对齐存进 16 字节，v4/v6 共用一张表。
type FlowKey = ([u8; 16], u16, [u8; 16]);

#[derive(Default)]
pub struct PortMap {
    map: BTreeMap<FlowKey, u16>,
}

impl PortMap {
    pub fn new() -> Self {
        Self {
            map: BTreeMap::new(),
        }
    }

    pub fn lookup(&self, cip: &[u8; 16], cport: u16, sip: &[u8; 16]) -> Option<u16> {
        self.map.get(&(*cip, cport, *sip)).copied()
    }

    pub fn forget(&mut self, cip: &[u8; 16], cport: u16, sip: &[u8; 16]) {
        self.map.remove(&(*cip, cport, *sip));
    }

    pub fn len(&self) -> usize {
        self.map.len()
    }
}

// RFC 1624：把校验和里的旧 16 位值换成新值。HC' = ~(~HC + ~old + new)
fn csum_patch(csum: &mut [u8], old: u16, new: u16) {
    let mut sum = (!u16::from_be_bytes([csum[0], csum[1]])) as u32;
    sum += (!old) as u32 & 0xffff;
    sum += new as u32;
    while (sum >> 16) != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    let res = !(sum as u16);
    csum[0] = (res >> 8) as u8;
    csum[1] = (res & 0xff) as u8;
}

/// 解析出 (L4 偏移, 源IP 16字节, 目的IP 16字节)。只认 IPv4/IPv6 的 TCP，其余返回 None。
///
/// IPv6 只处理「无扩展头」的常见情形：next_header 直接是 TCP(6)。带扩展头的包会被放过
/// （不改写），表现为该连接接不住 —— 这是**保守失败**，不会把包改坏。
fn tcp_frame(frame: &[u8]) -> Option<(usize, [u8; 16], [u8; 16])> {
    if frame.len() < 14 {
        return None;
    }
    let ethertype = u16::from_be_bytes([frame[12], frame[13]]);
    let mut src = [0u8; 16];
    let mut dst = [0u8; 16];
    match ethertype {
        0x0800 => {
            if frame.len() < 14 + 20 {
                return None;
            }
            let ihl = ((frame[14] & 0x0f) as usize) * 4;
            if ihl < 20 || frame[14 + 9] != 6 {
                return None; // 非 TCP
            }
            let l4 = 14 + ihl;
            if frame.len() < l4 + 20 {
                return None;
            }
            src[..4].copy_from_slice(&frame[26..30]);
            dst[..4].copy_from_slice(&frame[30..34]);
            Some((l4, src, dst))
        }
        0x86DD => {
            if frame.len() < 14 + 40 {
                return None;
            }
            if frame[14 + 6] != 6 {
                return None; // next_header 不是 TCP（含带扩展头的情形，保守放过）
            }
            let l4 = 14 + 40;
            if frame.len() < l4 + 20 {
                return None;
            }
            src.copy_from_slice(&frame[22..38]);
            dst.copy_from_slice(&frame[38..54]);
            Some((l4, src, dst))
        }
        _ => None,
    }
}

/// RX 改写：真实目的端口 → FIXED_PORT，并记下映射。就地改 `frame`。
/// 非 TCP / 解析不了的帧原样放过。
pub fn rewrite_rx(frame: &mut [u8], map: &mut PortMap) {
    let Some((l4, src, dst)) = tcp_frame(frame) else {
        return;
    };
    let dport = u16::from_be_bytes([frame[l4 + 2], frame[l4 + 3]]);
    if dport == FIXED_PORT {
        return; // 已经是改写后的，别重复处理
    }
    let sport = u16::from_be_bytes([frame[l4], frame[l4 + 1]]);
    map.map.insert((src, sport, dst), dport);

    // TCP 校验和在 L4+16..L4+18
    let mut c = [frame[l4 + 16], frame[l4 + 17]];
    csum_patch(&mut c, dport, FIXED_PORT);
    frame[l4 + 16] = c[0];
    frame[l4 + 17] = c[1];
    frame[l4 + 2] = (FIXED_PORT >> 8) as u8;
    frame[l4 + 3] = (FIXED_PORT & 0xff) as u8;
}

/// TX 改写：源端口 FIXED_PORT → 真实目的端口（查反向映射）。就地改 `frame`。
/// 出方向的 src 是「服务IP」、dst 是「客户端IP」，所以查表时要反过来。
pub fn rewrite_tx(frame: &mut [u8], map: &PortMap) {
    let Some((l4, src, dst)) = tcp_frame(frame) else {
        return;
    };
    let sport = u16::from_be_bytes([frame[l4], frame[l4 + 1]]);
    if sport != FIXED_PORT {
        return;
    }
    let dport = u16::from_be_bytes([frame[l4 + 2], frame[l4 + 3]]); // 客户端端口
                                                                    // key = (客户端IP, 客户端端口, 服务IP)：出方向 src=服务IP、dst=客户端IP
    let Some(real) = map.lookup(&dst, dport, &src) else {
        return;
    };

    let mut c = [frame[l4 + 16], frame[l4 + 17]];
    csum_patch(&mut c, FIXED_PORT, real);
    frame[l4 + 16] = c[0];
    frame[l4 + 17] = c[1];
    frame[l4] = (real >> 8) as u8;
    frame[l4 + 1] = (real & 0xff) as u8;
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;
    use alloc::vec::Vec;

    // 造一个最小的 IPv4/TCP 帧：eth(14) + ip(20) + tcp(20)
    fn v4_frame(sip: [u8; 4], dip: [u8; 4], sport: u16, dport: u16) -> Vec<u8> {
        let mut f = vec![0u8; 14 + 20 + 20];
        f[12] = 0x08;
        f[13] = 0x00;
        f[14] = 0x45; // ver4 ihl5
        f[14 + 9] = 6; // TCP
        f[26..30].copy_from_slice(&sip);
        f[30..34].copy_from_slice(&dip);
        f[34..36].copy_from_slice(&sport.to_be_bytes());
        f[36..38].copy_from_slice(&dport.to_be_bytes());
        f
    }

    fn v6_frame(sport: u16, dport: u16) -> Vec<u8> {
        let mut f = vec![0u8; 14 + 40 + 20];
        f[12] = 0x86;
        f[13] = 0xDD;
        f[14] = 0x60; // ver6
        f[14 + 6] = 6; // next header = TCP
        f[22] = 0xfd; // src fd00::1
        f[37] = 1;
        f[38] = 0x20; // dst 2001:db8::5
        f[39] = 0x01;
        f[53] = 5;
        f[54..56].copy_from_slice(&sport.to_be_bytes());
        f[56..58].copy_from_slice(&dport.to_be_bytes());
        f
    }

    #[test]
    fn v4_roundtrip_restores_original_port() {
        let mut m = PortMap::new();
        let mut rx = v4_frame([10, 0, 0, 2], [93, 184, 216, 34], 51000, 443);
        rewrite_rx(&mut rx, &mut m);
        // 目的端口应已被改成 FIXED_PORT
        assert_eq!(u16::from_be_bytes([rx[36], rx[37]]), FIXED_PORT);
        assert_eq!(m.len(), 1);

        // 回程：src=服务IP:FIXED，dst=客户端IP:客户端端口
        let mut tx = v4_frame([93, 184, 216, 34], [10, 0, 0, 2], FIXED_PORT, 51000);
        rewrite_tx(&mut tx, &m);
        assert_eq!(
            u16::from_be_bytes([tx[34], tx[35]]),
            443,
            "源端口应还原成 443"
        );
    }

    #[test]
    fn v6_roundtrip_restores_original_port() {
        let mut m = PortMap::new();
        let mut rx = v6_frame(51000, 8443);
        rewrite_rx(&mut rx, &mut m);
        assert_eq!(u16::from_be_bytes([rx[56], rx[57]]), FIXED_PORT);
        assert_eq!(m.len(), 1);

        // 造回程帧：src/dst 互换
        let mut tx = vec![0u8; 14 + 40 + 20];
        tx[12] = 0x86;
        tx[13] = 0xDD;
        tx[14] = 0x60;
        tx[14 + 6] = 6;
        tx[22] = 0x20; // src = 原目的 2001:db8::5
        tx[23] = 0x01;
        tx[37] = 5;
        tx[38] = 0xfd; // dst = 原源 fd00::1
        tx[53] = 1;
        tx[54..56].copy_from_slice(&FIXED_PORT.to_be_bytes());
        tx[56..58].copy_from_slice(&51000u16.to_be_bytes());
        rewrite_tx(&mut tx, &m);
        assert_eq!(
            u16::from_be_bytes([tx[54], tx[55]]),
            8443,
            "v6 源端口应还原"
        );
    }

    #[test]
    fn checksum_patch_is_reversible() {
        // 端口改写两次（A→B→A）后校验和必须回到原值 —— 增量修正正确性的直接判据
        let mut c = [0x12u8, 0x34];
        let orig = c;
        csum_patch(&mut c, 443, FIXED_PORT);
        assert_ne!(c, orig);
        csum_patch(&mut c, FIXED_PORT, 443);
        assert_eq!(c, orig, "改回去校验和必须一致");
    }

    #[test]
    fn non_tcp_frames_pass_through_untouched() {
        let mut m = PortMap::new();
        // ARP
        let mut arp = vec![0u8; 60];
        arp[12] = 0x08;
        arp[13] = 0x06;
        let before = arp.clone();
        rewrite_rx(&mut arp, &mut m);
        assert_eq!(arp, before, "ARP 不该被碰");
        assert_eq!(m.len(), 0);

        // UDP over IPv4
        let mut udp = v4_frame([10, 0, 0, 2], [8, 8, 8, 8], 5000, 53);
        udp[14 + 9] = 17; // UDP
        let before = udp.clone();
        rewrite_rx(&mut udp, &mut m);
        assert_eq!(udp, before, "UDP 不该被碰（它走 C++ 侧的手工 NAT）");
        assert_eq!(m.len(), 0);
    }

    #[test]
    fn truncated_frames_never_panic() {
        // panic="abort" 下，任何畸形输入都不能 panic —— 设备是不可信输入源
        let mut m = PortMap::new();
        for n in 0..80usize {
            let mut f = v4_frame([10, 0, 0, 2], [1, 1, 1, 1], 1234, 443);
            f.truncate(n);
            rewrite_rx(&mut f, &mut m);
            rewrite_tx(&mut f, &m);
        }
        for n in 0..80usize {
            let mut f = v6_frame(1234, 443);
            f.truncate(n);
            rewrite_rx(&mut f, &mut m);
            rewrite_tx(&mut f, &m);
        }
    }

    #[test]
    fn tx_without_mapping_is_left_alone() {
        // 没有映射时不能瞎改（否则会把无关连接的端口改坏）
        let m = PortMap::new();
        let mut tx = v4_frame([1, 2, 3, 4], [5, 6, 7, 8], FIXED_PORT, 9999);
        let before = tx.clone();
        rewrite_tx(&mut tx, &m);
        assert_eq!(tx, before);
    }
}
