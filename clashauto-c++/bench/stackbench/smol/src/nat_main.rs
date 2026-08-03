// 候选 C-catchall —— smoltcp + phy 层端口改写 shim，实现真正的 catch-all。
// 单个 addr:None, port=FIXED 的监听接住设备发往**任意 (IP,端口)** 的 TCP 连接。
// 证明 R5 里"改派发逻辑加通配分支"那条路可行（而且比 fork smoltcp 更轻——零 fork）。
//
// 用法: smoltcp2socks_ca <tap-if> <stack-ip> <netmask>
#[path = "natdev.rs"]
mod natdev;

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::os::unix::io::AsRawFd;
use std::sync::{Arc, Mutex};

use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet};

use smoltcp::socket::tcp;
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, HardwareAddress, IpAddress, IpCidr, IpListenEndpoint};


use natdev::{NatTap, PortMap, FIXED_PORT};

const WND: usize = 128 * 1024;
fn backlog() -> usize {
    std::env::var("SMOL_BACKLOG").ok().and_then(|v| v.parse().ok()).unwrap_or(64)
}

// ★ up 是 Option：R10 查明的洞①——原来只在 smoltcp socket 走到 Closed 才 drop 整个 Conn，
//   而 Closed 在 TIME_WAIT 之后，几秒里一直攥着这个已经 EOF、没用了的上游内核 fd。
//   高频短连接下 1024 个 fd 秒满。改成：上游读到 EOF 且下行排空就立刻 take() 掉它关 fd，
//   与 smoltcp socket 的 TIME_WAIT 彻底解耦。lwIP 的 C forwarder 一直就是这么早关的。
struct Conn { up: Option<TcpStream>, up_eof: bool, dev_fin: bool, dn: Vec<u8> }

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
    if args.len() != 4 { eprintln!("usage: smoltcp2socks_nat <tap> <ip> <mask>"); std::process::exit(2); }
    let (tap_name, ip_str, mask_str) = (&args[1], &args[2], &args[3]);

    // ★ R15：像真实网关守护进程那样，启动就把自己的 fd 上限抬高。否则默认 ulimit=1024
    //   低于 NAT_CAP(4096)，flow 表的 FIFO 淘汰在撞 fd 墙前根本触发不了，进程会卡死在
    //   1020 条死流上、新流全被 EMFILE 丢弃（100% 丢包）。抬到 65536 让 NAT_CAP 成为真正的界。
    unsafe {
        let lim = libc::rlimit { rlim_cur: 65536, rlim_max: 65536 };
        libc::setrlimit(libc::RLIMIT_NOFILE, &lim);
    }

    let map: PortMap = Arc::new(Mutex::new(HashMap::new()));
    let smac = [0x02u8, 0x00, 0x5b, 0x00, 0x00, 0x02];
    let mut device = NatTap::new(tap_name, map.clone(), smac)
        .unwrap_or_else(|e| { eprintln!("tap: {e}"); std::process::exit(1) });
    let tap_fd = device.as_raw_fd();

    let mac = EthernetAddress(smac);
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


    eprintln!("smoltcp2socks_nat up on (UDP=裸NAT绕过栈) {tap_name} ip={ip_str} FIXED_PORT={FIXED_PORT} backlog={bl} (真 catch-all)");

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
                            conns.insert(h, Conn { up: Some(up), up_eof: false, dev_fin: false, dn: Vec::new() }); }
                Err(_) => { sockets.get_mut::<tcp::Socket>(h).abort(); }
            }
        }

        let handles: Vec<SocketHandle> = conns.keys().copied().collect();
        for h in handles {
            let mut drop_it = false;
            {
                let s = sockets.get_mut::<tcp::Socket>(h);
                let c = conns.get_mut(&h).unwrap();
                // 设备 → 上游（上游可能已被早关，此时把设备来的数据丢弃即可）
                while s.can_recv() {
                    let mut buf = [0u8; 64 * 1024];
                    let n = s.recv_slice(&mut buf).unwrap_or(0);
                    if n == 0 { break; }
                    let Some(up) = c.up.as_mut() else { break }; // 上游没了：读掉但不转发
                    let mut off = 0;
                    while off < n {
                        match up.write(&buf[off..n]) {
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
                // 上游 → 设备
                if !c.up_eof {
                    if let Some(up) = c.up.as_mut() {
                        while s.can_send() && c.dn.is_empty() {
                            let mut buf = [0u8; 64 * 1024];
                            match up.read(&mut buf) {
                                Ok(0) => { c.up_eof = true; break; }
                                Ok(n) => { let sent = s.send_slice(&buf[..n]).unwrap_or(0);
                                           if sent < n { c.dn.extend_from_slice(&buf[sent..n]); } }
                                Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                                Err(_) => { c.up_eof = true; break; }
                            }
                        }
                    }
                }
                if !c.dn.is_empty() && s.can_send() { let sent = s.send_slice(&c.dn).unwrap_or(0); c.dn.drain(..sent); }
                if c.up_eof && c.dn.is_empty() && s.may_send() { s.close(); }
                if c.dev_fin { if let Some(up) = c.up.as_ref() { up.shutdown(std::net::Shutdown::Write).ok(); } }
                // ★ 洞①修复：上游已 EOF 且下行全部灌给了设备 → 立刻关上游 fd，不等 TIME_WAIT。
                //   此后 smoltcp socket 自己走完 FIN/TIME_WAIT，但那期间不再占一个内核 fd。
                if c.up_eof && c.dn.is_empty() && c.up.is_some() {
                    c.up = None; // drop = close(fd)
                }
                // ★ 洞②缓解：走到 TimeWait 就回收 smoltcp socket，不空等 2·MSL 定时器——
                //   否则上万 TIME_WAIT socket 堆在 SocketSet 里让每次 poll() 变 O(n)。
                //   到 TimeWait 说明我们的最后一个 ACK 已发出，提前 remove 是安全的。
                if matches!(s.state(), tcp::State::TimeWait | tcp::State::Closed) { drop_it = true; }
            }
            if drop_it { sockets.remove(h); conns.remove(&h); }
        }

        let mut fds: Vec<libc::pollfd> = Vec::with_capacity(conns.len() + 1);
        fds.push(libc::pollfd { fd: tap_fd, events: libc::POLLIN, revents: 0 });
        for c in conns.values() {
            if let Some(up) = c.up.as_ref() {
                fds.push(libc::pollfd { fd: up.as_raw_fd(), events: libc::POLLIN, revents: 0 });
            }
        }
        let timeout_ms = match iface.poll_delay(Instant::now(), &sockets) {
            Some(d) => (d.total_millis() as i32).clamp(0, 10), None => 10 };
        unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, timeout_ms); }
    }
}

fn ip_to_u32(a: IpAddress) -> u32 {
    match a { IpAddress::Ipv4(v) => u32::from_be_bytes(v.octets()), _ => 0 }
}
