#pragma once

// 用户态 TCP/IP 栈（Linux，基于 vendored lwIP，PIMPL 把 lwIP 头藏进 .cpp）。
//
// 职责：被劫持设备的以太帧经 LanGateway 过滤后送入 inputFrame()：
//   · IP/TCP 帧 → 交 lwIP(ethernet_input→ip4_input，accept-all 补丁使其终结发往任意公网 IP 的连接)
//     → 每条 TCP 连接经 Socks5Tcp 拨 mihomo 网关口(带每设备用户名) → 双向桥接字节。
//   · IP/UDP 帧 → 不进 lwIP，直接解析，经 Socks5Udp 转发（含 DNS）；回程手工封包发回设备。
//   · lwIP 出网(linkoutput) → 序列化成以太帧(dst=设备MAC，已由静态 ARP 表解析) → IL2Endpoint::send。
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
    NetStack(IL2Endpoint *endpoint, quint16 socksPort, QObject *parent = nullptr);
    ~NetStack() override;

    // 初始化 lwIP + netif(hwaddr=本机MAC) + catch-all TCP 监听。失败置 *err。
    bool init(const QByteArray &localMac6, QString *err);

    // 登记/注销被劫持设备：静态 ARP(ip↔mac) + 记录其 mihomo 身份用户名。
    void addDevice(const QString &ip, const QByteArray &mac6, const QString &socksUser);
    void removeDevice(const QString &ip);

    // 送入一个「已确认属于某被劫持设备」的以太帧（含 14 字节以太头）。
    void inputFrame(const QByteArray &frame);

private:
    // UDP（含 DNS）不走 lwIP：手工解析设备发出的 UDP → Socks5Udp 转发；回程手工封包发回设备。
    void handleUdpFrame(const QByteArray &frame, int ihl);
    void onUdpResponse(const QString &victimIp, const QHostAddress &fromIp, quint16 fromPort,
                       const QByteArray &payload);

    struct Impl;
    Impl *d;
};
