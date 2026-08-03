// R14：UDP 走「裸 NAT」彻底绕开 smoltcp —— UDP 无连接，根本不需要协议栈的状态机。
// 很多 tun2socks 就是 TCP 走栈、UDP 走一张 NAT 表。R13 里 smoltcp 的 UDP socket 慢 1.5×
// （双拷贝 + 逐报 metadata + dispatch），这版把 UDP 从 smoltcp 里整个拿掉，看能不能追平/
// 反超 lwIP。TCP 仍照常交给 smoltcp。
//
// NatTap 是个 Device：receive() 里先把 UDP 帧截下来自己 NAT（转内核 socket、回包造帧直接
// 写 TAP），只把 TCP 帧交给 smoltcp。TX 侧沿用端口改写 shim（只对 TCP 生效）。
use std::collections::HashMap;
use std::net::UdpSocket;
use std::os::unix::io::{AsRawFd, RawFd};

use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::time::Instant;

#[path = "catchall.rs"]
mod ca_shared;
pub use ca_shared::{PortMap, FIXED_PORT};
use ca_shared::{rewrite_rx_pub as rewrite_rx, rewrite_tx_pub as rewrite_tx};

const MTU: usize = 1500;
const FRAME: usize = MTU + 14;
const UDP_PORT: u16 = 5203;

#[inline] fn be16(b: &[u8]) -> u16 { ((b[0] as u16) << 8) | b[1] as u16 }
#[inline] fn ones_sum(data: &[u8], mut sum: u32) -> u32 {
    let mut i = 0;
    while i + 1 < data.len() { sum += be16(&data[i..]) as u32; i += 2; }
    if i < data.len() { sum += (data[i] as u32) << 8; }
    sum
}
#[inline] fn fold(mut s: u32) -> u16 { while (s >> 16) != 0 { s = (s & 0xffff) + (s >> 16); } !(s as u16) }

// NAT 流键：客户端 (IP,port)。值：内核 socket + 造回包所需的一切。
struct Flow {
    sock: UdpSocket,
    cmac: [u8; 6],   // 客户端 MAC（回包 eth 目的）
    cip: [u8; 4],    // 客户端 IP
    cport: u16,
    dip: [u8; 4],    // 原始目的 IP（回包源）
    dport: u16,      // 原始目的端口（回包源端口）
}

pub struct NatTap {
    fd: RawFd,
    map: PortMap,     // TCP 端口改写 shim 用
    rx: Vec<u8>,
    tx: Vec<u8>,
    smac: [u8; 6],    // 本栈 MAC（回包 eth 源）
    flows: HashMap<(u32, u16), Flow>,
    inj: Vec<u8>,     // 造回包的缓冲
}

impl NatTap {
    pub fn new(name: &str, map: PortMap, smac: [u8; 6]) -> std::io::Result<Self> {
        let fd = unsafe { libc::open(b"/dev/net/tun\0".as_ptr() as *const i8, libc::O_RDWR) };
        if fd < 0 { return Err(std::io::Error::last_os_error()); }
        #[repr(C)]
        struct Ifreq { name: [u8; 16], flags: u16, _pad: [u8; 22] }
        let mut req = Ifreq { name: [0; 16], flags: (libc::IFF_TAP | libc::IFF_NO_PI) as u16, _pad: [0; 22] };
        let nb = name.as_bytes();
        req.name[..nb.len().min(15)].copy_from_slice(&nb[..nb.len().min(15)]);
        const TUNSETIFF: libc::c_ulong = 0x400454ca;
        if unsafe { libc::ioctl(fd, TUNSETIFF, &mut req) } < 0 {
            let e = std::io::Error::last_os_error(); unsafe { libc::close(fd); } return Err(e);
        }
        let fl = unsafe { libc::fcntl(fd, libc::F_GETFL, 0) };
        unsafe { libc::fcntl(fd, libc::F_SETFL, fl | libc::O_NONBLOCK); }
        Ok(Self { fd, map, rx: vec![0u8; FRAME], tx: vec![0u8; FRAME], smac,
                  flows: HashMap::new(), inj: vec![0u8; FRAME] })
    }

    // 帧是不是 IPv4/UDP 到 UDP_PORT？是则做 NAT 转发并返回 true（消费掉，不给 smoltcp）。
    fn try_udp(&mut self, f: &[u8]) -> bool {
        if f.len() < 14 + 20 + 8 { return false; }
        if f[12] != 0x08 || f[13] != 0x00 { return false; }        // 非 IPv4
        let ihl = (f[14] & 0x0f) as usize * 4;
        if ihl < 20 || f[14 + 9] != 17 { return false; }           // 非 UDP
        let l4 = 14 + ihl;
        if f.len() < l4 + 8 { return false; }
        let dport = be16(&f[l4 + 2..]);
        if dport != UDP_PORT { return false; }
        let sport = be16(&f[l4..]);
        let cip = [f[26], f[27], f[28], f[29]];
        let dip = [f[30], f[31], f[32], f[33]];
        let cmac = [f[6], f[7], f[8], f[9], f[10], f[11]];
        let ulen = be16(&f[l4 + 4..]) as usize;
        let plen = ulen.saturating_sub(8).min(f.len() - (l4 + 8));
        let key = (u32::from_be_bytes(cip), sport);
        let flow = self.flows.entry(key).or_insert_with(|| {
            let s = UdpSocket::bind("127.0.0.1:0").unwrap();
            s.connect(("127.0.0.1", UDP_PORT)).unwrap();
            s.set_nonblocking(true).ok();
            Flow { sock: s, cmac, cip, cport: sport, dip, dport }
        });
        let _ = flow.sock.send(&f[l4 + 8..l4 + 8 + plen]);
        true
    }

    // 造 eth+ip+udp 回包写回 TAP。src=原始目的(IP,port)，dst=客户端(MAC,IP,port)。
    fn inject(smac: &[u8; 6], fl: &Flow, fd: RawFd, buf: &mut [u8], payload: &[u8]) {
        let iplen = 20 + 8 + payload.len();
        let tot = 14 + iplen;
        if tot > buf.len() { return; }
        // eth
        buf[0..6].copy_from_slice(&fl.cmac);
        buf[6..12].copy_from_slice(smac);
        buf[12] = 0x08; buf[13] = 0x00;
        // ip
        let ip = &mut buf[14..14 + 20];
        ip[0] = 0x45; ip[1] = 0; ip[2..4].copy_from_slice(&(iplen as u16).to_be_bytes());
        ip[4..6].copy_from_slice(&0u16.to_be_bytes()); ip[6] = 0x40; ip[7] = 0; // DF, frag=0
        ip[8] = 64; ip[9] = 17; ip[10] = 0; ip[11] = 0;
        ip[12..16].copy_from_slice(&fl.dip);
        ip[16..20].copy_from_slice(&fl.cip);
        let c = fold(ones_sum(&buf[14..14 + 20], 0));
        buf[24..26].copy_from_slice(&c.to_be_bytes());
        // udp
        let u0 = 14 + 20;
        let udplen = 8 + payload.len();
        buf[u0..u0 + 2].copy_from_slice(&fl.dport.to_be_bytes());
        buf[u0 + 2..u0 + 4].copy_from_slice(&fl.cport.to_be_bytes());
        buf[u0 + 4..u0 + 6].copy_from_slice(&(udplen as u16).to_be_bytes());
        buf[u0 + 6..u0 + 8].copy_from_slice(&0u16.to_be_bytes());
        buf[u0 + 8..u0 + 8 + payload.len()].copy_from_slice(payload);
        // udp 校验和（含伪首部）
        let mut ph = 0u32;
        ph = ones_sum(&fl.dip, ph); ph = ones_sum(&fl.cip, ph);
        ph += 17u32; ph += udplen as u32;
        let c = fold(ones_sum(&buf[u0..u0 + udplen], ph));
        let c = if c == 0 { 0xffff } else { c };
        buf[u0 + 6..u0 + 8].copy_from_slice(&c.to_be_bytes());
        unsafe { libc::write(fd, buf.as_ptr() as *const _, tot); }
    }

    // 非阻塞排空所有流的内核 socket，回包造帧注入。每次 receive() 调一遍。
    fn pump_replies(&mut self) {
        let mut pl = [0u8; 2048];
        let fd = self.fd; let smac = self.smac;
        for fl in self.flows.values() {
            loop {
                match fl.sock.recv(&mut pl) {
                    Ok(n) if n > 0 => Self::inject(&smac, fl, fd, &mut self.inj, &pl[..n]),
                    _ => break,
                }
            }
        }
    }
    pub fn raw_fd(&self) -> RawFd { self.fd }
}

impl AsRawFd for NatTap { fn as_raw_fd(&self) -> RawFd { self.fd } }

pub struct NtRx<'a> { buf: &'a mut [u8] }
pub struct NtTx<'a> { fd: RawFd, buf: &'a mut Vec<u8>, map: PortMap }

impl<'a> RxToken for NtRx<'a> {
    fn consume<R, F: FnOnce(&[u8]) -> R>(self, f: F) -> R { f(self.buf) }
}
impl<'a> TxToken for NtTx<'a> {
    fn consume<R, F: FnOnce(&mut [u8]) -> R>(self, len: usize, f: F) -> R {
        self.buf.resize(len.max(FRAME), 0);
        let r = f(&mut self.buf[..len]);
        rewrite_tx(&mut self.buf[..len], &self.map);
        let _ = unsafe { libc::write(self.fd, self.buf.as_ptr() as *const _, len) };
        r
    }
}

impl Device for NatTap {
    type RxToken<'a> = NtRx<'a> where Self: 'a;
    type TxToken<'a> = NtTx<'a> where Self: 'a;

    fn receive(&mut self, _ts: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        self.pump_replies(); // 先把 UDP 回包注入
        loop {
            let n = unsafe { libc::read(self.fd, self.rx.as_mut_ptr() as *mut _, FRAME) };
            if n <= 0 { return None; }
            let n = n as usize;
            // UDP 帧自己 NAT 掉，不进 smoltcp；继续读下一帧
            let is_udp = {
                let frame = unsafe { std::slice::from_raw_parts(self.rx.as_ptr(), n) };
                // 借用规避：先判类型再决定是否调用 try_udp（try_udp 需要 &mut self）
                frame.len() >= 14 + 28 && frame[12] == 0x08 && frame[13] == 0x00
                    && (frame[14] & 0x0f) as usize * 4 >= 20
                    && frame[14 + 9] == 17
            };
            if is_udp {
                // 拷一份给 try_udp（避免 &self.rx 与 &mut self 冲突）
                let mut tmp = [0u8; FRAME];
                tmp[..n].copy_from_slice(&self.rx[..n]);
                if self.try_udp(&tmp[..n]) { continue; }
            }
            rewrite_rx(&mut self.rx[..n], &self.map);
            let NatTap { rx, tx, fd, map, .. } = self;
            return Some((NtRx { buf: &mut rx[..n] }, NtTx { fd: *fd, buf: tx, map: map.clone() }));
        }
    }
    fn transmit(&mut self, _ts: Instant) -> Option<Self::TxToken<'_>> {
        Some(NtTx { fd: self.fd, buf: &mut self.tx, map: self.map.clone() })
    }
    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.medium = Medium::Ethernet; c.max_transmission_unit = FRAME; c
    }
}
