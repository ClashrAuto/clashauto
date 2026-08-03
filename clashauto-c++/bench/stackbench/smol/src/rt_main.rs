// 候选 C-catchall —— smoltcp + phy 层端口改写 shim，实现真正的 catch-all。
// 单个 addr:None, port=FIXED 的监听接住设备发往**任意 (IP,端口)** 的 TCP 连接。
// 证明 R5 里"改派发逻辑加通配分支"那条路可行（而且比 fork smoltcp 更轻——零 fork）。
//
// 用法: smoltcp2socks_ca <tap-if> <stack-ip> <netmask>
#[path = "rawtap.rs"]
mod rawtap;

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::os::unix::io::AsRawFd;
use std::sync::{Arc, Mutex};

use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet};

use smoltcp::socket::tcp;
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, HardwareAddress, IpAddress, IpCidr, IpListenEndpoint};

use rawtap::{RawTap, PortMap, FIXED_PORT};

const WND: usize = 128 * 1024;
fn backlog() -> usize {
    std::env::var("SMOL_BACKLOG").ok().and_then(|v| v.parse().ok()).unwrap_or(64)
}

struct Conn { up: TcpStream, up_eof: bool, dev_fin: bool, dn: Vec<u8> }

fn new_listener(sockets: &mut SocketSet) -> SocketHandle {
    let rx = tcp::SocketBuffer::new(vec![0u8; WND]);
    let tx = tcp::SocketBuffer::new(vec![0u8; WND]);
    let mut s = tcp::Socket::new(rx, tx);
    // 只挂一个端口：FIXED。所有真实目的端口都被 shim 改写成了它。
    s.listen(IpListenEndpoint { addr: None, port: FIXED_PORT }).unwrap();
    s.set_nagle_enabled(false);
    sockets.add(s)
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 { eprintln!("usage: smoltcp2socks_rt <tap> <ip> <mask>"); std::process::exit(2); }
    let (tap_name, ip_str, mask_str) = (&args[1], &args[2], &args[3]);

    let map: PortMap = Arc::new(Mutex::new(HashMap::new()));
    let mut device = RawTap::new(tap_name, map.clone())
        .unwrap_or_else(|e| { eprintln!("tap: {e}"); std::process::exit(1) });
    let tap_fd = device.as_raw_fd();

    let mac = EthernetAddress([0x02, 0x00, 0x5b, 0x00, 0x00, 0x02]);
    let mut cfg = Config::new(HardwareAddress::Ethernet(mac));
    cfg.random_seed = 0x5b5b_5b5b;
    let mut iface = Interface::new(cfg, &mut device, Instant::now());

    let prefix: u8 = mask_str.split('.').map(|x| x.parse::<u8>().unwrap_or(0).count_ones() as u8).sum();
    let ip: IpAddress = ip_str.parse().unwrap();
    iface.update_ip_addrs(|a| { a.push(IpCidr::new(ip, prefix)).unwrap(); });
    iface.set_any_ip(true);
    if let IpAddress::Ipv4(self_v4) = ip {
        iface.routes_mut().add_default_ipv4_route(self_v4).unwrap();
    }

    let mut sockets = SocketSet::new(vec![]);
    let bl = backlog();
    let mut listeners: Vec<SocketHandle> = (0..bl).map(|_| new_listener(&mut sockets)).collect();
    let mut conns: HashMap<SocketHandle, Conn> = HashMap::new();

    eprintln!("smoltcp2socks_rt up on {tap_name} ip={ip_str} FIXED_PORT={FIXED_PORT} backlog={bl} (真 catch-all)");

    loop {
        iface.poll(Instant::now(), &mut device, &mut sockets);

        // 监听 socket 被连上 → 建 conn。真实目的端口从 shim 记的映射里查。
        let promoted: Vec<SocketHandle> = listeners.iter().copied().filter(|h| {
            let s = sockets.get::<tcp::Socket>(*h);
            s.state() != tcp::State::Listen && s.state() != tcp::State::Closed
        }).collect();
        for h in promoted {
            listeners.retain(|x| *x != h);
            listeners.push(new_listener(&mut sockets));
            let (real_port, ok) = {
                let s = sockets.get::<tcp::Socket>(h);
                match (s.remote_endpoint(), s.local_endpoint()) {
                    (Some(rem), Some(loc)) => {
                        let cip = ip_to_u32(rem.addr);
                        let sip = ip_to_u32(loc.addr);
                        let p = map.lock().unwrap().get(&(cip, rem.port, sip)).copied();
                        (p.unwrap_or(0), p.is_some())
                    }
                    _ => (0, false),
                }
            };
            if !ok { sockets.get_mut::<tcp::Socket>(h).abort(); continue; }
            match TcpStream::connect(("127.0.0.1", real_port)) {
                Ok(up) => { up.set_nonblocking(true).ok(); up.set_nodelay(true).ok();
                            conns.insert(h, Conn { up, up_eof: false, dev_fin: false, dn: Vec::new() }); }
                Err(_) => { sockets.get_mut::<tcp::Socket>(h).abort(); }
            }
        }

        let handles: Vec<SocketHandle> = conns.keys().copied().collect();
        for h in handles {
            let mut drop_it = false;
            {
                let s = sockets.get_mut::<tcp::Socket>(h);
                let c = conns.get_mut(&h).unwrap();
                while s.can_recv() {
                    let mut buf = [0u8; 64 * 1024];
                    let n = s.recv_slice(&mut buf).unwrap_or(0);
                    if n == 0 { break; }
                    let mut off = 0;
                    while off < n {
                        match c.up.write(&buf[off..n]) {
                            Ok(0) => { drop_it = true; break; }
                            Ok(w) => off += w,
                            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => std::thread::yield_now(),
                            Err(_) => { drop_it = true; break; }
                        }
                    }
                    if drop_it { break; }
                }
                if matches!(s.state(),
                    tcp::State::CloseWait | tcp::State::LastAck | tcp::State::Closing
                  | tcp::State::TimeWait | tcp::State::Closed) && !s.can_recv() { c.dev_fin = true; }
                if !c.up_eof {
                    while s.can_send() && c.dn.is_empty() {
                        let mut buf = [0u8; 64 * 1024];
                        match c.up.read(&mut buf) {
                            Ok(0) => { c.up_eof = true; break; }
                            Ok(n) => { let sent = s.send_slice(&buf[..n]).unwrap_or(0);
                                       if sent < n { c.dn.extend_from_slice(&buf[sent..n]); } }
                            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                            Err(_) => { c.up_eof = true; break; }
                        }
                    }
                }
                if !c.dn.is_empty() && s.can_send() { let sent = s.send_slice(&c.dn).unwrap_or(0); c.dn.drain(..sent); }
                if c.up_eof && c.dn.is_empty() && s.may_send() { s.close(); }
                if c.dev_fin { c.up.shutdown(std::net::Shutdown::Write).ok(); }
                if s.state() == tcp::State::Closed { drop_it = true; }
            }
            if drop_it { sockets.remove(h); conns.remove(&h); }
        }

        let mut fds: Vec<libc::pollfd> = Vec::with_capacity(conns.len() + 1);
        fds.push(libc::pollfd { fd: tap_fd, events: libc::POLLIN, revents: 0 });
        for c in conns.values() { fds.push(libc::pollfd { fd: c.up.as_raw_fd(), events: libc::POLLIN, revents: 0 }); }
        let timeout_ms = match iface.poll_delay(Instant::now(), &sockets) {
            Some(d) => (d.total_millis() as i32).clamp(0, 10), None => 10 };
        unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, timeout_ms); }
    }
}

fn ip_to_u32(a: IpAddress) -> u32 {
    match a { IpAddress::Ipv4(v) => u32::from_be_bytes(v.octets()), _ => 0 }
}
