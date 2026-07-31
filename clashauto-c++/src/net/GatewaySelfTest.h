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

// 「网关已在跑 + 再开进程内 TUN」的**组合**自测（COAST_COMBO_SELFTEST，Linux + root）。
//
// 为什么必须单列这一条：线上真实故障就是「两条路各自都验过、组合从没验过」——
// lwIP 全进程只能有一份栈，而局域网扫描每跑一轮网关就把它建起来了；进程内 TUN 当时自己 new 一个，
// 于是 init() 恒被判重拒掉，用户点「增强」永远打不开（"已有一个网关协议栈实例在运行"）。
// 修法是让 TUN 挂到同一份栈上当第二张网卡（addNic），本自测就是它的验收标准。
//
// 值：1 = 先起网关再开 TUN（复现线上顺序）；2 = 先开 TUN 再挂网关网卡（反向顺序）。
// 断言（缺一不可，任何一条不成立都 FAIL 而不是「跑过了」）：
//   ① 前提：网关路（curl → TAP → lwIP → 假 SOCKS）与本机路（curl → 真目标）各自本来就通；
//   ② TUN 开得起来 —— 这一条就是修复前必然失败的那一条；
//   ③ **组合期间两条路同时通**：假 SOCKS 的 CONNECT 计数增加 且 经 TUN 的 curl 返回码不变；
//   ④ 关掉 TUN 之后网关路仍然通（计数继续增加）、本机网络恢复原状。
// 网关的协议栈刻意建在一条**独立的工作线程**上（与正式 App 的 LanGateway 同形），
// 这样跨线程 marshal 那一段也一并被覆盖到 —— 单线程跑的话那段代码根本不会被执行。
//
// 返回码：0=通过，1=断言失败，2=前提不成立（环境本来就不通），3=环境错误（TAP/栈/root）。
// 配套脚本：validate/combo_selftest.sh（建 TAP + 配路由/静态邻居 + 跑两种顺序）。
int runComboSelfTest();

// NdpSpoofer::parseRouterAdvert 的纯解析自测（COAST_NDP_RA_SELFTEST=1 触发）。
//
// 为什么单独给它一个钩子：从 RA 学 v6 路由器这条路，在**没有 IPv6 的网络上永远跑不到**
//（真机实证：本项目的树莓派测试台整条 LAN 一条 RA 都没有，主动发 RS 也没人应答），
// 而它又恰恰是「双栈设备 v6 漏代理」这一类静默故障的唯一防线 —— 没有自测就等于没有覆盖。
// 本钩子用手工拼的 RA 字节流覆盖：正常解析（LL/MAC/前缀）、SLLA 选项优先于以太源 MAC、
// 以及四个必须拒绝的用例（hop limit≠255 / Router Lifetime==0 / 源地址非链路本地 / 选项长度为 0）。
// 不需要 root、不碰网络、毫秒级完成。返回 0=全部通过，1=有断言失败。
int runNdpRaSelfTest();
