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
