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
    e.input(1, &tcp_frame(sport, dport, 0x10, seq + 1, synack_seq.wrapping_add(1), b""));
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
    e.input(1, &tcp_frame(51000, 443, 0x10, 1001, synack_seq.wrapping_add(1), b""));
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
    e.input(1, &tcp_frame(51001, 443, 0x18, 5001, synack_seq.wrapping_add(1), b"ping"));
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
    assert!(!e.input(99, &tcp_frame(1, 2, 0x02, 1, 0, b"")), "未知网卡应被拒");
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

    let mut seq: u32 = 2001;      // 设备下一个要发的序号
    let mut peer_ack: u32 = 2001; // 我方已确认到哪
    let mut wnd: u32 = 65535;     // 我方通告窗口（下面会被真值覆盖）
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
            e.input(1, &tcp_frame(51020, 443, 0x18, seq, ackno_to_peer, &payload));
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
        delivered, cycles, secs, mbps, wnd_seen_max
    );
    assert!(delivered > 0, "一个字节都没送达");
}
