#pragma once

#include <QtGlobal>  // Q_OS_LINUX：下面 gatewayTproxy 的默认值按平台分
#include <QRect>
#include <QString>
#include <QStringList>

struct AppConfig {
    QString userDir;
    QString configDir; // userDir/config：放 config.yaml/full.yaml 等配置 yaml；logs\、Country.mmdb、cache 仍在 userDir 根
    QString host = "127.0.0.1";
    QString secret; // external-controller 访问密钥：为空时首次加载随机生成并落盘到用户 config.yaml
    int uiPort = 9191; // REST API 端口（对齐 default.yaml external-controller）；用 9191 避开原版 9090
    int mixedPort = 7890;
    int autoUpdateMinutes = 0;
    bool webProxy = true;
    bool tun = false;
    bool nodeOnlyAvailable = true;
    bool clearConnections = true;
    bool increment = false;
    bool closeToTray = false; // 关闭到托盘（config.mini）默认关：正常显示窗口 + ✕ 退出。见 AppConfigLoader::load 首次落地归一化
    bool autoStart = false;
    bool nodeSwitchNote = true;
    bool allowRuleEnabled = false;
    bool noAllowRuleEnabled = false;
    QString allowRule;
    QString noAllowRule;
    QString theme = "black";
    bool autoTheme = false;
    bool autoLanguage = true; // 跟随系统语言（默认开）：按系统区域自动选 zh-CN/en-US，忽略 language 手选；关则用 language
    // GeoIP 数据库自动更新周期（天）。0 = 关。设置页「GeoIP 数据」那一行的下拉。
    // 以前是每小时的后台 checkAll 里无条件跟着查一次（用户没有任何开关），现在由它节流。
    int geoipUpdateDays = 1;
    // 接收测试版：更新检查与「一键更新」改用 prerelease 频道。CI 对**非主分支**的每次推送都会发
    // 一个 prerelease（tag 带 -beta.<sha>），主分支才发正式版。默认关 —— 普通用户不该被卷进测试流。
    bool receiveBeta = false;
    // 启用 IPv6。默认关 —— 打开后 ConfigBuilder 才会往 full.yaml 写 ipv6/dns.ipv6/
    // dns.fake-ip-range6 三件套。注意三者缺一不可：只开 dns.ipv6 而没有 v6 fake-ip 池时，
    // fake-ip 模式下核心对 AAAA 照样回空答案（见 mihomo dns/middleware.go 的 withFakeIP）。
    bool ipv6 = false;
    // ———— CoastCore：把网关的**出站**搬进本进程（config.yaml 键 `coastcore`）————
    // 默认 **false = 零行为变化**，网关照旧经回环 SOCKS 拨 mihomo。
    // 打开后网关终结出的连接在进程内直接出站，那一跳整个消失 —— 实测它占网关软件成本的 **65%**
    //（docs/gateway-bottleneck-audit.md 第十节）。判不了的情形（协议没编进来、规则要先解析 IP）
    // 仍**回退** mihomo，按原因记账，见 GatewayDiag 的 cc=… 那几栏。
    // ★ 这里默认 false 而不是跟着上游分支取 true：v1.1 上这条路刚接完线、还没做过真机验证，
    //   默认打开等于拿所有用户当小白鼠。等真机 A/B 有数了再谈改默认值。
    bool coastcore = false;
    // 严格模式：上面那些「判不了」的情形**拒绝回退**，直接让该连接失败（config.yaml 键
    // `coastcore_strict`）。用途是把「离完全替换 mihomo 还差多少」暴露出来 —— 静默回退会把
    // 差距藏起来。代价是这些连接会断，所以单独一个开关。默认关。
    bool coastcoreStrict = false;
    // 透明网关的**数据面**走哪条路。
    //   true  = 「内核转发 + nftables TPROXY」：ARP 劫持照旧（那是「设备什么都不用改」的来源），
    //           但劫持来的帧不再喂进 lwIP，而是由内核转发、TPROXY 投给核心。
    //   false = 传统的 lwIP 用户态栈。
    // 需要 nftables + iproute2 + root。
    //
    // ★ **Linux 默认 true**（2026-08-03）。产品目标是「万兆路由/交换机级性能」，而实测证明
    //   lwIP 在架构上到不了：
    //     千兆链路满速对照（内网 iperf3 打网关上的 netns 假外网，确保真穿数据面；
    //     纯链路基准 941 Mb/s 零重传）
    //       lwIP    944 Mb/s   coast 0.90 核 + 核心 ~0      = 0.90 核 → 0.95 核/Gbps
    //       TPROXY  940 Mb/s   coast 0.06 核 + 核心 0.18 核 = 0.24 核 → 0.25 核/Gbps
    //   lwIP 跑满千兆就吃掉 0.9 个核，而它是**单线程**的（见 NetStack.h 顶部），单核封顶
    //   ≈1.05 Gbps —— 万兆要在一个线程里做约 9.5 核的活，物理上做不到，换 CPU 也没用。
    //   TPROXY 0.25 核/Gbps、万兆约 2.5 核，且内核转发天然按 CPU 队列并行 → 可行。
    //
    //   之前默认 false 的两个理由现在都不成立了：
    //   ① 「跑够久之前要可回退」—— 这几轮把正确性缺口逐个补完并真机验证过：ICMP 重定向把
    //      流量赶出代理、Docker 的 FORWARD policy DROP 吃掉内核转发、局域网隔离在 TPROXY 下
    //      失效、崩溃后 route_localnet/ip_forward/send_redirects 残留（见 TproxyRules.cpp）。
    //   ② 「TPROXY 会丢 IPv6，相对 lwIP 是功能回归」—— 上一次改动已补齐（proxied6 集合 +
    //      prerouting6 链 + ip -6 策略路由 + forward_accept 的 v6 两条 + listener 听 `::` +
    //      邻居表播种），真机验证 curl -6 / ping6 / 禁网策略在 v6 上全部正确。
    //
    //   失败仍是安全的：install 失败时 configureLocal **原地返回、保持未接管状态**，设备继续
    //   用真网关上网，而不是半开着把人打断网。
    //   ★ 回退的含义**已经变了**：lwIP 被移除后 Linux 上没有用户态兜底，
    //     `gatewayTproxy: false` / `COAST_GATEWAY_DATAPATH=userspace` 等于**关掉网关**
    //     （NetStack_stub.cpp 的 init() 直接报错）。这是明确的取舍，不是遗漏。
    //   仅 Linux 有效；Windows/macOS 这个字段恒被忽略（Windows 走用户态 smoltcp、macOS 走 pf）。
#if defined(Q_OS_LINUX)
    bool gatewayTproxy = true;
#else
    bool gatewayTproxy = false;
#endif
    // macOS 的内核态数据面：pf 的 rdr + DIOCNATLOOK（见 net/PfRules.h）。与 gatewayTproxy
    // 互斥 —— TPROXY 是 Linux netfilter 独有的，BSD 上没有等价物。
    // ★ 两者都 false 时**没有可用数据面**（用户态栈已随 lwIP 一起从非 Windows 平台移除），
    //   网关直接报错不可用。
    //
    // ★ **macOS 默认 true**：目标是「去掉用户态栈架构」，而 lwIP 在 macOS 上同样是单线程，
    //   有着与 Linux 侧实测一致的天花板（跑满千兆约 0.9 核、单核封顶 ~1 Gbps）。pf rdr 走的是
    //   内核转发，CPU 代价与 Linux 的 TPROXY 同量级。这个默认值最终让 lwIP 得以整体删除。
    // ★ 已知限制（BSD 固有，不是没写完）：**redir 只代理 TCP**。DNS(UDP53) 由 PfRules 单独
    //   rdr 到核心的 dns.listen 上，不受影响；但其余 UDP（QUIC/HTTP3 等）无法接管，会直连出去。
    //   Linux 的 TPROXY 两者都收，所以这是 macOS 独有的取舍。
    // 回退：配置键 gatewayPf: false，或环境变量 COAST_GATEWAY_DATAPATH=userspace
    //（`lwip` 仍作别名接受）。★ 同 gatewayTproxy：在 macOS 上回退 = 网关不可用，没有兜底。
#if defined(Q_OS_MACOS)
    bool gatewayPf = true;
#else
    bool gatewayPf = false;
#endif
    // 无可用代理节点时的兜底行为。默认 false = 回落直连（clash 既定语义：普通代理设备跟随
    // 规则、规则最终回落 DIRECT）。true = fail-closed：无订阅节点时把"该走代理"的流量导向
    // REJECT,避免被劫持设备在机场跑路/订阅过期时**静默裸奔**(真实 IP 泄露、翻墙失效)。
    // ★ 默认 false 是用户 2026-08-03 明确拍板的:能上网优先于防泄露,且真实用户几乎总配了节点,
    //   无节点只是配置未完成的中间态;fail-closed 会因节点抖动误伤断网。隐私优先的用户可开。
    // 配置键 noNodeReject;环境变量 COAST_NO_NODE_REJECT=1/0 覆盖(供测试/应急)。
    // 已知范围:只覆盖"订阅完全无节点"这个静态可判场景(最常见);"有节点但运行时全部测速失败
    //   回落 DIRECT"是运行时状态,本项不拦(见 ConfigBuilder 注入处的说明)。
    bool noNodeReject = false;
    QString language = "zh-CN";

    QString clashExecutable() const;
    QString clashConfig() const;

    // qrc(:/…) 复制出来的文件默认只读，补上属主读写权限，供 config.yaml/default.yaml 等种子后续编辑保存。
    static void makeWritable(const QString &path);
};

class AppConfigLoader
{
public:
    static AppConfig load();

private:
    static QString valueFromYaml(const QString &text, const QString &key, const QString &fallback);
    static QString nestedValueFromYaml(const QString &text, const QString &section, const QString &key, const QString &fallback);
    static bool boolFromYaml(const QString &text, const QString &key, bool fallback);
    static bool nestedBoolFromYaml(const QString &text, const QString &section, const QString &key, bool fallback);
    static int intFromYaml(const QString &text, const QString &key, int fallback);
};
