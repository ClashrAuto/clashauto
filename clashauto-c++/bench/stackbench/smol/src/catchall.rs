// smoltcp 的 catch-all 端口 —— 不 fork smoltcp，在 phy 层加一个端口改写 shim。
//
// 背景（见 docs/lwip-alternatives.md R5）：smoltcp 的 IpListenEndpoint 能 addr:None
// （任意目的 IP），但 port 必须是具体值，没有 lwIP 那种 local_port==0 的通配分支。
// 透明网关要接住设备发往**任意 (IP,端口)** 的连接，这是刚需。
//
// 这个 shim 的做法：
//   RX（设备→栈）：把 TCP 目的端口改写成一个固定端口 FIXED，记下 (客户端IP,客户端端口,服务IP)
//                  → 原始目的端口。栈这边只挂一个 addr:None, port=FIXED 的监听就全接住了。
//   forwarder：连接的 local_port 恒为 FIXED，用上面的映射查回真实目的端口，据此连上游。
//   TX（栈→设备）：把 TCP 源端口从 FIXED 改回原始目的端口，设备看到的回包就来自真实 (IP,端口)。
//
// 校验和：只动了 16 位端口，用 RFC 1624 增量修正，不做全量重算。
//
// 这样单个 smoltcp 监听 socket 就等价于 lwIP 的 accept-all，且**零 fork**。
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use smoltcp::phy::{Device, DeviceCapabilities, RxToken, TxToken};
use smoltcp::time::Instant;

pub const FIXED_PORT: u16 = 1;

// key = (client_ip, client_port, server_ip)  ->  真实目的端口
pub type PortMap = Arc<Mutex<HashMap<(u32, u16, u32), u16>>>;

fn u32_ip(b: &[u8]) -> u32 {
    ((b[0] as u32) << 24) | ((b[1] as u32) << 16) | ((b[2] as u32) << 8) | (b[3] as u32)
}

// RFC 1624：把校验和里旧 16 位值换成新值。HC' = ~(~HC + ~old + new)
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

// 返回 (ihl 起始的 L4 偏移, 是不是 TCP)。只认无选项/有选项的 IPv4+TCP，其它一律放过。
fn tcp_offsets(frame: &[u8]) -> Option<usize> {
    if frame.len() < 14 + 20 { return None; }
    if frame[12] != 0x08 || frame[13] != 0x00 { return None; } // 非 IPv4
    let ihl = ((frame[14] & 0x0f) as usize) * 4;
    if ihl < 20 { return None; }
    if frame[14 + 9] != 6 { return None; }                      // 非 TCP
    let l4 = 14 + ihl;
    if frame.len() < l4 + 20 { return None; }
    Some(l4)
}

// RX 改写：dst_port(真实) → FIXED，并记录映射。就地改 buf。
fn rewrite_rx(buf: &mut [u8], map: &PortMap) {
    let l4 = match tcp_offsets(buf) { Some(x) => x, None => return };
    let dport = u16::from_be_bytes([buf[l4 + 2], buf[l4 + 3]]);
    if dport == FIXED_PORT { return; }
    let sip = u32_ip(&buf[26..30]);
    let dip = u32_ip(&buf[30..34]);
    let sport = u16::from_be_bytes([buf[l4], buf[l4 + 1]]);
    map.lock().unwrap().insert((sip, sport, dip), dport);
    // TCP 校验和随端口改写增量修正
    let (a, b) = buf.split_at_mut(l4 + 16);
    csum_patch(&mut b[..2], dport, FIXED_PORT); // b 从 l4+16 起即 checksum 字段
    let _ = a;
    buf[l4 + 2] = (FIXED_PORT >> 8) as u8;
    buf[l4 + 3] = (FIXED_PORT & 0xff) as u8;
}

// TX 改写：src_port FIXED → 真实 dst_port（查反向映射）。就地改 buf。
fn rewrite_tx(buf: &mut [u8], map: &PortMap) {
    let l4 = match tcp_offsets(buf) { Some(x) => x, None => return };
    let sport = u16::from_be_bytes([buf[l4], buf[l4 + 1]]);
    if sport != FIXED_PORT { return; }
    // 出方向：src=服务IP:FIXED dst=客户端IP:客户端端口。映射 key 是 (客户端IP,客户端端口,服务IP)
    let sip = u32_ip(&buf[26..30]); // 服务 IP
    let dip = u32_ip(&buf[30..34]); // 客户端 IP
    let dport = u16::from_be_bytes([buf[l4 + 2], buf[l4 + 3]]); // 客户端端口
    let real = match map.lock().unwrap().get(&(dip, dport, sip)).copied() {
        Some(p) => p,
        None => return,
    };
    csum_patch_at(buf, l4 + 16, FIXED_PORT, real);
    buf[l4] = (real >> 8) as u8;
    buf[l4 + 1] = (real & 0xff) as u8;
}

fn csum_patch_at(buf: &mut [u8], off: usize, old: u16, new: u16) {
    let mut c = [buf[off], buf[off + 1]];
    csum_patch(&mut c, old, new);
    buf[off] = c[0];
    buf[off + 1] = c[1];
}

// ---- Device 包装 ----
pub struct CatchAll<D: Device> {
    inner: D,
    map: PortMap,
}

impl<D: Device> CatchAll<D> {
    pub fn new(inner: D, map: PortMap) -> Self { Self { inner, map } }
}

// 把底层 TAP 的 fd 透出来，供 poll 用——不必再开第二个 fd（第二个 fd 会和 smoltcp
// 抢同一队列的包，读走的帧另一个 fd 就看不到，poll 会漏事件）。
impl<D: Device + std::os::unix::io::AsRawFd> std::os::unix::io::AsRawFd for CatchAll<D> {
    fn as_raw_fd(&self) -> std::os::unix::io::RawFd { self.inner.as_raw_fd() }
}

pub struct CaRx<T: RxToken> { tok: T, map: PortMap }
pub struct CaTx<T: TxToken> { tok: T, map: PortMap }

impl<T: RxToken> RxToken for CaRx<T> {
    fn consume<R, F: FnOnce(&[u8]) -> R>(self, f: F) -> R {
        let map = self.map;
        // smoltcp 0.12 的 RxToken::consume 给的是 &[u8]（不可变），要就地改必须先拷出来
        self.tok.consume(|frame| {
            let mut owned = frame.to_vec();
            rewrite_rx(&mut owned, &map);
            f(&owned)
        })
    }
}

impl<T: TxToken> TxToken for CaTx<T> {
    fn consume<R, F: FnOnce(&mut [u8]) -> R>(self, len: usize, f: F) -> R {
        let map = self.map;
        self.tok.consume(len, |buf| {
            let r = f(buf);          // 先让 smoltcp 把包写进 buf
            rewrite_tx(buf, &map);   // 再把 FIXED 改回真实端口
            r
        })
    }
}

impl<D: Device> Device for CatchAll<D> {
    type RxToken<'a> = CaRx<D::RxToken<'a>> where Self: 'a;
    type TxToken<'a> = CaTx<D::TxToken<'a>> where Self: 'a;

    fn receive(&mut self, ts: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        let map = self.map.clone();
        self.inner.receive(ts).map(|(r, t)| {
            (CaRx { tok: r, map: map.clone() }, CaTx { tok: t, map })
        })
    }
    fn transmit(&mut self, ts: Instant) -> Option<Self::TxToken<'_>> {
        let map = self.map.clone();
        self.inner.transmit(ts).map(|t| CaTx { tok: t, map })
    }
    fn capabilities(&self) -> DeviceCapabilities { self.inner.capabilities() }
}
