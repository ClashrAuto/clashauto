// NetStack 的非 Windows 桩实现 —— 用户态 TCP 栈只在 Windows 上存在。
//
// ── 为什么有这个文件 ────────────────────────────────────────────────────────────
// 三端的网关数据面各走各的内核路径，只有 Windows 没得选：
//   · Linux  → TPROXY（TproxyRules.cpp，nftables + 策略路由）
//   · macOS  → pf rdr（PfRules.cpp，DIOCNATLOOK 取原始目的地）
//   · Windows→ 用户态栈（NetStack.cpp + rust/coaststack 的 smoltcp）
//              WfpRedirect.h:91 记着 gatewayTproxy/gatewayPf 在 Windows 上恒为 false。
// lwIP 被移除之前，NetStack.cpp 是三端都编的（lwIP 是可移植 C）；移除之后它依赖 Npcap 与
// Rust 静态库，只在 WIN32 编。于是非 Windows 需要一个桩，否则 LanGateway_linux.cpp 里那句
// `m_net = new NetStack(...)` 链接不过。
//
// ── 为什么是"桩"而不是给 LanGateway 到处加 #ifdef ────────────────────────────────
// LanGateway_linux.cpp 里有二十来处 `if (m_net)`（tproxy/pf 模式下 m_net 本来就恒为空，
// 那些判空早就写好了）。逐处加平台宏 = 二十来个新的分支，而**开发机是 Windows，编不了也
// 验不了 Linux/mac**，只能等 CI 回红灯。桩把改动收敛成一个文件、零调用点改动：
// init() 返回 false，上层现成的错误路径原样接住。
//
// ── 失败语义：直接报错，不降级 ──────────────────────────────────────────────────
// 用户已确认（2026-08-04）：移除 lwIP 后 Linux/macOS 不再有用户态兜底 —— TPROXY/pf 装不上
// 就是"网关不可用"，报错即可。所以这里**故意**不做任何事，只给一句能落到 UI 上的说明。
// 走到这里本身就说明上层逻辑有问题：LanGateway 只在 !tproxy && !pf 时才建 NetStack，
// 而那两条在各自平台上都应该是可用的。

#include "NetStack.h"

// setOutboundFactory 取得所有权 ⇒ 桩要 delete 它 ⇒ 需要**完整类型**（NetStack.h 里只是前向声明）。
#include "IOutbound.h"

#include <QHostAddress>

// PIMPL 的完整定义：析构里 `delete d` 需要完整类型。桩不持有任何状态。
struct NetStack::Impl {
};

NetStack::NetStack(quint16 socksPort, QObject *parent)
    : QObject(parent), d(new Impl)
{
    Q_UNUSED(socksPort);
}

NetStack::~NetStack()
{
    delete d;
}

bool NetStack::init(QString *err)
{
    if (err) {
#if defined(Q_OS_MACOS)
        *err = QStringLiteral("本平台的网关数据面是 pf rdr，没有用户态协议栈可用"
                              "（pf 规则装不上时网关不可用）");
#elif defined(Q_OS_LINUX)
        *err = QStringLiteral("本平台的网关数据面是 TPROXY，没有用户态协议栈可用"
                              "（nftables/策略路由装不上时网关不可用）");
#else
        *err = QStringLiteral("本平台不支持透明网关");
#endif
    }
    return false;
}

// init() 恒失败 ⇒ 下面这些在生产里一个都到不了。仍然给出定义（而不是留成未定义符号），
// 是为了让"谁不小心绕过 init 直接调"表现为一次无害的空操作，而不是链接错误或崩溃。
bool NetStack::addNic(IL2Endpoint *ep, const QByteArray &localMac6, const QString &localIp,
                      const QString &netmask, QString *err)
{
    Q_UNUSED(ep);
    Q_UNUSED(localMac6);
    Q_UNUSED(localIp);
    Q_UNUSED(netmask);
    if (err)
        *err = QStringLiteral("本平台没有用户态协议栈");
    return false;
}

void NetStack::removeNic(IL2Endpoint *ep)
{
    Q_UNUSED(ep);
}

bool NetStack::hasNic(IL2Endpoint *ep) const
{
    Q_UNUSED(ep);
    return false;
}

void NetStack::addDevice(const QString &ip, const QByteArray &mac6, const QString &socksUser,
                         bool reject)
{
    Q_UNUSED(ip);
    Q_UNUSED(mac6);
    Q_UNUSED(socksUser);
    Q_UNUSED(reject);
}

void NetStack::removeDevice(const QString &ip)
{
    Q_UNUSED(ip);
}

void NetStack::addDeviceV6(IL2Endpoint *from, const QString &ip6, const QByteArray &mac6,
                           const QString &socksUser, bool reject)
{
    Q_UNUSED(from);
    Q_UNUSED(ip6);
    Q_UNUSED(mac6);
    Q_UNUSED(socksUser);
    Q_UNUSED(reject);
}

void NetStack::removeDeviceV6(const QString &ip6)
{
    Q_UNUSED(ip6);
}

const char *NetStack::activeTcpStack() const
{
    return "none";
}

void NetStack::inputFrame(IL2Endpoint *from, const QByteArray &frame)
{
    Q_UNUSED(from);
    Q_UNUSED(frame);
}

// ———— CoastCore 时期新增的三个 setter ————
//
// ★ 它们在 NetStack.h 里是**公开**接口，而调用点（LanGateway_linux.cpp 的 applyCoastCoreLocal、
//   LocalTunService.cpp 的 start）是**三端都编**的，所以桩必须给出定义 —— 否则非 Windows 在
//   **链接期**炸一串 undefined reference。2026-08-04 的 CI 红灯正是这个：阶段 2a/3a/3b
//   （83a84d9 / 637ee33 / 1daaabd）往 NetStack.h 加了 setter、只在 Windows 的 NetStack.cpp 里
//   实现，桩没跟上，于是 Linux x64/arm64 与 macOS-Qt 三个 job 一起红，而 Windows 两个照常绿
//   —— 开发机是 Windows，本地怎么编都发现不了，只能等 CI。
//
// ⚠️ 往 NetStack.h 加公开方法时，**这个文件必须同步**。这是桩这种写法的固有维护义务：
//   它换来了「LanGateway 里二十来处 if (m_net) 一处都不用改」，代价就是这条纪律。
void NetStack::setOutboundFactory(OutboundFactory *f)
{
    // ★ **必须 delete，不能只是忽略**：这个 setter 的契约是「取得所有权，旧工厂内部 delete」
    //   （见 NetStack.h 的声明处），调用方据此**不会**再删 —— LocalTunService.cpp:655 明确写着
    //   「这里绝不能再 delete m_factory」。桩若把参数丢掉，每次换出站就漏一个工厂对象。
    //   直接删掉它，语义上等价于「装进来又立刻被销毁」：本平台没有栈可以驱动它，本来也用不上。
    //   OutboundFactory 有虚析构（IOutbound.h），经基类指针删是安全的。
    delete f;
}

void NetStack::setDnsLearner(std::shared_ptr<DnsResolver> learner)
{
    // 按值收下，出作用域自行释放（shared_ptr 的删除器在构造处就已类型擦除，
    // 这里 DnsResolver 不完整也没关系）。
    Q_UNUSED(learner);
}

void NetStack::setLocalDnsEnabled(bool on)
{
    Q_UNUSED(on);
}

// 私有的 UDP/DNS 那几个方法**故意不定义**：它们只被 NetStack.cpp 内部调用，
// 桩里没有任何调用点，未定义也不会产生未解析符号。
