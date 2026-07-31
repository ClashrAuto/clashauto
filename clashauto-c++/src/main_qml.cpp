// Qt Quick (QML) 前端入口。复用既有 C++ 后端（AppConfig/CoreController/ClashService/
// SubscriptionStore/TrayController），仅通过 QmlBridge 这层薄胶水暴露给 QML。
// 用 QApplication（而非 QGuiApplication）：TrayController/QSystemTrayIcon 依赖 QtWidgets。
//
// 启动行为：正式版**默认自动拉起核心**（对齐旧 Widgets 版 MainWindow 的 startCore-on-launch）。
// 本地测 UI 时设环境变量 COAST_NO_AUTOSTART=1 可跳过自动起核心——本机常已有一个实例在跑，
// 避免端口(9090)/系统代理/TUN 冲突。ClashService::start() 只是只读轮询 REST API，始终安全。
#include "AppConfig.h"
#include "ClashService.h"
#include "CoreController.h"
#include "DeviceStore.h"
#include "HistoryStore.h" // 上网历史库（SQLite）
#include "LanScanner.h"   // COAST_SCAN_SELFTEST 的扫描耗时自检
#include "LatencyProbe.h" // 状态页延迟卡：直连/路由/DNS/代理四个数
#include "SubscriptionStore.h"
#include "TrayController.h"
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
#ifdef COAST_HAVE_OPENSSL
#include "net/core/crypto/CryptoSelfTest.h" // COAST_CRYPTO_SELFTEST：AEAD/KDF 已知答案测试（需 OpenSSL）
#endif
#include "net/core/RuleEngine.h"             // COAST_RULE_SELFTEST：分流规则匹配自测
#include "net/core/ProxyConfigBuilder.h"     // COAST_PROXYCFG_SELFTEST：proxies YAML → ProxyNode 解析自测
#include "net/core/DnsMessage.h"            // COAST_DNS_SELFTEST：DNS 报文层 KAT
#include "net/core/SelfRouteGuard.h"        // COAST_SELFROUTE_SELFTEST：自身流量排除（接 TUN 的前置）
#include "net/inbound/MixedInbound.h"       // COAST_INBOUND_SELFTEST：本机 HTTP/SOCKS5 入站自测
#ifdef COAST_HAVE_QUIC
#include "net/core/proto/Hysteria2Outbound.h"   // COAST_QUIC_SELFTEST：Hy2 的 QPACK/Huffman KAT
#include "net/core/transport/QuicTransport.h"   // COAST_QUIC_SELFTEST：msquic 运行时可加载性
#endif
#include "net/LanGateway.h"
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include "net/GatewaySelfTest.h"
#endif

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
#include <sys/resource.h> // setrlimit：抬高 fd 上限（网关每条连接一个 fd）
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

#if defined(Q_OS_UNIX)
namespace {
// 把每进程可打开文件数的**软限制**抬到硬限制。
//
// ★ 为什么必须做：透明网关是**每条被代理连接占一个 fd**（出站 socket）。发行版默认软限制普遍是
//   1024，而真机压测里 tcpActive 轻松到 965 —— 于是 fd 顶死在 1024，新连接**打不开 socket**。
//   现象极具迷惑性：不是报"too many open files"，而是表现为**建连变慢+大面积失败**
//   （connMs 直方图整个塌到 ≥100ms 那一桶、74% 连接失败），很容易被误判成"网络慢"或"要上多进程"。
//   真机数据：fd 占用 763 → 977 → 1024 → 顶死不动。硬限制是 524288，抬上去即可，无需 root。
//   放在 main 最前面：任何监听/拨号之前生效。失败只警告不致命（受 cgroup/容器约束时抬不动）。
void raiseFdLimit()
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return;
    if (rl.rlim_cur >= rl.rlim_max)
        return; // 已经顶格
    const rlim_t before = rl.rlim_cur;
    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) == 0) {
        std::fprintf(stderr, "[LIMIT] fd soft limit %llu -> %llu\n",
                     (unsigned long long)before, (unsigned long long)rl.rlim_cur);
    } else {
        std::fprintf(stderr, "[LIMIT] raise fd limit failed (soft=%llu hard=%llu)\n",
                     (unsigned long long)before, (unsigned long long)rl.rlim_max);
    }
    std::fflush(stderr);
}
} // namespace
#endif

int main(int argc, char *argv[])
{
#if defined(Q_OS_UNIX)
    raiseFdLimit(); // 必须在建任何 socket 之前
#endif
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
    // 透明网关 headless 自测（Linux + COAST_GATEWAY_SELFTEST）：不建 GUI，跑 TAP+NetStack+假SOCKS
    // 后退出（配合 validate/gateway_selftest.sh）。用 offscreen 平台即可（无显示环境）。
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    if (qEnvironmentVariableIsSet("COAST_GATEWAY_SELFTEST"))
        return runGatewaySelfTest();
    // RA 解析自测：纯字节解析，不碰网络、不需要 root、毫秒级。单列一个钩子是因为「从 RA 学 v6
    // 路由器」这条路在没有 IPv6 的网络上永远跑不到（本项目的测试台就是如此），没有它等于零覆盖。
    if (qEnvironmentVariableIsSet("COAST_NDP_RA_SELFTEST"))
        return runNdpRaSelfTest();
#endif

    // 加密层已知答案测试（COAST_CRYPTO_SELFTEST=1）：跑 AEAD(AES-GCM/ChaCha20-Poly1305)、HKDF-SHA1、
    // EVP_BytesToKey 的公开测试向量，逐项 PASS/FAIL 打到 stderr，返回非零退出码表示有失败。跨平台、
    // 不建 GUI、不碰网络 —— 加密写错是静默的，唯一防线就是拿 RFC/NIST 向量逐字节比对（见 CryptoSelfTest.cpp）。
#ifdef COAST_HAVE_OPENSSL
    if (qEnvironmentVariableIsSet("COAST_CRYPTO_SELFTEST"))
        return coastcore::runCryptoSelfTest();
#endif

    // 规则引擎自测（COAST_RULE_SELFTEST=1）：DOMAIN-SUFFIX 边界 / 精确 vs 子域 / 关键字 / v4·v6 CIDR /
    // MATCH 兜底 等同步断言,全过返回 0。同样不建 GUI、不碰网络。
    if (qEnvironmentVariableIsSet("COAST_RULE_SELFTEST"))
        return RuleEngine::selfTest() ? 0 : 1;

    // proxies YAML → ProxyNode 解析自测（COAST_PROXYCFG_SELFTEST=1）：拿内置样例（ss/vmess/vless/
    // vless+reality/trojan/hysteria2/tuic，含 ws-opts/sni/reality-opts/alpn，两种缩进形状）逐字段断言，
    // 全过返回 0。纯解析、不建 GUI、不碰网络（见 net/core/ProxyConfigBuilder.cpp）。
    if (qEnvironmentVariableIsSet("COAST_PROXYCFG_SELFTEST"))
        return coastcore::proxyConfigSelfTest() ? 0 : 1;

    // 系统代理机制自检（COAST_SYSPROXY_SELFTEST=1）：读 → 设 → 读回核对 → **还原**。
    // 「把系统代理改指进程内入站」只有在真实桌面会话里才验得了（CI / 无头机都不行），而它出错
    // 的后果是本机上不了网 —— 让用户拿 GUI 去试代价太大，所以给一条几秒钟就有答案的命令。
    if (qEnvironmentVariableIsSet("COAST_SYSPROXY_SELFTEST"))
        return CoreController::systemProxySelfTest() ? 0 : 1;

    // 自身流量排除自检（COAST_SELFROUTE_SELFTEST=1）：探物理出口 → 钉住 → **读回核对** →
    // 真连一次 → **反向对照**（钉到回环口必须连不通）。
    // 反向对照是这条自检的重点：读回一致只证明选项被存下了，不证明它在导流；少了它，
    // 「绑定完全没生效」也会显示 PASS。
    if (qEnvironmentVariableIsSet("COAST_SELFROUTE_SELFTEST")) {
        QString report;
        const bool ok = SelfRouteGuard::selfTest(&report);
        fprintf(stderr, "%s\n", qUtf8Printable(report));
        return ok ? 0 : 1;
    }

    // 本机混合入站自检（COAST_INBOUND_SELFTEST=1）：SOCKS5 CONNECT / HTTP CONNECT / HTTP 绝对形式
    // 三种客户端各打一遍，验协议解析 + 双向转发 + 绝对形式被正确改写成源形式。
    // 全在进程内（自带靶服务器和直连出站桩），不需要任何节点/订阅/网络。
    if (qEnvironmentVariableIsSet("COAST_INBOUND_SELFTEST"))
        return MixedInbound::selfTest() ? 0 : 1;

    // DNS 报文层自检（COAST_DNS_SELFTEST=1）：解析设备查询 / 合成 fake-ip 应答两个方向逐字节比对。
    // 网关自己出 DNS 应答之后，报文写错的后果是「上网时好时坏」这种最难查的形态 —— 必须有向量钉住。
    if (qEnvironmentVariableIsSet("COAST_DNS_SELFTEST")) {
        QString why;
        const bool ok = coastcore::dnsSelfTest(&why);
        fprintf(stderr, "DNS message KAT : %s%s\n", ok ? "PASS" : "FAIL",
                ok ? "" : qUtf8Printable(QStringLiteral("  ") + why));
        return ok ? 0 : 1;
    }

#ifdef COAST_HAVE_QUIC
    // QUIC 自检（COAST_QUIC_SELFTEST=1）：**发布前的守门人**，回答两个只有跑起来才知道的问题：
    //   ① msquic 动态库在**这个包里**真的加载得到吗（MsQuicOpen2 + RegistrationOpen）——
    //      编译期链上 ≠ 运行期找得到。曾经就发生过「产物 NEEDED libmsquic.so.2 却没打进包」，
    //      那是个装上去起不来的包；CI 现在跑这一项，把它挡在发布之前。
    //   ② Hysteria2 认证要用的 QPACK/Huffman 解码是否仍然正确（KAT，见 Hysteria2Outbound.cpp）——
    //      那张常量表写错过一次，且错得很隐蔽（服务端说认证成功、客户端说被拒）。
    // 顺带报告 TUIC 的 keying exporter 有没有（没有属预期：需 msquic 2.6+，届时 tuic 回退内核）。
    // 不建 GUI、不碰网络；有任一失败返回非零。
    if (qEnvironmentVariableIsSet("COAST_QUIC_SELFTEST")) {
        const bool lib = coastcore::QuicTransport::libraryAvailable();
        QString why;
        const bool kat = hysteria2QpackSelfTest(&why);
        // 纯 ASCII 输出：这几行会被 CI 重定向到文件再读回来，Windows 那侧是 GBK/ANSI，
        // 中文必然变乱码（同 WM_ENDSESSION 那段的既有约定）。
        fprintf(stderr, "msquic runtime loadable : %s\n", lib ? "PASS" : "FAIL");
        fprintf(stderr, "Hy2 QPACK/Huffman KAT   : %s%s\n", kat ? "PASS" : "FAIL",
                kat ? "" : qUtf8Printable(QStringLiteral("  ") + why));
        fprintf(stderr, "TUIC keying exporter    : %s\n",
                coastcore::QuicTransport::keyingMaterialSupported()
                        ? "available"
                        : "unavailable (needs msquic 2.6+; tuic falls back to the core - expected)");
        return (lib && kat) ? 0 : 1;
    }
#endif

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
            const QString full = builder.ensureFullConfig(false);
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
    auto *devicesCtrl = new DevicesController(deviceStore, clash, core, lanGateway, history, config, &app);
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
    QObject::connect(tray, &TrayController::openWindowRequested, rootWindow, [rootWindow] {
        if (rootWindow) {
            rootWindow->show();
            rootWindow->raise();
            rootWindow->requestActivate();
        }
    });

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
