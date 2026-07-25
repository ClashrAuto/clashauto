#pragma once

// 透明网关编排器 —— DevicesController 用的稳定公共 API（跨平台可编译）。
//
// 内部（仅 Linux 实现，其余平台为 no-op 桩，isAvailable()=false）：
//   ArpSpoofer（对被劫持设备伪装成网关、对网关伪装成设备 = 双向 MITM）
//     + IL2Endpoint（AF_PACKET 二层收发）
//     + NetStack（lwIP 用户态栈：把捕获到的该设备 IP 帧终结成 TCP/UDP 连接）
//     + Socks5Tcp/Udp（每条连接拨 mihomo，user=dev-<mac> → IN-USER 规则/inboundUser 归属）。
//
// 安全：只劫持 enableDevice() 显式开启的设备；disableAll() / 析构 / 崩溃看门狗必须**可靠还原
// ARP**（给被劫持设备重发「网关的真实 MAC」），否则设备断网。启动时应调用 recoverFromCrash()
// 读取上次留下的劫持清单先还原再继续。
//
// 权限：Linux 需 CAP_NET_RAW/root。isAvailable() 反映「平台支持 且 能打开 AF_PACKET」。
#include <QObject>
#include <QString>
#include <QStringList>

class LanGateway : public QObject
{
    Q_OBJECT
public:
    explicit LanGateway(QObject *parent = nullptr);
    ~LanGateway() override;

    // 配置本机拓扑 + mihomo SOCKS 端点。localMac/gatewayMac 为 "aa:bb:cc:dd:ee:ff"。
    // 每次网段/网关变化（扫描后）都可重配。socksPort 通常 = config.mixedPort(7890)。
    // netmask 为主网卡子网掩码（点分，如 "255.255.255.0"）：被劫持设备发往「本网段内且非网关本身」
    // 的帧不进用户态栈、照常二层直达——否则设备回给本机 LAN IP 的包会被 lwIP 误终结，导致本机
    // 无法直连该设备（SSH/网页/共享）。空掩码=不做旁路（退回旧行为，全部帧进栈）。
    void configure(const QString &ifname, const QString &localIp, const QString &localMac,
                   const QString &gatewayIp, const QString &gatewayMac, quint16 socksPort,
                   const QString &netmask = QString());

    // 平台是否可用（Linux 且能打开二层）。DevicesController.gatewayReady 返回它。
    bool isAvailable() const;

    // 开始劫持某设备：ip+mac 为目标，socksUser 为其 mihomo 身份（dev-<mac 短哈希>）。
    // 成功后该设备所有 IP 流量经用户态栈拨 mihomo。失败置 *err。
    bool enableDevice(const QString &mac, const QString &ip, const QString &socksUser, QString *err);
    // 停止劫持某设备并还原其 ARP。
    void disableDevice(const QString &mac);
    // 停止全部（退出/急停）：还原所有被劫持设备的 ARP。
    void disableAll();

    // 启动时调用：若上次异常退出留有劫持清单，先给这些设备发还原 ARP（panic-restore）。
    void recoverFromCrash();

    QStringList activeDevices() const; // 当前被劫持的 mac 列表

signals:
    void deviceError(const QString &mac, const QString &message);
    void statusChanged(); // 劫持集合变化

private:
    class Impl;
    Impl *d;
};
