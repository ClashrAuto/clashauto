// Coast 用户态 TCP 数据面（smoltcp）—— 以 C ABI 静态库形式供 C++ 侧的 NetStack 调用。
//
// 只做 TCP。UDP/DNS、ARP/NDP 投毒、SOCKS 拨号与每设备身份全在 C++ 侧，
// 职责边界见 src/net/coaststack.h 的文件头。
//
// Phase 1（当前）：C ABI 骨架 —— 符号齐全、ABI 正确、零 panic，引擎是桩。
// Phase 2：接 smoltcp 引擎本体（catch-all 端口改写 shim + 转发循环）。

#![allow(clippy::missing_safety_doc)]

extern crate alloc;

mod ffi;
mod portmap;
mod engine;

#[cfg(test)]
mod e2e;
pub use ffi::*;
