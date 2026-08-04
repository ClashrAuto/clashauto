#include "CoreRouter.h"

#include "DnsResolver.h"
#include "ProxyConfig.h"
#include "RuleEngine.h"

#include <QDebug>
#include <QHostAddress>

#include <cstdio>

namespace coastcore {

CoreDialerFactory::Router makeRouter(std::shared_ptr<ProxyConfigStore> store,
                                     std::shared_ptr<RuleEngine> rules, bool debugLog)
{
    return [store, rules, debugLog](const QString &dstHost, const QString &user) -> QString {
        Q_UNUSED(user);
        // ★★ 仍是 fake-ip 的目的地 = 「没反查到域名」，必须回退 mihomo ★★
        //   正常路径上 NetStack 的 accept 已经用 DNS 旁听学到的映射把 fake-ip 改写成域名了
        //   （见 NetStack 里的 fakeip 改写 + DnsResolver::learnFromDnsResponse），所以走到这里
        //   还是个 198.18.x.x，说明**没学到**（映射被容量清空/进程刚起来设备用的是旧 DNS 缓存等）。
        //   这时绝不能交给进程内出站——它会把假 IP 原样发给节点，节点路由不到、必然 i/o timeout
        //   （真机实测过：节点侧 55 次 dial 198.18.x.x 失败）。核心自己知道映射，交给它才对。
        //   （本机入站拿到的是客户端给的真实域名，走不到这一支；留着是因为两个入口共用一份实现。）
        if (!dstHost.isEmpty()) {
            const QHostAddress dst(dstHost);
            if (!dst.isNull() && DnsResolver::isFakeIp(dst))
                return QString();
        }
        const std::shared_ptr<const ProxyConfig> cfg = store ? store->current() : nullptr;
        if (!cfg)
            return QString(); // 无配置 → 回退 mihomo（安全）
        switch (cfg->mode()) {
        case ProxyConfig::Mode::Direct:
            return QStringLiteral("DIRECT"); // 直连恒正确 → 进程内 DirectOutbound
        case ProxyConfig::Mode::Global:
            return cfg->selected(); // 单选节点；协议未注册时 CoreDialerFactory 自动回退 mihomo
        case ProxyConfig::Mode::Rule: {
            // 规则模式：用 RuleEngine 做首命中匹配，再把 target（多为**策略组名**）解析成能拨的节点。
            // 三种情况一律回退核心（宁可少走进程内，也绝不误路由）：
            //   · needsResolve —— 撞上「不带 no-resolve 的 IP 规则」而我们手上只有域名，判不了
            //     （核心会解析后再判，结论必然对）。判据与实测依据见 RuleEngine::matchEx 的注释。
            //   · 没命中任何规则（连 MATCH 都没有）；
            //   · target 解析不出节点，或解析成 REJECT（拒绝要由核心执行，我们没有"拒绝"出站）。
            if (!rules)
                return QString();
            QHostAddress ipForRule;
            QString hostForRule;
            if (!dstHost.isEmpty()) {
                QHostAddress probe;
                if (probe.setAddress(dstHost))
                    ipForRule = probe;     // 目的地是 IP 字面量
                else
                    hostForRule = dstHost; // 目的地是域名（多半是 fake-ip 反查回来的）
            }
            const RuleEngine::MatchOutcome mo = rules->matchEx(hostForRule, ipForRule);
            const QString node =
                (mo.needsResolve || mo.target.isEmpty()) ? QString() : cfg->resolveTarget(mo.target);
            if (debugLog) {
                qInfo().noquote() << "[CCROUTE] host="
                                  << (hostForRule.isEmpty() ? dstHost : hostForRule)
                                  << "needsResolve=" << mo.needsResolve << "target=" << mo.target
                                  << "node=" << node
                                  << "groupMapSize=" << cfg->groupToNode().size()
                                  << "nodes=" << cfg->nodes().size();
            }
            if (node.isEmpty() || node == QStringLiteral("REJECT"))
                return QString();
            return node; // "DIRECT" 或具体节点名
        }
        }
        return QString();
    };
}

// ===================== 自测 =====================
namespace {

bool expectRoute(const char *what, const CoreDialerFactory::Router &r, const QString &host,
                 const QString &want)
{
    const QString got = r(host, QStringLiteral("u"));
    const bool ok = (got == want);
    std::fprintf(stderr, "  [%s] %s → \"%s\"%s\n", ok ? "PASS" : "FAIL", what,
                 qUtf8Printable(got), ok ? "" : qUtf8Printable(QStringLiteral("（期望 \"") + want + QStringLiteral("\"）")));
    return ok;
}

} // namespace

bool routerSelfTest()
{
    std::fprintf(stderr, "=== CoreRouter self-test ===\n");
    bool ok = true;

    QVector<ProxyNode> nodes;
    nodes.push_back(ProxyNode::direct());
    ProxyNode n;
    n.name = QStringLiteral("HK-01");
    n.type = QStringLiteral("trojan");
    nodes.push_back(n);

    // —— Direct 模式：恒 DIRECT ——
    {
        auto store = std::make_shared<ProxyConfigStore>();
        store->reload(std::make_shared<const ProxyConfig>(nodes, QStringLiteral("HK-01"),
                                                          ProxyConfig::Mode::Direct));
        ok &= expectRoute("Direct 模式 → DIRECT", makeRouter(store, nullptr),
                          QStringLiteral("example.com"), QStringLiteral("DIRECT"));
    }
    // —— Global 模式：恒 selected ——
    {
        auto store = std::make_shared<ProxyConfigStore>();
        store->reload(std::make_shared<const ProxyConfig>(nodes, QStringLiteral("HK-01"),
                                                          ProxyConfig::Mode::Global));
        ok &= expectRoute("Global 模式 → selected", makeRouter(store, nullptr),
                          QStringLiteral("example.com"), QStringLiteral("HK-01"));
    }
    // —— 无配置：必须回退（空串）——
    {
        auto store = std::make_shared<ProxyConfigStore>();
        ok &= expectRoute("无配置 → 回退核心", makeRouter(store, nullptr),
                          QStringLiteral("example.com"), QString());
    }
    // —— fake-ip 目的地：必须回退，**这条是踩过真机坑的**（节点侧 55 次 dial 198.18.x.x 失败）——
    {
        auto store = std::make_shared<ProxyConfigStore>();
        store->reload(std::make_shared<const ProxyConfig>(nodes, QStringLiteral("HK-01"),
                                                          ProxyConfig::Mode::Direct));
        ok &= expectRoute("fake-ip 目的地 → 回退核心（即使 Direct 模式）", makeRouter(store, nullptr),
                          QStringLiteral("198.18.0.7"), QString());
    }
    // —— Rule 模式但没有规则引擎：必须回退，不能瞎猜 ——
    {
        auto store = std::make_shared<ProxyConfigStore>();
        store->reload(std::make_shared<const ProxyConfig>(nodes, QStringLiteral("HK-01"),
                                                          ProxyConfig::Mode::Rule));
        ok &= expectRoute("Rule 模式无规则引擎 → 回退核心", makeRouter(store, nullptr),
                          QStringLiteral("example.com"), QString());
    }

    std::fprintf(stderr, "=== CoreRouter self-test: %s ===\n", ok ? "PASS" : "FAIL");
    std::fflush(stderr);
    return ok;
}

} // namespace coastcore
