#include "OutboundRegistry.h"

#include "../ProxyConfig.h" // ProxyNode（构造器入参类型）

// 说明：本文件只实现注册表容器本身 + 内建注册的**调用点**，不含任何具体协议。协议实现分散在各自单元。

OutboundRegistry &OutboundRegistry::instance()
{
    // 懒初始化 + 一次性内建注册。函数内 static 的构造在 C++11 起是线程安全的（编译器插入 guard），
    // 所以即便多个线程首次同时取 instance() 也只会构造一次、只跑一遍 registerBuiltinProtocols。
    static OutboundRegistry s_reg;
    static bool s_inited = [] {
        registerBuiltinProtocols(s_reg);
        return true;
    }();
    Q_UNUSED(s_inited);
    return s_reg;
}

void OutboundRegistry::registerProto(const QString &type, TcpCreator tcp, UdpCreator udp)
{
    m_entries.insert(type, Entry{std::move(tcp), std::move(udp)});
}

bool OutboundRegistry::has(const QString &type) const
{
    return m_entries.contains(type);
}

IOutboundTcp *OutboundRegistry::createTcp(const ProxyNode &node, QObject *parent) const
{
    const auto it = m_entries.constFind(node.type);
    if (it == m_entries.constEnd() || !it->tcp)
        return nullptr; // 未注册或无 TCP 构造器 → 交回拨号侧回退
    return it->tcp(node, parent);
}

IOutboundUdp *OutboundRegistry::createUdp(const ProxyNode &node, QObject *parent) const
{
    const auto it = m_entries.constFind(node.type);
    if (it == m_entries.constEnd() || !it->udp)
        return nullptr; // 未注册或该协议不支持 UDP → 回退
    return it->udp(node, parent);
}

// —— 内建协议注册点 ——
// 后续每个「协议出站」单元在这里加**一行**：先 #include 自己的头，再调 registerXxx(reg)。例如：
//   #include "ShadowsocksOutbound.h"   // 提供 void registerShadowsocks(OutboundRegistry&);
//   #include "TrojanOutbound.h"        // 提供 void registerTrojan(OutboundRegistry&);
//   ...
//   registerShadowsocks(reg);
//   registerTrojan(reg);
// 现在故意留空：本骨架单元没有任何协议实现，注册表为空 → 具名节点全部回退 fallback，行为不变。
void registerBuiltinProtocols(OutboundRegistry &reg)
{
    Q_UNUSED(reg);
    // TODO(协议出站单元)：在此逐行注册内建协议（Shadowsocks / Trojan / VMess / VLESS / …）。
}
