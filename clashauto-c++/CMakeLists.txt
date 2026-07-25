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
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Widgets Network Qml Quick QuickControls2)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Qml Quick QuickControls2)

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
    src/TrayController.cpp   src/TrayController.h
    # 设备页后端（局域网发现 + 台账；框架无关）
    src/LanScanner.cpp       src/LanScanner.h
    src/DeviceStore.cpp      src/DeviceStore.h
    # 透明网关——跨平台可编译层。L2 工厂按平台各一份实现（Linux=AF_PACKET / mac=BPF /
    # Windows=Npcap待做，现桩 nullptr）；每个 .cpp 用 #if <os> 守卫，同一平台只有一个定义。
    src/net/IL2Endpoint.h
    src/net/L2Endpoint_linux.cpp
    src/net/L2Endpoint_mac.cpp
    src/net/L2Endpoint_win.cpp
    src/net/ArpSpoofer.cpp   src/net/ArpSpoofer.h
    src/net/Socks5Client.cpp src/net/Socks5Client.h
    src/net/LanGateway.h
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
        qml/DevicesPage.qml
        qml/DeviceRow.qml
        qml/SubscriptionsPage.qml
        qml/SettingsPage.qml
        qml/LogsPage.qml
        qml/AboutPage.qml
        qml/Card.qml
        qml/NavButton.qml
        qml/FooterSwitch.qml
        qml/MetricCard.qml
        qml/NodeRow.qml
        qml/BandwidthChart.qml
        qml/LogTimeline.qml
        qml/ConnectionsWindow.qml
        qml/UpdateWindow.qml
        qml/RuleEditorWindow.qml
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
    target_sources(clashauto-qml PRIVATE
        src/net/NetStack.cpp src/net/NetStack.h
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
        ${LWIP_DIR}/netif/ethernet.c
    )
    target_include_directories(clashauto-qml PRIVATE
        "${LWIP_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/net/lwip_port")
    # Windows：Npcap SDK（env NPCAP_SDK 指向解压目录）+ 链接。按目标架构选 Lib 子目录。
    if(WIN32)
        set(NPCAP_SDK "$ENV{NPCAP_SDK}")
        if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
            set(_npcap_lib "${NPCAP_SDK}/Lib/ARM64")
        else()
            set(_npcap_lib "${NPCAP_SDK}/Lib/x64")
        endif()
        target_include_directories(clashauto-qml PRIVATE "${NPCAP_SDK}/Include")
        target_link_directories(clashauto-qml PRIVATE "${_npcap_lib}")
        target_link_libraries(clashauto-qml PRIVATE wpcap Packet iphlpapi ws2_32)
        # 延迟加载 Npcap DLL：没装 Npcap 的机器上 Coast 仍能启动（否则缺 wpcap.dll 直接起不来），
        # 仅在真正用网关时按需加载；加载不到则优雅失败（见 L2Endpoint_win 的 LoadLibrary 检查）。
        if(MSVC)
            target_link_options(clashauto-qml PRIVATE /DELAYLOAD:wpcap.dll /DELAYLOAD:Packet.dll)
            target_link_libraries(clashauto-qml PRIVATE delayimp)
        endif()
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
    $<$<PLATFORM_ID:Windows>:dwmapi>
    $<$<PLATFORM_ID:Windows>:shell32>
    $<$<PLATFORM_ID:Windows>:advapi32>
    $<$<PLATFORM_ID:Windows>:wininet>
)

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
        src/MacSpeedItem.mm src/MacSpeedItem.h)
    set_source_files_properties(src/MacHelperClient.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties(src/MacWindow.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties(src/MacSpeedItem.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

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
