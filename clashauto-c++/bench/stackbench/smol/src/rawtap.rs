// 自带端口改写的原始 TAP 设备 —— R8 的 CatchAll<TunTapInterface> 有一次多余拷贝：
// smoltcp 0.12 的 RxToken::consume 只给 &[u8]（不可变），要改写就得先 to_vec 一份。
// 那份拷贝是每包 ~1500 字节 × 百万 pps，正是 R8 里那 4.9% 的来源。
//
// 这里直接实现 Device，自己管 TAP fd：RX 读进自己的缓冲**原地**改写（省掉 to_vec），
// TX 让 smoltcp 写进自己的缓冲后**原地**改写再写 fd。改写逻辑复用 catchall.rs。
//
// 对照组意义：如果这版把 4.9% 拿回来了，说明「shim 的开销全在那次拷贝，协议路径零成本」；
// 拿不回来，说明改写本身（查表+校验和）也有份，R8 的乐观预测就要收回。
use std::os::unix::io::{AsRawFd, RawFd};

use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::time::Instant;

#[path = "catchall.rs"]
mod ca_shared;
pub use ca_shared::{PortMap, FIXED_PORT};
use ca_shared::{rewrite_rx_pub as rewrite_rx, rewrite_tx_pub as rewrite_tx};

const MTU: usize = 1500;
const FRAME: usize = MTU + 14;

pub struct RawTap {
    fd: RawFd,
    map: PortMap,
    // 复用的收发缓冲，避免每包 malloc（smoltcp 自带的 TunTapInterface 反而是每包 vec!）
    rx: Vec<u8>,
    tx: Vec<u8>,
}

impl RawTap {
    pub fn new(name: &str, map: PortMap) -> std::io::Result<Self> {
        let fd = unsafe { libc::open(b"/dev/net/tun\0".as_ptr() as *const i8, libc::O_RDWR) };
        if fd < 0 { return Err(std::io::Error::last_os_error()); }
        #[repr(C)]
        struct Ifreq { name: [u8; 16], flags: u16, _pad: [u8; 22] }
        let mut req = Ifreq { name: [0; 16], flags: (libc::IFF_TAP | libc::IFF_NO_PI) as u16, _pad: [0; 22] };
        let nb = name.as_bytes();
        req.name[..nb.len().min(15)].copy_from_slice(&nb[..nb.len().min(15)]);
        const TUNSETIFF: libc::c_ulong = 0x400454ca;
        if unsafe { libc::ioctl(fd, TUNSETIFF, &mut req) } < 0 {
            let e = std::io::Error::last_os_error(); unsafe { libc::close(fd); }
            return Err(e);
        }
        // 非阻塞：主循环靠 poll 唤醒，read 要能立刻返回 EAGAIN
        let fl = unsafe { libc::fcntl(fd, libc::F_GETFL, 0) };
        unsafe { libc::fcntl(fd, libc::F_SETFL, fl | libc::O_NONBLOCK); }
        Ok(Self { fd, map, rx: vec![0u8; FRAME], tx: vec![0u8; FRAME] })
    }
}

impl AsRawFd for RawTap {
    fn as_raw_fd(&self) -> RawFd { self.fd }
}

pub struct RtRx<'a> { buf: &'a mut [u8] }
pub struct RtTx<'a> { fd: RawFd, buf: &'a mut Vec<u8>, map: PortMap }

impl<'a> RxToken for RtRx<'a> {
    fn consume<R, F: FnOnce(&[u8]) -> R>(self, f: F) -> R {
        f(self.buf) // 已在 receive() 里原地改写过，直接交给 smoltcp
    }
}

impl<'a> TxToken for RtTx<'a> {
    fn consume<R, F: FnOnce(&mut [u8]) -> R>(self, len: usize, f: F) -> R {
        self.buf.resize(len.max(FRAME), 0);
        let r = f(&mut self.buf[..len]);        // smoltcp 把包写进来（src_port=FIXED）
        rewrite_tx(&mut self.buf[..len], &self.map); // 原地改回真实端口
        let _ = unsafe { libc::write(self.fd, self.buf.as_ptr() as *const _, len) };
        r
    }
}

impl Device for RawTap {
    type RxToken<'a> = RtRx<'a> where Self: 'a;
    type TxToken<'a> = RtTx<'a> where Self: 'a;

    fn receive(&mut self, _ts: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        let n = unsafe { libc::read(self.fd, self.rx.as_mut_ptr() as *mut _, FRAME) };
        if n <= 0 { return None; }
        let n = n as usize;
        rewrite_rx(&mut self.rx[..n], &self.map); // ★ 原地改写，无 to_vec
        // 借用拆分：rx 交给 RxToken，tx 交给 TxToken（互不重叠）
        let RawTap { rx, tx, fd, map, .. } = self;
        Some((RtRx { buf: &mut rx[..n] }, RtTx { fd: *fd, buf: tx, map: map.clone() }))
    }

    fn transmit(&mut self, _ts: Instant) -> Option<Self::TxToken<'_>> {
        Some(RtTx { fd: self.fd, buf: &mut self.tx, map: self.map.clone() })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.medium = Medium::Ethernet;
        c.max_transmission_unit = FRAME;
        c
    }
}
