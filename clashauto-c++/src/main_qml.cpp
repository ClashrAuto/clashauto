// Qt Quick (QML) 前端入口。复用既有 C++ 后端（AppConfig/CoreController/ClashService/
// SubscriptionStore/TrayController），仅通过 QmlBridge 这层薄胶水暴露给 QML。
// 用 QApplication（而非 QGuiApplication）：TrayController/QSystemTrayIcon 依赖 QtWidgets。
//
// 启动行为：正式版**默认自动拉起核心**（对齐旧 Widgets 版 MainWindow 的 startCore-on-launch）。
// 本地测 UI 时设环境变量 COAST_NO_AUTOSTART=1 可跳过自动起核心——本机常已有一个实例在跑，
// 避免端口(9090)/系统代理/TUN 冲突。ClashService::start() 只是只读轮询 REST API，始终安全。
#include "AppConfig.h"
#include "VersionManifest.h"
#include "ClashService.h"
#include "CoreController.h"
#include "DeviceStore.h"
#include "HistoryStore.h" // 上网历史库（SQLite）
#include "StaticDepsSelfTest.h" // COAST_STATICDEPS_SELFTEST：插件有没有真被链进/部署进来
#include "LanScanner.h"   // COAST_SCAN_SELFTEST 的扫描耗时自检
#include "net/TproxyRules.h" // COAST_TPROXY_SELFTEST 的规则层自测
#include "net/PfRules.h"
#include "net/core/SelfRouteGuard.h" // COAST_SELFROUTE_SELFTEST
#include "net/LocalTunService.h"      // COAST_TUNSERVICE_SELFTEST / COAST_OUTBOUND_PROBE
#include "net/core/RuleEngine.h"          // COAST_RULE_SELFTEST
#include "net/core/ProxyConfigBuilder.h"  // COAST_PROXYCFG_SELFTEST
#ifdef COAST_HAVE_OPENSSL
#  include "net/core/crypto/CryptoSelfTest.h" // COAST_CRYPTO_SELFTEST（需 OpenSSL）
#endif      // COAST_PF_SELFTEST 的 pf 规则层自测（macOS）
#include "LatencyProbe.h" // 状态页延迟卡：直连/路由/DNS/代理四个数
#include "SingleInstance.h" // 单实例守卫：两个实例并存会互相清掉网关的 nft/pf 规则
#include "SubscriptionStore.h"
#include "TrayController.h"
#if defined(Q_OS_MACOS)
#include "MacWindow.h"
#endif
#include "qml/QmlBridge.h"
#include "qml/I18n.h"
#include "qml/SubscriptionsController.h"
#include "qml/LogModel.h"
#include "qml/AboutController.h"
#include "qml/SettingsController.h"
#include "qml/UpdateController.h"
#include "qml/DevicesController.h"
#include "qml/NpcapInstaller.h"
#include "PowerWatcher.h" // 睡眠/唤醒：挂起前撤劫持还原 ARP，醒来补回
#include "net/GatewayDiag.h"
#include "net/GatewayPanic.h" // 崩溃兜底：进程被打死时裸发还原 ARP
#include "net/LanGateway.h"
// 不加平台守卫：RA 解析自测(runNdpRaSelfTest)现在所有编网关的平台都有，含 Windows。
// 头里另一个声明 runGatewaySelfTest() 只在 POSIX 有定义，但**声明**不需要守卫——
// 只要调用点带守卫就不会产生未定义引用。
#include "net/GatewaySelfTest.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

#include <cstdio>

#if defined(Q_OS_UNIX)
#include <QSocketNotifier>
#include <csignal>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(Q_OS_WIN)
#include <QAbstractNativeEventFilter>
#include <functional>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
// —————————————— 关机/注销时的可靠清理（Windows 上 aboutToQuit 的补强）——————————————
// Unix 有上面那套 SIGTERM 自管道兜底，Windows 什么都没有：全靠 Qt 在收到 **WM_ENDSESSION** 时
// 同步 emit aboutToQuit。可那已经是「所有程序都同意关机之后」的最后一刻，系统随时可能直接把进程
// 干掉；漏一次的代价很实在：
//   · LanGateway::disableAll 没跑 → 被代理设备的网关 ARP 一直指着本机 MAC，而本机重启后既没在
//     转发、也没人还原 → 那台设备**彻底断网（连直连都断）**，只能等两侧邻居缓存老化；
//   · stopCore 没跑 → WinINET 系统代理留在 127.0.0.1:7890，重启后内核起来之前整机都连不上。
// 所以把清理提前到 **WM_QUERYENDSESSION**（系统征询阶段，早于 WM_ENDSESSION，有几秒预算）。
// 分两档，因为征询阶段的关机**有可能被别的程序取消**：
//   early —— 还原 ARP/NDP + 落盘。全都能自愈：万一关机取消了，下一轮扫描 resumeProxies 会把
//            劫持重新上回去，落盘更是无害。
//   late  —— 停核心 + 还原系统代理。只在 WM_ENDSESSION(wParam=TRUE)「确定要关了」时才做，免得
//            关机被取消后用户的代理莫名其妙没了。
// 两档都幂等，之后 Qt 的 aboutToQuit 链再跑一遍无害。返回 false = 不拦截，交回 Qt 默认处理。
class WinSessionEndFilter final : public QAbstractNativeEventFilter
{
public:
    WinSessionEndFilter(std::function<void()> early, std::function<void()> late)
        : m_early(std::move(early)), m_late(std::move(late))
    {
    }

    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override
    {
        const MSG *msg = static_cast<const MSG *>(message);
        if (!msg)
            return false;
        if (msg->message == WM_QUERYENDSESSION) {
            runEarly();
        } else if (msg->message == WM_ENDSESSION && msg->wParam) {
            runEarly();
            if (!m_lateDone) {
                m_lateDone = true;
                // 纯 ASCII：Windows 控制台/重定向文件是 GBK，中文会变成乱码（实测）。
                std::fprintf(stderr, "[SESSION] WM_ENDSESSION: stop core + restore system proxy\n");
                std::fflush(stderr);
                m_late();
            }
        }
        return false;
    }

private:
    void runEarly()
    {
        if (m_earlyDone)
            return;
        m_earlyDone = true;
        // 一行日志，一次会话最多两行：现场排查「关机那次到底还原没还原」时这就是唯一凭据。
        std::fprintf(stderr, "[SESSION] WM_QUERYENDSESSION: restore device ARP + flush stores\n");
        std::fflush(stderr);
        m_early();
    }
    std::function<void()> m_early, m_late;
    bool m_earlyDone = false, m_lateDone = false;
};
} // namespace
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // 渲染后端：走 Qt 默认的 RHI（Windows=D3D11 / macOS=Metal / Linux=OpenGL），**不再强制软件后端**。
    //
    // 曾经强制 QSGRendererInterface::Software（理由：UI 纯 2D 无 ShaderEffect、无 GPU 机器更轻、
    // 打包能省 opengl32sw/d3dcompiler ~24MB）。但软件后端只能走 QPainter 光栅化，代价是：
    //  - 圆角/圆点/开关这类小圆弧边缘只有 1 像素硬跳变（实测覆盖率 4→实心），肉眼就是毛边/锯齿；
    //    RHI 下同一个圆点是 2 像素渐变（2→7→实心）。且 antialiasing/layer.smooth/layer.textureSize
    //    超采样在软件后端全部无效（实测逐像素完全相同），QML 层面无解。
    //  - 文字只能走原生渲染，拿不到距离场渲染那种平滑字形。
    // 无 GPU 的机器不用担心：Windows 的 D3D11 有系统自带 WARP 兜底（本项目的无显卡 QEMU 虚机实测
    // 可跑），Linux 侧 .deb 已 Depends libgl1/libopengl0/libegl1（Mesa llvmpipe 软件 GL）。
    // 真遇到起不来的环境，仍可用环境变量退回：QT_QUICK_BACKEND=software（不再被代码覆盖）。
    //
    // 插件自证（COAST_STATICDEPS_SELFTEST=1）：TLS 后端 / QSQLITE / svg·ico·png / QML 模块
    // 是不是真的在这个二进制里。静态 Qt 下它们靠链接期导入，漏了**不报任何编译或链接错误**；
    // 共享 Qt 下则验部署有没有把插件带全。两种构建都要跑，所以不加 #ifdef。
    // ★ 放在单实例守卫**之前** —— 守卫遇到已有实例会 return 0，和"自测通过"撞车。
    if (qEnvironmentVariableIsSet("COAST_STATICDEPS_SELFTEST"))
        return runStaticDepsSelfTest();
#ifdef COAST_HAVE_RUST_STACK
    // Rust(smoltcp) 数据面的**链接 + ABI 自证**（COAST_RUSTSTACK_SELFTEST=1）。
    // 纯 FFI 往返，不碰网络/不需要 root/毫秒级。存在的意义：Phase 1 的库运行时还没接进
    // NetStack，没有这条的话「库编出来了」和「库真能被调用、ABI 对得上」在 CI 绿灯里分不清。
    //
    // ★ 必须放在**单实例守卫之前**。守卫在本机已有实例时会直接 return 0，
    //   而"退出码 0"恰好和"自测通过"撞车 —— 我就是这么被骗了一次：断言故意改坏也返回 0。
    if (qEnvironmentVariableIsSet("COAST_RUSTSTACK_SELFTEST"))
        return runRustStackSelfTest();
    // NetStack 级：整条 smoltcp 路径（要建 NetStack + 假 SOCKS，仍不碰真网卡/不需要 root）
    if (qEnvironmentVariableIsSet("COAST_SMOLGW_SELFTEST"))
        return runSmolGatewaySelfTest();
    // 每卡入站的**接线**自测：装全局工厂 + 每卡专属口，断言拨的是后者。
    // 见 GatewaySelfTest.h —— 两个真机 bug 都在这条路上，而当时既有自测全绿。
    if (qEnvironmentVariableIsSet("COAST_NICWIRING_SELFTEST"))
        return runNicWiringSelfTest();
    if (qEnvironmentVariableIsSet("COAST_SOCKSUDPUSER_SELFTEST"))
        return runSocksUdpUserSelfTest();
    if (qEnvironmentVariableIsSet("COAST_TCPREAP_SELFTEST"))
        return runTcpReapSelfTest();
    // 软件路径吞吐基准（去掉网卡，量纯软件成本）。放在单实例守卫**之前** ——
    // 自测/基准一旦跑在守卫之后，遇到已有实例会走 notifyExistingAndQuit 返回 0，
    // 于是"断言坏了"也照样绿（这个坑本仓库踩过）。
    if (qEnvironmentVariableIsSet("COAST_GW_THROUGHPUT"))
        return runGatewayThroughputBench();
    // CoastCore 进程内出站端到端自测：证明那五个移植阶段的代码真的会被执行。
    if (qEnvironmentVariableIsSet("COAST_GW_COASTCORE_SELFTEST"))
        return runCoastCoreOutboundSelfTest();
    // 自身流量排除的自测（随 CoastCore 引擎一起移植进来）。
    // ★ 必须在这里注册：不注册的话设了这个 env 只是把 GUI 正常启动，进程不退出 ——
    //   自测"挂住"而不是"失败"，比失败更难查。
    // ★ 移植进来的自测钩子**必须逐个注册**。不注册的后果不是"跑不了"，而是
    //   「设了 env 只是把 GUI 正常启动、进程永不退出」—— 自测挂住而不是失败，比失败难查得多。
    //   这个坑本轮踩了两次（SELFROUTE、CRYPTO），所以这里一次把已编入的全部接上。
#ifdef COAST_HAVE_OPENSSL
    // 加密层已知答案测试：AEAD(AES-GCM/ChaCha20-Poly1305) + HKDF。缺 OpenSSL 时这块根本没编入。
    if (qEnvironmentVariableIsSet("COAST_CRYPTO_SELFTEST"))
        return coastcore::runCryptoSelfTest();
#endif
    // 分流规则匹配自测（纯逻辑，不需要网络/节点）
    if (qEnvironmentVariableIsSet("COAST_RULE_SELFTEST"))
        return RuleEngine::selfTest() ? 0 : 1;
    // proxies YAML → ProxyNode 解析自测
    if (qEnvironmentVariableIsSet("COAST_PROXYCFG_SELFTEST"))
        return coastcore::proxyConfigSelfTest() ? 0 : 1;

    // 进程内 TUN 的两个钩子。★ 注册它们不代表默认启用 —— TUN 只有点「增强」/自测才会跑，
    //   而 selfTest 会**真的接管默认路由**，只能在容器/虚机那种可牺牲网络的地方跑，且要 root。
    //   （不注册的话设了 env 只是把 GUI 启动、进程不退出 —— 本轮已经栽过三次，不再犯。）
    if (qEnvironmentVariableIsSet("COAST_TUNSERVICE_SELFTEST"))
        return LocalTunService::selfTest();
    // 出站探针：**不碰 TUN**，只验「这个节点经进程内出站通不通」。
    // 把前提和组合分开测 —— 两者混在一起时一个 000 读不出任何结论。
    if (qEnvironmentVariableIsSet("COAST_OUTBOUND_PROBE"))
        return LocalTunService::outboundProbe();

    if (qEnvironmentVariableIsSet("COAST_SELFROUTE_SELFTEST")) {
        QString report;
        const bool ok = SelfRouteGuard::selfTest(&report);
        std::fprintf(stderr, "%s\n", qUtf8Printable(report));
        return ok ? 0 : 1;
    }
    // 真网卡版：真 Npcap + 真机帧（需管理员）。不投毒，靠靶机静态路由导流。
    if (qEnvironmentVariableIsSet("COAST_SMOLGW_REALNIC_SELFTEST"))
        return runSmolGatewayRealNicSelfTest();
#endif

    // 系统 ARP 表解析自测（纯文本，不碰网络、不需要权限、毫秒级）。
    // ★ 必须放在 `#ifdef COAST_HAVE_RUST_STACK` 之**外**：那个宏只在 Windows + COAST_RUST=ON
    //   时定义，放进去等于在 Linux/mac 上「设了 env 只会把 GUI 启动、进程不退出」——本文件
    //   上面那条注释已经为同一个坑记过三次，这里不再犯第四次。而这个自测恰恰要在 Linux CI 上
    //   跑（Windows 那种输出形态的解析器现在也是运行期分派的，Linux 一样覆盖得到）。
    if (qEnvironmentVariableIsSet("COAST_ARPPARSE_SELFTEST"))
        return LanScanner::runArpParseSelfTest() ? 0 : 1;

    // 「每网卡出口」的配置生成自测（造临时台账 + 真跑一遍 ensureFullConfig，对产物做断言）。
    // 同样放在 Rust 守卫之外：它要在 Linux CI 上跑，而且产物路径会打出来供 `core -t -f` 复核。
    if (qEnvironmentVariableIsSet("COAST_TIDE_SELFTEST"))
        return ConfigBuilder::runTideSelfTest() ? 0 : 1;
    // 集成内核落位：唯一能挡住「全新安装打不开」的守卫。开发机永远碰不到那个场景
    // （userDir/command/core 早就下载过了），所以必须由自测在 CI 上盯着。
    if (qEnvironmentVariableIsSet("COAST_BUNDLEDCORE_SELFTEST"))
        return CoreController::runBundledCoreSelfTest() ? 0 : 1;
    if (qEnvironmentVariableIsSet("COAST_NICEGRESS_SELFTEST"))
        return ConfigBuilder::runNicEgressSelfTest() ? 0 : 1;

    // 实时拓扑转储（COAST_TOPO_DUMP=1）：不是自测，是**真机排查的第一步** —— 把「这台机器上
    // 我们究竟看到了什么」（谁是主网卡、每张卡的网关/网关 MAC、ARP 表分接口的样子、跨接口
    // 有没有同 IP 不同 MAC）打出来。自测能验解析器，验不了这台机器上的解析结果。
    // 版本清单的适配自测（清单 → GitHub releases 形状）。翻错的表现是「更新页一片空白」
    // 或者「挑到别的产品线的包」，两者都不会在编译期暴露。
    if (qEnvironmentVariableIsSet("COAST_MANIFEST_SELFTEST")) {
        return VersionManifest::runSelfTest() ? 0 : 1;
    }

    if (qEnvironmentVariableIsSet("COAST_TOPO_DUMP")) {
        auto *scanner = new LanScanner(&app);
        scanner->probeTopology([](const QString &report) {
            std::fputs(qUtf8Printable(report), stdout);
            std::fflush(stdout);
            qApp->exit(0);
        });
        // 兜底：读 ARP 表那步若整个回调链断了，别把进程挂死在这里。
        QTimer::singleShot(15000, qApp, [] {
            std::fputs("拓扑转储超时（读 ARP 表没回来）\n", stdout);
            qApp->exit(2);
        });
        return app.exec();
    }

    // 透明网关 headless 自测（Linux + COAST_GATEWAY_SELFTEST）：不建 GUI，跑 TAP+NetStack+假SOCKS
    // 后退出（配合 validate/gateway_selftest.sh）。用 offscreen 平台即可（无显示环境）。
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    // 数据面自测要 TAP(Linux)/feth+BPF(mac)，只有 POSIX 有。
    if (qEnvironmentVariableIsSet("COAST_GATEWAY_SELFTEST"))
        return runGatewaySelfTest();
#endif
    // RA 解析自测：纯字节解析，不碰网络、不需要 root、毫秒级。单列一个钩子是因为「从 RA 学 v6
    // 路由器」这条路在没有 IPv6 的网络上永远跑不到（本项目的测试台就是如此），没有它等于零覆盖。
    // ★ 这个钩子**不再限于 POSIX**：NdpSpoofer 在 Windows 上同样编进产物，以前却连这一个
    //   纯解析自测都跑不了（钩子被上面那个守卫罩住了）。见 GatewaySelfTest.cpp 里的说明。
    // ★★ 位置从守卫**之后**挪到了这里。原因：lwIP 移除后，Linux 的发布门禁
    //    （validate/gateway_selftest.sh）只剩这一条 —— 它要是能静默"通过"，那条门禁就等于没有。
    //    挪上来是安全的：本函数不碰内核状态、不建 GUI、不与运行中的实例争任何资源。
    if (qEnvironmentVariableIsSet("COAST_NDP_RA_SELFTEST"))
        return runNdpRaSelfTest();

    // ★★ 单实例守卫**必须在 removeStale() 之前**：下一行会按固定表名清理内核里的陈旧规则，
    //    而它分不清那是"上次崩溃的残留"还是"另一个活着的实例正在用的"。真机复现过：第二个
    //    实例起来又退出，把生产实例的 nft 表/劫持/策略路由全清空，生产实例却毫不知情地继续
    //    显示"网关已开启"，被代理设备静默退回直连。详见 SingleInstance.h。
    //    豁免 --tun-elevated：Windows 开增强时以管理员重启自身，旧的非提权实例尚未退干净，
    //    走探测会连上它并把自己当成二次启动退出 —— 增强就永远开不起来。
    if (!app.arguments().contains(QStringLiteral("--tun-elevated"))
        && SingleInstance::notifyExistingAndQuit()) {
        // ★ 剩下的 TPROXY / pf 规则层自测**必须**留在守卫之后：它们会往内核里真装 nft/pf 规则，
        //   与正在跑的生产实例并存是危险的。但"守卫命中就 return 0"和"自测通过"撞车 ——
        //   于是在这里把它区分开：请求了自测却被守卫挡下，一律报失败。
        //   这个坑真骗过我一次（断言故意改坏，退出码仍是 0），别再让它重演。
        for (const char *hook : {"COAST_TPROXY_SELFTEST", "COAST_PF_SELFTEST"}) {
            if (qEnvironmentVariableIsSet(hook)) {
                std::fprintf(stderr,
                             "SELFTEST: %s 被单实例守卫挡下（本机已有 Coast 在跑）——"
                             "报失败而不是静默返回 0。请先退出那个实例。\n",
                             hook);
                return 3;
            }
        }
        return 0;
    }
    SingleInstance singleInstance;

    // 透明网关**规则层**自测（COAST_TPROXY_SELFTEST=1）：装 nft/策略路由 → 核对 → 增删设备
    // → 拆 → 核对拆干净。不需要真设备、不改路由决策（设备集合为空时规则一条都不命中）。
    // 需要 root。与下面几个 env 自检钩子同一惯例。
    // 启动即清一遍 TPROXY 的陈旧内核规则：上一次进程若是被 kill -9 打死的,nft 表/ip rule/路由
    // 还留在内核里,而"规则在、没进程接管"= 被覆盖的设备**完全断网**。幂等,没装过也安全。
    // 放在最前面：任何后续初始化失败都不该让用户卡在断网状态。
    TproxyRules::removeStale();

    if (qEnvironmentVariableIsSet("COAST_TPROXY_SELFTEST"))
        return runTproxyRulesSelfTest();

    // pf 规则层自测（COAST_PF_SELFTEST=1，macOS）：与上面的 TPROXY 自测同一套约定 ——
    // 装载 → 核对规则与挂载点 → 增删设备 → 拆除 → 核对拆干净。需要 root。
    // 不产生真实流量、不接管任何设备（表里只放 192.0.2.0/24，rdr 绑 lo0），跑在正用的机器上也安全。
    if (qEnvironmentVariableIsSet("COAST_PF_SELFTEST"))
        return runPfRulesSelfTest();

    // 用可定制的 Basic 样式：macOS 原生 Quick 样式不允许自定义控件 background（会报
    // "current style does not support customization"），本 app 全是自绘控件，必须 Basic。
    QQuickStyle::setStyle("Basic");
    QApplication::setApplicationName("Coast");
    QApplication::setOrganizationName("Coast");
#ifndef Q_OS_MACOS
    // mac 的 Dock/程序坞图标由 .app 包内的 icon.icns 提供（含 macOS 规范留白，尺寸与其它 App 一致）；
    // 若在此 setWindowIcon(全出血的 icon.ico) 会覆盖 Dock 图标、显得比别的图标大一圈，故 mac 不设。
    QApplication::setWindowIcon(QIcon(":/assets/icon.ico"));
#endif
    // 关闭主窗口不再退出程序：✕ 只隐藏窗口（mac 恒隐藏、留 Dock；Win/Linux 按「关闭到托盘」决定，
    // 见 Main.qml onClosing）。真正退出走托盘/菜单栏「退出程序」或 mac 的 Cmd+Q。
    QApplication::setQuitOnLastWindowClosed(false);
    QFontDatabase::addApplicationFont(":/assets/iconfont.ttf");   // family "iconfont"（logo/流量卡图标）
    QFontDatabase::addApplicationFont(":/assets/remixicon.ttf"); // family "remixicon"（Remix Icon 通用 UI 图标集，Apache-2.0）

    // —— 应用字体 ——
    // 全 UI 统一用 MiSans（不再用等宽字体）。**正文/标题一律不加粗**，层级只靠字号/颜色区分；
    // 唯一例外是「图标内的字母」（侧栏 logo 角标、托盘图标角标）——那里只有 20~30px，
    // 不加粗看不清。Semibold 就是给这些例外用的真实半粗体（不走合成粗体，避免发虚）。
    QFontDatabase::addApplicationFont(":/assets/fonts/MiSans-Regular.ttf");  // family "MiSans"
    QFontDatabase::addApplicationFont(":/assets/fonts/MiSans-Semibold.ttf"); // 归入 "MiSans"（typo family），weight=Semibold
    // 全局默认字体设为 MiSans：所有未显式指定的属性都由 QML Text 通过 QFont resolve 继承下来
    // （family / 字重 / hinting / styleStrategy 都算），字号仍由各 QML 的 font.pixelSize 决定。
    //
    // hinting = PreferNoHinting：**别把字形吸附到像素网格**。Windows 默认是全 hinting——笔画被
    // 阈值化成硬边、中文尤其显得又锐又硬（实测边缘中间调占比仅 78%）。关掉 hinting 后是 94%，
    // 笔画回到字体设计的形状、边缘有正常的灰阶过渡。（原生文字渲染下尤其明显；换 RHI 后端后
    // 文字走距离场渲染，这两项设置一并保留，退回软件后端时依然生效。）
    // styleStrategy = PreferAntialias：补上 NoHinting 单用时笔画偏淡的问题（实心像素 352→766），
    // 小字号也强制抗锯齿，不会在某些字号退化成锯齿硬边。
    {
        QFont uiFont = app.font();
        uiFont.setFamily(QStringLiteral("MiSans"));
        uiFont.setWeight(QFont::Normal);
        uiFont.setHintingPreference(QFont::PreferNoHinting);
        uiFont.setStyleStrategy(QFont::PreferAntialias);
        app.setFont(uiFont);
    }

    // 退出自检（COAST_QUIT_SELFTEST=1）：起来 3 秒后自己走一遍退出流程（与托盘「退出程序」、
    // 自更新那条“必须退出”的路同一条）。外面量「进程多久真的没了」就能验：退出没被拦下、
    // aboutToQuit 那串清理（还原 ARP/落盘/停核心/还原系统代理）不会把退出拖住。
    // mac 上顺带打印应用菜单里绑着 ⌘Q 的是哪一项 —— Swift 端那个 bug 正是这一项被换掉了。
    // 这条路没法用单测覆盖：它要的正是一个真的应用生命周期。
    if (qEnvironmentVariableIsSet("COAST_QUIT_SELFTEST")) {
        QTimer::singleShot(3000, &app, [] {
#if defined(Q_OS_MACOS)
            qInfo().noquote() << "QUIT-SELFTEST menuItem=" + macQuitMenuItemDescription();
#endif
            qApp->quit();
        });
    }

    // 扫描耗时自检（COAST_SCAN_SELFTEST=1）：只跑一轮局域网发现，把每一版快照的时刻/设备数
    // 打到 stdout 后退出，不建 GUI、不起核心。设备页「刷新很慢」是可测的：阶段①的墙上时间
    // ≈ ceil(网段主机数 / kMaxInFlight) × kProbeTimeoutMs —— 谁再把并发调小（曾经 120→24，
    // 首版快照从 ~2.5s 拖到 ~12.7s），这里的数字立刻变难看。本项目没有单测框架，沿用
    // COAST_GATEWAY_SELFTEST / COAST_ISSUE_SELFTEST 那套 env 自检钩子的惯例。
    if (qEnvironmentVariableIsSet("COAST_SCAN_SELFTEST")) {
        auto *scanner = new LanScanner(&app);
        auto *clock = new QElapsedTimer;
        auto *snap = new int(0);
        // 顺滑度：16ms 心跳，量事件循环最长被按住多久。扫描是在 GUI 线程上跑的，这个数就是
        // 「设备页会不会卡住」的直接指标（当年一次性建 120 个 socket 实测按住 1846ms）。
        auto *beat = new QElapsedTimer;
        auto *worstStallMs = new qint64(0);
        auto *beatTimer = new QTimer(&app);
        beatTimer->setInterval(16);
        QObject::connect(beatTimer, &QTimer::timeout, &app, [beat, worstStallMs] {
            if (!beat->isValid()) {
                beat->start();
                return;
            }
            const qint64 late = beat->restart();
            if (late > *worstStallMs)
                *worstStallMs = late;
        });
        beatTimer->start();
        QObject::connect(scanner, &LanScanner::discovered, &app,
                         [clock, snap](const QVector<DeviceRecord> &devs) {
                             std::printf("[scan] snapshot#%d t=%lldms devices=%d\n", ++(*snap),
                                         static_cast<long long>(clock->elapsed()), int(devs.size()));
                             std::fflush(stdout);
                         });
        QObject::connect(scanner, &LanScanner::scanningChanged, &app,
                         [clock, worstStallMs](bool scanning) {
                             if (scanning)
                                 return;
                             std::printf("[scan] done t=%lldms worst-event-loop-stall=%lldms\n",
                                         static_cast<long long>(clock->elapsed()),
                                         static_cast<long long>(*worstStallMs));
                             std::fflush(stdout);
                             QCoreApplication::quit();
                         });
        clock->start();
        scanner->scanFull();
        QTimer::singleShot(90000, &app, &QCoreApplication::quit); // 卡死也别挂着不退
        return app.exec();
    }

    // 上网历史库自检（COAST_HISTORY_SELFTEST=1）：在临时目录建一个库，喂两拍伪造的 /connections
    // 快照（第二拍连接全消失 = 全部断开），落盘后把记录数和两个聚合查询打到 stdout 再退出。
    // 这些 SQL 字符串编译期一个字都不检查，跑错了只会安静地返回空表——必须真跑一遍。
    if (qEnvironmentVariableIsSet("COAST_HISTORY_SELFTEST")) {
        const QString dir = QDir::temp().filePath(QStringLiteral("coast-history-selftest"));
        QDir(dir).removeRecursively();
        HistoryStore h(dir, nullptr); // devices=nullptr → mac 记空串，查询用空 mac（= 全部设备）
        std::printf("[hist] driver-ok=%d db=%s\n", int(h.isOpen()),
                    h.databasePath().toUtf8().constData());
        const QByteArray snapshot = R"([
            {"id":"c1","start":"2026-07-27T00:00:00.000+08:00","upload":100,"download":900,
             "chains":["Auto"],"metadata":{"host":"example.com","destinationIP":"93.184.216.34",
             "network":"tcp","sourceIP":"127.0.0.1","process":"firefox.exe"}},
            {"id":"c2","start":"2026-07-27T00:00:01.000+08:00","upload":10,"download":40,
             "chains":["DIRECT"],"metadata":{"host":"example.com","destinationIP":"93.184.216.34",
             "network":"udp","sourceIP":"127.0.0.1"}},
            {"id":"c3","start":"2026-07-27T00:00:02.000+08:00","upload":0,"download":0,
             "chains":["DIRECT"],"metadata":{"host":"zero-bytes.example","network":"tcp"}}
        ])";
        QJsonParseError perr{};
        const QJsonArray snap = QJsonDocument::fromJson(snapshot, &perr).array();
        std::printf("[hist] parsed=%d err=%s\n", int(snap.size()),
                    perr.errorString().toUtf8().constData());
        h.observe(snap);
        h.observe(QJsonArray()); // 全部消失 = 全部断开 → 有流量的两条入库，0 字节那条应被丢弃
        h.flush(false);
        std::printf("[hist] records=%lld (expect 2: 0 字节的连接不入库)\n",
                    static_cast<long long>(h.recordCount()));
        for (const HistoryStore::DomainTotal &d : h.topDomains(QString(), 7, 5))
            std::printf("[hist] top %s = %lld bytes\n", d.host.toUtf8().constData(),
                        static_cast<long long>(d.bytes));
        for (const HistoryStore::DayTotal &d : h.dailyTraffic(QString(), 3))
            std::printf("[hist] day %s up=%lld down=%lld\n", d.day.toUtf8().constData(),
                        static_cast<long long>(d.up), static_cast<long long>(d.down));
        std::fflush(stdout);
        return 0;
    }

    // 设备台账自检（COAST_DEVICEDB_SELFTEST=1）：在临时目录里跑一遍「旧 devices.json 导入 →
    // 发现+编辑 → 存 → 重开读回 → ConfigBuilder 那条查询」，打到 stdout 后退出。
    // 台账是用户数据（别名/代理开关/累计流量），迁移把它写坏了没有任何补救，所以整条链必须真跑。
    if (qEnvironmentVariableIsSet("COAST_DEVICEDB_SELFTEST")) {
        const QString dir = QDir::temp().filePath(QStringLiteral("coast-devicedb-selftest"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QString legacy = QDir(dir).filePath(QStringLiteral("devices.json"));
        {   // ① 放一份旧版 devices.json，验证一次性导入
            QFile f(legacy);
            f.open(QIODevice::WriteOnly);
            // policyTarget 用 DIRECT：既走「global + 指定目标」这条分支，生成的规则又一定能被
            // 核心解析（指向一个订阅里才有的节点名，-t 会因为「proxy not found」失败，那是配置的
            // 引用完整性，不是本自检要验的东西）。
            f.write(R"([{"mac":"aa:bb:cc:dd:ee:01","alias":"老设备","proxyEnabled":true,
                         "policyMode":"global","policyTarget":"DIRECT","totalDown":"12345",
                         "todayDown":"999","vendor":"Acme","ip":"192.168.1.9",
                         "firstSeen":"2026-07-01T10:00:00"}])");
            f.close();
        }
        {   // ② 导入 + 新发现一台 + 用户编辑 + 存盘
            DeviceStore store(dir);
            std::printf("[devdb] 导入后设备数=%d  旧文件还在=%d  备份存在=%d\n",
                        int(store.devices().size()), int(QFile::exists(legacy)),
                        int(QFile::exists(legacy + QStringLiteral(".migrated"))));
            DeviceRecord d;
            d.mac = QStringLiteral("aa:bb:cc:dd:ee:02");
            d.ip = QStringLiteral("192.168.1.10");
            d.vendor = QStringLiteral("Foo Inc");
            d.autoName = QStringLiteral("living-room-tv");
            d.autoType = DeviceType::TvBox;
            d.online = true;
            d.lastSeen = QDateTime::currentDateTime();
            store.mergeDiscovered({d});
            store.setAlias(d.mac, QStringLiteral("客厅电视"));
            store.setProxyEnabled(d.mac, true);
            store.setPolicy(d.mac, DevicePolicyMode::Direct, QString());
            store.save();
        }
        {   // ③ 重开：验证往返（上一个 store 已析构、连接已注销）
            DeviceStore store(dir);
            for (const DeviceRecord &d : store.devices())
                std::printf("[devdb] 读回 %s 别名=%s ip=%s 厂商=%s 类型=%s 代理=%d 策略=%s "
                            "累计下行=%lld 今日下行=%lld 在线=%d\n",
                            d.mac.toUtf8().constData(), d.alias.toUtf8().constData(),
                            d.ip.toUtf8().constData(), d.vendor.toUtf8().constData(),
                            DeviceStore::typeKey(d.effectiveType()).toUtf8().constData(),
                            int(d.proxyEnabled),
                            DeviceStore::modeKey(d.policyMode).toUtf8().constData(),
                            static_cast<long long>(d.totalDown),
                            static_cast<long long>(d.todayDown), int(d.online));
        }
        // ④ ConfigBuilder 生成网关 listener / IN-USER 规则时走的就是这条查询
        for (const DeviceStore::ProxyDeviceRow &r : DeviceStore::proxiedDevices(dir))
            std::printf("[devdb] 代理中 mac=%s user=%s 策略=%s 目标=%s\n",
                        r.mac.toUtf8().constData(),
                        DeviceStore::socksUser(r.mac).toUtf8().constData(),
                        r.policyMode.toUtf8().constData(), r.policyTarget.toUtf8().constData());

        // ⑤ 真的生成一遍配置：把 configDir 指到临时目录（种子从 qrc 拷过去），产出 full.yaml。
        // 这一步把「库 → ConfigBuilder → YAML」整条链跑通；生成的文件可以再拿真核心 -t 校验，
        // 确认 coast-gateway listener 和 IN-USER 规则确实按库里的设备写出来了。
        {
            AppConfig cfg = AppConfigLoader::load();
            cfg.configDir = dir;
            cfg.userDir = dir;
            ConfigBuilder builder(cfg);
            const QString full = builder.ensureFullConfig(false, cfg.ipv6);
            QFile ff(full);
            int listeners = 0, inUserRules = 0;
            if (ff.open(QIODevice::ReadOnly)) {
                const QString text = QString::fromUtf8(ff.readAll());
                listeners = text.contains(QStringLiteral("coast-gateway")) ? 1 : 0;
                inUserRules = int(text.count(QStringLiteral("IN-USER,")));
            }
            std::printf("[devdb] 生成配置 %s  coast-gateway=%d  IN-USER 规则=%d\n",
                        full.toUtf8().constData(), listeners, inUserRules);
        }
        std::fflush(stdout);
        return 0;
    }

    // 订阅节点启停的写入自检（COAST_SUBS_SELFTEST=1）：让 `subscribe.yaml` 的**写路径**
    // 能从外部驱动。理由与设置自检同 —— 这份文件两条产品线共用，一条线写完另一条读不读得懂，
    // 只有真写一次才答得上来。`device` 表就是在这个问题上出的事（两条线用了不同的列、
    // 不同的时间戳格式），所以剩下的每一处共享状态都值得照同一把尺子过一遍。
    // 只翻第 0 个订阅的第 0 个节点的启停位，不联网、不拉取。
    if (qEnvironmentVariableIsSet("COAST_SUBS_SELFTEST")) {
        AppConfig cfg = AppConfigLoader::load();
        SubscriptionStore store(cfg, &app);
        const QVector<SubscriptionSummary> subs = store.load();
        if (subs.isEmpty()) {
            std::printf("[subs] SKIP 没有订阅\n");
            std::fflush(stdout);
            return 0;
        }
        const QVector<SubscriptionNodeSummary> nodes = store.nodes(0);
        if (nodes.isEmpty()) {
            std::printf("[subs] SKIP 第 0 个订阅没有节点\n");
            std::fflush(stdout);
            return 0;
        }
        const bool before = nodes.first().use;
        store.setNodeEnabled(0, 0, !before);
        const QVector<SubscriptionNodeSummary> after = store.nodes(0);
        const bool now = !after.isEmpty() && after.first().use;
        std::printf("[subs] 订阅0/节点0 「%s」: %s -> 写入 %s -> 读回 %s\n",
                    nodes.first().name.toUtf8().constData(),
                    before ? "true" : "false", !before ? "true" : "false",
                    now ? "true" : "false");
        std::printf("[subs] 启用节点数: %d / %d\n",
                    store.load().isEmpty() ? -1 : store.load().first().enabledNodeCount, nodes.size());
        std::printf("[subs] %s\n", now == !before ? "OK" : "FAIL 写进去又读不回来");
        std::fflush(stdout);
        return now == !before ? 0 : 1;
    }

    // 设置写入自检（COAST_SETTINGS_SELFTEST=1）：把「保存一个设置」这条路径变成
    // **可以从外部驱动**的，然后读回来看写没写进去、别的键有没有被带走。
    //
    // 存在的理由：`config.yaml` 是两条产品线共用的，一条线保存设置时**会不会把另一条线
    // 认识、自己不认识的键抹掉**，是个只有真写一次才答得上来的问题。而在 GUI 里它只能
    // 靠点开关触发 —— 实测 `System Events click at` 与 `cliclick` 合成的点击都驱动不了
    // QML 控件，于是这条路径在自动化里根本走不到。
    //
    // 走 `setClearConnections` 这个**已有的公开入口**（Q_INVOKABLE），不为测试放宽封装；
    // 它只影响「切节点时断不断旧连接」，不动网络、不动系统代理。
    if (qEnvironmentVariableIsSet("COAST_SETTINGS_SELFTEST")) {
        AppConfig cfg = AppConfigLoader::load();
        auto *core = new CoreController(cfg, &app);
        auto *clash = new ClashService(&app);
        SettingsController settings(core, clash, &app);
        const bool before = cfg.clearConnections;
        settings.setClearConnections(!before);
        const AppConfig reread = AppConfigLoader::load();
        std::printf("[settings] configDir=%s\n", cfg.configDir.toUtf8().constData());
        std::printf("[settings] clearConnections: %s -> 写入 %s -> 读回 %s\n",
                    before ? "true" : "false", !before ? "true" : "false",
                    reread.clearConnections ? "true" : "false");
        std::printf("[settings] %s\n", reread.clearConnections == !before ? "OK" : "FAIL 写进去又读不回来");
        std::fflush(stdout);
        return reread.clearConnections == !before ? 0 : 1;
    }

    // 状态页「流量构成 / 连接速览」自检（COAST_CONNSTATS_SELFTEST=1）：喂两拍伪造的 /connections
    // 快照给 QmlBridge::observeConnections，把四个输出打到 stdout 再退出，不建 GUI、不发网络请求。
    // 这块是纯算术（逐连接取增量 + 直连/代理分桶 + 两种排序），没有 UI 能验，只能这么跑一遍：
    // 第二拍必须只计**增量**，否则同一份流量每拍重复累加，总量会随挂机时间线性虚涨。
    if (qEnvironmentVariableIsSet("COAST_CONNSTATS_SELFTEST")) {
        const QString dir = QDir::temp().filePath(QStringLiteral("coast-connstats-selftest"));
        QDir(dir).removeRecursively();
        AppConfig cfg;
        cfg.userDir = dir;
        cfg.configDir = QDir(dir).filePath(QStringLiteral("config"));
        QDir().mkpath(cfg.configDir);
        auto *core = new CoreController(cfg, &app);
        auto *clash = new ClashService(&app);
        auto *subs = new SubscriptionStore(cfg, &app);
        QmlBridge bridge(&cfg, core, clash, subs, &app);
        // 台账里放一台设备：验「sourceIP → 设备名」这段（没有台账时该列退回显示 IP）。
        auto *store = new DeviceStore(cfg.configDir, &app);
        DeviceRecord dev;
        dev.mac = QStringLiteral("aa:bb:cc:dd:ee:ff");
        dev.ip = QStringLiteral("192.168.20.140");
        dev.autoName = QStringLiteral("Xiaomi-Phone");
        dev.online = true;
        store->mergeDiscovered({dev});
        bridge.setDeviceStore(store);

        auto snap = [](const char *json) {
            return QJsonDocument::fromJson(QByteArray(json)).array();
        };
        // 第一拍：代理 2 条（其中一条来自局域网设备）、直连 1 条、REJECT 1 条（两桶都不该记）
        const QJsonArray t0 = snap(R"([
            {"id":"a","start":"2026-07-28T10:00:01+08:00","chains":["HK-01","节点选择"],
             "download":1000,"upload":100,
             "metadata":{"host":"youtube.com","sourceIP":"192.168.20.140"}},
            {"id":"b","start":"2026-07-28T10:00:02+08:00","chains":["DIRECT"],
             "download":2000,"upload":200,
             "metadata":{"host":"mirrors.aliyun.com","sourceIP":"127.0.0.1"}},
            {"id":"c","start":"2026-07-28T10:00:03+08:00","chains":["JP-03"],
             "download":50,"upload":5,
             "metadata":{"host":"api.github.com","sourceIP":"127.0.0.1"}},
            {"id":"d","start":"2026-07-28T10:00:04+08:00","chains":["REJECT"],
             "download":9999,"upload":9999,
             "metadata":{"host":"ads.example.com","sourceIP":"127.0.0.1"}}
        ])");
        // 第二拍：a/b 累计值变大（只该记增量）、c 断开消失、e 新建
        const QJsonArray t1 = snap(R"([
            {"id":"a","start":"2026-07-28T10:00:01+08:00","chains":["HK-01","节点选择"],
             "download":1500,"upload":150,
             "metadata":{"host":"youtube.com","sourceIP":"192.168.20.140"}},
            {"id":"b","start":"2026-07-28T10:00:02+08:00","chains":["DIRECT"],
             "download":2200,"upload":220,
             "metadata":{"host":"mirrors.aliyun.com","sourceIP":"127.0.0.1"}},
            {"id":"d","start":"2026-07-28T10:00:04+08:00","chains":["REJECT"],
             "download":9999,"upload":9999,
             "metadata":{"host":"ads.example.com","sourceIP":"127.0.0.1"}},
            {"id":"e","start":"2026-07-28T10:00:09+08:00","chains":["SG-02"],
             "download":300,"upload":30,
             "metadata":{"host":"cdn.jsdelivr.net","sourceIP":"192.168.20.140"}}
        ])");
        auto feed = [&bridge](const QJsonArray &a) {
            QMetaObject::invokeMethod(&bridge, "observeConnections", Qt::DirectConnection,
                                      Q_ARG(QJsonArray, a));
        };
        auto dump = [&bridge](const char *tag) {
            std::printf("[connstats] %s direct=%.0f proxy=%.0f total=%s\n", tag,
                        bridge.directBytes(), bridge.proxyBytes(),
                        bridge.totalText().toUtf8().constData());
            for (const QVariant &v : bridge.recentConnections()) {
                const QVariantMap m = v.toMap();
                std::printf("[connstats]   recent %-20s dev=%-14s direct=%d\n",
                            m.value("host").toString().toUtf8().constData(),
                            m.value("device").toString().toUtf8().constData(),
                            int(m.value("direct").toBool()));
            }
            for (const QVariant &v : bridge.topConnections()) {
                const QVariantMap m = v.toMap();
                std::printf("[connstats]   top    %-20s dev=%-14s bytes=%.0f\n",
                            m.value("host").toString().toUtf8().constData(),
                            m.value("device").toString().toUtf8().constData(),
                            m.value("bytes").toDouble());
            }
        };
        feed(t0);
        dump("tick1"); // 期望 direct=2200 proxy=1155（REJECT 的 9999+9999 不计）
        feed(t1);
        dump("tick2"); // 期望 direct=2200+220 proxy=1155+550+330（只加增量；c 断开不回退）
        std::fflush(stdout);
        return 0;
    }

    // 与 Widgets 版 MainWindow 相同的后端装配（AppConfigLoader 载入配置；资源已内嵌 qrc，不再找 Clashr-Auto）。
    AppConfig config = AppConfigLoader::load();
    auto *core = new CoreController(config, &app);
    // ★ 集成内核**在任何人问「装了没」之前**先落位。
    // isCoreInstalled() 查的是 userDir/command/core，而全新安装那里是空的（集成内核在
    // 可执行文件同目录）。落位原先只写在 startCore() 里，而通往 startCore() 的每一条路
    // 都先用 isCoreInstalled() 挡一道 —— 自动拉起、开启增强、开关代理都是。
    // 于是全新安装：界面起来了，什么也没发生，旁边躺着一个好端端的内核。
    // 放在这里而不是只放在 autoStartCore 里，是因为用户可能在 600ms 自动拉起之前就点开关。
    core->seedBundledCore();
    auto *clash = new ClashService(&app);
    auto *subs = new SubscriptionStore(config, &app);
    // TrayController 用 MainWindow* 做 show/raise；QML 版无 MainWindow，传 nullptr，改用它的
    // openWindowRequested 信号重开 QML 主窗（见下方连接）。托盘图标 + 流量 + 三个 toggle 信号仍可用。
    auto *tray = new TrayController(nullptr, &app);

    clash->setEndpoint(config.host, config.uiPort);
    clash->setMixedPort(config.mixedPort);
    clash->setSecret(config.secret);
    clash->setClearConnectionsOnSwitch(config.clearConnections);

    QmlBridge bridge(&config, core, clash, subs, &app);

    // 各页面适配器（薄胶水，复用后端；堆分配交给 app 清理，避免栈对象与 QObject 父子重复析构）。
    auto *subsController = new SubscriptionsController(subs, core, &app);
    auto *logModel = new LogModel(core, clash, &app);
    auto *about = new AboutController(config, &app);
    auto *settingsCtrl = new SettingsController(core, clash, &app);
    auto *updateCtrl = new UpdateController(config, core, &app);
    // 设备页：局域网发现台账 + 透明网关 + 控制器（自持连接轮询/热更新；页面显隐时开停）。
    auto *deviceStore = new DeviceStore(config.configDir, &app);
    // 状态页的连接速览要显示「这条连接是哪台设备发起的」——把只读台账交给 bridge
    // （它先于 DeviceStore 构造，只能后置注入）。
    bridge.setDeviceStore(deviceStore);
    // 上网历史库（SQLite）：订阅 ClashService 每 2s 的连接快照，连接断开时落一条记录。
    // 复用同一次 /connections 请求，不额外发包；设备详情的「常用域名」也从它查。
    auto *history = new HistoryStore(config.configDir, deviceStore, &app);
    QObject::connect(clash, &ClashService::connectionsSnapshot, history, &HistoryStore::observe);
    // 退出前把待写缓冲 + 仍在途的长连接都落盘（aboutToQuit 时对象还活着；析构里也有一道保险）。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, history, [history] { history->flush(true); });
    // 网关数据面诊断日志：<userDir>/logs/gateway-diag.log，10s 一行、空窗口不写、4 MiB 轮转。
    // 必须在建 LanGateway 之前设好——否则第一批采样会因为没有路径被丢掉。
    // COAST_GW_DIAG=0 关闭 / COAST_GW_DIAG_MS=<ms> 改采样间隔。
    GatewayDiag::setLogDir(config.userDir);
    // 崩溃兜底：装 SIGSEGV/abort/未处理异常的处理器。必须在建 LanGateway **之前**装好——上劫持
    // 时会往里登记还原帧，登记之后随时可能崩。
    GatewayPanic::installHandlers();
    auto *lanGateway = new LanGateway(&app);
    // 启动即先还原上次异常退出遗留的 ARP 投毒（panic-restore），避免被劫持设备一直断网。
    lanGateway->recoverFromCrash();
    auto *devicesCtrl = new DevicesController(deviceStore, clash, core, lanGateway, history, &app);
    // CoastCore 进程内出站（config.yaml 的 `coastcore`，默认关 = 零行为变化）。
    // 打开后网关终结出的连接不再经回环 SOCKS 拨 mihomo，而在进程内直接出站 ——
    // 那一跳实测占网关软件成本的 65%（docs/gateway-bottleneck-audit.md 第十节）。
    devicesCtrl->setCoastCore(config.coastcore, config.coastcoreStrict, config.configDir);
    // 窗口显隐 → 停/起那些**只喂界面**的活儿。点 ✕ 是「只隐藏不销毁」，QML 场景整棵还在，
    // 不接这一条的话托盘态与开着窗口一样贵（实测两边都是 6.5%）。
    // 各家自己决定停什么：ClashService 停 1s 的 /proxies；DevicesController 把 1s 的连接聚合停掉、
    // 在线态热更新降到 60s（**不能停** —— 新设备提醒与代理自愈都挂在它上面）。
    QObject::connect(&bridge, &QmlBridge::uiVisibleChanged, clash, &ClashService::setUiActive);
    QObject::connect(&bridge, &QmlBridge::uiVisibleChanged, devicesCtrl, &DevicesController::setUiVisible);
    // Npcap 安装引导（Windows 专用；其它平台 supported()=false，设备页那条提示条不显示）。
    // 状态页「今日流量」卡的数据源（小时柱 + 进程/设备/域名 Top5）——同样是后置注入。
    bridge.setHistoryStore(history);
    // 状态页「延迟」卡：直连 / 到路由 / DNS 三项自己测（TCP 握手 RTT），到代理那项取核心的测速结果。
    // 网关 IP 跟着设备页的拓扑变化走（换网络后要重新指向新网关）。
    auto *latency = new LatencyProbe(clash, &app);
    latency->setGatewayIp(devicesCtrl->gatewayIp());
    QObject::connect(devicesCtrl, &DevicesController::topologyChanged, latency,
                     [latency, devicesCtrl] { latency->setGatewayIp(devicesCtrl->gatewayIp()); });
    auto *npcapInstaller = new NpcapInstaller(config, core, &app);
    // 装完立刻重扫一轮：onDiscovered → ensureGatewayConfigured 会重新 open 二层端点，
    // 此时 wpcap.dll 已在系统里（延迟加载，无需重启程序），gatewayReady 随之转真。
    QObject::connect(npcapInstaller, &NpcapInstaller::finished, devicesCtrl, [devicesCtrl](bool ok) {
        if (ok)
            devicesCtrl->scan();
    });

    QObject::connect(&app, &QCoreApplication::aboutToQuit, deviceStore, &DeviceStore::save);

    // COAST_GATEWAY_TESTDEV=<ip>：headless 联调钩子——启动后激活设备控制器开始扫描，一旦发现
    // 该 IP 的设备(在线、同网段、非本机)就自动给它开代理,无需 GUI 点击。仅用于 SSH/无显示环境
    // 下验证透明网关(如树莓派上代理另一台设备)。默认不触发,对正式运行零影响。
    if (qEnvironmentVariableIsSet("COAST_GATEWAY_TESTDEV")) {
        const QString testIp = qEnvironmentVariable("COAST_GATEWAY_TESTDEV");
        auto *testTimer = new QTimer(&app);
        testTimer->setInterval(2000);
        QObject::connect(testTimer, &QTimer::timeout, devicesCtrl,
                         [devicesCtrl, deviceStore, testIp]() {
                             // 持续 setActive(true):headless 下 QML 的设备页不是可见页,它的
                             // onVisibleChanged 会 setActive(false) 把连接轮询关掉;每拍重新逼开,
                             // 好让 pollConnections/aggregate 跑起来(仅联调用)。
                             devicesCtrl->setActive(true);
                             for (const DeviceRecord &d : deviceStore->devices()) {
                                 if (d.ip != testIp || d.isSelf || !d.online || d.proxyEnabled)
                                     continue;
                                 std::fprintf(stderr,
                                              "[TESTDEV] enabling proxy for %s (%s) inLan=%d\n",
                                              d.ip.toLatin1().constData(),
                                              d.mac.toLatin1().constData(), d.inLanSubnet ? 1 : 0);
                                 std::fflush(stderr);
                                 devicesCtrl->setProxyEnabled(d.mac, true);
                                 return;
                             }
                         });
        testTimer->start();
    }

    // 退出必须可靠还原所有被劫持设备的 ARP（否则设备断网）。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, lanGateway, &LanGateway::disableAll);
    // disableAll 之后再彻底拆一次：它只清空设备集合，拆不掉 TPROXY 的 nft 表 / ip rule /
    // route_localnet。这些是**内核持久状态**,进程走了还留着,而"规则在、没进程接管"= 设备断网。
    // 实测 SIGTERM 后析构函数并不会跑,所以这一条不能省（见 LanGateway::shutdown 的注释）。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, lanGateway, &LanGateway::shutdown);

    // 睡眠/唤醒：合盖睡下去时本机就不转发了，必须在挂起前把被代理设备还给真网关（否则它一直
    // 指着一台睡着的机器 = 断网），醒来再重扫拓扑补挂。直连（非队列）确保在挂起窗口内跑完。
    auto *powerWatcher = new PowerWatcher(&app);
    QObject::connect(powerWatcher, &PowerWatcher::aboutToSleep, devicesCtrl,
                     &DevicesController::handleSleep, Qt::DirectConnection);
    QObject::connect(powerWatcher, &PowerWatcher::wokeUp, devicesCtrl,
                     &DevicesController::handleWake);
    // 新设备提醒（蹭网检测）：首轮扫描后发现新设备 → 托盘气泡。
    QObject::connect(devicesCtrl, &DevicesController::newDeviceFound, tray,
                     [tray](const QString &name) {
                         tray->notify(QCoreApplication::translate("Devices", "发现新设备"), name);
                     });
    // 邻居安全监视：ArpWatch 检测到「有人代理本机 / 抢我劫持的设备」→ 托盘气泡（标题/正文已在控制器里组好）。
    QObject::connect(devicesCtrl, &DevicesController::securityAlertRaised, tray,
                     [tray](const QString &title, const QString &body) { tray->notify(title, body); });

    // 托盘 toggle → 后端（与 Widgets 版契约一致）。核心生命周期仍由用户显式触发。
    // 增强(TUN) 走 bridge.toggleTun（而非直连 core）：Windows 上开启且非提权时先弹 UAC 提权重启，
    // 与页脚「增强」开关同一入口。否则托盘开增强会绕过提权、建不了 wintun 网卡。
    QObject::connect(tray, &TrayController::toggleCoreRequested, core, &CoreController::toggleCore);
    QObject::connect(tray, &TrayController::toggleProxyRequested, core, &CoreController::toggleProxy);
    QObject::connect(tray, &TrayController::toggleTunRequested, &bridge, &QmlBridge::toggleTun);
    QObject::connect(clash, &ClashService::trafficUpdated, tray, &TrayController::setTraffic);
    QObject::connect(core, &CoreController::statusChanged, tray, &TrayController::setStatus);

    // 节点切换通知（config.note「切换通知」开时）：QmlBridge 检出活动节点变化 → 托盘气泡。
    QObject::connect(&bridge, &QmlBridge::notifyRequested, tray, &TrayController::notify);
    // 用户把「切换通知」关→开：重注册系统通知（重显托盘图标），尝试恢复失效的通知注册。
    QObject::connect(&bridge, &QmlBridge::reinitNotificationsRequested, tray,
                     &TrayController::reinitForNotifications);

    // 退出时停核心、还原系统代理（对齐 Widgets 版 aboutToQuit）。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, core, &CoreController::stopCore);

#if defined(Q_OS_WIN)
    // 关机/注销：别等 Qt 在 WM_ENDSESSION 里 emit aboutToQuit（见文件顶部 WinSessionEndFilter 的
    // 说明）。early 在 WM_QUERYENDSESSION 就跑，把「漏了会让别人断网」的还原动作做掉。
    app.installNativeEventFilter(new WinSessionEndFilter(
        [lanGateway, deviceStore, history] {
            lanGateway->disableAll(); // 同步：返回时还原 ARP/NDP 的帧已经写到网卡上
            deviceStore->save();
            history->flush(true);
        },
        [core] { core->stopCore(); }));
#endif

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("bridge", &bridge);
    engine.rootContext()->setContextProperty("nodeModel", bridge.nodeModel());
    engine.rootContext()->setContextProperty("subs", subsController);
    engine.rootContext()->setContextProperty("logModel", logModel);
    engine.rootContext()->setContextProperty("about", about);
    engine.rootContext()->setContextProperty("settings", settingsCtrl);
    engine.rootContext()->setContextProperty("updater", updateCtrl);
    engine.rootContext()->setContextProperty("devices", devicesCtrl);
    engine.rootContext()->setContextProperty("npcap", npcapInstaller);
    engine.rootContext()->setContextProperty("latency", latency);

    // 界面语言（i18n）：按 config.language 在「加载 QML 前」装好翻译器，首帧即是目标语言（zh-CN 为默认，
    // 不装翻译器 → 用中文源串；en-US 装英文表）。设置页切语言经 languageChangeRequested → 运行时 retranslate。
    auto *i18n = new I18n(&engine, &app);
    // 跟随系统语言(config.autoLanguage)开→按系统区域选；关→用手选 language。
    i18n->setLanguage(config.autoLanguage ? I18n::systemLanguage() : config.language);
    tray->retranslate(); // 托盘菜单在翻译器安装前就已构造（Qt 托盘）；装好后刷成目标语言
    QObject::connect(settingsCtrl, &SettingsController::languageChangeRequested, i18n, &I18n::setLanguage);
    QObject::connect(settingsCtrl, &SettingsController::languageChangeRequested, tray, &TrayController::retranslate);
    // 内核换完立刻重判一次侧栏的 "core" 角标。连在 C++ 这一层而不是某个 QML 页里，
    // 是因为触发点有两个（设置页的「更新内核」和更新窗内核页的「更新」），两处走的
    // 都是 `settings.updateCore()` —— 连在源头上，谁按的都一样灭。
    QObject::connect(settingsCtrl, &SettingsController::coreReplaced, about, &AboutController::checkCore);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("ClashAuto", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    // 主窗（ApplicationWindow 根即 QQuickWindow）：托盘/菜单栏「控制面板」据此重开。
    // 关闭主窗只是 hide（见 Main.qml onClosing），这里负责再把它显示出来。mac 上窗口再显示时
    // Main.qml 的 onVisibleChanged 会经 bridge.setMacDockVisible(true) 让 Dock 图标回来。
    auto *rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    auto showMainWindow = [rootWindow] {
        if (rootWindow) {
            rootWindow->show();
            rootWindow->raise();
            rootWindow->requestActivate();
        }
    };
    QObject::connect(tray, &TrayController::openWindowRequested, rootWindow, showMainWindow);

    // 第二次启动（用户又双击了一次图标）：那个进程已在 main 开头探测到本实例并退出，只把
    // 「请显示窗口」这一个请求转了过来。若不响应，用户看到的是"点了没反应"，多半会去杀进程
    // 重开——而杀进程正是留下陈旧内核规则、让被覆盖设备断网的那条路。
    singleInstance.listen(showMainWindow);

    clash->start(); // 只读轮询 REST API

#if defined(Q_OS_WIN)
    // 由页脚/托盘「增强」以管理员身份重启而来（--tun-elevated）：等旧(非提权)实例硬杀核心、
    // 释放 9090/系统代理并退出后，本(提权)实例带 TUN 冷启动核心（对齐 Widgets 版）。
    const bool tunElevated = app.arguments().contains(QStringLiteral("--tun-elevated"));
#else
    const bool tunElevated = false;
#endif

    if (tunElevated) {
        QTimer::singleShot(1000, core, [core] {
            if (core && !core->isRunning()) {
                core->setTunEnabled(true);
                core->startCore();
            }
        });
    } else if (qEnvironmentVariableIsEmpty("COAST_NO_AUTOSTART")) {
        // 正式版：启动即自动拉起核心（复刻旧版 MainWindow：有内核就起）。延时 600ms 让 UI 先就绪。
        // Windows 上若上次开着增强(TUN)而当前非提权，autoStartCore 会先按需提权重启（见 QmlBridge）。
        // 本地测 UI 时 COAST_NO_AUTOSTART=1 跳过（本机已有实例，避免端口/代理/TUN 冲突）。
        QTimer::singleShot(600, &bridge, &QmlBridge::autoStartCore);
    }

#if defined(Q_OS_UNIX)
    // 优雅退出：systemctl stop / kill / Ctrl+C 都发 SIGTERM/SIGINT。Qt 默认**不**把它们转成
    // aboutToQuit —— 于是进程被直接杀掉，上面挂在 aboutToQuit 上的清理链（LanGateway::disableAll
    // 还原被劫持设备的 ARP/NDP、CoreController::stopCore、DeviceStore/History 落盘）全都不跑：设备的
    // v4/v6 邻居缓存留在本机 MAC 上 → 断网，只能等下次启动 recoverFromCrash 才还原。对一个可无 GUI
    // 常驻（headless/服务化）的网关来说，这个还原必须在收到终止信号时就发生。
    // 用**自管道(self-pipe)**把异步信号安全地转进 Qt 事件循环：信号处理器只做一次 write（
    // async-signal-safe 的极少数操作之一），QSocketNotifier 在事件循环里读到后调 quit() → 正常
    // 走完 aboutToQuit 清理链。绝不在信号处理器里直接碰 Qt（非异步信号安全，会死锁/崩溃）。
    static int s_sigFd[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, s_sigFd) == 0) {
        auto *sn = new QSocketNotifier(s_sigFd[1], QSocketNotifier::Read, &app);
        QObject::connect(sn, &QSocketNotifier::activated, &app, [] { QCoreApplication::quit(); });
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = [](int) {
            const char c = 1;
            ssize_t r = ::write(s_sigFd[0], &c, 1); // 唯一动作：写一字节唤醒事件循环
            (void)r;
        };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        ::sigaction(SIGTERM, &sa, nullptr);
        ::sigaction(SIGINT, &sa, nullptr);
    }
#endif

    return app.exec();
}
