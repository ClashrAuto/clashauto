#pragma once

// 透明网关 headless 自测入口（Linux）。由环境变量 COAST_GATEWAY_SELFTEST 触发（见 main_qml）。
//
// 目的：在**不需要真实 ARP、不需要真机**的前提下，端到端验证用户态栈 + SOCKS 桥接的转发逻辑：
//   用一个 TAP 设备把**真实内核 TCP**（外部 curl）当作「被劫持设备」，其流量经 NetStack 终结、
//   拨到一个内置的假 SOCKS5 服务器。因为 lwIP 的 accept 回调只在 TCP 三次握手**完成后**才触发，
//   所以「假 SOCKS 收到带正确用户名的 CONNECT」这一件事，即证明了：帧入栈→双向握手→catch-all
//   监听→SOCKS 连接+每设备用户名认证 整条链路都通。
//
// 配套脚本 validate/gateway_selftest.sh 负责建 TAP、配路由/静态邻居、跑 curl、断言。
// 返回码：0=通过，1=超时/失败，3=环境错误（TAP/栈初始化失败）。
int runGatewaySelfTest();

// NdpSpoofer::parseRouterAdvert 的纯解析自测（COAST_NDP_RA_SELFTEST=1 触发）。
//
// 为什么单独给它一个钩子：从 RA 学 v6 路由器这条路，在**没有 IPv6 的网络上永远跑不到**
//（真机实证：本项目的树莓派测试台整条 LAN 一条 RA 都没有，主动发 RS 也没人应答），
// 而它又恰恰是「双栈设备 v6 漏代理」这一类静默故障的唯一防线 —— 没有自测就等于没有覆盖。
// 本钩子用手工拼的 RA 字节流覆盖：正常解析（LL/MAC/前缀）、SLLA 选项优先于以太源 MAC、
// 以及四个必须拒绝的用例（hop limit≠255 / Router Lifetime==0 / 源地址非链路本地 / 选项长度为 0）。
// 不需要 root、不碰网络、毫秒级完成。返回 0=全部通过，1=有断言失败。
int runNdpRaSelfTest();

// Rust(smoltcp) 数据面的链接+ABI 自证（COAST_RUSTSTACK_SELFTEST=1，仅 Windows 且开了
// COAST_RUST 时编入）。0=通过。见 RustStackSelfTest.cpp 的文件头。
int runRustStackSelfTest();
