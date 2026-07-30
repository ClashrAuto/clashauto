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
// 每个「协议出站」单元在这里加一行：#include 自己的头 + 调 registerXxx(reg)。
// 已注册的协议在拨号侧(CoreDialerFactory)会被进程内实现接管；未注册/未来协议仍回退 fallback(mihomo)。
#include "TrojanOutbound.h"      // registerTrojan      —— "trojan" (TLS)
#include "VlessOutbound.h"       // registerVless       —— "vless" (tcp/tls/ws)

// ss / vmess / reality 要裸 AEAD(AES-GCM/ChaCha20) 或 X25519 —— 都来自 OpenSSL libcrypto，而它是**可选**
// 依赖（macOS 通用构建 / Windows arm64 拿不到可用的 libcrypto，见 CMakeLists 的 COAST_HAVE_OPENSSL）。
// 缺它时这三个协议整块不编、不注册 → 具名节点回退 mihomo，功能不缺。
#ifdef COAST_HAVE_OPENSSL
#include "ShadowsocksOutbound.h" // registerShadowsocks —— "ss" (AEAD)
#include "VmessOutbound.h"       // registerVmess       —— "vmess" (VMessAEAD)
#include "RealityOutbound.h"     // registerReality     —— "reality" (uTLS+REALITY over 自建 TLS1.3)
#endif

#ifdef COAST_HAVE_QUIC
#include "Hysteria2Outbound.h"   // registerHysteria2   —— "hysteria2"/"hy2" (QUIC)
#include "TuicOutbound.h"        // registerTuic        —— "tuic" (QUIC)
#endif

void registerBuiltinProtocols(OutboundRegistry &reg)
{
    registerTrojan(reg);
    registerVless(reg);
#ifdef COAST_HAVE_OPENSSL
    registerShadowsocks(reg);
    registerVmess(reg);
    registerReality(reg);
#endif
#ifdef COAST_HAVE_QUIC
    // QUIC 系仅在构建带上 msquic 时编入(CMake find_package(msquic) 成功 → 定义 COAST_HAVE_QUIC)。
    // 否则 hysteria2/tuic 未注册 → 具名节点回退 mihomo,不影响其余协议。
    registerHysteria2(reg);
    registerTuic(reg);
#endif
}
