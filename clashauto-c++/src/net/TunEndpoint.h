#pragma once

// 本机 TUN 入口的跨平台门面 —— 「进程内增强(TUN)」的设备层。
//
// ★ 关键设计：**TUN 端点伪装成 IL2Endpoint（以太端点），而不是给 NetStack 加一条 L3 通道。**
//   TUN/wintun 收发的是**裸 IP 包**，而 NetStack/lwIP 这一整套（netif、设备表、DNS 劫持、
//   fake-ip 反查、按目的地选出站）都是按以太帧写的。若为 TUN 另开一条 L3 路径，等于把这套逻辑
//   再实现一遍 —— 正是本项目一直在避免的「两套逻辑慢慢漂移」。所以转换做在最底层：
//     · 读：给 IP 包**前置 14 字节以太头**后交给上层；
//     · 写：把上层给的以太帧**剥掉 14 字节**，只把 IP 包写回 TUN。
//   上层一行都不用改，网关那条路验过的东西（含 lwIP 的 accept-all 补丁）直接复用。
//
// 合成 MAC 放在这里而不是各平台 .cpp 里：**两端必须一致**，且 NetStack::addDevice 要用 peer MAC
// 登记静态邻居。放各自文件里迟早漂移（Linux 一个值、Windows 另一个值，只在真机上才炸）。
// TUN 上只有「本机」一个来源，不需要区分设备；ARP/NDP 在 TUN 上不存在（没有二层邻居）。
#include <QByteArray>

class IL2Endpoint;
class QObject;

namespace coastcore {

constexpr int kTunEthHdr = 14;
// **本地管理**地址（第二字节的 0x02 位），固定值而不是随机：日志/抓包里一眼认得出是 TUN 合成的。
constexpr unsigned char kTunPeerMac[6] = {0x02, 0x43, 0x4f, 0x41, 0x53, 0x54};  // 02:"COAST"
constexpr unsigned char kTunLocalMac[6] = {0x02, 0x43, 0x4f, 0x41, 0x53, 0x01};

inline QByteArray tunPeerMac()
{
    return QByteArray(reinterpret_cast<const char *>(kTunPeerMac), 6);
}
inline QByteArray tunLocalMac()
{
    return QByteArray(reinterpret_cast<const char *>(kTunLocalMac), 6);
}

} // namespace coastcore

// 工厂：返回当前平台的 TUN 端点；不支持的平台返回 nullptr（上层据此回退 mihomo 的 TUN）。
// 所有权归 parent。open(ifname) 后才真正建网卡。
// 需要 root / Administrator；拿不到权限时 open() 置 *err 说明原因并返回 false。
//
// ★ 地址/路由**不由端点配置**，由调用方做（Linux `ip addr`/`ip route`、Windows `netsh` 或
//   iphlpapi）。端点只管收发帧 —— 这样各平台的设备层保持一样窄，路由策略（含环路规避）
//   集中在一处，不会散到两个平台文件里。Windows 侧调用方需要接口索引：见 ifIndex()。
IL2Endpoint *createTunEndpoint(QObject *parent);
