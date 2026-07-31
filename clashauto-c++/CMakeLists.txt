cmake_minimum_required(VERSION 3.20)

# C 语言用于 vendored lwIP(.c)：不启用则 CMake 会静默忽略 .c 源，链接时报一堆 lwIP 未定义符号。
project(clashauto_cpp VERSION 0.2.1 LANGUAGES C CXX)

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

# OpenSSL libcrypto —— CoastCore 协议出站的裸 AEAD（AES-GCM / ChaCha20-Poly1305，见
# src/net/core/crypto/Aead.cpp）+ REALITY 的 X25519。只用 libcrypto 的 EVP（哈希/HMAC 走 Qt）。
#
# ★ **可选**（与 msquic / QtDBus 同策略：缺了就降级，绝不让构建失败）。曾经写成 REQUIRED，
#   结果 **macOS universal 与 Windows arm64 两个 job 直接构建失败**（CI 实测）：
#     · macOS 要 `-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64` 出通用二进制，而 brew 的 libcrypto 是
#       **单架构**的 → 即便 find 得到也链不进通用二进制；
#     · Windows arm64 是交叉编译，环境里没有 arm64 的 OpenSSL，find_package 会找到**宿主 x64** 的
#       → 架构不匹配、编译过而链接炸（这一条正是「只按平台猜」的写法漏掉的，故改用下面的真链测试）。
#   降级后果**很轻**：只有「需要裸 AEAD/X25519 的三个协议」(ss / vmess / reality) 不编入、不注册，
#   拨号侧照旧回退 mihomo（功能不缺，只是不走进程内）；direct / trojan / vless 仍是进程内（它们只用
#   QSslSocket + Qt 哈希）。本地 MinGW 自带（opt/include、opt/lib），可用 -DOPENSSL_ROOT_DIR 提示。
set(COAST_HAVE_OPENSSL OFF)
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

# msquic —— CoastCore 的 QUIC 系出站(Hysteria2 / TUIC,见 src/net/core/transport/QuicTransport.cpp)。
# **可选**:找到才编入 QUIC 协议并定义 COAST_HAVE_QUIC;找不到则 hysteria2/tuic 不注册、具名节点回退
# mihomo,其余协议不受影响(与 QtDBus 一样的「缺了就降级、别让构建失败」策略)。
# CI 需提供 msquic(预编译包,见 QuicTransport.cpp 的集成说明);本地 MinGW/QEMU 无 → 默认关。
# 用 -Dmsquic_DIR=... 或 -DCOAST_MSQUIC_ROOT=... 指向 SDK。
# ★ Hysteria2 已真机拨通（对官方 hysteria 2.10 服务端，见 tools/outbound_e2e/README）。
# ★ TUIC token 需 msquic 的 ConnectionExportKeyingMaterial —— 头文件里明写 "Available from v2.6"，
#   而 msquic **最新发行版仍是 v2.5.9**（packages.microsoft.com 亦然），故 tuic 现阶段必然回退内核。
set(COAST_HAVE_QUIC OFF)
set(COAST_MSQUIC_LINK "")
# 先试 CMake config 包（自己源码编的 msquic 会装 msquicConfig.cmake）。
find_package(msquic QUIET)
if(msquic_FOUND)
    set(COAST_HAVE_QUIC ON)
    set(COAST_MSQUIC_LINK msquic)
else()
    # ★ 再手工找：**发行版/官方源装来的 msquic 不带 CMake config**。
    #   实测 microsoft/msquic 的 GitHub Release **一个预编译资产都没有**（v2.5.9 assets=0），
    #   Linux 侧的正路是 packages.microsoft.com 的 `libmsquic`（jammy/amd64 与 noble/arm64 都有 2.5.9，
    #   正对上 CI 的 ubuntu-22.04 / ubuntu-24.04-arm 两个 runner），它只装 msquic.h + libmsquic.so，
    #   没有 CMake config → 只靠 find_package 会永远找不到。
    #   也可用 -DCOAST_MSQUIC_ROOT=<sdk> 指一份自备的（含 include/ 与 lib/）。
    find_path(COAST_MSQUIC_INCLUDE_DIR msquic.h
              HINTS ${COAST_MSQUIC_ROOT} $ENV{COAST_MSQUIC_ROOT}
              PATH_SUFFIXES include)
    find_library(COAST_MSQUIC_LIBRARY NAMES msquic
                 HINTS ${COAST_MSQUIC_ROOT} $ENV{COAST_MSQUIC_ROOT}
                 PATH_SUFFIXES lib lib64)
    if(COAST_MSQUIC_INCLUDE_DIR AND COAST_MSQUIC_LIBRARY)
        set(COAST_HAVE_QUIC ON)
        set(COAST_MSQUIC_LINK ${COAST_MSQUIC_LIBRARY})
        message(STATUS "msquic 手工定位: ${COAST_MSQUIC_LIBRARY} (include: ${COAST_MSQUIC_INCLUDE_DIR})")
    endif()
endif()
if(COAST_HAVE_QUIC)
    message(STATUS "msquic found → CoastCore QUIC (Hysteria2/TUIC) enabled")
else()
    message(STATUS "msquic NOT found → CoastCore QUIC disabled (hysteria2/tuic fall back to mihomo)")
endif()

# ★ -DCOAST_REQUIRE_QUIC=ON → 找不到 msquic 就**直接失败**，而不是静默降级。
#   存在的理由是一次真实的翻车：CI 全绿、发布也成功，但下载产物一看根本没有 QUIC ——
#   因为 msquic 的安装步骤悄悄失败了，而 CMake 这边「找不到就关掉」，构建照样绿。
#   发布流水线必须传这个开关：**要么带着 QUIC 发，要么别发**，不许发一个悄悄少了功能的包。
option(COAST_REQUIRE_QUIC "构建必须带上 msquic（QUIC/Hysteria2）；找不到即失败" OFF)
if(COAST_REQUIRE_QUIC AND NOT COAST_HAVE_QUIC)
    message(FATAL_ERROR
        "要求带 QUIC(COAST_REQUIRE_QUIC=ON) 但没找到 msquic。\n"
        "  用 -DCOAST_MSQUIC_ROOT=<sdk> 指一份含 include/(msquic.h) 与 lib/(libmsquic.so|msquic.lib) 的目录。\n"
        "  Linux: packages.microsoft.com 的 libmsquic 只装 .so，头要另取（见 tools/outbound_e2e/README）。\n"
        "  Windows: NuGet 包 Microsoft.Native.Quic.MsQuic.Schannel 里 include/ 与 lib/<arch>/ 都有。")
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
    src/ConfigBuilder.cpp    src/ConfigBuilder.h
    src/SubscriptionStore.cpp src/SubscriptionStore.h
    src/CoreController.cpp   src/CoreController.h
    src/ClashService.cpp     src/ClashService.h
    src/SubParser.cpp        src/SubParser.h
    # GeoIP 库(Country.mmdb)的校验 + 原子落盘：下载侧只暂存，CoreController 起核心前才换上去
    src/MmdbFile.cpp         src/MmdbFile.h
    src/TrayController.cpp   src/TrayController.h
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
    # 出站抽象：把「连接接到哪个出站」与「出站是谁」解耦（Socks5=拨 mihomo，后续=进程内协议）
    src/net/IOutbound.h
    src/net/Socks5Client.cpp src/net/Socks5Client.h
    # 本机混合入站（HTTP + SOCKS5）——CoastCore 的第二个入口，只依赖 Qt Network，四平台都编。
    # 与 NetStack/lwIP 无关（那部分在下面 POSIX 段里），见 docs/mihomo-replacement-gap.md。
    src/net/inbound/MixedInbound.cpp src/net/inbound/MixedInbound.h
    # CoastCore 进程内出站骨架（跨平台，只用 Qt Core/Network）：不可变配置快照 + 热重载、进程内直连
    # 出站、按(目的地/设备)选出站的拨号工厂（默认全部回退 Socks5，行为不变）。
    src/net/core/ProxyConfig.cpp       src/net/core/ProxyConfig.h
    src/net/core/ProxyConfigBuilder.cpp src/net/core/ProxyConfigBuilder.h
    src/net/core/DirectOutbound.cpp    src/net/core/DirectOutbound.h
    src/net/core/CoreDialerFactory.cpp src/net/core/CoreDialerFactory.h
    # 分流判定的**唯一**实现：局域网网关与本机入站共用（复制一份就会开始漂移）
    src/net/core/CoreRouter.cpp        src/net/core/CoreRouter.h
    # 自身流量排除：接进程内 TUN 的前置条件（不解环路就是整机断网）。自检钩子
    # COAST_SELFROUTE_SELFTEST=1。
    src/net/core/SelfRouteGuard.cpp    src/net/core/SelfRouteGuard.h
    # 协议出站的共享基建（跨平台）：AEAD/KDF 加密层(OpenSSL libcrypto)、TLS/WebSocket 传输原语、
    # 协议注册表（加协议=加文件+注册一行）。各协议出站单元并行建立在这之上。
    # Kdf 是纯 Qt（QCryptographicHash/QMessageAuthenticationCode），不依赖 OpenSSL，恒编入。
    src/net/core/crypto/Kdf.cpp           src/net/core/crypto/Kdf.h
    src/net/core/transport/TlsClient.cpp  src/net/core/transport/TlsClient.h
    src/net/core/transport/WsClient.cpp   src/net/core/transport/WsClient.h
    src/net/core/proto/OutboundRegistry.cpp src/net/core/proto/OutboundRegistry.h
    # 进程内出站协议实现（各自实现 IOutbound*，经 OutboundRegistry 注册 type→构造器）。
    # 只用 QSslSocket + Qt 哈希的两个恒编入；需要裸 AEAD/X25519 的 ss/vmess/reality 见下方 OpenSSL 条件块。
    src/net/core/proto/TrojanOutbound.cpp      src/net/core/proto/TrojanOutbound.h
    src/net/core/proto/VlessOutbound.cpp       src/net/core/proto/VlessOutbound.h
    # 分流规则引擎（mihomo 风格首命中 + 自带只读 mmdb GEOIP 查询）+ DNS 解析器（A/AAAA 缓存 + fake-ip 池）。
    src/net/core/RuleEngine.cpp  src/net/core/RuleEngine.h
    src/net/core/DnsResolver.cpp src/net/core/DnsResolver.h
    # DNS 报文层（解析设备的查询 / 合成 fake-ip 应答）——让网关的 DNS 这一环不再依赖核心
    src/net/core/DnsMessage.cpp  src/net/core/DnsMessage.h
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
# Linux/macOS（POSIX，gcc/clang）：编入用户态栈(vendored lwIP 子集 + NetStack) 与 LanGateway 真实现。
#   lwIP 走 NO_SYS raw API，移植配置在 src/net/lwip_port/，ip4.c/tcp_in.c 有 accept-all/any-port 补丁。
#   LanGateway_linux.cpp 实为 POSIX 通用（无平台系统调用；二层差异全在 createL2Endpoint：
#   Linux AF_PACKET / mac BPF）。netns 自测钩子(GatewaySelfTest, 用 TAP)仅 Linux。
# Windows：也编真实现——二层用 Npcap（需 Npcap SDK，CI 里下载，见 release.yml），lwIP 用 MSVC
#          结构体打包(cc.h 有 _MSC_VER 分支)。链 wpcap/Packet/iphlpapi/ws2_32。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE OR WIN32)
    set(LWIP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/src")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
        # 网关 headless 自测钩子：Linux 用 TAP、mac 用 feth+真实 BPF 端点（Windows Npcap 暂不做 CI 自测）。
        target_sources(clashauto-qml PRIVATE
            src/net/GatewaySelfTest.cpp src/net/GatewaySelfTest.h)
    endif()
    # 本机 TUN 入口（进程内「增强」）：把裸 IP 包合成以太帧后喂给同一套 NetStack/lwIP，
    # 上层一行不用改。共享的合成 MAC/以太头常量与设计总纲在 TunEndpoint.h。
    # macOS(utun) 待做。现在还没有调用方——先编进来，免得它悄悄腐烂。
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(clashauto-qml PRIVATE src/net/TunEndpoint_linux.cpp src/net/TunEndpoint.h)
    elseif(WIN32)
        # wintun.dll 运行期动态加载（仓库没有它的 .lib/.h），所以这里不需要额外 link 什么；
        # 用到的 ConvertInterfaceLuidToIndex 来自已链接的 iphlpapi。
        target_sources(clashauto-qml PRIVATE src/net/TunEndpoint_win.cpp src/net/TunEndpoint.h)
    elseif(APPLE)
        # utun：系统自带（PF_SYSTEM/kern_control），无需额外库。
        target_sources(clashauto-qml PRIVATE src/net/TunEndpoint_mac.cpp src/net/TunEndpoint.h)
    endif()
    target_sources(clashauto-qml PRIVATE
        src/net/NetStack.cpp src/net/NetStack.h
        src/net/NdpSpoofer.cpp src/net/NdpSpoofer.h  # IPv6 的 ICMPv6/NDP 投毒器（ArpSpoofer 的 v6 对应物）
        src/net/LanGateway_linux.cpp
        ${LWIP_DIR}/core/init.c        ${LWIP_DIR}/core/def.c
        ${LWIP_DIR}/core/dns.c         ${LWIP_DIR}/core/inet_chksum.c
        ${LWIP_DIR}/core/ip.c          ${LWIP_DIR}/core/mem.c
        ${LWIP_DIR}/core/memp.c        ${LWIP_DIR}/core/netif.c
        ${LWIP_DIR}/core/pbuf.c        ${LWIP_DIR}/core/raw.c
        ${LWIP_DIR}/core/stats.c       ${LWIP_DIR}/core/sys.c
        ${LWIP_DIR}/core/tcp.c         ${LWIP_DIR}/core/tcp_in.c
        ${LWIP_DIR}/core/tcp_out.c     ${LWIP_DIR}/core/timeouts.c
        ${LWIP_DIR}/core/udp.c
        ${LWIP_DIR}/core/ipv4/etharp.c ${LWIP_DIR}/core/ipv4/icmp.c
        ${LWIP_DIR}/core/ipv4/ip4.c    ${LWIP_DIR}/core/ipv4/ip4_addr.c
        ${LWIP_DIR}/core/ipv4/ip4_frag.c ${LWIP_DIR}/core/ipv4/acd.c
        # —— IPv6 双栈：与 ipv4/ 对称的一套核心文件。ip6.c 有 accept-all 补丁、nd6.c 有静态邻居补丁。
        #   受 lwipopts 开关保护的文件（mld6/ip6_frag 在本移植关着）仍要编——它们内部按宏自缩成近空。
        ${LWIP_DIR}/core/ipv6/ip6.c    ${LWIP_DIR}/core/ipv6/ip6_addr.c
        ${LWIP_DIR}/core/ipv6/ip6_frag.c ${LWIP_DIR}/core/ipv6/nd6.c
        ${LWIP_DIR}/core/ipv6/icmp6.c  ${LWIP_DIR}/core/ipv6/ethip6.c
        ${LWIP_DIR}/core/ipv6/mld6.c   ${LWIP_DIR}/core/ipv6/inet6.c
        ${LWIP_DIR}/netif/ethernet.c
    )
    target_include_directories(clashauto-qml PRIVATE
        "${LWIP_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/net/lwip_port")
    # Windows：只要 Npcap SDK 的**头文件**（env NPCAP_SDK 指向解压目录）——wpcap 不进链接行。
    # L2Endpoint_win.cpp 在运行时 LoadLibrary("wpcap.dll") + GetProcAddress 取全部 pcap_*，
    # 所以没装 Npcap 的机器上 Coast 照常启动（只是网关不可用）。原先靠 MSVC 的 /DELAYLOAD 做到
    # 这一点，但那条件只覆盖 MSVC：MinGW 构建把 wpcap.dll 写进导入表，无 Npcap 的机器上进程
    # 直接 0xC0000135 起不来。改成运行时绑定后两种工具链行为一致，也不再需要 delayimp。
    if(WIN32)
        set(NPCAP_SDK "$ENV{NPCAP_SDK}")
        target_include_directories(clashauto-qml PRIVATE "${NPCAP_SDK}/Include")
        target_link_libraries(clashauto-qml PRIVATE iphlpapi ws2_32)
    endif()
else()
    target_sources(clashauto-qml PRIVATE src/net/LanGateway_stub.cpp)
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
    # wintun.dll(按架构) 仅 Windows 构建内嵌（qrc），运行时释放到核心旁供 TUN 使用；mac/Linux 不含。
    target_sources(clashauto-qml PRIVATE resources_win.qrc)
endif()

target_link_libraries(clashauto-qml PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    # OpenSSL 是**可选**依赖，见上方 COAST_HAVE_OPENSSL 条件块（不在这里无条件链接）。
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

# CoastCore 需要裸 AEAD / X25519 的部分（ss / vmess / reality + 加密 KAT 自测）：仅在 OpenSSL 可用时
# 编入 + 定义 COAST_HAVE_OPENSSL（见 OutboundRegistry.cpp / main_qml.cpp 的 #ifdef）。
# 不可用时这三个协议不注册 → 拨号侧回退 mihomo；direct/trojan/vless 不受影响（只用 QSslSocket + Qt 哈希）。
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
    target_compile_definitions(clashauto-qml PRIVATE COAST_HAVE_OPENSSL)
endif()

# CoastCore QUIC 系（Hysteria2 / TUIC）：仅在找到 msquic 时编入 + 定义 COAST_HAVE_QUIC（见
# src/net/core/proto/OutboundRegistry.cpp 的 #ifdef）。找不到则整块不编，hysteria2/tuic 未注册、回退 mihomo。
if(COAST_HAVE_QUIC)
    target_sources(clashauto-qml PRIVATE
        src/net/core/transport/QuicTransport.cpp  src/net/core/transport/QuicTransport.h
        src/net/core/proto/Hysteria2Outbound.cpp  src/net/core/proto/Hysteria2Outbound.h
        src/net/core/proto/TuicOutbound.cpp       src/net/core/proto/TuicOutbound.h)
    target_link_libraries(clashauto-qml PRIVATE ${COAST_MSQUIC_LINK})
    # Windows 还得链 ntdll：msquic 的 msquic_winuser.h 里 QuicAddrFromString 是**头文件内联**实现，
    # 内部调用 RtlIpv4StringToAddressExA / RtlIpv6StringToAddressExA —— 这俩符号只在 ntdll.lib 里。
    # QuicTransport.cpp 用了 QuicAddrFromString（openConnection 把 IP 字面量钉成
    # QUIC_PARAM_CONN_REMOTE_ADDRESS），于是这个依赖被内联进**我们的**目标文件，不链 ntdll
    # 就 LNK2019（CI 两个架构实测都挂）。注意这是**链接期**问题：MinGW 本地 -fsyntax-only
    # 语法检查复现不出来 —— 不是我们直接调的 API，别删。
    if(WIN32)
        target_link_libraries(clashauto-qml PRIVATE ntdll)
    endif()
    if(COAST_MSQUIC_INCLUDE_DIR)
        target_include_directories(clashauto-qml PRIVATE ${COAST_MSQUIC_INCLUDE_DIR})
    endif()
    target_compile_definitions(clashauto-qml PRIVATE COAST_HAVE_QUIC QUIC_API_ENABLE_PREVIEW_FEATURES)

    # TUIC 的 token 要 msquic 的 keying-material exporter。**别只靠 QUIC_API_ENABLE_PREVIEW_FEATURES
    # 判断**——那个宏只表示「请求打开 preview 分支」，被定义 ≠ 这套 API 真的存在：拿最新发布版
    # msquic v2.5.9 的真头编译（树莓派实测）时，即便定义了它，QUIC_KEYING_MATERIAL_CONFIG 与
    # QUIC_API_TABLE::ConnectionExportKeyingMaterial 依然不存在，QuicTransport.cpp 直接编不过。
    # 所以照 OpenSSL 那套「真试一次」的办法：能编过才定义 COAST_HAVE_QUIC_KEYING；
    # 探不到时 TUIC 会在 token 长度检查处明确失败（Hysteria2 不受影响，它不用 exporter）。
    include(CMakePushCheckState)
    include(CheckCXXSourceCompiles)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES ${COAST_MSQUIC_LINK})
    set(CMAKE_REQUIRED_DEFINITIONS -DQUIC_API_ENABLE_PREVIEW_FEATURES)
    if(COAST_MSQUIC_INCLUDE_DIR)
        set(CMAKE_REQUIRED_INCLUDES ${COAST_MSQUIC_INCLUDE_DIR})
    endif()
    check_cxx_source_compiles("
        #include <msquic.h>
        int main() {
            QUIC_KEYING_MATERIAL_CONFIG cfg{};
            const QUIC_API_TABLE *t = nullptr;
            return (t && t->ConnectionExportKeyingMaterial && cfg.OutputLength) ? 0 : 1;
        }
    " COAST_QUIC_KEYING_OK)
    cmake_pop_check_state()
    if(COAST_QUIC_KEYING_OK)
        target_compile_definitions(clashauto-qml PRIVATE COAST_HAVE_QUIC_KEYING)
        message(STATUS "msquic keying-material exporter 可用 → TUIC token 可导出")
    else()
        message(STATUS "msquic 无 keying-material exporter → TUIC 认证不可用(Hysteria2 不受影响)")
    endif()
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
