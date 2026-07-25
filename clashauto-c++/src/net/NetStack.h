#pragma once

// 用户态 TCP/IP 栈（基于 vendored lwIP，PIMPL 把 lwIP 头藏进 .cpp）。
//
// 职责：被劫持设备的以太帧经 LanGateway 过滤后送入 inputFrame()：
//   · IP/TCP 帧 → 交 lwIP(ethernet_input→ip4_input，accept-all 补丁使其终结发往任意公网 IP 的连接)
//     → 每条 TCP 连接经 Socks5Tcp 拨 mihomo 网关口(带每设备用户名) → 双向桥接字节。
//   · IP/UDP 帧 → 不进 lwIP，直接解析，经 Socks5Udp 转发（含 DNS）；回程手工封包发回设备。
//   · lwIP 出网(linkoutput) → 序列化成以太帧(dst=设备MAC，已由静态 ARP 表解析) → IL2Endpoint::send。
//
// **一个栈、多张网卡**：lwIP 只能有一个实例（lwip_init 全局、ARP 表全局、PCB 链全局），但一个实例
// 可以挂多个 netif。所以「同时代理有线 A 路由 + WiFi B 路由」的做法是 addNic() 一张卡一个 netif，
// 而不是开多个 NetStack。每个 netif 用**本机在该网卡上的真实 IP + 真实掩码**：
//   · 出方向 ip4_route(目的=设备IP) 靠子网匹配挑中正确的 netif（若像早期那样给 0.0.0.0/0 占位掩码，
//     两张卡都「匹配一切」，ip4_route 永远返回链表里第一张 → B 网段设备的回包会从 A 网卡发出去）；
//   · etharp_add_static_entry 内部也走 ip4_route，静态 ARP 因此自动落到正确的 netif 上。
// 每 netif 的二层端点/本机 MAC 放在 netif->state 里，C 回调从那里取上下文（不再用全局单例）。
//
// 线程：全部在 Qt 主事件循环（NO_SYS 单线程 lwIP）。QTimer 周期 sys_check_timeouts()。
#include <QByteArray>
#include <QObject>
#include <QString>

class IL2Endpoint;
class QHostAddress;

class NetStack final : public QObject
{
    Q_OBJECT
public:
    explicit NetStack(quint16 socksPort, QObject *parent = nullptr);
    ~NetStack() override;

    // 初始化 lwIP + catch-all TCP 监听 + 定时器泵。全进程只能有一个实例。失败置 *err。
    bool init(QString *err);

    // 挂一张网卡：ep 为其二层端点，localMac6/localIp/netmask 为本机在这张卡上的信息。
    // localIp/netmask 必须有效——它们决定出方向选哪个 netif（见文件头说明）。
    bool addNic(IL2Endpoint *ep, const QByteArray &localMac6, const QString &localIp,
                const QString &netmask, QString *err);
    // 摘掉一张网卡（网卡消失/重配时）。其上的设备静态 ARP 由 removeDevice 各自清理。
    void removeNic(IL2Endpoint *ep);
    bool hasNic(IL2Endpoint *ep) const;

    // 登记/注销被劫持设备：静态 ARP(ip↔mac) + 记录其 mihomo 身份用户名。
    void addDevice(const QString &ip, const QByteArray &mac6, const QString &socksUser);
    void removeDevice(const QString &ip);

    // 送入一个「已确认属于某被劫持设备」的以太帧（含 14 字节以太头）。
    // from = 收到该帧的二层端点，用来定位注入哪个 netif（也决定 UDP 回程从哪张卡发出）。
    void inputFrame(IL2Endpoint *from, const QByteArray &frame);

    // 前置声明公开（定义仍在 .cpp）：让 .cpp 内的自由函数/lwIP C 回调（在匿名命名空间里）
    // 能命名该类型。不暴露任何实现细节。
    struct Impl;
    struct Nic;

private:
    // UDP（含 DNS）不走 lwIP：手工解析设备发出的 UDP → Socks5Udp 转发；回程手工封包发回设备。
    // NAT 粒度是「每个设备源端口一条独立的 SOCKS UDP 关联」，所以回程不需要（也无法）靠
    // (fromIp,fromPort) 反查设备端口——vport 由「回包从哪条关联进来」直接给出。细节见 .cpp。
    void handleUdpFrame(Nic *nic, const QByteArray &frame, int ihl);
    void onUdpResponse(const QString &victimIp, quint16 vport, const QHostAddress &fromIp,
                       quint16 fromPort, const QByteArray &payload);

    Impl *d;
};
