#pragma once

// CoastCore 的出站工厂 —— 「按 (目的地, 设备) 选一个出站实现」的接线点。
//
// 它是 NetStack 的 OutboundFactory，但自己不实现任何协议：拿到一次拨号请求后，先问一个**路由钩子**
// router(dstHost, user) 「这条该走哪个节点」，再据答案分派：
//   · router 返回空  → 交给**回退工厂** fallback（现状 = Socks5OutboundFactory，即照旧拨 mihomo）。
//   · router 返回 "DIRECT" → 造进程内 DirectOutbound*（直连，见 DirectOutbound.h）。
//   · router 返回具体协议节点名 → 本单元还没有协议出站实现，暂时也回退到 fallback（留 TODO，后续
//     「协议出站」单元往这里填 Shadowsocks/Trojan/… 的构造）。
//
// ★ 行为不变的保证：默认 router 为空 → 一切照旧全走 fallback（mihomo）。DirectOutbound 虽已就绪，
//   但在没人给 router 之前不会被路由到。这样这个骨架单元可以先落地、后续再把分流逻辑接上来，
//   期间线上行为与「纯 Socks5OutboundFactory」完全一致。
//
// 生命周期：CoreDialerFactory **拥有** fallback（析构时 delete 它）；不拥有 store（由上层持有）。
#include "../IOutbound.h"

#include <QString>

#include <functional>

class ProxyConfigStore;

class CoreDialerFactory : public OutboundFactory
{
public:
    // 路由钩子：给目的主机 dstHost 与设备身份 user，返回应走的节点名；返回空串 = 交回退工厂。
    using Router = std::function<QString(const QString &dstHost, const QString &user)>;

    // store = 当前出站配置快照的来源（本单元暂只透传，供后续按节点造协议出站时查参数）；
    // fallback = 回退工厂，**所有权转移给本类**（dtor 里 delete）。现状传入 Socks5OutboundFactory。
    CoreDialerFactory(ProxyConfigStore *store, OutboundFactory *fallback);
    ~CoreDialerFactory() override;

    // 装分流逻辑。默认无 router（全走 fallback）。router 会在数据面线程上被同步调用，须自身线程安全
    //（典型实现 = 读 store->current() 这份不可变快照做判定，天然无锁安全）。
    void setRouter(Router router);

    // 严格模式：**拒绝回退核心** —— 判不了/协议缺/规则要解析 IP 时直接让该连接失败，而不是
    // 静默交给 mihomo。默认关。存在的理由是「离完全替换核心还差多少」这个问题：静默回退会把
    // 差距藏起来，开着它差距就变成明确的失败 + GatewayDiag 里 cc=… 的原因分布。
    void setStrict(bool on) { m_strict = on; }

    IOutboundTcp *createTcp(QObject *parent) override;
    IOutboundUdp *createUdp(QObject *parent) override;

private:
    ProxyConfigStore *m_store = nullptr; // 不持有
    OutboundFactory *m_fallback = nullptr; // 持有，dtor delete
    bool m_strict = false;
    Router m_router;                       // 空 = 全部回退
};
