cmake_minimum_required(VERSION 3.20)

# C 语言用于 vendored lwIP(.c)：不启用则 CMake 会静默忽略 .c 源，链接时报一堆 lwIP 未定义符号。
# LANGUAGES 只剩 CXX：`C` 是为 vendored lwIP（124 个 .c）与 lwip_port/coast_lwip_diag.c 开的，
# 两者已随 lwIP 一起删除，工程里再没有任何 .c。少一门语言 = 少一次 CMake 的编译器探测
# （交叉编译 Windows ARM64 时尤其省事）。要加回来只需在这里补一个 C。
project(clashauto_cpp VERSION 1.0.0 LANGUAGES CXX)

# Version string shown in the app UI. Defaults to the project version for
# local builds; CI overrides it with -DAPP_VERSION=<major.minor.commitcount>.
if(NOT APP_VERSION)
    set(APP_VERSION "${PROJECT_VERSION}")
endif()

# macOS 需要 Objective-C++（.mm）来调 SMAppService / NSXPCConnection（特权 helper）+ AppKit 毛玻璃
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# QML 前端需要 Qt6（qt_add_qml_module / loadFromModule 为 Qt6 API）。
# Widgets 仍需链接：QSystemTrayIcon（TrayController）、QApplication 依赖它。
# Sql：上网历史库（SQLite，见 src/HistoryStore.h）。只用 Qt 自带的 QSQLITE 驱动，不外挂任何库；
# 打包时记得带上 plugins/sqldrivers（windeployqt 会自动跟着 Qt6::Sql 复制）。
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Widgets Network Qml Quick QuickControls2 Sql)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Qml Quick QuickControls2 Sql)

#   结果 **macOS universal 与 Windows arm64 两个 job 直接构建失败**（CI 实测）：
#     · macOS 要 `-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64` 出通用二进制，而 brew 的 libcrypto 是
#       **单架构**的 → 即便 find 得到也链不进通用二进制；
#     · Windows arm64 是交叉编译，环境里没有 arm64 的 OpenSSL，find_package 会找到**宿主 x64** 的
#       → 架构不匹配、编译过而链接炸（这一条正是「只按平台猜」的写法漏掉的，故改用下面的真链测试）。
#   降级后果**很轻**：只有「需要裸 AEAD/X25519 的三个协议」(ss / vmess / reality) 不编入、不注册，
#   拨号侧照旧回退 mihomo（功能不缺，只是不走进程内）；direct / trojan / vless 仍是进程内（它们只用
#   QSslSocket + Qt 哈希）。本地 MinGW 自带（opt/include、opt/lib），可用 -DOPENSSL_ROOT_DIR 提示。
set(COAST_HAVE_OPENSSL OFF)
# ★ Windows 上**静态链** libcrypto：动态链会引入 libcrypto-*.dll 的运行期依赖，
#   缺了就是 0xC0000135「进程直接起不来」（本地复现过；本仓库为 wpcap 也栽过同一类）。
#   静态链下来没有任何要随包发布的东西。
#
#   ⚠️ 一度以为静态不行 —— 那是**误判**，值得记下来：改成静态后加密自测"挂住"，
#   我当时读成"静态 libcrypto 有问题"。真因是下面那个真链测试**自己没链系统库**
#   （undefined reference to __imp_getnameinfo 等），于是测试失败 → COAST_HAVE_OPENSSL=OFF
#   → 加密自测被 #ifdef 编译掉 → 那个 env 只是把 GUI 正常启动、进程不退出。
#   「自测挂住」这个现象本轮出现了**三次**，每次都是同一个根因：钩子没编进去/没注册。
#   下次再看到"自测不退出"，先查它到底有没有被编进这个二进制，别急着怀疑被测的库。
if(WIN32)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
    # ★「找到」≠「能用」：交叉编译/通用构建里 find_package 很可能给出**另一个架构**的 libcrypto
    #   —— Windows arm64 交叉编译会找到宿主的 x64 库、macOS 通用构建拿到的 brew 库是单架构。
    #   这种情况下编译能过、**链接必炸**（实测 CI 上就是这么挂的）。所以别猜平台，直接**真链一次**：
    #   编译+链接一个调用 EVP 的最小程序，过了才认。这一招同时覆盖架构不匹配、通用构建、装坏了的库。
    include(CMakePushCheckState)
    include(CheckCXXSourceCompiles)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES OpenSSL::Crypto)
    # ★ 系统库要加在**这里**，不能只加给主目标：静态 libcrypto 自己会引用 getaddrinfo /
    #   CryptAcquireContext 这些，测试不链它们就必然 undefined reference —— 而那会被读成
    #   "OpenSSL 不可用"，静默降级掉三个协议。这正是上面记的那次误判的根因。
    if(WIN32)
        # z：有些发行版的静态 libcrypto 编进了压缩支持（本机 MinGW 就是），缺了就是
        #    undefined reference to `deflate` —— 同样会被读成"OpenSSL 不可用"。
        list(APPEND CMAKE_REQUIRED_LIBRARIES crypt32 ws2_32 advapi32 user32 z)
    endif()
    check_cxx_source_compiles("
        #include <openssl/evp.h>
        int main() { return EVP_aes_128_gcm() != nullptr ? 0 : 1; }
    " COAST_OPENSSL_LINKS)
    cmake_pop_check_state()
    if(COAST_OPENSSL_LINKS)
        set(COAST_HAVE_OPENSSL ON)
    else()
        message(STATUS "OpenSSL 找到了但链不上（架构不匹配/装坏）→ 按缺失处理")
    endif()
endif()
if(COAST_HAVE_OPENSSL)
    message(STATUS "OpenSSL found → CoastCore ss/vmess/reality (in-process) enabled")
else()
    message(STATUS "OpenSSL NOT usable → ss/vmess/reality fall back to mihomo (direct/trojan/vless unaffected)")
endif()

# Linux 的睡眠/唤醒事件走 systemd-logind 的 D-Bus 信号 PrepareForSleep（外加一把 delay 抑制锁，
# 保证我们能在挂起前把被代理设备的 ARP 还回去）。QtDBus 属 qtbase，装了 Qt 基本就有；**故意做成
# 可选**——万一某个构建环境缺它，只是这条链路降级为不可用，不该让整个构建失败。
# Windows/macOS 不需要（各有原生通知）。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 QUIET COMPONENTS DBus)
endif()

# QML 模块资源默认前缀 :/qt/qml/（引擎的 qrc import path）。不设 NEW 时用旧前缀，
# 运行时 loadFromModule("ClashAuto","Main") 找不到模块 → "contains no type named Main"。
if(POLICY QTP0001)
    qt_policy(SET QTP0001 NEW)
endif()

# ————————————————————————————————————————————————————————————————
# 复用的既有后端（原样编译进 QML 目标；不含 Widgets 的 MainWindow/main.cpp）。
# TrayController.cpp 会 #include "MainWindow.h"（仅用其 QWidget 基类方法），故 MainWindow.h
# 留在源码树里可被包含，但 MainWindow.cpp 不参与本目标的编译/链接。
# ————————————————————————————————————————————————————————————————
set(BACKEND_SOURCES
    src/AppConfig.cpp        src/AppConfig.h
    src/VersionManifest.cpp  src/VersionManifest.h
    src/ConfigBuilder.cpp    src/ConfigBuilder.h
    src/SubscriptionStore.cpp src/SubscriptionStore.h
    src/CoreController.cpp   src/CoreController.h
    src/ClashService.cpp     src/ClashService.h
    src/SubParser.cpp        src/SubParser.h
    # GeoIP 库(Country.mmdb)的校验 + 原子落盘：下载侧只暂存，CoreController 起核心前才换上去
    src/MmdbFile.cpp         src/MmdbFile.h
    src/TrayController.cpp   src/TrayController.h
    # 单实例守卫：两个实例并存时，后者的 removeStale/remove 会清掉前者装的 nft/pf 规则
    src/SingleInstance.cpp   src/SingleInstance.h
    # 设备页后端（局域网发现 + 台账；框架无关）
    src/LanScanner.cpp       src/LanScanner.h
    src/DeviceStore.cpp      src/DeviceStore.h
    # 全应用共用的 SQLite 库 coast.db：device 表（设备台账）+ conn 表（上网历史）
    src/Sqlite.cpp           src/Sqlite.h
    src/HistoryStore.cpp     src/HistoryStore.h
    # 状态页延迟卡：直连/到路由/DNS 三项自测（TCP 握手 RTT），到代理那项取核心的测速结果
    src/LatencyProbe.cpp     src/LatencyProbe.h
    # 系统睡眠/唤醒（Windows WM_POWERBROADCAST / mac NSWorkspace / Linux logind）——挂起前撤劫持
    src/PowerWatcher.cpp     src/PowerWatcher.h
    # 透明网关——跨平台可编译层。L2 工厂按平台各一份实现（Linux=AF_PACKET / mac=BPF /
    # Windows=Npcap待做，现桩 nullptr）；每个 .cpp 用 #if <os> 守卫，同一平台只有一个定义。
    src/net/IL2Endpoint.h
    src/net/NpcapStatus.h    # Windows：Npcap 装没装 / 是否被锁成仅管理员（header-only）
    src/net/L2Endpoint_linux.cpp
    src/net/L2Endpoint_mac.cpp
    src/net/L2Endpoint_win.cpp
    src/net/ArpSpoofer.cpp   src/net/ArpSpoofer.h
    # 崩溃兜底：预存还原帧，进程被 SIGSEGV/abort 打死时由信号处理器裸发（跨平台可编译）
    src/net/GatewayPanic.cpp src/net/GatewayPanic.h
    # 被动 ARP/NDP 冲突监视（检测「本机被劫持 / 被劫持设备被他人争抢」）——纯解析，跨平台可编译
    src/net/ArpWatch.cpp     src/net/ArpWatch.h
    src/net/Socks5Client.cpp src/net/Socks5Client.h
    # ———— CoastCore：进程内出站引擎（从 feat/coastcore-outbound 移植，阶段 1）————
    # 出站抽象：把「连接接到哪个出站」与「出站是谁」解耦
    #（Socks5* = 拨 mihomo；core/ 下的 = 进程内协议）。
    # ★ 阶段 1 **只搬不接线**：下面这些文件没有任何构造点，编进来是零行为变化。
    #   接线在阶段 2（NetStack 的 SmolConn 改用 IOutboundTcp），见 docs/coastcore-merge-plan.md。
    # ★ 这里只列**纯 Qt 依赖**的那一批。需要 OpenSSL 裸 AEAD/X25519 的（ss/vmess/reality/utls）
    #   与需要 msquic 的（hysteria2/tuic）暂不编入 —— OutboundRegistry.cpp 有 #ifdef 守卫，
    #   缺了它们只是那些协议未注册、回退 mihomo，不影响编译。
    src/net/IOutbound.h
    # 进程内出站的轻量埋点（core/ 各处都在用）
    src/net/InprocTelemetry.cpp src/net/InprocTelemetry.h
    src/net/core/ProxyConfig.cpp        src/net/core/ProxyConfig.h
    src/net/core/ProxyConfigBuilder.cpp src/net/core/ProxyConfigBuilder.h
    src/net/core/DirectOutbound.cpp     src/net/core/DirectOutbound.h
    src/net/core/CoreDialerFactory.cpp  src/net/core/CoreDialerFactory.h
    # 分流判定的**唯一**实现：局域网网关与本机入站共用（复制一份就会开始漂移）
    src/net/core/CoreRouter.cpp         src/net/core/CoreRouter.h
    # 自身流量排除（接进程内 TUN 的前置条件）。自检钩子 COAST_SELFROUTE_SELFTEST=1。
    src/net/core/SelfRouteGuard.cpp     src/net/core/SelfRouteGuard.h
    # Kdf 是纯 Qt（QCryptographicHash/QMessageAuthenticationCode），不依赖 OpenSSL，恒编入。
    src/net/core/crypto/Kdf.cpp             src/net/core/crypto/Kdf.h
    src/net/core/transport/TlsClient.cpp    src/net/core/transport/TlsClient.h
    src/net/core/transport/WsClient.cpp     src/net/core/transport/WsClient.h
    src/net/core/proto/OutboundRegistry.cpp src/net/core/proto/OutboundRegistry.h
    # 只用 QSslSocket + Qt 哈希的两个协议，恒编入
    src/net/core/proto/TrojanOutbound.cpp   src/net/core/proto/TrojanOutbound.h
    src/net/core/proto/VlessOutbound.cpp    src/net/core/proto/VlessOutbound.h
    # 分流规则引擎（mihomo 风格首命中 + 只读 mmdb GEOIP）+ DNS 解析器（A/AAAA 缓存 + fake-ip 池）
    src/net/core/RuleEngine.cpp   src/net/core/RuleEngine.h
    src/net/core/DnsResolver.cpp  src/net/core/DnsResolver.h
    # DNS 报文层（解析设备查询 / 合成 fake-ip 应答）
    src/net/core/DnsMessage.cpp   src/net/core/DnsMessage.h
    # ———— 进程内 TUN（「增强」按钮那条路）：搬进来但**默认不启用** ————
    # 与阶段 1 同一个纪律：编进来不等于跑起来。它会真的接管默认路由，
    # 需要一台可牺牲网络的机器验证 —— 本机是 QEMU 虚机且网络在用，只保证编得过。
    # 会话层：把设备层 + 激活层 + NetStack + 出站串成一个可开关的整体。
    # 本机混合入站（HTTP + SOCKS5）——CoastCore 的第二个入口，只依赖 Qt Network。
    # LocalTunService 用它做本机流量入口；同样默认不启用。
    src/net/inbound/MixedInbound.cpp src/net/inbound/MixedInbound.h
    src/net/LocalTunService.cpp src/net/LocalTunService.h
    # 激活层（跨平台）：配地址 / 接管默认路由 / Linux 策略路由，以及**原样还原**。
    # 与设备层分开是有意的：设备层失败只是 TUN 起不来，这一层失败会改变整机路由。
    src/net/TunSession.cpp      src/net/TunSession.h
    src/net/LanGateway.h
    # 数据面诊断日志（<userDir>/logs/gateway-diag.log）。跨平台编译：main_qml.cpp 要调 setLogDir，
    # 而计数器只有 POSIX 上的网关才真的往里写。
    src/net/GatewayDiag.cpp  src/net/GatewayDiag.h
)

# QML 绑定胶水（新增的薄适配层）
set(QML_GLUE_SOURCES
    src/qml/QmlBridge.cpp     src/qml/QmlBridge.h
    src/qml/NodeListModel.cpp src/qml/NodeListModel.h
    src/qml/ConnectionsModel.cpp src/qml/ConnectionsModel.h
    src/qml/SubscriptionsController.cpp src/qml/SubscriptionsController.h
    src/qml/LogModel.cpp                src/qml/LogModel.h
    src/qml/AboutController.cpp         src/qml/AboutController.h
    src/qml/SettingsController.cpp      src/qml/SettingsController.h
    src/qml/UpdateController.cpp         src/qml/UpdateController.h
    src/qml/I18n.cpp                     src/qml/I18n.h
    # Npcap 安装窗后端（Windows 才有实际逻辑；其它平台 supported()=false）
    src/qml/NpcapInstaller.cpp           src/qml/NpcapInstaller.h
    # 设备页 QML 胶水（薄适配层）
    src/qml/DevicesController.cpp        src/qml/DevicesController.h
    src/qml/DeviceListModel.cpp          src/qml/DeviceListModel.h
    src/qml/DeviceConnectionsModel.cpp   src/qml/DeviceConnectionsModel.h
)

qt_add_executable(clashauto-qml
    src/main_qml.cpp
    ${BACKEND_SOURCES}
    ${QML_GLUE_SOURCES}
    resources.qrc
)

# Theme.qml 注册为单例（供 `import ClashAuto` 后以 Theme.xxx 访问）——必须在 qt_add_qml_module 前设置。
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(clashauto-qml
    URI ClashAuto
    VERSION 1.0
    RESOURCE_PREFIX "/qt/qml"   # 模块落到 :/qt/qml/ClashAuto/，正是引擎默认搜索的 import path
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/StatusPage.qml
        qml/NodesPage.qml
        qml/DevicesPage.qml
        qml/DeviceRow.qml
        qml/DeviceTrafficBg.qml
        qml/DeviceDetailWindow.qml
        qml/SubscriptionsPage.qml
        qml/SettingsPage.qml
        qml/LogsPage.qml
        qml/AboutPage.qml
        qml/Card.qml
        qml/ThemedCombo.qml
        qml/NavButton.qml
        qml/FooterSwitch.qml
        qml/MetricCard.qml
        qml/NodeRow.qml
        qml/BandwidthChart.qml
        qml/LogTimeline.qml
        qml/ConnectionsWindow.qml
        qml/UpdateWindow.qml
        qml/RuleEditorWindow.qml
        qml/NpcapWindow.qml
)

# —— 透明网关：平台相关部分 ——
# 三端各走各的数据面，**只有 Windows 需要用户态栈**：
#   · Linux   → TPROXY（TproxyRules.cpp：nftables + 策略路由，内核转发）
#   · macOS   → pf rdr（PfRules.cpp：DIOCNATLOOK 取原始目的地）
#   · Windows → 用户态栈（NetStack.cpp + rust/coaststack 的 smoltcp）
#               —— WfpRedirect.h:91 记着 gatewayTproxy/gatewayPf 在 Windows 上恒为 false。
# 因此 NetStack.cpp 只在 WIN32 编，非 Windows 由 NetStack_stub.cpp 顶上（init() 直接报错，
# 不降级；用户已确认「报错就行」）。vendored lwIP 曾经是三端共用的用户态栈，已随此改动整体删除
# —— 过程与依据见 docs/lwip-alternatives.md。
#   LanGateway_linux.cpp 实为 POSIX 通用（无平台系统调用；二层差异全在 createL2Endpoint：
#   Linux AF_PACKET / mac BPF / Windows Npcap）。netns 自测钩子(GatewaySelfTest, 用 TAP)仅 Linux。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE OR WIN32)
    # 网关自测钩子。文件里分两段，守卫也分两段（见 GatewaySelfTest.cpp）：
    #   · 数据面自测(COAST_GATEWAY_SELFTEST)：要 TAP(Linux)/feth+BPF(mac)，只在 POSIX 编；
    #   · RA 解析自测(COAST_NDP_RA_SELFTEST)：纯字节解析，**所有编网关的平台都编，含 Windows**。
    # 以前整个文件只在 Linux/mac 编，于是 Windows 随包发布了 NdpSpoofer::parseRouterAdvert
    # 却连这一个不需要网络/root 的自测都跑不了 —— 而它正是「双栈设备 v6 漏代理」的唯一防线。
    target_sources(clashauto-qml PRIVATE
        src/net/GatewaySelfTest.cpp src/net/GatewaySelfTest.h)
    target_sources(clashauto-qml PRIVATE
        src/net/NetStack.h
        # ★ ArpSpoofer/NdpSpoofer 是**三端都要**的：把设备劫持过来靠的就是 ARP/NDP 投毒，
        #   与之后用哪条数据面（TPROXY / pf / 用户态栈）无关。别跟着 NetStack 一起收进 WIN32。
        src/net/NdpSpoofer.cpp src/net/NdpSpoofer.h  # IPv6 的 ICMPv6/NDP 投毒器（ArpSpoofer 的 v6 对应物）
        src/net/LanGateway_linux.cpp
        # 内核态转发规则（nftables TPROXY + 策略路由）。非 Linux 由 #if 守卫成空实现,
        # 所以三端都编——省得调用方到处写平台分支。
        src/net/TproxyRules.cpp src/net/TproxyRules.h
        # macOS 的内核态转发规则（pf rdr + DIOCNATLOOK 取原始目的地）。与 TproxyRules 同样
        # 三端都编、非 macOS 由 #if 守卫成空实现，省得调用方到处写平台分支。
        src/net/PfRules.cpp src/net/PfRules.h
    )
    if(NOT WIN32)
        target_sources(clashauto-qml PRIVATE src/net/NetStack_stub.cpp)
    endif()
    # Windows：只要 Npcap SDK 的**头文件**（env NPCAP_SDK 指向解压目录）——wpcap 不进链接行。
    # L2Endpoint_win.cpp 在运行时 LoadLibrary("wpcap.dll") + GetProcAddress 取全部 pcap_*，
    # 所以没装 Npcap 的机器上 Coast 照常启动（只是网关不可用）。原先靠 MSVC 的 /DELAYLOAD 做到
    # 这一点，但那条件只覆盖 MSVC：MinGW 构建把 wpcap.dll 写进导入表，无 Npcap 的机器上进程
    # 直接 0xC0000135 起不来。改成运行时绑定后两种工具链行为一致，也不再需要 delayimp。
    # TUN 设备层：各平台一个实现（Windows=wintun 运行期加载 / Linux=/dev/net/tun / mac=utun）
    if(WIN32)
        target_sources(clashauto-qml PRIVATE src/net/TunEndpoint_win.cpp src/net/TunEndpoint.h)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(clashauto-qml PRIVATE src/net/TunEndpoint_linux.cpp src/net/TunEndpoint.h)
    elseif(APPLE)
        target_sources(clashauto-qml PRIVATE src/net/TunEndpoint_mac.cpp src/net/TunEndpoint.h)
    endif()

    if(WIN32)
        set(NPCAP_SDK "$ENV{NPCAP_SDK}")
        target_include_directories(clashauto-qml PRIVATE "${NPCAP_SDK}/Include")
        target_link_libraries(clashauto-qml PRIVATE iphlpapi ws2_32)

        # ———— coaststack：smoltcp 的 TCP 数据面（Rust 静态库）————
        # **只在 Windows 编**：Linux 走 TPROXY、macOS 走 pf rdr，都不需要用户态栈，
        # 所以 mac/Linux 的 CI 与外部签名仓库 integemjack/schat.build 一行都不用动
        # （那个仓库推不动、忘了同步也不报红灯，绕开它是 Windows-only 方案的最大红利）。
        # 选型与收益见 docs/lwip-alternatives.md；C ABI 见 src/net/coaststack.h。
        #
        # ★ lwIP 移除后这不再是"两条路里的一条"，而是 **Windows 唯一的数据面**。
        #   COAST_RUST=OFF 仍保留为应急开关（比如 cargo 装不上还想出个包），但代价明确：
        #   NetStack.cpp 整个不编、换成桩，**Windows 上网关不可用**（app 其余功能照常）。
        #   —— 这正是当初把它做成 option 的理由，只是从"退回 lwIP"变成了"网关关掉"。
        option(COAST_RUST "Build the Rust (smoltcp) TCP datapath on Windows" ON)
        if(COAST_RUST)
            target_sources(clashauto-qml PRIVATE src/net/NetStack.cpp)
            # ★ triple 必须按 **C++ 编译器** 选，不能只看架构：
            #   staticlib 的 ABI/CRT 要与 C++ 工具链一致（MSVC 的 .lib vs MinGW 的 .a），
            #   混了会在链接期炸出一堆缺符号。CI 是 MSVC，本地开发常是 MinGW。
            if(MSVC)
                if(CMAKE_VS_PLATFORM_NAME MATCHES "ARM64|arm64")
                    set(COAST_RUST_TARGET aarch64-pc-windows-msvc)
                else()
                    set(COAST_RUST_TARGET x86_64-pc-windows-msvc)
                endif()
                set(COAST_RUST_LIBNAME coaststack.lib)
            else()
                set(COAST_RUST_TARGET x86_64-pc-windows-gnu)
                set(COAST_RUST_LIBNAME libcoaststack.a)
            endif()

            set(COAST_RUST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust/coaststack")
            set(COAST_RUST_OUT "${COAST_RUST_DIR}/target/${COAST_RUST_TARGET}/release/${COAST_RUST_LIBNAME}")

            # ★ 依赖必须是**全部** .rs，不能手写几个。
            #   这里原本只列了 lib.rs 与 ffi.rs —— 于是改 engine.rs / portmap.rs 时
            #   `ninja` 直接报 "no work to do"，Coast.exe 链的还是**上一版**静态库。
            #   后果极其隐蔽：Rust 单测跑的是新代码（cargo 自己算依赖，而且用的是
            #   test profile、输出路径都不同），C++ 自测跑的却是旧库，两边都绿，
            #   而绿灯说明不了任何事情。踩过一次，用 GLOB 收全。
            #   （GLOB 在 configure 期展开：新增 .rs 文件需要重跑一次 cmake。
            #     比起"改了不重建"，这个代价可以接受。）
            file(GLOB COAST_RUST_SRCS "${COAST_RUST_DIR}/src/*.rs")

            # --locked：Cargo.lock 已提交，解析结果逐字节可复现（没有 vendor 依赖源码，
            # 理由见 rust/coaststack/README.md）。
            add_custom_command(
                OUTPUT "${COAST_RUST_OUT}"
                COMMAND ${CMAKE_COMMAND} -E env cargo build --release --locked
                        --target ${COAST_RUST_TARGET}
                WORKING_DIRECTORY "${COAST_RUST_DIR}"
                DEPENDS "${COAST_RUST_DIR}/Cargo.toml" "${COAST_RUST_DIR}/Cargo.lock"
                        ${COAST_RUST_SRCS}
                        "${COAST_RUST_DIR}/.cargo/config.toml"
                COMMENT "cargo build coaststack (${COAST_RUST_TARGET})"
                VERBATIM)
            add_custom_target(coaststack_build DEPENDS "${COAST_RUST_OUT}")
            add_library(coaststack STATIC IMPORTED)
            set_target_properties(coaststack PROPERTIES IMPORTED_LOCATION "${COAST_RUST_OUT}")
            add_dependencies(clashauto-qml coaststack_build)

            # Rust std 引入的系统符号。缺了就是一串 LNK2019 —— 这几个是 MSVC 上的常见集合。
            # （CRT 保持 /MD：rust 的 *-pc-windows-msvc 默认就链动态 CRT，与本工程一致；
            #   谁要是给 CMake 加了 /MT，必须同步给 rustc 加 -Ctarget-feature=+crt-static。）
            target_link_libraries(clashauto-qml PRIVATE coaststack
                userenv bcrypt ntdll synchronization)
            target_compile_definitions(clashauto-qml PRIVATE COAST_HAVE_RUST_STACK=1)
            # 链接+ABI 自证（COAST_RUSTSTACK_SELFTEST=1）。没有它，"库编出来了"和
            # "库真能被调用、ABI 对得上"在 CI 绿灯里分不清。
            target_sources(clashauto-qml PRIVATE src/net/RustStackSelfTest.cpp)
        else()
            # 没有 Rust 数据面 = Windows 上也没有用户态栈 → 用与 mac/Linux 同一个桩。
            target_sources(clashauto-qml PRIVATE src/net/NetStack_stub.cpp)
        endif()
    endif()
else()
    target_sources(clashauto-qml PRIVATE src/net/LanGateway_stub.cpp)
endif()

# CoastCore 需要裸 AEAD / X25519 的部分（ss / vmess / reality + 加密 KAT 自测）：仅在 OpenSSL
# **可用**时编入 + 定义 COAST_HAVE_OPENSSL（见 OutboundRegistry.cpp 的 #ifdef）。
# 不可用时这三个协议不注册 -> 拨号侧回退 mihomo；direct/trojan/vless 不受影响。
if(COAST_HAVE_OPENSSL)
    target_sources(clashauto-qml PRIVATE
        src/net/core/crypto/Aead.cpp               src/net/core/crypto/Aead.h
        src/net/core/crypto/CryptoSelfTest.cpp     src/net/core/crypto/CryptoSelfTest.h
        # uTLS 指纹 + 自建最小 TLS1.3 客户端（REALITY 用；QSslSocket 无法控制 ClientHello 指纹）
        src/net/core/transport/UtlsClient.cpp      src/net/core/transport/UtlsClient.h
        src/net/core/proto/ShadowsocksOutbound.cpp src/net/core/proto/ShadowsocksOutbound.h
        src/net/core/proto/VmessOutbound.cpp       src/net/core/proto/VmessOutbound.h
        src/net/core/proto/RealityOutbound.cpp     src/net/core/proto/RealityOutbound.h)
    # 只链 libcrypto，不链 libssl（TLS 由 Qt 的 QSslSocket 负责）。
    target_link_libraries(clashauto-qml PRIVATE OpenSSL::Crypto)
    # 静态 libcrypto 需要的系统库（动态版由 DLL 自己带，静态版必须我们链）。
    # 多链无害：这几个 Windows 目标本来就常用到。
    if(WIN32)
        target_link_libraries(clashauto-qml PRIVATE crypt32 ws2_32 advapi32 user32 z)
    endif()
    target_compile_definitions(clashauto-qml PRIVATE COAST_HAVE_OPENSSL)
endif()

# Windows：把 assets/icon.ico 作为 exe 图标嵌入。
if(WIN32)
    set(APP_ICON_ABS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
        @ONLY)
    target_sources(clashauto-qml PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
    # Windows 原生标题栏染色（DWM）——仅 Windows 编译，链接的 dwmapi 已在下方 target_link_libraries。
    target_sources(clashauto-qml PRIVATE src/WinWindow.cpp src/WinWindow.h)
    # 这里曾经还有一个 resources_win.qrc，内嵌四个架构的 wintun.dll。已删 —— 核心自带，见
    # CoreController::startCore 里那段注释。别再加回来。
endif()

target_link_libraries(clashauto-qml PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    $<$<PLATFORM_ID:Windows>:dwmapi>
    $<$<PLATFORM_ID:Windows>:shell32>
    $<$<PLATFORM_ID:Windows>:advapi32>
    $<$<PLATFORM_ID:Windows>:wininet>
    # NpcapInstaller：下载来的安装器要提权运行，先验 Authenticode 签名（WinVerifyTrust + 签名者名）
    $<$<PLATFORM_ID:Windows>:wintrust>
    $<$<PLATFORM_ID:Windows>:crypt32>
)

# Linux：有 QtDBus 才编入 logind 那条睡眠通道（见 src/PowerWatcher.cpp 的 COAST_HAVE_DBUS 分支）。
if(Qt6DBus_FOUND)
    target_link_libraries(clashauto-qml PRIVATE Qt6::DBus)
    target_compile_definitions(clashauto-qml PRIVATE COAST_HAVE_DBUS)
endif()

target_compile_options(clashauto-qml PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

# GUI 子系统：Windows 上不弹控制台窗口
set_target_properties(clashauto-qml PROPERTIES WIN32_EXECUTABLE ON)

# 产物文件名统一为 clashauto（去掉历史上的 -qml/-cpp 后缀）：Windows→clashauto.exe，
# Linux→clashauto。CMake 目标名仍叫 clashauto-qml（内部标识，不影响产物名）。
# macOS 例外——见下方 if(APPLE)，输出「Coast.app」bundle。
if(WIN32)
    set_target_properties(clashauto-qml PROPERTIES OUTPUT_NAME "Coast")   # Windows 产物 Coast.exe（扁平自包含）
elseif(NOT APPLE)
    set_target_properties(clashauto-qml PROPERTIES OUTPUT_NAME "coast")   # Linux 命令惯例小写 coast（Win=Coast.exe / mac=Coast.app）
endif()

# macOS：打成「Coast.app」bundle；Clashr-Auto 运行时资源由 CI 放进 Contents/Clashr-Auto。
if(APPLE)
    set_target_properties(clashauto-qml PROPERTIES
        OUTPUT_NAME "Coast"
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "Coast"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.yuehongsun.coast"
        MACOSX_BUNDLE_BUNDLE_VERSION "${APP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_VERSION}")
    set(APP_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns")
    if(EXISTS "${APP_ICNS}")
        set_target_properties(clashauto-qml PROPERTIES MACOSX_BUNDLE_ICON_FILE "icon.icns")
        target_sources(clashauto-qml PRIVATE "${APP_ICNS}")
        set_source_files_properties("${APP_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    endif()

    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(SYSTEMCONFIGURATION_FRAMEWORK SystemConfiguration REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(SERVICEMANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)

    target_sources(clashauto-qml PRIVATE
        src/MacHelperClient.mm src/MacHelperClient.h
        src/MacWindow.mm src/MacWindow.h
        src/MacSpeedItem.mm src/MacSpeedItem.h
        # 睡眠/唤醒的 mac 后端（NSWorkspace 通知）；跨平台声明在 src/PowerWatcher.h
        src/PowerWatcher_mac.mm)
    set_source_files_properties(src/MacHelperClient.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties(src/MacWindow.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties(src/MacSpeedItem.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties(src/PowerWatcher_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

    target_link_libraries(clashauto-qml PRIVATE
        ${SECURITY_FRAMEWORK}
        ${SYSTEMCONFIGURATION_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${SERVICEMANAGEMENT_FRAMEWORK}
        ${APPKIT_FRAMEWORK})

    # 特权 helper（以 root 运行的 launchd daemon）：独立可执行，只依赖 Foundation（NSXPC）。
    add_executable(clashauto-helper helper/helper_main.mm)
    set_target_properties(clashauto-helper PROPERTIES OUTPUT_NAME "com.yuehongsun.coast.helper")
    target_include_directories(clashauto-helper PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/helper"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set_source_files_properties(helper/helper_main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(clashauto-helper PRIVATE
        ${FOUNDATION_FRAMEWORK}
        ${SYSTEMCONFIGURATION_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK})

    add_dependencies(clashauto-qml clashauto-helper)
    add_custom_command(TARGET clashauto-qml POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:clashauto-qml>/Library/LaunchDaemons"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clashauto-helper>"
            "$<TARGET_BUNDLE_CONTENT_DIR:clashauto-qml>/MacOS/com.yuehongsun.coast.helper"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/helper/com.yuehongsun.coast.helper.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:clashauto-qml>/Library/LaunchDaemons/com.yuehongsun.coast.helper.plist"
        VERBATIM)
endif()

# Generate Version.h with the resolved APP_VERSION.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
    @ONLY
)
target_include_directories(clashauto-qml PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")   # 让 src/qml/*.cpp 既可 "AppConfig.h" 也可 "../AppConfig.h"
