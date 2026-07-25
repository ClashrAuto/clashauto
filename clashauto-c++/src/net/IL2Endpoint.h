#pragma once

// 二层（以太帧）收发抽象 —— 透明网关的最底层。
// 平台后端：Linux = AF_PACKET(SOCK_RAW)；macOS = BPF(/dev/bpf，经 root helper)；Windows = Npcap。
// M1 只实现 Linux；其余平台 createL2Endpoint() 返回 nullptr（LanGateway 据此 isAvailable=false）。
//
// 契约：
//  - open() 绑定到指定网卡，成功后 frameReceived() 会在 Qt 事件循环线程发出**每一个**收到的完整
//    以太帧（含 14 字节以太头；不含 FCS）。用 QSocketNotifier 驱动读取，切勿轮询/阻塞。
//  - send() 发送一个完整以太帧（调用方负责填好 dst/src MAC 与 ethertype）。
//  - localMac() 返回本网卡 6 字节 MAC；ifIndex() 返回接口索引（AF_PACKET sll_ifindex 用）。
//  - 需要 CAP_NET_RAW / root。open() 失败（权限/网卡不存在）时置 *err 并返回 false。
#include <QByteArray>
#include <QObject>
#include <QString>

class IL2Endpoint : public QObject
{
    Q_OBJECT
public:
    explicit IL2Endpoint(QObject *parent = nullptr) : QObject(parent) {}
    ~IL2Endpoint() override = default;

    virtual bool open(const QString &ifname, QString *err) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual bool send(const QByteArray &frame) = 0; // 完整以太帧（>=14 字节）
    virtual QByteArray localMac() const = 0;         // 6 字节
    virtual int ifIndex() const = 0;
    virtual int mtu() const = 0;                      // 接口 MTU（不含以太头），失败回 1500

signals:
    // 收到一个完整以太帧（含以太头）。接收方零拷贝语义：帧内容仅在槽内有效，需要保留请拷贝。
    void frameReceived(const QByteArray &frame);
};

// 工厂：返回当前平台的二层端点实现；不支持的平台返回 nullptr。所有权归 parent。
IL2Endpoint *createL2Endpoint(QObject *parent = nullptr);
