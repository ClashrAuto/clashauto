// smoltcp 引擎本体 —— 每张网卡一个独立 Interface + SocketSet。
//
// ── 为什么每卡一个实例（而不是像 lwIP 那样一个栈挂多个 netif）───────────────────
// lwIP 是 NO_SYS 全局状态，多网卡只能靠 ip4_route 按子网从一条 netif 链里挑，掩码配错就会
// 把 B 网段的回包从 A 网卡发出去（NetStack.cpp:1383-1386 记着这个坑）。smoltcp 的 Interface
// 不是全局的，每卡一个实例天然隔离，且设备归属本来就由 C++ 侧维护，根本不需要栈做路由决策。
//
// ── 回调重入的处理 ──────────────────────────────────────────────────────────
// 回调里可以再调 coast_*（典型：conn_new 里立刻 conn_close）。若在持有 &mut Engine 时直接
// 派发回调，重入就会造成别名 UB。所以这里把「跑 poll」和「派发回调」拆成两步：
//   poll_collect() 只收集事件、不碰回调 → FFI 层放掉借用 → 逐个派发。
// lwIP 那边为了同一个问题养了一整套 ConnWatch/g_destroyedConn/markConnDestroyed（~60 行 +
// 4 个 needsAbortReturn 返回点，NetStack.cpp:532-587）；poll 模型天然消除这一整类问题，
// 这是换栈**最大的结构性收益**。
//
// ── 关于 ConnData 的那次拷贝 ────────────────────────────────────────────────
// 事件里带的是数据副本，不是零拷贝。这是刻意的取舍：零拷贝要求在持有借用时同步派发回调，
// 正好回到上面那个重入问题。而拷贝的代价在本系统里可以忽略——1500 B memcpy 约 0.15 µs，
// 而同一条路径上 Npcap 每帧要 12~39 µs（docs/lwip-alternatives.md R19/R21 实测），
// 差两个数量级。**先要正确，再谈那 1%。**

use alloc::collections::BTreeMap;
use alloc::vec;
use alloc::vec::Vec;

use smoltcp::iface::{Config, Interface, PollResult, SocketHandle, SocketSet};
use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::socket::tcp;
use smoltcp::time::{Duration, Instant};
use smoltcp::wire::{
    EthernetAddress, HardwareAddress, IpAddress, IpCidr, IpListenEndpoint, Ipv4Address, Ipv6Address,
};

use crate::portmap::{self, PortMap, FIXED_PORT};

/// 与 lwIP 的 TCP_WND / TCP_SND_BUF 对齐（各 128 KiB），保证换栈前后是同口径对比。
/// 缓冲是 `vec![0; N]`（走 calloc），页不碰不落地——R6 实测：声称预分配 32 MiB 的配置
/// 空载 RSS 只有 4.1 MB，比 lwIP 的 6.6 还低。所以这个数不必为了省内存而调小。
const RX_BUF: usize = 128 * 1024;
const TX_BUF: usize = 128 * 1024;

/// 监听池大小 = 「两次 poll 之间能接住几个 SYN」的突发容忍度，**不是并发上限**
/// （R6 实测：backlog=1 时 64 路并发照样跑通，只是吞吐 -7%）。真正的上限是内存。
const LISTEN_BACKLOG: usize = 32;

pub type ConnId = u64;
pub type NicId = u32;

/// 引擎产出的事件。FFI 层收走后再派发成 C 回调（见文件头「回调重入」）。
pub enum Event {
    OutFrame { nic: NicId, data: Vec<u8> },
    ConnNew { id: ConnId, nic: NicId, src: [u8; 16], is_v6: bool, sport: u16, dst: [u8; 16], dport: u16 },
    ConnData { id: ConnId, data: Vec<u8> },
    ConnSent { id: ConnId, n: u32 },
    ConnClosed { id: ConnId, is_abort: bool },
}

// ———————————————————————— phy：由 C++ 喂帧的队列设备 ————————————————————————

/// smoltcp 的 Device 实现：收队列由 `coast_stack_input` 填，发队列由 poll 后排空。
/// 端口改写**不在这里**做（在 FFI 出入口做），所以这个设备只搬字节，不需要共享 PortMap。
pub struct QueueDevice {
    rx: Vec<Vec<u8>>,
    tx: Vec<Vec<u8>>,
    mtu: usize,
}

impl QueueDevice {
    fn new(mtu: usize) -> Self {
        Self { rx: Vec::new(), tx: Vec::new(), mtu }
    }
    fn push_rx(&mut self, frame: Vec<u8>) {
        self.rx.push(frame);
    }
    fn take_tx(&mut self) -> Vec<Vec<u8>> {
        core::mem::take(&mut self.tx)
    }
    fn rx_pending(&self) -> bool {
        !self.rx.is_empty()
    }
}

pub struct QRx(Vec<u8>);
pub struct QTx<'a>(&'a mut Vec<Vec<u8>>);

impl RxToken for QRx {
    fn consume<R, F: FnOnce(&[u8]) -> R>(self, f: F) -> R {
        f(&self.0)
    }
}

impl<'a> TxToken for QTx<'a> {
    fn consume<R, F: FnOnce(&mut [u8]) -> R>(self, len: usize, f: F) -> R {
        let mut buf = vec![0u8; len];
        let r = f(&mut buf);
        self.0.push(buf);
        r
    }
}

impl Device for QueueDevice {
    type RxToken<'a> = QRx where Self: 'a;
    type TxToken<'a> = QTx<'a> where Self: 'a;

    fn receive(&mut self, _t: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        if self.rx.is_empty() {
            return None;
        }
        let frame = self.rx.remove(0);
        Some((QRx(frame), QTx(&mut self.tx)))
    }
    fn transmit(&mut self, _t: Instant) -> Option<Self::TxToken<'_>> {
        Some(QTx(&mut self.tx))
    }
    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.medium = Medium::Ethernet;
        c.max_transmission_unit = self.mtu;
        c
    }
}

// ———————————————————————— 网卡 ————————————————————————

struct Nic {
    iface: Interface,
    device: QueueDevice,
    sockets: SocketSet<'static>,
    listeners: Vec<SocketHandle>,
    portmap: PortMap,
    /// 本机在这张卡上的 MAC 与 IP —— 合成 ARP 应答时要用（见 learn_neighbor）。
    our_mac: [u8; 6],
    our_ip: [u8; 16],
    our_is_v6: bool,
    /// 已注入邻居缓存的 (设备IP → MAC, 上次注入时刻ms)。
    /// smoltcp 的邻居项会过期，所以要按间隔刷新，不能只注入一次。
    neighbors: BTreeMap<[u8; 16], ([u8; 6], u64)>,
}

fn new_listener(sockets: &mut SocketSet<'static>) -> Option<SocketHandle> {
    let rx = tcp::SocketBuffer::new(vec![0u8; RX_BUF]);
    let tx = tcp::SocketBuffer::new(vec![0u8; TX_BUF]);
    let mut s = tcp::Socket::new(rx, tx);
    // addr:None = 收任意目的 IP（等价于 lwIP 的 accept-all 补丁）；
    // port 只能是具体值 —— 任意目的**端口**靠 portmap 的改写实现（见 portmap.rs）。
    if s.listen(IpListenEndpoint { addr: None, port: FIXED_PORT }).is_err() {
        return None;
    }
    s.set_nagle_enabled(false); // 对应 lwIP 的 tcp_nagle_disable
    Some(sockets.add(s))
}

// ———————————————————————— 连接 ————————————————————————

struct Conn {
    nic: NicId,
    handle: SocketHandle,
    /// 已 peek 给 C++、但还没被 `coast_conn_recved` 消费的字节数。
    /// **这就是背压的闸门**：不消费 = smoltcp 的 rx 缓冲不释放 = 通告窗口不打开
    /// （对应 lwIP 的「收到先不 tcp_recved」）。
    peeked: usize,
    /// 上一次看到的 send_queue，用于算 conn_sent 的增量（对应 lwIP 的 tcp_sent）。
    last_send_queue: usize,
    /// 客户端 (IP,端口) 与服务 IP —— 关连接时要据此清 portmap，否则表会随连接泄漏。
    cip: [u8; 16],
    cport: u16,
    sip: [u8; 16],
    closed_reported: bool,
}

// ———————————————————————— 引擎 ————————————————————————

pub struct Engine {
    nics: BTreeMap<NicId, Nic>,
    conns: BTreeMap<ConnId, Conn>,
    next_id: ConnId,
    /// 最近一次 poll 的时刻，供 learn_neighbor 的刷新间隔用（input 时没有时间参数）。
    last_seen_ms: u64,
    pub conns_accepted: u64,
    pub conns_closed: u64,
    pub conns_aborted: u64,
    pub conns_refused: u64,
    pub tx_frames: u64,
}

impl Engine {
    pub fn new() -> Self {
        Self {
            nics: BTreeMap::new(),
            conns: BTreeMap::new(),
            next_id: 1, // 0 恒为无效 id
            last_seen_ms: 0,
            conns_accepted: 0,
            conns_closed: 0,
            conns_aborted: 0,
            conns_refused: 0,
            tx_frames: 0,
        }
    }

    pub fn has_nic(&self, nic: NicId) -> bool {
        self.nics.contains_key(&nic)
    }
    pub fn conn_count(&self) -> usize {
        self.conns.len()
    }

    pub fn add_nic(&mut self, nic: NicId, mac: [u8; 6], ip: [u8; 16], is_v6: bool, prefix: u8, now_ms: u64) -> bool {
        if self.nics.contains_key(&nic) {
            return false;
        }
        let mut device = QueueDevice::new(1514);
        let mut cfg = Config::new(HardwareAddress::Ethernet(EthernetAddress(mac)));
        // 固定种子：本模块单线程、且 ISN 随机性不是我们的安全边界（连接是我们自己终结的）。
        cfg.random_seed = 0x5b5b_5b5b_5b5b_5b5b;
        let mut iface = Interface::new(cfg, &mut device, Instant::from_millis(now_ms as i64));

        // smoltcp 0.12 的 Ipv4Address/Ipv6Address 就是 core::net 的类型
        let addr: IpAddress = if is_v6 {
            IpAddress::Ipv6(Ipv6Address::from(ip))
        } else {
            let mut o = [0u8; 4];
            o.copy_from_slice(&ip[..4]);
            IpAddress::Ipv4(Ipv4Address::from(o))
        };
        // update_ip_addrs 的闭包返回 ()，用捕获的标志把结果带出来
        let mut ok = false;
        iface.update_ip_addrs(|a| ok = a.push(IpCidr::new(addr, prefix)).is_ok());
        if !ok {
            return false;
        }

        // ★ any_ip：收下「目的 IP 不是本机」的包（等价于 lwIP 的 accept-all 补丁）。
        iface.set_any_ip(true);
        // ★ 单开 any_ip 不够：smoltcp 收到非本机目的的包时还要过一道 routes.lookup(dst)，
        //   而且判据是**查出来的下一跳必须是我们自己的地址**（ipv4.rs 里那句
        //   `.map_or(true, |router_addr| !self.has_ip_addr(router_addr))`，注释叫
        //   "check if the packet is routed locally"）。所以默认路由的网关要填**本栈自己的 IP**；
        //   填对端会被判成"不是本地路由"照样丢，症状是 ARP 通、ping 通、TCP 全超时且零日志。
        //   这条是 bench 阶段 R5 用两轮才定位到的，别改。
        let routed = match addr {
            IpAddress::Ipv4(v4) => iface.routes_mut().add_default_ipv4_route(v4).is_ok(),
            IpAddress::Ipv6(v6) => iface.routes_mut().add_default_ipv6_route(v6).is_ok(),
        };
        if !routed {
            return false;
        }

        let mut sockets = SocketSet::new(Vec::new());
        let mut listeners = Vec::with_capacity(LISTEN_BACKLOG);
        for _ in 0..LISTEN_BACKLOG {
            match new_listener(&mut sockets) {
                Some(h) => listeners.push(h),
                None => return false,
            }
        }

        self.nics.insert(
            nic,
            Nic {
                iface,
                device,
                sockets,
                listeners,
                portmap: PortMap::new(),
                our_mac: mac,
                our_ip: ip,
                our_is_v6: is_v6,
                neighbors: BTreeMap::new(),
            },
        );
        true
    }

    pub fn remove_nic(&mut self, nic: NicId) -> bool {
        if self.nics.remove(&nic).is_none() {
            return false;
        }
        // 该卡上的连接一并作废（socket 随 SocketSet 一起没了）
        self.conns.retain(|_, c| c.nic != nic);
        true
    }

    /// 喂帧：改写目的端口 → 推进收队列。帧内容会被拷贝（调用方的缓冲只在调用内有效）。
    pub fn input(&mut self, nic: NicId, frame: &[u8]) -> bool {
        if !self.nics.contains_key(&nic) {
            return false;
        }
        // ★ 先从这一帧的 (源MAC, 源IP) 自动学邻居 —— 必须在真帧之前入队，
        //   否则 smoltcp 解析完 TCP 想回包时查不到 MAC，会改发 ARP 请求，
        //   而 ARP 应答在生产里根本进不到栈（被 LanGateway 截给 ArpSpoofer 了）
        //   → 回包永久黑洞。lwIP 那边是靠 etharp_add_static_entry 显式注入解决的；
        //   这里从帧里自动学，连 API 都不用加。
        self.learn_neighbor(nic, frame);

        let Some(n) = self.nics.get_mut(&nic) else { return false };
        let mut buf = frame.to_vec();
        portmap::rewrite_rx(&mut buf, &mut n.portmap);
        n.device.push_rx(buf);
        true
    }

    /// 从收到的帧里学 (源IP → 源MAC)，并合成一个 ARP 应答喂进栈让它填邻居缓存。
    ///
    /// 为什么用「合成 ARP 应答」而不是直接写缓存：smoltcp 0.12 的 NeighborCache 是
    /// InterfaceInner 的私有字段，只暴露了 flush。合成帧是**零 fork** 的做法，且与
    /// 我们本来就掌控入口帧的事实天然契合。
    fn learn_neighbor(&mut self, nic: NicId, frame: &[u8]) {
        const REFRESH_MS: u64 = 30_000; // 邻居项会过期，按间隔刷新
        if frame.len() < 34 {
            return;
        }
        let ethertype = u16::from_be_bytes([frame[12], frame[13]]);
        if ethertype != 0x0800 {
            return; // v6 的邻居注入见 TODO（需合成 ICMPv6 NA）
        }
        let mut src_mac = [0u8; 6];
        src_mac.copy_from_slice(&frame[6..12]);
        let mut src_ip = [0u8; 16];
        src_ip[..4].copy_from_slice(&frame[26..30]);

        let now = self.last_seen_ms;
        let Some(n) = self.nics.get_mut(&nic) else { return };
        if n.our_is_v6 {
            return;
        }
        match n.neighbors.get(&src_ip) {
            Some((mac, t)) if *mac == src_mac && now.saturating_sub(*t) < REFRESH_MS => return,
            _ => {}
        }
        n.neighbors.insert(src_ip, (src_mac, now));

        // 合成 ARP 应答：sha/spa = 设备，tha/tpa = 本机
        let mut a = vec![0u8; 42];
        a[0..6].copy_from_slice(&n.our_mac);
        a[6..12].copy_from_slice(&src_mac);
        a[12] = 0x08;
        a[13] = 0x06; // ARP
        a[14] = 0x00; a[15] = 0x01; // htype ethernet
        a[16] = 0x08; a[17] = 0x00; // ptype ipv4
        a[18] = 6; a[19] = 4;
        a[20] = 0x00; a[21] = 0x02; // oper = reply
        a[22..28].copy_from_slice(&src_mac);
        a[28..32].copy_from_slice(&src_ip[..4]);
        a[32..38].copy_from_slice(&n.our_mac);
        a[38..42].copy_from_slice(&n.our_ip[..4]);
        n.device.push_rx(a);
    }

    /// 跑一轮 poll，**只收集事件不派发回调**（重入安全，见文件头）。
    pub fn poll_collect(&mut self, now_ms: u64, out: &mut Vec<Event>) {
        self.last_seen_ms = now_ms;
        let now = Instant::from_millis(now_ms as i64);
        // 收集本轮要发的事件；nic 逐个处理
        let nic_ids: Vec<NicId> = self.nics.keys().copied().collect();
        for nid in nic_ids {
            // 反复 poll 直到没有新进展或收队列排空（一次 poll 只处理一帧）
            loop {
                let Some(n) = self.nics.get_mut(&nid) else { break };
                let had_rx = n.device.rx_pending();
                let r = n.iface.poll(now, &mut n.device, &mut n.sockets);
                if !had_rx && r == PollResult::None {
                    break;
                }
                if !n.device.rx_pending() && r == PollResult::None {
                    break;
                }
            }
            self.promote_listeners(nid, out);
            self.pump_conns(nid, out);
            self.drain_tx(nid, out);
        }
    }

    /// 监听 socket 被连上 → 转成连接，并补一个新的监听顶上。
    fn promote_listeners(&mut self, nid: NicId, out: &mut Vec<Event>) {
        let Some(n) = self.nics.get_mut(&nid) else { return };
        let promoted: Vec<SocketHandle> = n
            .listeners
            .iter()
            .copied()
            .filter(|h| {
                let s = n.sockets.get::<tcp::Socket>(*h);
                !matches!(s.state(), tcp::State::Listen | tcp::State::Closed)
            })
            .collect();

        for h in promoted {
            n.listeners.retain(|x| *x != h);
            if let Some(nh) = new_listener(&mut n.sockets) {
                n.listeners.push(nh);
            }

            let (cip, cport, sip, is_v6) = {
                let s = n.sockets.get::<tcp::Socket>(h);
                match (s.remote_endpoint(), s.local_endpoint()) {
                    (Some(rem), Some(loc)) => {
                        let (c, v6) = addr_bytes(rem.addr);
                        let (sv, _) = addr_bytes(loc.addr);
                        (c, rem.port, sv, v6)
                    }
                    _ => {
                        // 拿不到端点：状态异常，直接 abort，不建连接
                        n.sockets.get_mut::<tcp::Socket>(h).abort();
                        self.conns_refused += 1;
                        continue;
                    }
                }
            };

            // 查回真实目的端口（RX 改写时记的）
            let Some(real_dport) = n.portmap.lookup(&cip, cport, &sip) else {
                // 没有映射说明这条连接不是经我们改写进来的（不该发生）——保守拒绝
                n.sockets.get_mut::<tcp::Socket>(h).abort();
                self.conns_refused += 1;
                continue;
            };

            let id = self.next_id;
            self.next_id = self.next_id.wrapping_add(1);
            if self.next_id == 0 {
                self.next_id = 1; // 0 恒为无效
            }
            self.conns.insert(
                id,
                Conn { nic: nid, handle: h, peeked: 0, last_send_queue: 0, cip, cport, sip, closed_reported: false },
            );
            self.conns_accepted += 1;
            out.push(Event::ConnNew { id, nic: nid, src: cip, is_v6, sport: cport, dst: sip, dport: real_dport });
        }
    }

    /// 每条连接：上行数据（peek，不消费）、下行已发出量、结束通知。
    fn pump_conns(&mut self, nid: NicId, out: &mut Vec<Event>) {
        let ids: Vec<ConnId> = self.conns.iter().filter(|(_, c)| c.nic == nid).map(|(k, _)| *k).collect();
        for id in ids {
            let Some(c) = self.conns.get_mut(&id) else { continue };
            let Some(n) = self.nics.get_mut(&nid) else { continue };
            let s = n.sockets.get_mut::<tcp::Socket>(c.handle);

            // ① 上行：peek 出「已到达但还没交给 C++」的部分。
            //    ★ 只 peek 不 recv —— recv 会释放 rx 缓冲、打开通告窗口，那正是我们要扣住的闸门。
            //      窗口在 C++ 调 coast_conn_recved 之后才还（见 Engine::recved）。
            let avail = s.recv_queue();
            if avail > c.peeked {
                let want = avail - c.peeked;
                let mut buf = vec![0u8; want];
                // peek_slice 从队首开始，所以要跳过已经交出去的 c.peeked 字节
                let mut tmp = vec![0u8; avail];
                let got = s.peek_slice(&mut tmp).unwrap_or(0);
                if got > c.peeked {
                    let n_new = got - c.peeked;
                    buf.truncate(n_new);
                    buf.copy_from_slice(&tmp[c.peeked..got]);
                    c.peeked += n_new;
                    out.push(Event::ConnData { id, data: buf });
                }
            }

            // ② 下行：send_queue 减少的量 = 已经发出去并被确认腾出的缓冲（对应 tcp_sent）
            let sq = s.send_queue();
            if sq < c.last_send_queue {
                let freed = (c.last_send_queue - sq) as u32;
                out.push(Event::ConnSent { id, n: freed });
            }
            c.last_send_queue = sq;

            // ③ 结束
            let st = s.state();
            let dead = matches!(st, tcp::State::Closed) || !s.is_open();
            if dead && !c.closed_reported {
                c.closed_reported = true;
                // smoltcp 不直接暴露"是 RST 还是正常关"，用是否还有未确认数据近似：
                // 走到 Closed 且发送队列非空 → 对端 RST 掉了。
                let is_abort = sq > 0;
                out.push(Event::ConnClosed { id, is_abort });
            }
        }
        // 已上报结束的连接，清掉（连同 portmap 项，否则表随连接泄漏）
        let dead: Vec<ConnId> = self
            .conns
            .iter()
            .filter(|(_, c)| c.closed_reported && c.nic == nid)
            .map(|(k, _)| *k)
            .collect();
        for id in dead {
            self.drop_conn(id);
        }
    }

    fn drain_tx(&mut self, nid: NicId, out: &mut Vec<Event>) {
        let Some(n) = self.nics.get_mut(&nid) else { return };
        let frames = n.device.take_tx();
        for mut f in frames {
            // 出方向把源端口从 FIXED_PORT 改回真实目的端口
            portmap::rewrite_tx(&mut f, &n.portmap);
            self.tx_frames += 1;
            out.push(Event::OutFrame { nic: nid, data: f });
        }
    }

    fn drop_conn(&mut self, id: ConnId) {
        if let Some(c) = self.conns.remove(&id) {
            if let Some(n) = self.nics.get_mut(&c.nic) {
                n.portmap.forget(&c.cip, c.cport, &c.sip);
                n.sockets.remove(c.handle);
            }
            self.conns_closed += 1;
        }
    }

    // ———————————————————————— 连接操作（FFI 转发过来）————————————————————————

    pub fn send(&mut self, id: ConnId, data: &[u8]) -> Result<usize, i32> {
        let c = self.conns.get(&id).ok_or(crate::ffi::COAST_ERR_NOCONN)?;
        let n = self.nics.get_mut(&c.nic).ok_or(crate::ffi::COAST_ERR_NONIC)?;
        let s = n.sockets.get_mut::<tcp::Socket>(c.handle);
        if !s.may_send() {
            return Err(crate::ffi::COAST_ERR_STATE);
        }
        s.send_slice(data).map_err(|_| crate::ffi::COAST_ERR_STATE)
    }

    pub fn sndbuf(&mut self, id: ConnId) -> Result<usize, i32> {
        let c = self.conns.get(&id).ok_or(crate::ffi::COAST_ERR_NOCONN)?;
        let n = self.nics.get_mut(&c.nic).ok_or(crate::ffi::COAST_ERR_NONIC)?;
        let s = n.sockets.get::<tcp::Socket>(c.handle);
        Ok(s.send_capacity() - s.send_queue())
    }

    /// 归还接收窗口 —— 真正把字节从 smoltcp 的 rx 缓冲里消费掉。
    pub fn recved(&mut self, id: ConnId, want: u32) -> Result<(), i32> {
        let c = self.conns.get_mut(&id).ok_or(crate::ffi::COAST_ERR_NOCONN)?;
        let n = self.nics.get_mut(&c.nic).ok_or(crate::ffi::COAST_ERR_NONIC)?;
        let s = n.sockets.get_mut::<tcp::Socket>(c.handle);
        let take = (want as usize).min(c.peeked);
        if take == 0 {
            return Ok(());
        }
        let mut sink = vec![0u8; take];
        let got = s.recv_slice(&mut sink).map_err(|_| crate::ffi::COAST_ERR_STATE)?;
        c.peeked -= got;
        Ok(())
    }

    pub fn close(&mut self, id: ConnId) -> Result<(), i32> {
        let c = self.conns.get(&id).ok_or(crate::ffi::COAST_ERR_NOCONN)?;
        // ★ 窗口没还满就关，栈会改发 RST 而不是 FIN，设备侧表现为"下载到一半被断"。
        //   lwIP 那边是静默劣化（tcp_close 看到 rcv_wnd != TCP_WND_MAX 就发 RST，
        //   NetStack.cpp:709-713）；这里做成**显式失败**，让调用方必须先还窗口。
        if c.peeked != 0 {
            return Err(crate::ffi::COAST_ERR_STATE);
        }
        let n = self.nics.get_mut(&c.nic).ok_or(crate::ffi::COAST_ERR_NONIC)?;
        n.sockets.get_mut::<tcp::Socket>(c.handle).close();
        Ok(())
    }

    pub fn abort(&mut self, id: ConnId) -> Result<(), i32> {
        let c = self.conns.get(&id).ok_or(crate::ffi::COAST_ERR_NOCONN)?;
        let nid = c.nic;
        let h = c.handle;
        {
            let n = self.nics.get_mut(&nid).ok_or(crate::ffi::COAST_ERR_NONIC)?;
            n.sockets.get_mut::<tcp::Socket>(h).abort();
        }
        self.conns_aborted += 1;
        Ok(())
    }

    /// 关掉某个设备地址上的所有连接。对应 lwIP 那边遍历 tcp_active_pcbs。
    /// 不能省：lwIP 的 established 超时是 24 小时，不主动关会造成慢性连接表泄漏。
    pub fn close_device_conns(&mut self, dev: &[u8; 16]) -> i32 {
        let ids: Vec<ConnId> =
            self.conns.iter().filter(|(_, c)| &c.cip == dev).map(|(k, _)| *k).collect();
        let cnt = ids.len() as i32;
        for id in ids {
            let _ = self.abort(id);
        }
        cnt
    }

    /// 下一次期望被调用的延迟（毫秒）。没有待办定时器时返回 u64::MAX。
    pub fn next_poll_ms(&mut self, now_ms: u64) -> u64 {
        let now = Instant::from_millis(now_ms as i64);
        let mut best: Option<Duration> = None;
        for n in self.nics.values_mut() {
            if n.device.rx_pending() {
                return 0; // 还有帧没处理完，立刻再来一轮
            }
            if let Some(d) = n.iface.poll_delay(now, &n.sockets) {
                best = Some(match best {
                    Some(b) if b < d => b,
                    _ => d,
                });
            }
        }
        best.map(|d| d.total_millis()).unwrap_or(u64::MAX)
    }
}

fn addr_bytes(a: IpAddress) -> ([u8; 16], bool) {
    let mut out = [0u8; 16];
    match a {
        IpAddress::Ipv4(v4) => {
            out[..4].copy_from_slice(&v4.octets());
            (out, false)
        }
        IpAddress::Ipv6(v6) => {
            out.copy_from_slice(&v6.octets());
            (out, true)
        }
    }
}
