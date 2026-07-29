#pragma once

// 协议出站注册表 —— 把「加一个协议」的成本降到「加几个自己的文件 + 在一处注册一行」，尽量不动共享文件。
//
// ★ 为什么要注册表：CoastCore 计划并行开发多个协议出站单元（Shadowsocks / Trojan / VMess / VLESS…）。
//   若让拨号侧(CoreDialerFactory)直接 switch(node.type) 硬编码每个协议的构造，那么每加一个协议都要改
//   同一个 .cpp —— 多个单元并行改同一处必然冲突。改成注册表后：每个协议单元只写自己的
//   `XxxOutboundTcp/Udp` + 一个自由函数 `void registerXxx(OutboundRegistry&)`，把类型名映射到构造器；
//   共享侧只在 registerBuiltinProtocols() 里多调一行。冲突面从「一个大 switch」缩到「一行注册」。
//
// ★ 约定（协议单元照此做，务必一致）：
//   1) 协议类从 ProxyNode 构造：`XxxOutboundTcp(const ProxyNode&, QObject* parent)`，实现 IOutboundTcp。
//      不支持 UDP 的协议，UdpCreator 传空（nullptr）——createUdp 会因此返回 nullptr，拨号侧回退 fallback。
//   2) 每个协议单元在自己的头文件里声明：`void registerShadowsocks(OutboundRegistry&);` 之类的自由函数，
//      在 .cpp 里实现（内部 reg.registerProto("ss", tcpCreator, udpCreator)）。
//   3) 在 OutboundRegistry.cpp 的 registerBuiltinProtocols() 里 #include 该头并调一行 registerShadowsocks(reg)。
//   示例（Shadowsocks 单元大致长这样）：
//       // ShadowsocksOutbound.h
//       void registerShadowsocks(OutboundRegistry &reg);
//       // ShadowsocksOutbound.cpp
//       void registerShadowsocks(OutboundRegistry &reg) {
//           reg.registerProto(QStringLiteral("ss"),
//               [](const ProxyNode &n, QObject *p){ return new ShadowsocksOutboundTcp(n, p); },
//               [](const ProxyNode &n, QObject *p){ return new ShadowsocksOutboundUdp(n, p); });
//       }
//
// 线程性：注册在进程启动早期一次性完成（懒初始化，见 .cpp 的 instance()）；之后 has/createTcp/createUdp
//   只读那张表。数据面可能在工作线程上调 create*，故「注册」与「查询」不应交叠——实际用法里注册发生在
//   任何拨号之前，天然满足。表本身构造后不再改，读并发安全。
#include <QHash>
#include <QString>

#include <functional>

class IOutboundTcp;
class IOutboundUdp;
class QObject;
struct ProxyNode;

class OutboundRegistry
{
public:
    // 构造器签名：从一个 ProxyNode（已含 server/port/password/cipher/uuid/… 全部协议参数）造出站对象。
    // parent 用于 Qt 对象树（随拨号包装器一同析构）。
    using TcpCreator = std::function<IOutboundTcp *(const ProxyNode &, QObject *parent)>;
    using UdpCreator = std::function<IOutboundUdp *(const ProxyNode &, QObject *parent)>; // 可空

    // 进程级单例（懒初始化：首次调用时构造并跑一遍 registerBuiltinProtocols，线程安全靠函数内 static）。
    static OutboundRegistry &instance();

    // 注册一个协议类型。type 用小写协议名（"ss"/"trojan"/"vmess"/"vless"/…），与 ProxyNode::type 对齐。
    // udp 可为空 = 该协议不支持进程内 UDP。重复注册同名会覆盖（后注册者胜，便于测试替换）。
    void registerProto(const QString &type, TcpCreator tcp, UdpCreator udp);

    bool has(const QString &type) const;

    // 造出站对象。未注册 / 对应 creator 为空 → 返回 nullptr（拨号侧据此回退 fallback）。
    IOutboundTcp *createTcp(const ProxyNode &node, QObject *parent) const;
    IOutboundUdp *createUdp(const ProxyNode &node, QObject *parent) const;

private:
    OutboundRegistry() = default;

    struct Entry {
        TcpCreator tcp;
        UdpCreator udp;
    };
    QHash<QString, Entry> m_entries;
};

// 内建协议集中注册点。各协议单元把自己的 registerXxx(reg) 调用加进这里（见 .cpp 的 TODO）。
// 现在函数体为空：本单元只搭注册表骨架，尚无任何协议实现 —— 于是 registry.has(任何 type)==false，
// 拨号侧对具名节点一律回退 fallback，线上行为与「纯 Socks5」完全一致。
void registerBuiltinProtocols(OutboundRegistry &reg);
