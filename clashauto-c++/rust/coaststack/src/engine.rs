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
use alloc::collections::VecDeque;
use alloc::vec;
use alloc::vec::Vec;

use smoltcp::iface::{Config, Interface, PollResult, SocketHandle, SocketSet};
use smoltcp::phy::{Checksum, Device, DeviceCapabilities, Medium, RxToken, TxToken};
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
/// 排空收队列时，每处理多少帧补一次监听 socket（见 poll_collect 里的说明）。
const PROMOTE_EVERY: u32 = 16;

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
    /// ★ VecDeque 不是 Vec：原来是 `rx.remove(0)`，**每取一帧就 memmove 整个队列**，
    ///   队列深时退化成 O(n²)。千兆线速下一拍能积 ~2000 帧，正好落在最坏区间。
    rx: VecDeque<Vec<u8>>,
    tx: Vec<Vec<u8>>,
    mtu: usize,
    /// **仅供归因测量**：true = 收包不验 TCP 校验和（发送仍然生成）。
    ///
    /// ⚠️ 生产恒为 false，而且**不该**改成 true：我们是中间盒，终结设备的 TCP 之后
    ///    再以新连接转发出去，转发时会算一个**有效**的校验和。收包不验 = 把损坏的数据
    ///    洗成"看起来正确"的新连接，端到端校验就此失效。以太网 FCS 逐跳重算，救不了这个。
    ///    所以它只用来回答"校验和占那 33% 的多少"，不是可选的优化项。
    skip_rx_checksum: bool,

    /// 本次 `iface.poll()` 还允许交出多少帧；`usize::MAX` = 不限。
    ///
    /// 为什么需要它：smoltcp 的 `Interface::poll()` **内部会一直 receive() 到设备说没有为止**，
    /// 不是"一次一帧"（engine 里那句老注释是错的）。于是一次 poll 就能吃光全部 32 个
    /// 监听 socket，中途补充监听根本没机会插进去 —— 表现为一拍最多接受 32 条新连接。
    /// 给设备加个预算，把"一次 poll"切成小块，补充逻辑才有落点。
    rx_budget: usize,
}

impl QueueDevice {
    /// 收队列上限（帧）。超出即丢最旧的，并记 rx_dropped。
    ///
    /// ★ 为什么必须有界：被劫持设备是**不可信输入源**，而这个队列此前是无界 Vec ——
    ///   RxWorker 那套 8192 帧的高水位反压**根本不会触发**，因为它的 consumed() 语义是
    ///   「已交给引擎」而非「已处理」，工作线程永远"跟得上"。真实积压全落在这里，
    ///   没上限、没计数、没反压。本仓库被同类问题咬过（DNS 洪水打崩网关）。
    ///
    /// 取 16384：千兆线速下一拍（25ms 兜底周期）约 2000 帧，留 8 倍余量；
    /// 满帧时约 24 MB，是能接受的最坏驻留。丢最旧而不是最新 —— 新帧承载的是当前
    /// 窗口的进展，旧帧多半已经因为出窗而无用了。
    const RX_QUEUE_MAX: usize = 16384;

    fn new(mtu: usize) -> Self {
        Self { rx: VecDeque::new(), tx: Vec::new(), mtu, skip_rx_checksum: false, rx_budget: usize::MAX }
    }
    /// 返回 false = 队列已满，这一帧被丢（调用方据此记 rx_dropped）。
    fn push_rx(&mut self, frame: Vec<u8>) -> bool {
        if self.rx.len() >= Self::RX_QUEUE_MAX {
            self.rx.pop_front(); // 丢最旧
            self.rx.push_back(frame);
            return false;
        }
        self.rx.push_back(frame);
        true
    }
    fn take_tx(&mut self) -> Vec<Vec<u8>> {
        core::mem::take(&mut self.tx)
    }
    fn rx_pending(&self) -> bool {
        !self.rx.is_empty()
    }
    /// 取出发送队列里所有 ICMPv6 邻居请求(NS)的副本，供 answer_ns 代答用。
    fn tx_peek_ns(&self) -> Vec<Vec<u8>> {
        self.tx
            .iter()
            .filter(|f| {
                f.len() >= 14 + 40 + 24 && f[12] == 0x86 && f[13] == 0xDD && f[20] == 58
                    && f[54] == 135
            })
            .cloned()
            .collect()
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
        if self.rx_budget == 0 || self.rx.is_empty() {
            return None;
        }
        self.rx_budget -= 1;
        let frame = self.rx.pop_front()?;
        Some((QRx(frame), QTx(&mut self.tx)))
    }
    fn transmit(&mut self, _t: Instant) -> Option<Self::TxToken<'_>> {
        Some(QTx(&mut self.tx))
    }
    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.medium = Medium::Ethernet;
        c.max_transmission_unit = self.mtu;
        if self.skip_rx_checksum {
            // Tx = 只在发送时生成，收包不验（见字段上的警告，仅供测量）
            c.checksum.tcp = Checksum::Tx;
            c.checksum.ipv4 = Checksum::Tx;
        }
        c
    }
}

// ———————————————————————— 网卡 ————————————————————————

struct Nic {
    iface: Interface,
    device: QueueDevice,
    sockets: SocketSet<'static>,
    listeners: Vec<SocketHandle>,
    /// 已离开 Listen、但**还没**完成三次握手的 socket。
    /// ★ 必须等 Established 才 promote：lwIP 的 accept 回调就是在连接建立后才触发的，
    ///   若在 SynReceived 就拨上游，一次 SYN 洪水会给每个 SYN 各建一条 SOCKS 连接
    ///   （放大攻击）。这个差异是 A/B 自测抓出来的 —— 同一套断言在 lwIP 上跑不过。
    pending: Vec<SocketHandle>,
    portmap: PortMap,
    /// 本机在这张卡上的 MAC 与 IP —— 合成 ARP 应答时要用（见 learn_neighbor）。
    our_mac: [u8; 6],
    our_ip: [u8; 16],
    our_is_v6: bool,
    /// 已注入邻居缓存的 (设备IP → MAC, 上次注入时刻ms)。
    /// smoltcp 的邻居项会过期，所以要按间隔刷新，不能只注入一次。
    neighbors: BTreeMap<[u8; 16], ([u8; 6], u64)>,
    /// 已加过 /128 直连路由的设备 v6（路由槽有限，别重复 push）。
    v6_routed: alloc::collections::BTreeSet<[u8; 16]>,
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
    /// pump_conns 复用的连接 id 暂存表（避免每轮 poll 一次分配，见该函数）。
    pump_scratch: Vec<ConnId>,
    /// 仅供归因测量，见 set_skip_rx_checksum。新建网卡时套用到它的 device 上。
    skip_rx_checksum: bool,
    /// 收队列满而丢掉的帧数。非 0 = 工作线程跟不上（不是链路丢包），
    /// 与 txdrop/rxdrop 分属不同环节，别混。
    pub rx_overflow: u64,
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
            pump_scratch: Vec::new(),
            skip_rx_checksum: false,
            rx_overflow: 0,
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
        // ★ 必须在 Interface::new 之前设：smoltcp 会把 capabilities 拷进 InterfaceInner 缓存。
        device.skip_rx_checksum = self.skip_rx_checksum;
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
        // ★ 双栈：v4 网卡同时挂一条 EUI-64 链路本地 v6（fe80::/64）。
        //   生产的网卡就是双栈的（lwIP 侧用 netif_create_ip6_linklocal_address 做同一件事，
        //   NetStack.cpp:1422）。只挂 v4 的话，设备的 v6 流量进来会因为"没有本族地址"被丢，
        //   表现正是本项目已知的高危症状「双栈设备 v6 漏代理」。
        let mut ok = false;
        iface.update_ip_addrs(|a| {
            ok = a.push(IpCidr::new(addr, prefix)).is_ok();
            if ok && !is_v6 {
                let ll = link_local_from_mac(&mac);
                // 挂不上不算致命（v4 仍可用），但要能看出来
                let _ = a.push(IpCidr::new(IpAddress::Ipv6(ll), 64));
            }
        });
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
        // ★ 双栈时**两个族都要有默认路由**：any_ip 的检查是分族做的，
        //   只加 v4 路由的话，v6 包会在 routes.lookup 那关被丢 —— 症状是 v6 一个出帧都没有，
        //   和"根本没接管"长得一模一样。这正是 v4 那条坑（R5 用两轮才定位）的 v6 翻版。
        if !is_v6 {
            let ll = link_local_from_mac(&mac);
            let _ = iface.routes_mut().add_default_ipv6_route(ll);
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
                pending: Vec::new(),
                portmap: PortMap::new(),
                our_mac: mac,
                our_ip: ip,
                our_is_v6: is_v6,
                neighbors: BTreeMap::new(),
                v6_routed: alloc::collections::BTreeSet::new(),
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

    /// 仅供归因测量：关掉收包方向的校验和验证（见 `QueueDevice::skip_rx_checksum`）。
    ///
    /// ★ **必须在 `add_nic` 之前调**。smoltcp 的 `Interface::new()` 会把 device 的
    ///   capabilities **拷进 InterfaceInner 缓存起来**，建好之后再改设备是没有效果的。
    ///   第一版就是建完网卡再改，于是坏帧照样被丢 —— 而据此得出的「校验和不要钱」
    ///   是个假结论。是 `skip_rx_checksum_knob_actually_works` 那条证伪测试拦下来的。
    pub fn set_skip_rx_checksum(&mut self, on: bool) {
        self.skip_rx_checksum = on;
        for n in self.nics.values_mut() {
            n.device.skip_rx_checksum = on;
        }
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
        if !n.device.push_rx(buf) {
            self.rx_overflow += 1;
        }
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
        if ethertype == 0x86DD {
            self.learn_neighbor_v6(nic, frame);
            return;
        }
        if ethertype != 0x0800 {
            return;
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

    /// v6 版：合成一条 **ICMPv6 邻居通告(NA)** 喂进栈，让 smoltcp 填 v6 邻居缓存。
    ///
    /// 与 v4 同一个理由：NDP 帧在生产里被 LanGateway 截给 NdpSpoofer，根本进不到栈
    /// （NetStack.cpp 的 inputFrame 只放行 TCP/UDP），所以 smoltcp 发出的邻居请求永远等不到
    /// 回复 → v6 回包黑洞。lwIP 那边是靠 Coast 自己打的 nd6 静态邻居补丁解决的
    /// （nd6.c:1586 + 静态项永不老化）；这里同样零 fork。
    ///
    /// **这条不做的话，症状正是本项目已知的高危项「双栈设备 v6 漏代理」。**
    fn learn_neighbor_v6(&mut self, nic: NicId, frame: &[u8]) {
        const REFRESH_MS: u64 = 30_000;
        if frame.len() < 14 + 40 {
            return;
        }
        let mut src_mac = [0u8; 6];
        src_mac.copy_from_slice(&frame[6..12]);
        let mut src_ip = [0u8; 16];
        src_ip.copy_from_slice(&frame[22..38]);
        // 未指定地址(::)不学
        if src_ip.iter().all(|&b| b == 0) {
            return;
        }

        let now = self.last_seen_ms;
        let Some(n) = self.nics.get_mut(&nic) else { return };
        match n.neighbors.get(&src_ip) {
            Some((mac, t)) if *mac == src_mac && now.saturating_sub(*t) < REFRESH_MS => return,
            _ => {}
        }
        n.neighbors.insert(src_ip, (src_mac, now));

        // ★★ 关键：给这个设备加一条 /128 **直连**路由（下一跳 = 它自己）。
        //   根因（smoltcp net_trace 抓到的）：设备的全局 v6 相对本机唯一的 v6 地址
        //   （EUI-64 链路本地 fe80::/64）是 **off-link**，回包因此走默认路由，而默认路由的
        //   下一跳是**本机自己的链路本地地址** —— smoltcp 去邻居缓存里找它、当然找不到，
        //   于是永远只发 Neighbor Solicitation，SYN-ACK 一次都出不去。
        //   trace 原文：`address fe80::5bff:fe00:2 not in neighbor cache, sending NS`。
        //   v4 没这个问题纯属巧合：设备 IP 与本机同 /24，天然 on-link、直接查邻居。
        //   加了 /128 直连路由后，回包直接解析设备本身的 MAC（已由上面的 NA 填好）。
        //   注意**不能**改成"把默认路由的下一跳指向设备"——那样多设备时所有回包都会发给
        //   最后一个学到的 MAC。每设备一条 /128 才是对的，也是要把 IFACE_MAX_ROUTE_COUNT
        //   调大的原因（见 .cargo/config.toml）。
        // 判重靠自己的 v6_routed 集合：smoltcp 的 Routes 没有只读遍历 API，
        // 重复 push 会白占本就有限的路由槽。
        let dev_v6 = Ipv6Address::from(src_ip);
        if n.v6_routed.insert(src_ip) {
            n.iface.routes_mut().update(|list| {
                let _ = list.push(smoltcp::iface::Route {
                    cidr: IpCidr::new(IpAddress::Ipv6(dev_v6), 128),
                    via_router: IpAddress::Ipv6(dev_v6),
                    preferred_until: None,
                    expires_at: None,
                });
            });
        }

        // ★ 这是一条**未经请求**的 NA（smoltcp 还没问过）。按 RFC 4861，未经请求的 NA
        //   发往全节点组播 ff02::1，标志位只置 Override（不置 Solicited —— 没人请求过）。
        //   之前发给本机链路本地单播 + Solicited，被 smoltcp 丢掉了。
        let mut dst_b = [0u8; 16];
        dst_b[0] = 0xff;
        dst_b[1] = 0x02;
        dst_b[15] = 0x01;
        let our_ll_b = dst_b;

        // eth(14) + ipv6(40) + ICMPv6 NA(24) + TLLA 选项(8) = 86
        let icmp_len = 24 + 8usize;
        let mut f = vec![0u8; 14 + 40 + icmp_len];
        // 组播 IPv6 → 以太目的是 33:33 + 目的地址后 4 字节（RFC 2464）
        f[0] = 0x33; f[1] = 0x33;
        f[2..6].copy_from_slice(&dst_b[12..16]);
        f[6..12].copy_from_slice(&src_mac);
        f[12] = 0x86;
        f[13] = 0xDD;
        // IPv6 头
        f[14] = 0x60; // version 6
        f[18..20].copy_from_slice(&(icmp_len as u16).to_be_bytes()); // payload length
        f[20] = 58; // next header = ICMPv6
        f[21] = 255; // hop limit（NDP 强制 255，否则被丢）
        f[22..38].copy_from_slice(&src_ip); // src = 设备
        f[38..54].copy_from_slice(&our_ll_b); // dst = 我们
        // ICMPv6 邻居通告
        let i = 54usize;
        f[i] = 136; // type = NA
        f[i + 1] = 0; // code
        f[i + 4] = 0x20; // flags: 仅 Override（未经请求，故不置 Solicited）
        f[i + 8..i + 24].copy_from_slice(&src_ip); // target = 设备自己
        f[i + 24] = 2; // option: Target Link-Layer Address
        f[i + 25] = 1; // 长度 = 1 × 8 字节
        f[i + 26..i + 32].copy_from_slice(&src_mac);
        // ICMPv6 校验和（伪首部：src + dst + 长度 + next header）
        let mut ph = 0u32;
        ph = ones_sum16(&src_ip, ph);
        ph = ones_sum16(&our_ll_b, ph);
        ph += icmp_len as u32;
        ph += 58;
        let c = fold16(ones_sum16(&f[i..i + icmp_len], ph));
        f[i + 2..i + 4].copy_from_slice(&c.to_be_bytes());

        n.device.push_rx(f);
    }

    /// 跑一轮 poll，**只收集事件不派发回调**（重入安全，见文件头）。
    pub fn poll_collect(&mut self, now_ms: u64, out: &mut Vec<Event>) {
        self.last_seen_ms = now_ms;
        let now = Instant::from_millis(now_ms as i64);
        // 收集本轮要发的事件；nic 逐个处理
        let nic_ids: Vec<NicId> = self.nics.keys().copied().collect();
        for nid in nic_ids {
          // ★ 最多两轮：第一轮可能因为缺 v6 邻居而只发出一条 NS，drain_tx 会就地代答一条 NA
          //   注入收队列；不再跑一轮的话，那条 NA 要等下一次泵（25ms）才被消化，
          //   表现为"第一个 v6 连接慢一拍"。两轮即可收敛（NA 进来 → 重发 SYN-ACK）。
          for _round in 0..2 {
            // 反复 poll 直到没有新进展或收队列排空（一次 poll 只处理一帧）
            //
            // ★ 排空**途中**必须补充监听 socket。原来 promote_listeners 只在这个循环
            //   之后跑一次，于是「一拍之内最多接受 LISTEN_BACKLOG(32) 条新连接」：
            //   同一个 25ms 窗口里第 33 条起的 SYN 找不到处于 Listen 的 socket，被丢/回 RST，
            //   设备要等初始 RTO（约 1 秒）重传。而多源页面同时开 30~100 条连接很常见 ——
            //   症状是"偶发某些资源加载特别慢"，极难归因，且这条路**没有计数器**。
            //   修法不能是调大 LISTEN_BACKLOG：每个监听 socket 都实打实占
            //   RX_BUF+TX_BUF = 256 KiB，32 个已经是 8 MB 常驻，再乘 8 就是 64 MB。
            //   所以改成排空途中按帧数分摊地补 —— 池子不变，但一拍能接受的连接数不再有上限。
            loop {
                // ★ 借用必须收在这个块里：promote_listeners 要 &mut self，
                //   而 n 借的是 self.nics —— 不放开就是 E0499。
                let (had_rx, r, still_rx) = {
                    let Some(n) = self.nics.get_mut(&nid) else { break };
                    let had_rx = n.device.rx_pending();
                    // 每次 iface.poll 只放 PROMOTE_EVERY 帧进去，好让下面的补充逻辑插得进来
                    n.device.rx_budget = PROMOTE_EVERY as usize;
                    let r = n.iface.poll(now, &mut n.device, &mut n.sockets);
                    n.device.rx_budget = usize::MAX;
                    (had_rx, r, n.device.rx_pending())
                };
                if !had_rx && r == PollResult::None {
                    break;
                }
                // 每轮补一次监听 socket（一轮最多 PROMOTE_EVERY 帧，见上）
                self.promote_listeners(nid, out);
                if !still_rx && r == PollResult::None {
                    break;
                }
            }
            self.promote_listeners(nid, out);
            self.pump_conns(nid, out);
            self.drain_tx(nid, out);
            // drain_tx 里的 answer_ns 可能刚往收队列塞了 NA —— 有就再跑一轮消化掉
            let more = self.nics.get(&nid).map(|n| n.device.rx_pending()).unwrap_or(false);
            if !more {
                break;
            }
          }
        }
    }

    /// 监听 socket 被连上 → 转成连接，并补一个新的监听顶上。
    fn promote_listeners(&mut self, nid: NicId, out: &mut Vec<Event>) {
        let Some(n) = self.nics.get_mut(&nid) else { return };
        // ① 离开 Listen 的挪进 pending，并补一个新监听顶上（保持突发容忍度）
        let left: Vec<SocketHandle> = n
            .listeners
            .iter()
            .copied()
            .filter(|h| {
                let s = n.sockets.get::<tcp::Socket>(*h);
                !matches!(s.state(), tcp::State::Listen)
            })
            .collect();
        for h in left {
            n.listeners.retain(|x| *x != h);
            n.pending.push(h);
            if let Some(nh) = new_listener(&mut n.sockets) {
                n.listeners.push(nh);
            }
        }

        // ② pending 里握手失败/被拆的，直接回收
        let dead: Vec<SocketHandle> = n
            .pending
            .iter()
            .copied()
            .filter(|h| matches!(n.sockets.get::<tcp::Socket>(*h).state(), tcp::State::Closed))
            .collect();
        for h in dead {
            n.pending.retain(|x| *x != h);
            n.sockets.remove(h);
        }

        // ③ 只有真正 Established 的才 promote 成连接（与 lwIP 的 accept 时机一致）
        let promoted: Vec<SocketHandle> = n
            .pending
            .iter()
            .copied()
            .filter(|h| {
                matches!(n.sockets.get::<tcp::Socket>(*h).state(), tcp::State::Established)
            })
            .collect();

        for h in promoted {
            n.pending.retain(|x| *x != h);

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
        // ★ 复用暂存表，别每轮都分配。
        //   poll 从 40 次/秒提到亚毫秒级（NetStack.cpp 的 schedulePoll）之后，这个函数的
        //   调用频率涨了一两个数量级 —— 原来每次都 collect 一个新 Vec，在 2000 条连接时
        //   就是每秒几十 MB 的分配。提频是我们自己做的改动，它带来的成本也得自己收掉。
        //   ⚠️ 仍是 O(全部连接) 的扫描（按 nic 过滤）。多网卡时是 O(网卡 × 连接)；
        //      真到那一步应该改成按 nic 建索引，这里先把分配这一半解决掉。
        let mut ids = core::mem::take(&mut self.pump_scratch);
        ids.clear();
        ids.extend(self.conns.iter().filter(|(_, c)| c.nic == nid).map(|(k, _)| *k));
        for &id in ids.iter() {
            let Some(c) = self.conns.get_mut(&id) else { continue };
            let Some(n) = self.nics.get_mut(&nid) else { continue };
            let s = n.sockets.get_mut::<tcp::Socket>(c.handle);

            // ① 上行：peek 出「已到达但还没交给 C++」的部分。
            //    ★ 只 peek 不 recv —— recv 会释放 rx 缓冲、打开通告窗口，那正是我们要扣住的闸门。
            //      窗口在 C++ 调 coast_conn_recved 之后才还（见 Engine::recved）。
            //    ★ 只取**新增的尾巴**，别每次重拷整段积压。
            //      原实现每拍分配 avail 字节并 peek_slice 整段（最坏 RX_BUF=128 KiB），
            //      再把前 c.peeked 字节丢掉 —— 单包到达时放大可达 40 倍以上。
            //      25ms 一拍时这笔浪费还不显眼；把 poll 提到每轮事件循环一次之后，
            //      它会直接变成 CPU 瓶颈，所以必须和提频同批修。
            let avail = s.recv_queue();
            if avail > c.peeked {
                // 快路径：环形缓冲未绕回时 peek 一次就能给出连续的全部字节，
                // 直接切走 [peeked..avail] —— 一次分配、一次拷贝、零浪费。
                let mut done = false;
                if let Ok(buf) = s.peek(avail) {
                    if buf.len() >= avail {
                        let data = buf[c.peeked..avail].to_vec();
                        c.peeked = avail;
                        out.push(Event::ConnData { id, data });
                        done = true;
                    }
                }
                if !done {
                    // 慢路径：缓冲绕回，peek 只能给到绕回点 —— 退回整段拷贝。
                    let mut tmp = vec![0u8; avail];
                    let got = s.peek_slice(&mut tmp).unwrap_or(0);
                    if got > c.peeked {
                        let data = tmp[c.peeked..got].to_vec();
                        c.peeked = got;
                        out.push(Event::ConnData { id, data });
                    }
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
        self.pump_scratch = ids; // 还回去，下轮接着用

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

    /// 出方向若是 smoltcp 自己发的**邻居请求(NS)**，就地合成一条**被请求的 NA** 回喂给它。
    ///
    /// 为什么走这条路而不是"主动注入未经请求的 NA"：后者试过两版（单播到本机链路本地 /
    /// 组播到 ff02::1）都被 smoltcp 丢掉，而**回应它自己发出的 NS 是它期望的标准流程**，
    /// 接受条件最宽。生产里这条 NS 本来就发不到设备手上（NDP 帧被 LanGateway 截给 NdpSpoofer），
    /// 所以由我们代答既正确又必要 —— 否则 v6 回包永久黑洞（即"双栈设备 v6 漏代理"）。
    /// 返回 true 表示这一帧是 NS、已就地代答（仍照常发出，无害）。
    fn answer_ns(&mut self, nid: NicId, f: &[u8]) -> bool {
        if f.len() < 14 + 40 + 24 {
            return false;
        }
        if f[12] != 0x86 || f[13] != 0xDD || f[20] != 58 {
            return false;
        }
        let i = 54usize;
        if f[i] != 135 {
            return false; // 不是 Neighbor Solicitation
        }
        let mut target = [0u8; 16];
        target.copy_from_slice(&f[i + 8..i + 24]);
        let mut ns_src = [0u8; 16];
        ns_src.copy_from_slice(&f[22..38]);

        let Some(n) = self.nics.get_mut(&nid) else { return false };
        let Some((dev_mac, _)) = n.neighbors.get(&target).copied() else { return false };

        // 被请求的 NA：src = 被问的那个地址（设备），dst = NS 的源（我们）
        let icmp_len = 24 + 8usize;
        let mut na = vec![0u8; 14 + 40 + icmp_len];
        na[0..6].copy_from_slice(&n.our_mac);
        na[6..12].copy_from_slice(&dev_mac);
        na[12] = 0x86;
        na[13] = 0xDD;
        na[14] = 0x60;
        na[18..20].copy_from_slice(&(icmp_len as u16).to_be_bytes());
        na[20] = 58;
        na[21] = 255; // NDP 强制 255
        na[22..38].copy_from_slice(&target);
        na[38..54].copy_from_slice(&ns_src);
        let k = 54usize;
        na[k] = 136; // NA
        na[k + 4] = 0x60; // Solicited | Override
        na[k + 8..k + 24].copy_from_slice(&target);
        na[k + 24] = 2; // Target Link-Layer Address
        na[k + 25] = 1;
        na[k + 26..k + 32].copy_from_slice(&dev_mac);
        let mut ph = 0u32;
        ph = ones_sum16(&target, ph);
        ph = ones_sum16(&ns_src, ph);
        ph += icmp_len as u32;
        ph += 58;
        let c = fold16(ones_sum16(&na[k..k + icmp_len], ph));
        na[k + 2..k + 4].copy_from_slice(&c.to_be_bytes());

        n.device.push_rx(na);
        true
    }

    fn drain_tx(&mut self, nid: NicId, out: &mut Vec<Event>) {
        // 先扫一遍出帧里的 NS 并代答（要在 take_tx 之前拿到副本判断）
        let ns_frames: Vec<Vec<u8>> = self
            .nics
            .get(&nid)
            .map(|n| n.device.tx_peek_ns())
            .unwrap_or_default();
        for f in ns_frames {
            self.answer_ns(nid, &f);
        }

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

/// MAC → EUI-64 链路本地地址 fe80::/64（与 lwIP 的 netif_create_ip6_linklocal_address 同规则）。
fn link_local_from_mac(mac: &[u8; 6]) -> Ipv6Address {
    let mut b = [0u8; 16];
    b[0] = 0xfe;
    b[1] = 0x80;
    b[8] = mac[0] ^ 0x02; // 翻转 U/L 位
    b[9] = mac[1];
    b[10] = mac[2];
    b[11] = 0xff;
    b[12] = 0xfe;
    b[13] = mac[3];
    b[14] = mac[4];
    b[15] = mac[5];
    Ipv6Address::from(b)
}

fn ones_sum16(data: &[u8], mut sum: u32) -> u32 {
    let mut i = 0;
    while i + 1 < data.len() {
        sum += u16::from_be_bytes([data[i], data[i + 1]]) as u32;
        i += 2;
    }
    if i < data.len() {
        sum += (data[i] as u32) << 8;
    }
    sum
}
fn fold16(mut s: u32) -> u16 {
    while (s >> 16) != 0 {
        s = (s & 0xffff) + (s >> 16);
    }
    !(s as u16)
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
