// 候选 C —— smoltcp（Rust，sans-io，和 lwIP 一样是单线程无堆栈依赖的嵌入式栈）。
//
// 与候选 A/B 严格同拓扑同接口：TAP 收以太帧 → 栈终结 TCP →
// 用内核 socket 连 127.0.0.1:<原始目的端口> → 双向搬字节。
//
// ★ 一个必须挑明的能力差距：smoltcp **没有 catch-all 端口监听**。
//   它的 IpListenEndpoint 可以 addr=None（任意目的 IP，对应 lwIP 的 accept-all），
//   但 port 必须是具体值 —— 没有 lwIP 那种 local_port==0 的通配分支。
//   跑分只用 5201/5202，所以这里给这两个端口各预挂一批监听 socket 就够了；
//   真要上生产得给 65535 个端口预挂，或者去改 smoltcp 的 socket 派发逻辑。
//   这一点在结论里必须写清楚，不能只报数字。
//
// 用法: smoltcp2socks <tap-if> <stack-ip> <netmask>
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::os::unix::io::AsRawFd;

use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet};
use smoltcp::phy::{Device, Medium, TunTapInterface};
use smoltcp::socket::tcp;
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, HardwareAddress, IpAddress, IpCidr, IpListenEndpoint};

// 与候选 A 对齐：lwIP 的 TCP_WND / TCP_SND_BUF 都是 128 KiB
const WND: usize = 128 * 1024;
const PORTS: [u16; 2] = [5201, 5202];
// ★ 每个端口预挂的监听 socket 数 = **能同时接住的并发连接数上限**。
//   smoltcp 的缓冲是预分配的，一个监听 socket 就吃掉 2×WND=256 KiB，**空着也占**。
//   这正是并发测试要量的东西，所以做成 env 可配，免得为改一个数重编。
fn backlog() -> usize {
    std::env::var("SMOL_BACKLOG").ok().and_then(|v| v.parse().ok()).unwrap_or(64)
}

struct Conn {
    up: TcpStream,
    up_eof: bool,
    dev_fin: bool,
    // 上游 → 设备 的暂存（从 socket 读出来但 smoltcp 发送缓冲暂时塞不下的部分）
    dn: Vec<u8>,
}

fn new_listener(sockets: &mut SocketSet, port: u16) -> SocketHandle {
    let rx = tcp::SocketBuffer::new(vec![0u8; WND]);
    let tx = tcp::SocketBuffer::new(vec![0u8; WND]);
    let mut s = tcp::Socket::new(rx, tx);
    // addr: None = 收下发往**任意目的 IP** 的连接（对应 lwIP 的 accept-all 补丁）
    s.listen(IpListenEndpoint { addr: None, port }).unwrap();
    s.set_nagle_enabled(false);
    sockets.add(s)
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 {
        eprintln!("usage: smoltcp2socks <tap> <ip> <mask>");
        std::process::exit(2);
    }
    let (tap_name, ip_str, mask_str) = (&args[1], &args[2], &args[3]);

    let mut device = TunTapInterface::new(tap_name, Medium::Ethernet)
        .unwrap_or_else(|e| { eprintln!("tap: {e}"); std::process::exit(1) });
    let tap_fd = device.as_raw_fd();

    let mac = EthernetAddress([0x02, 0x00, 0x5b, 0x00, 0x00, 0x02]);
    let mut cfg = Config::new(HardwareAddress::Ethernet(mac));
    cfg.random_seed = 0x5b5b_5b5b;
    let mut iface = Interface::new(cfg, &mut device, Instant::now());

    let prefix = {
        let o: Vec<u8> = mask_str.split('.').map(|x| x.parse::<u8>().unwrap_or(0)).collect();
        o.iter().map(|b| b.count_ones() as u8).sum::<u8>()
    };
    let ip: IpAddress = ip_str.parse().unwrap();
    iface.update_ip_addrs(|a| { a.push(IpCidr::new(ip, prefix)).unwrap(); });
    // ★ 回程要以「原始目的 IP」为源地址发出去，必须允许非本机地址 —— 对应 gVisor 的
    //   SetSpoofing。smoltcp 靠 any_ip 打开这条路。
    iface.set_any_ip(true);
    // ★ any_ip 单开不够用。smoltcp 收到「不是发给我的」IP 包时还要过一道
    //   `routes.lookup(dst)` 检查，而且判据是 **查出来的下一跳必须是我们自己的地址**
    //   （ipv4.rs: `.map_or(true, |router_addr| !self.has_ip_addr(router_addr))`
    //     —— 注释里叫 "check if the packet is routed locally"）。
    //   所以这条默认路由的网关要填**本栈自己的 IP**，填对端(10.99.0.1)会被判成
    //   「不是本地路由」照样丢。症状是 ARP 通、ping 通、TCP 全超时且零日志。
    if let IpAddress::Ipv4(self_v4) = ip {
        iface.routes_mut().add_default_ipv4_route(self_v4).unwrap();
    }

    let mut sockets = SocketSet::new(vec![]);
    let mut listeners: HashMap<SocketHandle, u16> = HashMap::new();
    let bl = backlog();
    for p in PORTS { for _ in 0..bl { listeners.insert(new_listener(&mut sockets, p), p); } }
    let mut conns: HashMap<SocketHandle, Conn> = HashMap::new();

    eprintln!("smoltcp2socks up on {tap_name} ip={ip_str} wnd={WND} ports={PORTS:?} \
               backlog={bl} prealloc={}MiB",
              (PORTS.len() * bl * 2 * WND) / (1024 * 1024));

    loop {
        let now = Instant::now();
        iface.poll(now, &mut device, &mut sockets);

        // 1) 监听 socket 被连上 → 转成 conn，并补一个新的监听 socket 顶上
        let promoted: Vec<SocketHandle> = listeners.keys()
            .filter(|h| {
                let s = sockets.get::<tcp::Socket>(**h);
                s.state() != tcp::State::Listen && s.state() != tcp::State::Closed
            })
            .copied().collect();
        for h in promoted {
            let port = listeners.remove(&h).unwrap();
            listeners.insert(new_listener(&mut sockets, port), port);
            match TcpStream::connect(("127.0.0.1", port)) {
                Ok(up) => {
                    up.set_nonblocking(true).ok();
                    up.set_nodelay(true).ok();
                    conns.insert(h, Conn { up, up_eof: false, dev_fin: false, dn: Vec::new() });
                }
                Err(_) => { sockets.get_mut::<tcp::Socket>(h).abort(); }
            }
        }

        // 2) 双向搬字节
        let handles: Vec<SocketHandle> = conns.keys().copied().collect();
        for h in handles {
            let mut drop_it = false;
            {
                let s = sockets.get_mut::<tcp::Socket>(h);
                let c = conns.get_mut(&h).unwrap();

                // 设备 → 上游：smoltcp 的接收缓冲就是背压闸门，写不出去就不 recv
                while s.can_recv() {
                    let mut buf = [0u8; 64 * 1024];
                    let n = s.recv_slice(&mut buf).unwrap_or(0);
                    if n == 0 { break; }
                    let mut off = 0;
                    while off < n {
                        match c.up.write(&buf[off..n]) {
                            Ok(0) => { drop_it = true; break; }
                            Ok(w) => off += w,
                            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                                // 罕见：上游写满。这里同步等一小会儿即可——测的是栈不是这条边
                                std::thread::yield_now();
                            }
                            Err(_) => { drop_it = true; break; }
                        }
                    }
                    if drop_it { break; }
                }
                // ★ 判「设备发了 FIN」只能看状态机，**不能看 may_recv()**：
                //   may_recv() 在 SynReceived 阶段同样是 false，照它判的话连接刚建好就会
                //   被当成对端已关闭，立刻 shutdown 掉上游写端 —— 症状是 iperf3 报
                //   "unable to receive cookie"/"Connection reset by peer"，而栈进程活得好好的、
                //   CPU 占用是 0，看起来完全不像是它的锅。
                if matches!(s.state(),
                            tcp::State::CloseWait | tcp::State::LastAck
                          | tcp::State::Closing   | tcp::State::TimeWait
                          | tcp::State::Closed) && !s.can_recv() {
                    c.dev_fin = true;
                }

                // 上游 → 设备
                if !c.up_eof {
                    while s.can_send() && c.dn.is_empty() {
                        let mut buf = [0u8; 64 * 1024];
                        match c.up.read(&mut buf) {
                            Ok(0) => { c.up_eof = true; break; }
                            Ok(n) => {
                                let sent = s.send_slice(&buf[..n]).unwrap_or(0);
                                if sent < n { c.dn.extend_from_slice(&buf[sent..n]); }
                            }
                            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                            Err(_) => { c.up_eof = true; break; }
                        }
                    }
                }
                if !c.dn.is_empty() && s.can_send() {
                    let sent = s.send_slice(&c.dn).unwrap_or(0);
                    c.dn.drain(..sent);
                }
                if c.up_eof && c.dn.is_empty() && s.may_send() { s.close(); }
                if c.dev_fin { c.up.shutdown(std::net::Shutdown::Write).ok(); }
                if s.state() == tcp::State::Closed { drop_it = true; }
            }
            if drop_it { sockets.remove(h); conns.remove(&h); }
        }

        // 3) 等事件：TAP 可读 或 任一上游 socket 可读 或 smoltcp 的下一个定时器到点
        let mut fds: Vec<libc::pollfd> = Vec::with_capacity(conns.len() + 1);
        fds.push(libc::pollfd { fd: tap_fd, events: libc::POLLIN, revents: 0 });
        for c in conns.values() {
            fds.push(libc::pollfd { fd: c.up.as_raw_fd(), events: libc::POLLIN, revents: 0 });
        }
        let timeout_ms = match iface.poll_delay(Instant::now(), &sockets) {
            Some(d) => (d.total_millis() as i32).clamp(0, 10),
            None => 10,
        };
        unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, timeout_ms); }
    }
}
