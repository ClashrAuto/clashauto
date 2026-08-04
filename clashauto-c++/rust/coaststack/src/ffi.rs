// C ABI 边界 —— 与 src/net/coaststack.h 一一对应，改一边必须改另一边。
//
// Phase 1 的范围：**符号齐全、ABI 正确、零 panic**，引擎本体是桩（Phase 2 填）。
// 这样能先把工具链风险（MSVC/MinGW ABI 匹配、ARM64 交叉、CMake 接线、CI）出清，
// 再动 NetStack 一行代码。
//
// 纪律（整个文件都要守）：
//   · **零 unwrap / 零 panic** —— profile 是 panic="abort"，panic 会直接杀掉 GUI 进程。
//   · 所有裸指针入口先判空、判长度，非法就返回错误码。
//   · 所有 (ptr,len) 都是借用，不接管所有权。

use core::ffi::{c_char, c_void};

use crate::engine::{Engine, Event};

// ———————————————————————— 与头文件对齐的类型 ————————————————————————

pub const COAST_OK: i32 = 0;
pub const COAST_ERR_BADARG: i32 = -1;
pub const COAST_ERR_NOCONN: i32 = -2;
pub const COAST_ERR_NONIC: i32 = -3;
#[allow(dead_code)]
pub const COAST_ERR_NOMEM: i32 = -4;
pub const COAST_ERR_STATE: i32 = -5;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CoastAddr {
    pub is_v6: bool,
    pub bytes: [u8; 16],
}

pub type CoastConnId = u64;
pub type CoastNicId = u32;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CoastCallbacks {
    pub out_frame: Option<unsafe extern "C" fn(*mut c_void, CoastNicId, *const u8, usize)>,
    pub conn_new: Option<
        unsafe extern "C" fn(
            *mut c_void,
            CoastConnId,
            CoastNicId,
            *const CoastAddr,
            u16,
            *const CoastAddr,
            u16,
        ) -> bool,
    >,
    pub conn_data: Option<unsafe extern "C" fn(*mut c_void, CoastConnId, *const u8, usize)>,
    pub conn_sent: Option<unsafe extern "C" fn(*mut c_void, CoastConnId, u32)>,
    pub conn_closed: Option<unsafe extern "C" fn(*mut c_void, CoastConnId, bool)>,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct CoastStats {
    pub conns_accepted: u64,
    pub conns_closed: u64,
    pub conns_aborted: u64,
    pub conns_refused: u64,
    pub rx_frames: u64,
    pub tx_frames: u64,
    pub rx_dropped: u64,
    pub retransmits: u64,
    pub conns_active: u32,
    pub poll_max_gap_ms: u32,
}

// ———————————————————————— 栈实例 ————————————————————————

pub struct CoastStack {
    cb: CoastCallbacks,
    user: *mut c_void,
    stats: CoastStats,
    engine: Engine,
    last_poll_ms: u64,
}

impl CoastStack {
    fn new(cb: CoastCallbacks, user: *mut c_void) -> Self {
        Self { cb, user, stats: CoastStats::default(), engine: Engine::new(), last_poll_ms: 0 }
    }
}

// ———————————————————————— 事件派发（重入安全）————————————————————————
//
// ★ 关键约束：派发回调时**绝不能**持有 &mut CoastStack。
//   回调里允许再调 coast_*（典型：conn_new 里当场 conn_close），那会重新 &mut 同一个对象；
//   若外层还握着借用就是别名 UB。所以每个事件都「短借用取出所需 → 放掉 → 调回调」。
//   lwIP 那边为同一问题养了 ConnWatch/g_destroyedConn 一整套（NetStack.cpp:532-587），
//   这里靠结构消除。
unsafe fn dispatch(s: *mut CoastStack, events: alloc::vec::Vec<Event>) {
    for ev in events {
        let (cb, user) = {
            let st = &*s;
            (st.cb, st.user)
        };
        match ev {
            Event::OutFrame { nic, data } => {
                if let Some(f) = cb.out_frame {
                    f(user, nic, data.as_ptr(), data.len());
                }
                let st = &mut *s;
                st.stats.tx_frames += 1;
            }
            Event::ConnNew { id, nic, src, is_v6, sport, dst, dport } => {
                let sa = CoastAddr { is_v6, bytes: src };
                let da = CoastAddr { is_v6, bytes: dst };
                let accepted = match cb.conn_new {
                    Some(f) => f(user, id, nic, &sa, sport, &da, dport),
                    None => false,
                };
                if !accepted {
                    // C++ 侧拒收（拨 SOCKS 失败/设备被禁）→ 立刻 RST，别让半开连接挂着
                    let st = &mut *s;
                    let _ = st.engine.abort(id);
                    st.stats.conns_refused += 1;
                }
            }
            Event::ConnData { id, data } => {
                if let Some(f) = cb.conn_data {
                    f(user, id, data.as_ptr(), data.len());
                }
            }
            Event::ConnSent { id, n } => {
                if let Some(f) = cb.conn_sent {
                    f(user, id, n);
                }
            }
            Event::ConnClosed { id, is_abort } => {
                if let Some(f) = cb.conn_closed {
                    f(user, id, is_abort);
                }
            }
        }
    }
}

// —— 小工具：把裸指针安全地变成 &mut，非法就返回 None ——
#[inline]
unsafe fn stack_mut<'a>(s: *mut CoastStack) -> Option<&'a mut CoastStack> {
    if s.is_null() { None } else { Some(&mut *s) }
}

// ———————————————————————— 生命周期 ————————————————————————

#[no_mangle]
pub unsafe extern "C" fn coast_stack_new(
    cb: *const CoastCallbacks,
    user: *mut c_void,
) -> *mut CoastStack {
    if cb.is_null() {
        return core::ptr::null_mut();
    }
    let cb = *cb;
    // 五个回调缺一不可 —— 缺了说明调用方漏填，早失败好过运行期空指针。
    if cb.out_frame.is_none()
        || cb.conn_new.is_none()
        || cb.conn_data.is_none()
        || cb.conn_sent.is_none()
        || cb.conn_closed.is_none()
    {
        return core::ptr::null_mut();
    }
    alloc::boxed::Box::into_raw(alloc::boxed::Box::new(CoastStack::new(cb, user)))
}

#[no_mangle]
pub unsafe extern "C" fn coast_stack_free(s: *mut CoastStack) {
    if !s.is_null() {
        drop(alloc::boxed::Box::from_raw(s));
    }
}

// ———————————————————————— 网卡 ————————————————————————

#[no_mangle]
pub unsafe extern "C" fn coast_stack_add_nic(
    s: *mut CoastStack,
    nic: CoastNicId,
    mac6: *const u8,
    ip: *const CoastAddr,
    prefix_len: u8,
) -> i32 {
    let Some(st) = stack_mut(s) else { return COAST_ERR_BADARG };
    if mac6.is_null() || ip.is_null() {
        return COAST_ERR_BADARG;
    }
    let addr = *ip;
    let maxbits = if addr.is_v6 { 128 } else { 32 };
    if prefix_len > maxbits {
        return COAST_ERR_BADARG;
    }
    if st.engine.has_nic(nic) {
        return COAST_ERR_BADARG; // 重复注册：调用方状态机有问题，显式报错而不是静默覆盖
    }
    let mut mac = [0u8; 6];
    core::ptr::copy_nonoverlapping(mac6, mac.as_mut_ptr(), 6);
    if st.engine.add_nic(nic, mac, addr.bytes, addr.is_v6, prefix_len, st.last_poll_ms) {
        COAST_OK
    } else {
        COAST_ERR_NOMEM
    }
}

#[no_mangle]
pub unsafe extern "C" fn coast_stack_remove_nic(s: *mut CoastStack, nic: CoastNicId) -> i32 {
    let Some(st) = stack_mut(s) else { return COAST_ERR_BADARG };
    if st.engine.remove_nic(nic) { COAST_OK } else { COAST_ERR_NONIC }
}

// ———————————————————————— 数据面 ————————————————————————

#[no_mangle]
pub unsafe extern "C" fn coast_stack_input(
    s: *mut CoastStack,
    nic: CoastNicId,
    frame: *const u8,
    len: usize,
) -> i32 {
    let Some(st) = stack_mut(s) else { return COAST_ERR_BADARG };
    if frame.is_null() || len < 14 {
        if let Some(st) = stack_mut(s) {
            st.stats.rx_dropped += 1;
        }
        return COAST_ERR_BADARG;
    }
    let bytes = core::slice::from_raw_parts(frame, len);
    if !st.engine.input(nic, bytes) {
        st.stats.rx_dropped += 1;
        return COAST_ERR_NONIC;
    }
    st.stats.rx_frames += 1;
    COAST_OK
}

#[no_mangle]
pub unsafe extern "C" fn coast_stack_poll(s: *mut CoastStack, now_ms: u64) -> u64 {
    if s.is_null() {
        return u64::MAX;
    }
    let mut events = alloc::vec::Vec::new();
    {
        let st = &mut *s;
        // 泵的最坏间隔 —— 对应 lwIP 的 fastGapMax。平均值会把毛刺抹平，最坏间隔才决定性。
        if st.last_poll_ms != 0 && now_ms > st.last_poll_ms {
            let gap = (now_ms - st.last_poll_ms) as u32;
            if gap > st.stats.poll_max_gap_ms {
                st.stats.poll_max_gap_ms = gap;
            }
        }
        st.last_poll_ms = now_ms;
        st.engine.poll_collect(now_ms, &mut events);
    } // ← 借用在此结束，回调里才能安全地重入 coast_*
    dispatch(s, events);
    let st = &mut *s;
    st.engine.next_poll_ms(now_ms)
}

// ———————————————————————— 连接操作 ————————————————————————

#[no_mangle]
pub unsafe extern "C" fn coast_conn_send(
    s: *mut CoastStack,
    id: CoastConnId,
    data: *const u8,
    len: usize,
) -> i32 {
    if stack_mut(s).is_none() || data.is_null() {
        return COAST_ERR_BADARG;
    }
    if id == 0 {
        return COAST_ERR_NOCONN;
    }
    let st = &mut *s;
    let bytes = core::slice::from_raw_parts(data, len);
    match st.engine.send(id, bytes) {
        Ok(n) => n as i32,
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn coast_conn_sndbuf(s: *mut CoastStack, id: CoastConnId) -> i32 {
    if stack_mut(s).is_none() {
        return COAST_ERR_BADARG;
    }
    if id == 0 {
        return COAST_ERR_NOCONN;
    }
    let st = &mut *s;
    match st.engine.sndbuf(id) {
        Ok(n) => n as i32,
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn coast_conn_recved(s: *mut CoastStack, id: CoastConnId, n: u32) -> i32 {
    if stack_mut(s).is_none() {
        return COAST_ERR_BADARG;
    }
    if id == 0 {
        return COAST_ERR_NOCONN;
    }
    let st = &mut *s;
    match st.engine.recved(id, n) {
        Ok(()) => COAST_OK,
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn coast_conn_close(s: *mut CoastStack, id: CoastConnId) -> i32 {
    if stack_mut(s).is_none() {
        return COAST_ERR_BADARG;
    }
    if id == 0 {
        return COAST_ERR_NOCONN;
    }
    // 窗口未还满时返回 COAST_ERR_STATE —— 把 lwIP 那个"没还满就发 RST"的静默劣化
    // 变成显式失败（见 coaststack.h 里 coast_conn_close 的注释）。
    let st = &mut *s;
    match st.engine.close(id) {
        Ok(()) => COAST_OK,
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn coast_conn_abort(s: *mut CoastStack, id: CoastConnId) -> i32 {
    if stack_mut(s).is_none() {
        return COAST_ERR_BADARG;
    }
    if id == 0 {
        return COAST_ERR_NOCONN;
    }
    let st = &mut *s;
    match st.engine.abort(id) {
        Ok(()) => COAST_OK,
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn coast_stack_close_device_conns(
    s: *mut CoastStack,
    device: *const CoastAddr,
) -> i32 {
    if stack_mut(s).is_none() || device.is_null() {
        return COAST_ERR_BADARG;
    }
    let st = &mut *s;
    st.engine.close_device_conns(&(*device).bytes)
}

// ———————————————————————— 诊断 ————————————————————————

#[no_mangle]
pub unsafe extern "C" fn coast_stack_stats(s: *mut CoastStack, out: *mut CoastStats) {
    if out.is_null() {
        return;
    }
    let Some(st) = stack_mut(s) else {
        *out = CoastStats::default();
        return;
    };
    st.stats.conns_active = st.engine.conn_count() as u32;
    st.stats.conns_accepted = st.engine.conns_accepted;
    st.stats.conns_closed = st.engine.conns_closed;
    st.stats.conns_aborted = st.engine.conns_aborted;
    st.stats.conns_refused += st.engine.conns_refused;
    st.engine.conns_refused = 0;
    *out = st.stats;
    st.stats.poll_max_gap_ms = 0; // 瞬时量，读完清零
}

// 版本串：C 侧可打进 banner/诊断，确认链进来的库与头文件同版。
static VERSION: &[u8] = b"coaststack 0.1.0 (smoltcp engine)\0";

#[no_mangle]
pub extern "C" fn coast_stack_version() -> *const c_char {
    VERSION.as_ptr() as *const c_char
}

// ———————————————————————— 单元测试 ————————————————————————
// 本仓库此前没有任何单测框架（CLAUDE.md），Rust 侧是第一份。
#[cfg(test)]
mod tests {
    use super::*;
    use core::ptr;

    unsafe extern "C" fn cb_out(_u: *mut c_void, _n: CoastNicId, _f: *const u8, _l: usize) {}
    unsafe extern "C" fn cb_new(
        _u: *mut c_void, _i: CoastConnId, _n: CoastNicId,
        _s: *const CoastAddr, _sp: u16, _d: *const CoastAddr, _dp: u16,
    ) -> bool { true }
    unsafe extern "C" fn cb_data(_u: *mut c_void, _i: CoastConnId, _d: *const u8, _l: usize) {}
    unsafe extern "C" fn cb_sent(_u: *mut c_void, _i: CoastConnId, _n: u32) {}
    unsafe extern "C" fn cb_closed(_u: *mut c_void, _i: CoastConnId, _a: bool) {}

    fn cbs() -> CoastCallbacks {
        CoastCallbacks {
            out_frame: Some(cb_out), conn_new: Some(cb_new), conn_data: Some(cb_data),
            conn_sent: Some(cb_sent), conn_closed: Some(cb_closed),
        }
    }

    #[test]
    fn null_and_bad_args_never_panic() {
        unsafe {
            // 空指针入口一律返回错误码，不 panic —— 这是 panic="abort" 下的硬要求
            assert!(coast_stack_new(ptr::null(), ptr::null_mut()).is_null());
            assert_eq!(coast_stack_add_nic(ptr::null_mut(), 0, ptr::null(), ptr::null(), 0),
                       COAST_ERR_BADARG);
            assert_eq!(coast_stack_input(ptr::null_mut(), 0, ptr::null(), 0), COAST_ERR_BADARG);
            assert_eq!(coast_stack_poll(ptr::null_mut(), 0), u64::MAX);
            assert_eq!(coast_conn_send(ptr::null_mut(), 1, ptr::null(), 0), COAST_ERR_BADARG);
            coast_stack_free(ptr::null_mut()); // 空指针 free 必须安全
        }
    }

    #[test]
    fn callbacks_must_be_complete() {
        unsafe {
            let mut c = cbs();
            c.conn_new = None; // 漏填一个就该失败，而不是运行期空指针
            assert!(coast_stack_new(&c, ptr::null_mut()).is_null());
        }
    }

    #[test]
    fn nic_lifecycle() {
        unsafe {
            let c = cbs();
            let s = coast_stack_new(&c, ptr::null_mut());
            assert!(!s.is_null());
            let mac = [0x02u8, 0, 0x5b, 0, 0, 1];
            let mut ip = CoastAddr { is_v6: false, bytes: [0; 16] };
            ip.bytes[..4].copy_from_slice(&[192, 168, 20, 51]);

            assert_eq!(coast_stack_add_nic(s, 1, mac.as_ptr(), &ip, 24), COAST_OK);
            // 重复注册要显式报错，不能静默覆盖
            assert_eq!(coast_stack_add_nic(s, 1, mac.as_ptr(), &ip, 24), COAST_ERR_BADARG);
            // 前缀长度越界
            assert_eq!(coast_stack_add_nic(s, 2, mac.as_ptr(), &ip, 33), COAST_ERR_BADARG);
            // 未知网卡的帧要被计入 rx_dropped
            let frame = [0u8; 64];
            assert_eq!(coast_stack_input(s, 99, frame.as_ptr(), frame.len()), COAST_ERR_NONIC);
            assert_eq!(coast_stack_input(s, 1, frame.as_ptr(), frame.len()), COAST_OK);

            let mut st = CoastStats::default();
            coast_stack_stats(s, &mut st);
            assert_eq!(st.rx_frames, 1);
            assert_eq!(st.rx_dropped, 1);

            assert_eq!(coast_stack_remove_nic(s, 1), COAST_OK);
            assert_eq!(coast_stack_remove_nic(s, 1), COAST_ERR_NONIC);
            coast_stack_free(s);
        }
    }

    #[test]
    fn poll_tracks_worst_gap_and_clears_on_read() {
        unsafe {
            let c = cbs();
            let s = coast_stack_new(&c, ptr::null_mut());
            coast_stack_poll(s, 1000);
            coast_stack_poll(s, 1025);  // gap 25
            coast_stack_poll(s, 1225);  // gap 200 ← 最坏
            coast_stack_poll(s, 1250);  // gap 25
            let mut st = CoastStats::default();
            coast_stack_stats(s, &mut st);
            assert_eq!(st.poll_max_gap_ms, 200);
            coast_stack_stats(s, &mut st);
            assert_eq!(st.poll_max_gap_ms, 0, "瞬时量读完必须清零");
            coast_stack_free(s);
        }
    }
}
