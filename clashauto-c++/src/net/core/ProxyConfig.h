#pragma once

// CoastCore 出站配置模型 —— 把「用哪些出站节点、当前选谁、什么分流模式」做成一份**不可变快照**，
// 让进程内的拨号路径（CoreDialerFactory / 各协议出站）随时能拿到一份自洽、不会被人从背后改掉的配置。
//
// ★ 为什么是「不可变快照 + 原子 swap = 无停顿热重载」：
//   数据面（NetStack 那一侧，可能在专用工作线程上）每建一条新连接就要读一次「当前该走哪个出站」。
//   若让它直接读一个可变的配置对象，那么「用户在设置页改了订阅/切了节点」这类写操作就必须和成百上千
//   条在途连接的读操作抢同一把锁，改配置这一下会把整条数据面卡住（stop-the-world）。
//   换成快照就没这个问题：
//     · ProxyConfig 一经构造即只读，字段再不变 —— 任意多个线程无锁并发读同一份快照都安全。
//     · 重载 = 造一份**全新**的 ProxyConfig，用一次原子指针交换把 store 里的「当前快照」换过去。
//       在途的老连接手里攥着的仍是旧快照的 shared_ptr，读得好好的，直到它们自然结束；新连接调
//       current() 拿到的已是新快照。新旧快照的生命周期各由自己的 shared_ptr 引用计数管理，最后一个
//       持有者走完时旧快照自动释放 —— 全程没有一个读者被阻塞，也没有一条在途连接被打断。
//   这正是 mihomo 热重载配置时「已建立的连接留用旧规则、新连接吃新规则」的等价物，只是搬进了本进程。
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

// 一个出站节点（一台代理服务器 / 一种协议的一份参数）。
// 现阶段 CoastCore 只用到内建的 DIRECT（type=="direct"，直连），其余协议字段先占位、留空即可，
// 后续「协议出站」单元会逐个填充（Shadowsocks / Trojan / VMess / SOCKS5…）并在拨号侧按 type 分派。
struct ProxyNode
{
    QString name; // 节点显示名 / 唯一键（DIRECT 恒为 "DIRECT"）
    QString type; // 协议类型："direct" | "ss" | "trojan" | "socks5" | "vmess" | ...（现只用 direct）

    // —— 通用连接目标（direct 用不到；协议节点才有意义）——
    QString server; // 服务器地址（域名或 IP 字面量）
    quint16 port = 0;

    // —— 常见协议参数（现全部留空，后续协议单元按 type 取用；这里先把字段位子占好，避免以后到处改结构）——
    QString password; // ss / trojan / socks5 的密码
    QString cipher;   // ss 的加密方式（如 aes-128-gcm）；vmess 的 security
    QString username; // socks5 的用户名
    QString uuid;     // vmess / vless 的 UUID
    QString sni;      // trojan / tls 的 SNI（ServerName）
    QString network;  // 传输层："tcp" | "ws" | "grpc" | ...（留空=tcp）
    QString plugin;   // ss 的插件（"obfs" | "v2ray-plugin" | ...；留空=无插件，裸 AEAD）——进程内 ss 未实现插件
    QString obfs;     // hysteria2 的包混淆（"salamander" | ...；留空=无）——进程内 QUIC 未实现，见 registerHysteria2
    QString wsPath;   // ws 传输的 path
    QString wsHost;   // ws 传输的 Host 头
    bool tls = false; // 是否叠 TLS
    bool skipCertVerify = false; // 是否跳过证书校验（自签场景）
    QString alpn;     // ALPN（逗号分隔，如 "h3" / "h2,http/1.1"）；TUIC/QUIC 与部分 TLS 传输用
    // —— REALITY / uTLS（type=vless + reality 时用；见 proto/RealityOutbound）——
    QString realityPublicKey; // REALITY 服务端公钥（base64 或 hex，32B x25519）
    QString realityShortId;   // REALITY short-id（hex，0..8B）
    QString fingerprint;      // uTLS 指纹（"chrome" 等；空=chrome）
    QString flow;             // xtls flow（如 "xtls-rprx-vision"；当前未实现，占位）

    // 是否为内建 DIRECT 节点（type=="direct"）——拨号侧据此走 DirectOutbound 而非任何协议实现。
    bool isDirect() const { return type == QStringLiteral("direct"); }

    // 构造内建 DIRECT 节点。名字恒 "DIRECT"，与 mihomo 的内建策略同名，便于规则/UI 对齐。
    static ProxyNode direct()
    {
        ProxyNode n;
        n.name = QStringLiteral("DIRECT");
        n.type = QStringLiteral("direct");
        return n;
    }
};

// 一份出站配置的**不可变快照**。构造后所有字段只读，再不改动（要改就造一份新的，见 ProxyConfigStore）。
class ProxyConfig
{
public:
    // 分流模式，语义对齐 mihomo：
    //   Rule   —— 按规则表分流（规则命中哪个策略组/节点就走哪个）
    //   Global —— 一律走 selected 指定的那个节点
    //   Direct —— 一律直连（等价于强制走内建 DIRECT）
    enum class Mode { Rule, Global, Direct };

    ProxyConfig() = default;
    ProxyConfig(QVector<ProxyNode> nodes, QString selected, Mode mode)
        : m_nodes(std::move(nodes)), m_selected(std::move(selected)), m_mode(mode)
    {
    }
    // 带「策略组 → 具体节点」映射的重载（Rule 模式要用它把规则给出的组名解析成能拨的东西）。
    ProxyConfig(QVector<ProxyNode> nodes, QString selected, Mode mode,
                QHash<QString, QString> groupToNode)
        : m_nodes(std::move(nodes)), m_selected(std::move(selected)), m_mode(mode),
          m_groupToNode(std::move(groupToNode))
    {
    }

    // —— 只读访问器 ——
    const QVector<ProxyNode> &nodes() const { return m_nodes; }
    const QString &selected() const { return m_selected; }
    Mode mode() const { return m_mode; }

    // 把「规则/选择给出的名字」解析成**能拨的东西**。
    // ★ 为什么需要：规则表里的 target 绝大多数是**策略组名**（本项目实测：`🎯 全球直连` 803 条、
    //   `🚀 节点选择` 580 条、MATCH→`🐟 漏网之鱼`），而本快照只装节点。不解析的话 Rule 模式永远
    //   nodeByName 失配 → 整类回退核心。映射由 ClashService 沿各组的 now 链走到叶子后灌进来。
    // 规则：本身就是节点名 → 原样；是已知策略组 → 返回其叶子（可能是节点名或内建 DIRECT/REJECT）；
    //       内建 DIRECT/REJECT → 原样（调用方自己决定 REJECT 怎么办）；都不是 → 空（调用方回退核心）。
    QString resolveTarget(const QString &nameOrGroup) const
    {
        if (nameOrGroup.isEmpty()) {
            return QString();
        }
        if (nameOrGroup == QStringLiteral("DIRECT") || nameOrGroup == QStringLiteral("REJECT")) {
            return nameOrGroup;
        }
        if (nodeByName(nameOrGroup)) {
            return nameOrGroup; // 已经是具体节点
        }
        const QString leaf = m_groupToNode.value(nameOrGroup);
        if (leaf.isEmpty()) {
            return QString(); // 不认识（组名未知/映射还没到）→ 让调用方回退核心
        }
        if (leaf == QStringLiteral("DIRECT") || leaf == QStringLiteral("REJECT")
            || nodeByName(leaf)) {
            return leaf;
        }
        return QString(); // 叶子既不是内建也不在节点表里（订阅刚变？）→ 回退核心
    }

    const QHash<QString, QString> &groupToNode() const { return m_groupToNode; }

    // 按名字查节点；找不到返回 nullptr（指向内部 QVector 的元素，快照不可变故指针在快照存活期内有效）。
    const ProxyNode *nodeByName(const QString &name) const
    {
        for (const ProxyNode &n : m_nodes) {
            if (n.name == name)
                return &n;
        }
        return nullptr;
    }

private:
    QVector<ProxyNode> m_nodes; // 全部可选出站节点（含内建 DIRECT）
    QString m_selected;         // 当前选中的节点名（Global 模式下的目标）
    Mode m_mode = Mode::Rule;
    // 策略组/策略名 → 沿 now 链走到底的叶子（节点名或 DIRECT/REJECT）。见 resolveTarget。
    QHash<QString, QString> m_groupToNode;
};

// 「当前生效的出站配置」的持有者 + 热重载入口。数据面读 current()，控制面调 reload()，两者靠原子换手交接。
class ProxyConfigStore : public QObject
{
    Q_OBJECT
public:
    explicit ProxyConfigStore(QObject *parent = nullptr) : QObject(parent) {}

    // 取当前快照（线程安全）。
    // ★ 同步手段：这里用一把 QMutex 保护那个 shared_ptr 成员的读写。为什么不用 std::atomic_load(&sp)：
    //   对 std::shared_ptr 的非成员 atomic_load/store 在 C++20 起被弃用（换成 std::atomic<shared_ptr>），
    //   跨编译器/标准库版本的可用性与告警状态都不统一；而这把锁只在「换一个指针 / 拷一个 shared_ptr」的
    //   极短临界区里持有，争用可忽略。返回的是 shared_ptr 的一份拷贝：调用方之后读快照内容全程无锁，
    //   即使此刻另一个线程 reload() 换掉了 store 里的指针，这份拷贝指向的旧快照也被引用计数续着命。
    std::shared_ptr<const ProxyConfig> current() const
    {
        QMutexLocker lock(&m_mutex);
        return m_current;
    }

    // 原子换上新快照，并通知观察者（UI/日志）配置已变。在途连接仍用各自手里的旧快照，新连接取新快照。
    // next 可为 null（表示「暂无配置」）——调用方自行决定是否允许，本类不加限制。
    void reload(std::shared_ptr<const ProxyConfig> next)
    {
        {
            QMutexLocker lock(&m_mutex);
            m_current = std::move(next);
        }
        emit changed();
    }

signals:
    void changed(); // 快照已被 reload 换过（UI 可据此刷新「当前节点/模式」显示）

private:
    mutable QMutex m_mutex;
    std::shared_ptr<const ProxyConfig> m_current; // 当前生效快照（初始为 null）
};
