// 端到端测试 —— 证明引擎**真的能终结一条 TCP 连接**，不是"函数能调通"。
//
// 三条判据，每条都对应一个会造成真实故障的能力：
//   ① 喂合法 SYN 必须吐出 SYN-ACK        → 协议栈活着
//   ② SYN-ACK 的源端口必须是**真实目的端口**（不是内部 FIXED_PORT）→ catch-all 改写出方向闭环。
//      这条错了设备会直接 RST，且症状是"连不上"而非报错。
//   ③ 窗口没还满时 close() 必须被拒       → 背压闸门真的在扣窗口。
//      这条错了就是 lwIP 那个坑：发 RST 而不是 FIN，设备侧"下载到一半被断"。
//
// 帧是手工构造的，**校验和必须正确** —— smoltcp 默认校验，错了会被静默丢弃，
// 那样测试会以"没有 SYN-ACK"的形式失败，而不是以"校验和错"的形式，容易误判。

use alloc::vec;
use alloc::vec::Vec;

use crate::engine::{ConnId, Engine, Event};

const DEV_MAC: [u8; 6] = [0x02, 0x00, 0x00, 0x00, 0x00, 0xAA];
const OUR_MAC: [u8; 6] = [0x02, 0x00, 0x5b, 0x00, 0x00, 0x01];
const DEV_IP: [u8; 4] = [10, 99, 0, 1];
const OUR_IP: [u8; 4] = [10, 99, 0, 2];
/// 设备想访问的公网地址 —— 既不是本机地址也不是本机网段，必须靠 any_ip 才收得下。
const DST_IP: [u8; 4] = [93, 184, 216, 34];

fn ones_sum(data: &[u8], mut sum: u32) -> u32 {
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
fn fold(mut s: u32) -> u16 {
    while (s >> 16) != 0 {
        s = (s & 0xffff) + (s >> 16);
    }
    !(s as u16)
}

/// 造一个合法的 IPv4/TCP 帧（含正确的 IP 与 TCP 校验和）。
fn tcp_frame(sport: u16, dport: u16, flags: u8, seq: u32, ack: u32, payload: &[u8]) -> Vec<u8> {
    let tcp_len = 20 + payload.len();
    let ip_len = 20 + tcp_len;
    let mut f = vec![0u8; 14 + ip_len];
    f[0..6].copy_from_slice(&OUR_MAC);
    f[6..12].copy_from_slice(&DEV_MAC);
    f[12] = 0x08;
    f[13] = 0x00;
    f[14] = 0x45;
    f[16..18].copy_from_slice(&(ip_len as u16).to_be_bytes());
    f[22] = 64;
    f[23] = 6;
    f[26..30].copy_from_slice(&DEV_IP);
    f[30..34].copy_from_slice(&DST_IP);
    let ipc = fold(ones_sum(&f[14..34], 0));
    f[24..26].copy_from_slice(&ipc.to_be_bytes());

    let t = 34usize;
    f[t..t + 2].copy_from_slice(&sport.to_be_bytes());
    f[t + 2..t + 4].copy_from_slice(&dport.to_be_bytes());
    f[t + 4..t + 8].copy_from_slice(&seq.to_be_bytes());
    f[t + 8..t + 12].copy_from_slice(&ack.to_be_bytes());
    f[t + 12] = 0x50;
    f[t + 13] = flags;
    f[t + 14..t + 16].copy_from_slice(&65535u16.to_be_bytes());
    if !payload.is_empty() {
        f[t + 20..t + 20 + payload.len()].copy_from_slice(payload);
    }
    let mut ph = 0u32;
    ph = ones_sum(&DEV_IP, ph);
    ph = ones_sum(&DST_IP, ph);
    ph += 6;
    ph += tcp_len as u32;
    let c = fold(ones_sum(&f[t..t + tcp_len], ph));
    f[t + 16..t + 18].copy_from_slice(&c.to_be_bytes());
    f
}

fn mk_engine() -> Engine {
    let mut e = Engine::new();
    let mut ip = [0u8; 16];
    ip[..4].copy_from_slice(&OUR_IP);
    assert!(e.add_nic(1, OUR_MAC, ip, false, 24, 0), "add_nic 应成功");
    e
}

/// 从事件里挑出 TCP 出帧 → (flags, sport, dport, seq)
fn tcp_out(events: &[Event]) -> Vec<(u8, u16, u16, u32)> {
    let mut v = Vec::new();
    for ev in events {
        if let Event::OutFrame { data, .. } = ev {
            if data.len() >= 54 && data[12] == 0x08 && data[13] == 0x00 && data[23] == 6 {
                v.push((
                    data[47],
                    u16::from_be_bytes([data[34], data[35]]),
                    u16::from_be_bytes([data[36], data[37]]),
                    u32::from_be_bytes([data[38], data[39], data[40], data[41]]),
                ));
            }
        }
    }
    v
}

fn conn_new_of(events: &[Event]) -> Option<(ConnId, u16)> {
    events.iter().find_map(|ev| match ev {
        Event::ConnNew { id, dport, .. } => Some((*id, *dport)),
        _ => None,
    })
}

/// 完成三次握手，返回 (连接 id, SYN-ACK 的 seq)。
/// ★ 必须走完握手才会有 ConnNew —— 引擎刻意只在 Established 才 promote，与 lwIP 的 accept
///   时机一致；否则 SYN 洪水会给每个 SYN 各拨一条上游（这个差异是 A/B 自测抓出来的）。
fn establish(e: &mut Engine, sport: u16, dport: u16, seq: u32) -> (ConnId, u32) {
    let mut evs = Vec::new();
    e.input(1, &tcp_frame(sport, dport, 0x02, seq, 0, b""));
    e.poll_collect(10, &mut evs);
    let synack_seq = tcp_out(&evs)
        .into_iter()
        .find(|(fl, _, _, _)| fl & 0x12 == 0x12)
        .map(|(_, _, _, s)| s)
        .expect("握手第二步应有 SYN-ACK");
    evs.clear();
    e.input(
        1,
        &tcp_frame(sport, dport, 0x10, seq + 1, synack_seq.wrapping_add(1), b""),
    );
    e.poll_collect(20, &mut evs);
    let id = conn_new_of(&evs).expect("握手完成后应有 ConnNew").0;
    (id, synack_seq)
}

#[test]
fn syn_gets_synack_with_original_dport() {
    let mut e = mk_engine();
    let mut evs = Vec::new();

    assert!(e.input(1, &tcp_frame(51000, 443, 0x02, 1000, 0, b"")));
    e.poll_collect(10, &mut evs);

    // ① 协议栈活着
    let outs = tcp_out(&evs);
    let synack = outs.iter().find(|(fl, _, _, _)| *fl & 0x12 == 0x12);
    assert!(synack.is_some(), "没有 SYN-ACK；出帧={:?}", outs);

    // ② catch-all 改写在出方向闭环 —— 设备只认 443，这里若是 FIXED_PORT 设备会 RST
    let (_, sport, dport, _) = *synack.unwrap();
    assert_eq!(sport, 443, "SYN-ACK 源端口必须还原成 443");
    assert_eq!(dport, 51000, "SYN-ACK 目的端口应为设备源端口");

    // ③ 此时**还不该**有 ConnNew —— 握手没完成就拨上游 = SYN 洪水放大
    assert!(conn_new_of(&evs).is_none(), "SynReceived 阶段不该 promote");

    // ④ 补上第三次握手，ConnNew 才出现，且报的是原始目的端口
    let synack_seq = synack.unwrap().3;
    evs.clear();
    e.input(
        1,
        &tcp_frame(51000, 443, 0x10, 1001, synack_seq.wrapping_add(1), b""),
    );
    e.poll_collect(20, &mut evs);
    let (id, real) = conn_new_of(&evs).expect("握手完成后应有 ConnNew");
    assert_eq!(real, 443, "ConnNew 应报原始目的端口");
    assert_ne!(id, 0, "conn id 不能为 0（0 是无效值）");
    assert_eq!(e.conns_accepted, 1);
}

#[test]
fn window_is_gated_until_recved() {
    let mut e = mk_engine();
    let mut evs = Vec::new();
    let (id, synack_seq) = establish(&mut e, 51001, 443, 5000);

    // 设备发数据
    evs.clear();
    e.input(
        1,
        &tcp_frame(51001, 443, 0x18, 5001, synack_seq.wrapping_add(1), b"ping"),
    );
    e.poll_collect(30, &mut evs);

    let got = evs.iter().find_map(|ev| match ev {
        Event::ConnData { id: i, data } if *i == id => Some(data.clone()),
        _ => None,
    });
    assert_eq!(got.as_deref(), Some(&b"ping"[..]), "设备数据应交给 C++");

    // ★ 背压核心：交出去了但还没 recved → 窗口未还满 → 不许优雅关。
    //   这条守的是 lwIP 那个坑：没还满就 close 会发 RST 而不是 FIN，
    //   设备侧表现为"下载到一半被断"（NetStack.cpp:709-713）。
    assert_eq!(
        e.close(id),
        Err(crate::ffi::COAST_ERR_STATE),
        "窗口没还满时 close 必须被拒"
    );

    assert!(e.recved(id, 4).is_ok(), "还窗口应成功");
    assert!(e.close(id).is_ok(), "还满后应允许优雅关");
}

#[test]
fn multiple_conns_and_close_by_device() {
    let mut e = mk_engine();
    let mut ids = Vec::new();
    for i in 0..3u16 {
        let (id, _) = establish(&mut e, 52000 + i, 8080, 1000);
        ids.push(id);
    }
    assert_eq!(ids.len(), 3, "三条连接应各建一条，实得 {:?}", ids);
    assert!(ids.iter().all(|&i| i != 0));

    // 对应 lwIP 那边遍历 tcp_active_pcbs —— 换址/DHCP 续约时必须能整批关掉，
    // 否则连接表慢性泄漏（lwIP established 超时 24 小时）。
    let mut dev = [0u8; 16];
    dev[..4].copy_from_slice(&DEV_IP);
    assert_eq!(e.close_device_conns(&dev), 3, "应关掉该设备全部 3 条");
}

#[test]
fn malformed_frames_never_panic() {
    // 被劫持设备是**不可信输入源**；panic="abort" 下任何畸形帧都不能 panic
    // （panic 会直接杀掉整个 Qt GUI 进程）。
    let mut e = mk_engine();
    let mut evs = Vec::new();
    let base = tcp_frame(51000, 443, 0x02, 1, 0, b"x");
    for n in 0..base.len() {
        let mut f = base.clone();
        f.truncate(n);
        e.input(1, &f);
    }
    e.input(1, &vec![0u8; 60]);
    let mut junk = vec![0xFFu8; 200];
    junk[12] = 0x08;
    junk[13] = 0x00;
    e.input(1, &junk);
    // 声称是 IPv6 但内容是垃圾
    let mut v6junk = vec![0xABu8; 120];
    v6junk[12] = 0x86;
    v6junk[13] = 0xDD;
    e.input(1, &v6junk);
    e.poll_collect(10, &mut evs); // 不崩即通过
}

#[test]
fn unknown_nic_is_rejected() {
    let mut e = mk_engine();
    assert!(
        !e.input(99, &tcp_frame(1, 2, 0x02, 1, 0, b"")),
        "未知网卡应被拒"
    );
}

// ───────────────────────────────────────────────────────────────────────────
// 泵周期量化的**代价**测量（不是功能测试，是给"网关到底堵在哪"定量）。
//
// 生产的调用模式是：inputFrame() 只把帧塞进收队列（engine::input 不 poll），
// 事件与出帧**只在 coast_stack_poll 里产生**，而它唯一的调用点是 25 ms 的泵。
// 所以单连接的上行天花板 = 「一个 poll 周期能吃下多少字节」÷ 25 ms。
// 本测量就是把左边那个量测出来。
#[test]
fn measure_bytes_per_poll_cycle() {
    let mut e = mk_engine();
    let (id, synack_seq) = establish(&mut e, 51010, 443, 1000);
    let ack = synack_seq.wrapping_add(1);
    let payload = [0x5Au8; 1460];
    let mut seq: u32 = 1001;

    // 一个周期内尽力灌：200 帧 = 292 KB，远超接收窗口，多余的会被判出窗丢掉
    // （真实设备会重传，这里只关心"一个周期最多吃进多少"）。
    for _ in 0..200 {
        e.input(1, &tcp_frame(51010, 443, 0x18, seq, ack, &payload));
        seq = seq.wrapping_add(1460);
    }
    let mut evs = Vec::new();
    e.poll_collect(100, &mut evs);
    let delivered: usize = evs
        .iter()
        .filter_map(|ev| match ev {
            Event::ConnData { id: i, data } if *i == id => Some(data.len()),
            _ => None,
        })
        .sum();

    // 25 ms 一拍 → 折算单连接上行天花板
    // ⚠️ 这里**故意不折算 Mb/s**。本测量灌的是**出窗**数据，量到的是"收缓冲一个周期能吸收
    //    多少"，不是"真实设备被允许发多少"——后者受通告窗口(65535)约束，见
    //    measure_realistic_upstream_ceiling。早先拿这个数折算出的 41.9 Mb/s 偏乐观 2 倍。
    std::eprintln!(
        "[QUANT] 灌入 {} B（含出窗），单个 poll 周期收缓冲吸收 {} B —— 上限是 RX_BUF，
         [QUANT] **别拿它折算吞吐**，真实天花板见 measure_realistic_upstream_ceiling",
        200 * 1460,
        delivered
    );
    assert!(delivered > 0, "一个周期一个字节都没交出来");
    // 守住这条测量的语义：交出的量受接收窗口封顶，不会随灌入量线性增长。
    assert!(
        delivered < 200 * 1460,
        "居然全吃下了 —— 说明窗口/量化模型和我理解的不一样，结论要重算"
    );
}

/// 出方向 TCP 帧的完整字段：(flags, seq, ack, window)。
/// 比 tcp_out 多取 ack 与**通告窗口** —— 上一条测量缺的正是窗口，导致灌了出窗数据。
fn tcp_out_ex(events: &[Event]) -> Vec<(u8, u32, u32, u16)> {
    let mut v = Vec::new();
    for ev in events {
        if let Event::OutFrame { data, .. } = ev {
            if data.len() >= 54 && data[12] == 0x08 && data[13] == 0x00 && data[23] == 6 {
                let t = 34usize;
                v.push((
                    data[t + 13],
                    u32::from_be_bytes([data[t + 4], data[t + 5], data[t + 6], data[t + 7]]),
                    u32::from_be_bytes([data[t + 8], data[t + 9], data[t + 10], data[t + 11]]),
                    u16::from_be_bytes([data[t + 14], data[t + 15]]),
                ));
            }
        }
    }
    v
}

/// 严谨版上行天花板测量：**设备严格遵守通告窗口**，只发 (ack + wnd - seq) 允许的量。
/// 这才是真实设备的行为；上一条 measure_bytes_per_poll_cycle 灌了出窗数据，量到的是
/// "缓冲区能装多少"而不是"设备被允许发多少"，偏乐观。
#[test]
fn measure_realistic_upstream_ceiling() {
    let mut e = mk_engine();
    let (id, synack_seq) = establish(&mut e, 51020, 443, 2000);
    let ackno_to_peer = synack_seq.wrapping_add(1);
    let payload = [0x5Au8; 1460];

    let mut seq: u32 = 2001; // 设备下一个要发的序号
    let mut peer_ack: u32 = 2001; // 我方已确认到哪
    let mut wnd: u32 = 65535; // 我方通告窗口（下面会被真值覆盖）
    let mut wnd_seen_max: u32 = 0;

    const TARGET: usize = 1024 * 1024;
    let mut delivered = 0usize;
    let mut cycles = 0u32;
    let mut now = 1000u64;
    let mut idle = 0u32;

    while delivered < TARGET && cycles < 20000 {
        // 设备在窗口内尽量发
        let mut sent_this_cycle = 0usize;
        loop {
            let inflight = seq.wrapping_sub(peer_ack);
            if inflight as u64 + 1460 > wnd as u64 {
                break;
            }
            e.input(
                1,
                &tcp_frame(51020, 443, 0x18, seq, ackno_to_peer, &payload),
            );
            seq = seq.wrapping_add(1460);
            sent_this_cycle += 1460;
        }

        let mut evs = Vec::new();
        now += 25; // 生产泵周期
        e.poll_collect(now, &mut evs);
        cycles += 1;

        // C++ 侧最理想情况：收到就写 SOCKS 并**立刻**还窗口（无背压）
        let mut got = 0usize;
        for ev in &evs {
            if let Event::ConnData { id: i, data } = ev {
                if *i == id {
                    got += data.len();
                }
            }
        }
        if got > 0 {
            let _ = e.recved(id, got as u32);
            delivered += got;
        }

        for (flags, _sq, ackno, w) in tcp_out_ex(&evs) {
            if flags & 0x10 != 0 {
                peer_ack = ackno;
                wnd = w as u32;
                if wnd > wnd_seen_max {
                    wnd_seen_max = wnd;
                }
            }
        }

        if sent_this_cycle == 0 && got == 0 {
            idle += 1;
            if idle > 50 {
                break; // 卡死了，也是结论
            }
        } else {
            idle = 0;
        }
    }

    let secs = cycles as f64 * 0.025;
    let mbps = (delivered as f64) * 8.0 / secs / 1e6;
    std::eprintln!(
        "[REAL] 送达 {} B / {} 拍（{:.2}s @25ms）→ {:.1} Mb/s/连接；最大通告窗口 {} B",
        delivered,
        cycles,
        secs,
        mbps,
        wnd_seen_max
    );
    assert!(delivered > 0, "一个字节都没送达");
}

// ═══════════════════════════════════════════════════════════════════════════
// 第三版测量：**带窗口缩放选项**，并补上从没测过的下行。
//
// 前两版都错了，而且方向相反：
//   · measure_bytes_per_poll_cycle       —— 灌出窗数据，量的是收缓冲容量(128 KiB)，偏乐观
//   · measure_realistic_upstream_ceiling —— 合成 SYN **不带 WS 选项**，等于把窗口缩放关掉，
//                                           通告窗口被 RFC 封在 65535，偏悲观
// smoltcp 0.12 是支持 WS 的（socket/tcp.rs 里的 remote_win_scale / remote_win_shift，
// SYN-ACK 会回选项），而真实设备的 SYN 一定带 WS —— 所以正确的测法必须把选项带上。

/// 带 TCP 选项的帧（opts 长度必须是 4 的倍数）。
fn tcp_frame_opt(
    sport: u16,
    dport: u16,
    flags: u8,
    seq: u32,
    ack: u32,
    payload: &[u8],
    opts: &[u8],
) -> Vec<u8> {
    assert!(opts.len() % 4 == 0);
    let hdr_len = 20 + opts.len();
    let tcp_len = hdr_len + payload.len();
    let ip_len = 20 + tcp_len;
    let mut f = vec![0u8; 14 + ip_len];
    f[0..6].copy_from_slice(&OUR_MAC);
    f[6..12].copy_from_slice(&DEV_MAC);
    f[12] = 0x08;
    f[13] = 0x00;
    f[14] = 0x45;
    f[16..18].copy_from_slice(&(ip_len as u16).to_be_bytes());
    f[22] = 64;
    f[23] = 6;
    f[26..30].copy_from_slice(&DEV_IP);
    f[30..34].copy_from_slice(&DST_IP);
    let ipc = fold(ones_sum(&f[14..34], 0));
    f[24..26].copy_from_slice(&ipc.to_be_bytes());

    let t = 34usize;
    f[t..t + 2].copy_from_slice(&sport.to_be_bytes());
    f[t + 2..t + 4].copy_from_slice(&dport.to_be_bytes());
    f[t + 4..t + 8].copy_from_slice(&seq.to_be_bytes());
    f[t + 8..t + 12].copy_from_slice(&ack.to_be_bytes());
    f[t + 12] = ((hdr_len / 4) as u8) << 4;
    f[t + 13] = flags;
    // 设备侧通告 65535，配合 SYN 里的 shift=7 → 有效约 8 MiB，
    // 故意让**设备窗口不成为下行的限制**，这样量到的就是泵周期本身。
    f[t + 14..t + 16].copy_from_slice(&65535u16.to_be_bytes());
    if !opts.is_empty() {
        f[t + 20..t + 20 + opts.len()].copy_from_slice(opts);
    }
    if !payload.is_empty() {
        f[t + hdr_len..t + hdr_len + payload.len()].copy_from_slice(payload);
    }
    let mut ph = 0u32;
    ph = ones_sum(&DEV_IP, ph);
    ph = ones_sum(&DST_IP, ph);
    ph += 6;
    ph += tcp_len as u32;
    let c = fold(ones_sum(&f[t..t + tcp_len], ph));
    f[t + 16..t + 18].copy_from_slice(&c.to_be_bytes());
    f
}

/// 从出方向帧里解析 TCP 选项，取窗口缩放的 shift。
fn parse_ws_shift(frame: &[u8]) -> Option<u8> {
    if frame.len() < 54 {
        return None;
    }
    let t = 34usize;
    let hdr_len = ((frame[t + 12] >> 4) as usize) * 4;
    if hdr_len <= 20 || t + hdr_len > frame.len() {
        return None;
    }
    let mut i = t + 20;
    let end = t + hdr_len;
    while i < end {
        match frame[i] {
            0 => return None,
            1 => i += 1,
            3 => {
                return if i + 2 < end {
                    Some(frame[i + 2])
                } else {
                    None
                };
            }
            _ => {
                if i + 1 >= end {
                    return None;
                }
                let l = frame[i + 1] as usize;
                if l < 2 {
                    return None;
                }
                i += l;
            }
        }
    }
    None
}

/// 带 WS 的三次握手。返回 (连接 id, SYN-ACK 的 seq, 我方通告的 shift, 我方通告窗口原值)
fn establish_ws(e: &mut Engine, sport: u16, dport: u16, seq: u32) -> (ConnId, u32, u8, u16) {
    let opts = [3u8, 3, 7, 0]; // WS shift=7，Windows/Linux 的常见量级
    let mut evs = Vec::new();
    e.input(1, &tcp_frame_opt(sport, dport, 0x02, seq, 0, b"", &opts));
    e.poll_collect(10, &mut evs);

    let (mut synack_seq, mut shift, mut wnd) = (0u32, 0u8, 0u16);
    for ev in &evs {
        if let Event::OutFrame { data, .. } = ev {
            if data.len() >= 54 && data[12] == 0x08 && data[13] == 0x00 && data[23] == 6 {
                let t = 34usize;
                if data[t + 13] & 0x12 == 0x12 {
                    synack_seq =
                        u32::from_be_bytes([data[t + 4], data[t + 5], data[t + 6], data[t + 7]]);
                    wnd = u16::from_be_bytes([data[t + 14], data[t + 15]]);
                    shift = parse_ws_shift(data).unwrap_or(0);
                }
            }
        }
    }
    assert_ne!(synack_seq, 0, "带 WS 的 SYN 没拿到 SYN-ACK");
    evs.clear();
    e.input(
        1,
        &tcp_frame_opt(
            sport,
            dport,
            0x10,
            seq + 1,
            synack_seq.wrapping_add(1),
            b"",
            &[],
        ),
    );
    e.poll_collect(20, &mut evs);
    let id = conn_new_of(&evs).expect("握手完成后应有 ConnNew").0;
    (id, synack_seq, shift, wnd)
}

/// 出方向数据字节数（跳过纯 ACK）。
fn payload_bytes(evs: &[Event]) -> usize {
    let mut n = 0usize;
    for ev in evs {
        if let Event::OutFrame { data, .. } = ev {
            if data.len() > 54 && data[12] == 0x08 && data[13] == 0x00 && data[23] == 6 {
                let t = 34usize;
                let hl = ((data[t + 12] >> 4) as usize) * 4;
                if data.len() > 14 + 20 + hl {
                    n += data.len() - 14 - 20 - hl;
                }
            }
        }
    }
    n
}

/// 上行天花板：设备严格遵守通告窗口，每 `poll_ms` 毫秒推进一次协议栈。
/// 返回 (Mb/s, 实际送达字节, 拍数)。
fn measure_up(poll_ms: u64) -> (f64, usize, u32) {
    let mut e = mk_engine();
    let (id, synack_seq, shift, syn_wnd) = establish_ws(&mut e, 51030, 443, 3000);
    let ackno = synack_seq.wrapping_add(1);
    let payload = [0x5Au8; 1460];
    let mut seq: u32 = 3001;
    let mut peer_ack: u32 = 3001;
    // RFC 7323：SYN 段里的窗口字段不缩放，后续段才缩放。
    let mut wnd: u64 = syn_wnd as u64;

    const TARGET: usize = 4 * 1024 * 1024;
    let (mut delivered, mut cycles, mut now, mut idle) = (0usize, 0u32, 1000u64, 0u32);
    while delivered < TARGET && cycles < 200_000 {
        let mut sent = 0usize;
        loop {
            let inflight = seq.wrapping_sub(peer_ack) as u64;
            if inflight + 1460 > wnd {
                break;
            }
            e.input(
                1,
                &tcp_frame_opt(51030, 443, 0x18, seq, ackno, &payload, &[]),
            );
            seq = seq.wrapping_add(1460);
            sent += 1460;
        }
        let mut evs = Vec::new();
        now += poll_ms;
        e.poll_collect(now, &mut evs);
        cycles += 1;

        let mut got = 0usize;
        for ev in &evs {
            if let Event::ConnData { id: i, data } = ev {
                if *i == id {
                    got += data.len();
                }
            }
        }
        if got > 0 {
            let _ = e.recved(id, got as u32);
            delivered += got;
        }
        for (flags, _sq, a, w) in tcp_out_ex(&evs) {
            if flags & 0x10 != 0 {
                peer_ack = a;
                wnd = (w as u64) << shift;
            }
        }
        if sent == 0 && got == 0 {
            idle += 1;
            if idle > 200 {
                break;
            }
        } else {
            idle = 0;
        }
    }
    let secs = cycles as f64 * (poll_ms as f64 / 1000.0);
    ((delivered as f64) * 8.0 / secs / 1e6, delivered, cycles)
}

/// 下行天花板：设备通告一个大窗口（不让设备侧成为限制），量的就是推进节奏本身。
fn measure_down(poll_ms: u64, cycles_target: u32) -> (f64, usize) {
    let mut e = mk_engine();
    let (id, sa, _s, _w) = establish_ws(&mut e, 51031, 8080, 4000);
    let ack_base = sa.wrapping_add(1);
    let chunk = vec![0xA5u8; 128 * 1024];
    let (mut on_wire, mut cyc, mut now) = (0usize, 0u32, 2000u64);
    while cyc < cycles_target {
        loop {
            let room = e.sndbuf(id).unwrap_or(0);
            if room == 0 {
                break;
            }
            let n = room.min(chunk.len());
            match e.send(id, &chunk[..n]) {
                Ok(w) if w > 0 => {}
                _ => break,
            }
        }
        let mut evs = Vec::new();
        now += poll_ms;
        e.poll_collect(now, &mut evs);
        cyc += 1;
        on_wire += payload_bytes(&evs);
        e.input(
            1,
            &tcp_frame_opt(
                51031,
                8080,
                0x10,
                4001,
                ack_base.wrapping_add(on_wire as u32),
                b"",
                &[],
            ),
        );
    }
    let secs = cyc as f64 * (poll_ms as f64 / 1000.0);
    ((on_wire as f64) * 8.0 / secs / 1e6, on_wire)
}

/// ★ 推进节奏 → 吞吐 的扫描。
///
/// 这条替代了原来那个硬编码 25ms 的版本 —— 生产已经改成**每轮事件循环 poll 一次**
/// （NetStack.cpp 的 schedulePoll），25ms 只剩定时器兜底，所以"25ms 一拍"不再是生产模型。
/// 把周期变成参数之后，这条测试测的是那个真正不变的规律：
///
///     每连接吞吐 ≈ 有效窗口 ÷ 推进周期
///
/// 它同时是回归护栏：谁要是把 schedulePoll 拆了、退回纯定时器，25ms 那一行就是当时的数。
/// ⚠️ 下限是 1ms：smoltcp 的时间戳是毫秒粒度，再密的 poll 在**模型里**推不动时间
///    （真实生产里仍有意义 —— 收队列的排空不依赖时间推进）。
#[test]
fn throughput_scales_with_poll_rate() {
    std::eprintln!("  周期     上行 Mb/s    下行 Mb/s");
    let mut rows = Vec::new();
    for &ms in &[25u64, 5, 1] {
        let (up, _b, _c) = measure_up(ms);
        let (down, _) = measure_down(ms, 300);
        std::eprintln!("  {:>3} ms   {:>9.1}    {:>9.1}", ms, up, down);
        rows.push((ms, up, down));
    }

    let (_, up25, down25) = rows[0];
    let (_, up1, down1) = rows[2];
    // 1ms 相对 25ms 应当有数量级的提升；取 8× 做判据，留足实现细节的余量。
    assert!(
        up1 > up25 * 8.0,
        "上行没有随推进变密而提升：25ms={:.1} 1ms={:.1} —— \
         说明瓶颈已经不是推进周期（或者模型写错了）",
        up25,
        up1
    );
    assert!(
        down1 > down25 * 8.0,
        "下行没有随推进变密而提升：25ms={:.1} 1ms={:.1}",
        down25,
        down1
    );
}

/// backlog 不该限制**一拍之内**能接受多少条新连接。
///
/// 旧实现里 promote_listeners 只在收包循环**之后**跑一次，于是同一个 poll 周期里
/// 最多只有 LISTEN_BACKLOG(32) 个处于 Listen 的 socket 可用，第 33 条起的 SYN
/// 无人应答 → 设备等约 1 秒 RTO 重传。多源页面同时开 30~100 条连接很常见。
///
/// 这条测试对旧实现是**会失败**的（只会数到 32 个 SYN-ACK），所以它真的在测东西。
#[test]
fn backlog_does_not_cap_accepts_per_poll() {
    let mut e = mk_engine();
    const N: u16 = 100; // 远超 LISTEN_BACKLOG
    let mut evs = Vec::new();
    for i in 0..N {
        e.input(1, &tcp_frame(40000 + i, 443, 0x02, 1000 + i as u32, 0, b""));
    }
    e.poll_collect(10, &mut evs);

    let synacks = tcp_out(&evs)
        .into_iter()
        .filter(|(fl, _, _, _)| fl & 0x12 == 0x12)
        .count();
    assert_eq!(
        synacks, N as usize,
        "一拍之内应对全部 {} 个 SYN 回 SYN-ACK；只回了 {} 个 = backlog 仍在封顶
         （旧实现恒为 LISTEN_BACKLOG=32）",
        N, synacks
    );
}

/// 收队列必须有界 —— 被劫持设备是**不可信输入源**。
///
/// 这条守的不是性能而是安全：改之前这个队列是无界 Vec，而 RxWorker 那套 8192 帧的反压
/// 根本不会触发（consumed() 的语义是"已交给引擎"不是"已处理"）。本仓库被同类问题
/// 咬过一次（DNS 洪水打崩网关）。
#[test]
fn rx_queue_is_bounded_and_counted() {
    let mut e = mk_engine();
    // 不 poll，一路灌 —— 模拟工作线程卡住而设备在线速发包
    let payload = [0u8; 1400];
    const FLOOD: usize = 40_000; // 远超 RX_QUEUE_MAX(16384)
    for i in 0..FLOOD {
        e.input(
            1,
            &tcp_frame(50000, 443, 0x18, 1000 + i as u32 * 1400, 0, &payload),
        );
    }
    assert!(
        e.rx_overflow > 0,
        "灌了 {} 帧却一次溢出都没记 —— 队列还是无界的，或者计数没接上",
        FLOOD
    );
    // 溢出之后仍必须能正常工作（丢的是最旧的，不是把栈打死）
    let mut evs = Vec::new();
    e.poll_collect(10, &mut evs);
}

/// 证明 `set_skip_rx_checksum` 真的生效 —— 否则"关掉校验和没差别"这个测量结论是假的。
///
/// 判据：喂一个**校验和故意写错**的 SYN。
///   · 默认（验校验和）→ 被丢弃，没有 SYN-ACK
///   · 关掉验证         → 被接受，有 SYN-ACK
/// 两条都成立，才说明那个开关确实作用在收包路径上。
#[test]
fn skip_rx_checksum_knob_actually_works() {
    let mut e = mk_engine();

    // 校验和错的 SYN
    let mut bad = tcp_frame(51040, 443, 0x02, 7000, 0, b"");
    bad[34 + 16] ^= 0xFF; // 翻掉校验和高字节
    bad[34 + 17] ^= 0xFF;

    let mut evs = Vec::new();
    e.input(1, &bad);
    e.poll_collect(10, &mut evs);
    let synacks = tcp_out(&evs)
        .into_iter()
        .filter(|(fl, _, _, _)| fl & 0x12 == 0x12)
        .count();
    assert_eq!(
        synacks, 0,
        "默认应当校验并丢弃坏帧，却回了 {} 个 SYN-ACK —— 那么「关掉校验和」的对照实验没有意义",
        synacks
    );

    // 关掉验证后同一个坏帧应当被接受。
    // ★ 必须**重建引擎**：开关要在 add_nic 之前生效（smoltcp 缓存 capabilities）。
    let mut e = Engine::new();
    e.set_skip_rx_checksum(true);
    let mut ip = [0u8; 16];
    ip[..4].copy_from_slice(&OUR_IP);
    assert!(e.add_nic(1, OUR_MAC, ip, false, 24, 0));
    let mut bad2 = tcp_frame(51041, 443, 0x02, 7100, 0, b"");
    bad2[34 + 16] ^= 0xFF;
    bad2[34 + 17] ^= 0xFF;
    let mut evs2 = Vec::new();
    e.input(1, &bad2);
    e.poll_collect(20, &mut evs2);
    let synacks2 = tcp_out(&evs2)
        .into_iter()
        .filter(|(fl, _, _, _)| fl & 0x12 == 0x12)
        .count();
    assert!(
        synacks2 > 0,
        "关掉校验和之后坏帧仍被丢弃 —— 开关没生效，据它得出的「校验和不要钱」是假结论"
    );
}

/// 对端消失的连接必须被回收。
///
/// ★ smoltcp 的 `set_timeout` 与 `set_keep_alive` **默认都是 None** —— 不设的话，对端一旦
///   消失（设备休眠 / 拔网线 / 进程被杀），socket 就永远卡在那里，没有任何东西会收场。
///   真机后果：conns 只增不减（50 分钟 0→254，而核心侧同时只有 46 条）；由于每帧 poll 要
///   遍历全部 socket，吞吐随连接数反向塌陷 —— 同一条链路 conns≈10 时 407 KB/s、
///   conns≈80 时 21 KB/s。用户看到的是「用久了变慢、测速软件根本测不了、看视频却没事」。
///
/// ★ 这条用例只推时钟、不喂任何帧，正是「对端凭空消失」的最小复现。它同时守住两个设置：
///   去掉 timeout 则永不回收（本断言直接红）；去掉 keep_alive 则探测不发，静默计时器
///   照样会到期 —— 所以另有一条 idle_but_alive_connection_survives 守着「别砍活连接」。
#[test]
fn silent_peer_connection_is_reaped() {
    let mut e = mk_engine();
    let (id, _synack_seq) = establish(&mut e, 51030, 443, 3000);

    let mut now = 1000u64;
    let mut closed = false;
    for _ in 0..400 {
        now += 1000; // 1 秒一步：保活 75s 一次、静默中止 300s，400 步足够跨过去
        let mut evs = Vec::new();
        e.poll_collect(now, &mut evs);
        if evs
            .iter()
            .any(|ev| matches!(ev, Event::ConnClosed { id: i, .. } if *i == id))
        {
            closed = true;
            break;
        }
    }
    assert!(
        closed,
        "对端静默 400 秒后连接仍未回收 —— socket 永生，conns 只增不减"
    );
}

/// 反向守护：**还活着**的空闲连接不能被砍。
///
/// RFC 5382 REQ-5 要求已建立的 TCP 映射不短于 2 小时 4 分。只设 timeout 而不设 keep_alive，
/// 或者把判据写成「按空闲砍」，都会掐断推送 / IMAP / SSH 这类合法长连接。这里设备一个字节
/// 都不发，但**每次收到保活探测都回 ACK** —— 静默计时器应当不断归零，连接必须活下来。
#[test]
fn idle_but_alive_connection_survives() {
    let mut e = mk_engine();
    let (id, synack_seq) = establish(&mut e, 51031, 443, 4000);
    let ackno = synack_seq.wrapping_add(1);

    let mut now = 1000u64;
    let mut probes = 0u32;
    for _ in 0..400 {
        now += 1000;
        let mut evs = Vec::new();
        e.poll_collect(now, &mut evs);
        assert!(
            !evs.iter().any(|ev| matches!(ev, Event::ConnClosed { id: i, .. } if *i == id)),
            "设备一直在回 ACK，连接却被砍了（400 步里共收到 {probes} 次探测）—— 这会掐断推送/IMAP 这类合法长连接"
        );
        // ★ 设备**只在被探测到时**才回一个纯 ACK —— 真实的空闲连接不会自己发东西。
        //   这正是 keep_alive 的作用：没有它就没有探测，设备没有理由说话，静默计时器
        //   一路走到期，于是一条完全健康的长连接被砍掉。所以这条用例是 keep_alive 的守卫，
        //   删掉 set_keep_alive 它必红（若设备每秒主动发包，这条就白写了）。
        let probed = evs.iter().any(|ev| matches!(ev, Event::OutFrame { .. }));
        if probed {
            probes += 1;
            e.input(1, &tcp_frame(51031, 443, 0x10, 4001, ackno, &[]));
        }
    }
}

/// 一拍之内涌进来的 SYN 必须全部被接住。
///
/// ★ 真机症状：Pi 经网关同时开 8 条 curl 下载，**恰好 3 条从头到尾没出现在核心里**
///   （核心侧只有 5 条 cachefly 连接，而到得了核心的那 5 条全部下满 10 MB、性能正常）。
///   用户侧就是「测速软件根本测不了、fast.com 卡住、看视频却没事」—— 测速和多源页面
///   正是几十条并发建连。
///
/// ★ 这条路**不经过任何现有计数器**：tcpAcc/tcpAbort/socksFail/refuse 全是 0，因为 SYN
///   在 smoltcp 里就被静默丢了，根本走不到我们的回调。排查时我盯着那几个 0 看了好几轮
///   才发现方向不对，所以这条用例的价值不只是防回归，更是把这条盲路变成可观测的。
///
/// promote_listeners 的补充逻辑是「监听 socket 离开 Listen 后再补一个」—— 补充永远滞后
/// 于消耗。一拍内 SYN 数超过当时处于 Listen 的 socket 数，多出来的就没人接。
#[test]
fn burst_of_syns_all_get_accepted() {
    const N: u16 = 200; // 远超真实并发，确保不是"刚好够用"
    let mut e = mk_engine();
    for i in 0..N {
        e.input(1, &tcp_frame(51100 + i, 443, 0x02, 7000 + i as u32, 0, b""));
    }
    let mut evs = Vec::new();
    e.poll_collect(10, &mut evs);
    // ★ 判据是**回了几个 SYN-ACK**，不是 ConnNew —— 后者要等三次握手第三步才发
    //   （见 establish），拿它当判据只会恒为 0，测不到"有没有 Listen socket 接住"。
    let got = tcp_out(&evs)
        .into_iter()
        .filter(|(fl, _, _, _)| fl & 0x12 == 0x12)
        .count();
    assert_eq!(
        got, N as usize,
        "一拍内灌了 {N} 个 SYN，只回了 {got} 个 SYN-ACK —— 其余被静默丢弃，设备要等 RTO(~1s) 重传",
    );
}

/// 设备发来的 RST 必须让连接立刻死掉。
///
/// ★ 真机抓包（Pi 经网关下载，8 条并发）：curl 超时退出后，Pi 的内核已经没有那个 socket，
///   于是每收到一个包就回一个 RST —— **一条连接上抓到 2839 个一模一样的 RST**：
///     14:58:32  Pi:60020 > 服务器:443  Flags [R] seq 932167879 win 0
///     14:58:33  同上（seq 一字不差）
///     …（约 95 次/秒，持续到测试结束）
///   而网关侧同期 tcpAbort 只有 11 —— **这些 RST 一个都没被当回事**。
///
/// ★ 那个 seq 是**正确**的：SYN-ACK 的 ack 是 932165893，设备随后发了 1986 字节，
///   932165893 + 1986 = 932167879。所以这不是「序号对不上被当成攻击」，它本该被接受。
///   连接不死 → 我们继续发包 → 设备继续 RST → 环路。这条路解释了 conns 只增不减、
///   tcpAbort 恒低、以及并发时带宽被僵尸流量吃掉。
#[test]
fn device_rst_kills_the_connection() {
    let mut e = mk_engine();
    let (id, synack_seq) = establish(&mut e, 51040, 443, 9000);
    let ackno = synack_seq.wrapping_add(1);

    // 设备先发一段数据，让两边的序号都推进（复刻真机里"发过 1986 字节再 RST"的形态）。
    let payload = [0x41u8; 512];
    e.input(1, &tcp_frame(51040, 443, 0x18, 9001, ackno, &payload));
    let mut evs = Vec::new();
    e.poll_collect(30, &mut evs);

    // 设备侧 socket 没了 → 内核回 RST，seq = 它的 snd_nxt（= 9001 + 512）。
    evs.clear();
    e.input(1, &tcp_frame(51040, 443, 0x04, 9001 + 512, ackno, b""));
    e.poll_collect(40, &mut evs);

    let closed = evs
        .iter()
        .any(|ev| matches!(ev, Event::ConnClosed { id: i, .. } if *i == id));
    assert!(
        closed,
        "收到序号正确的 RST 后连接没死 —— 真机上这会变成每秒约 95 个 RST 的环路",
    );
}

/// 收队列有积压时，新 SYN 还能不能在一拍之内被回应。
///
/// ★ 真机抓包（Pi 经网关，8 条并发下载）：
///     14:57:52.627  Pi:60006 > 服务器 [S]
///     14:57:53.653  重传 [S]
///     14:57:54.673  重传 [S]
///     14:57:55.701  重传 [S]
///     14:57:55.704  服务器 > Pi:60006 [S.]   ← 3.08 秒、4 个 SYN 之后才回
///   等不到的那几条就是 55 秒超时、curl 报 Connection timed out 的失败流。
///
/// ★ 已有的 burst_of_syns_all_get_accepted 抓不到这个：它验的是「回不回」，在一个**空闲**
///   引擎上一次灌完再 poll。真机上是「引擎已经很忙、收队列积着上千帧」时才发生 ——
///   泵 25ms 一拍、每拍处理的帧数有限，排在队尾的 SYN 就要等好几拍。
///   所以这条用例的判据是**几拍**，不是「最终有没有」。
#[test]
fn syn_under_rx_backlog_is_answered_promptly() {
    const BACKLOG: usize = 2000; // 真机上 fed 峰值 1024 帧/10s，队列积到这个量级很常见
    const SPORT: u16 = 51050;
    let mut e = mk_engine();
    // 先建一条连接并让它有数据在跑，制造出"引擎正忙"的状态。
    let (_id, synack_seq) = establish(&mut e, 51049, 443, 8000);
    let ackno = synack_seq.wrapping_add(1);
    let payload = [0x42u8; 1400];
    let mut seq: u32 = 8001;
    for _ in 0..BACKLOG {
        e.input(1, &tcp_frame(51049, 443, 0x18, seq, ackno, &payload));
        seq = seq.wrapping_add(1400);
    }
    // 队尾插入一个新连接的 SYN —— 正是真机上那条等了 3 秒的。
    e.input(1, &tcp_frame(SPORT, 443, 0x02, 12345, 0, b""));

    // 数它要几拍才被回应。每拍 = 生产泵的 25ms。
    let mut ticks = 0;
    let mut now = 100u64;
    let mut answered = 0;
    while ticks < 400 {
        let mut evs = Vec::new();
        now += 25;
        e.poll_collect(now, &mut evs);
        ticks += 1;
        // 找目的端口 = SPORT 的 SYN-ACK（出帧里 TCP 的 dport 在 offset 34+2）
        for ev in &evs {
            if let Event::OutFrame { data, .. } = ev {
                if data.len() >= 54 && data[23] == 6 && (data[47] & 0x12) == 0x12 {
                    let dport = u16::from_be_bytes([data[36], data[37]]);
                    if dport == SPORT {
                        answered = ticks;
                    }
                }
            }
        }
        if answered > 0 {
            break;
        }
    }
    assert!(
        answered > 0,
        "积压 {BACKLOG} 帧后，SYN 在 400 拍(10 秒)内都没被回应"
    );
    // 一拍 25ms。真机上设备的初始 RTO 是 1 秒 —— 超过 40 拍就会触发重传，
    // 那正是抓包里看到的形态。留 4 拍(100ms)余量，超了就是队头阻塞。
    assert!(
        answered <= 4,
        "积压 {BACKLOG} 帧后，SYN 等了 {answered} 拍({}ms) 才被回应 —— 队头阻塞，设备会重传",
        answered * 25,
    );
}
