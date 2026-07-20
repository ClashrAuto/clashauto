cmake_minimum_required(VERSION 3.20)

project(clashauto_cpp VERSION 0.1.1 LANGUAGES CXX)

# Version string shown in the app UI. Defaults to the project version for
# local builds; CI overrides it with -DAPP_VERSION=<major.minor.commitcount>.
if(NOT APP_VERSION)
    set(APP_VERSION "${PROJECT_VERSION}")
endif()

# macOS 需要 Objective-C++（.mm）来调 SMAppService / NSXPCConnection（特权 helper）
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

add_executable(clashauto-cpp
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/TrafficChart.h
    src/TrafficChart.cpp
    src/ClashService.h
    src/ClashService.cpp
    src/AppConfig.h
    src/AppConfig.cpp
    src/CoreController.h
    src/CoreController.cpp
    src/TrayController.h
    src/TrayController.cpp
    src/ConfigBuilder.h
    src/ConfigBuilder.cpp
    src/SubscriptionStore.h
    src/SubscriptionStore.cpp
    src/SubParser.h
    src/SubParser.cpp
    resources.qrc
)

# Windows：把 assets/icon.ico 作为 exe 图标嵌入（否则桌面/资源管理器里图标是空的）。
# 用 configure_file 把图标绝对路径填进 .rc，避免 RC 编译器工作目录不同而找不到图标；
# MSVC(rc.exe) 与 MinGW(windres) 都接受正斜杠绝对路径。
if(WIN32)
    set(APP_ICON_ABS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
        @ONLY)
    target_sources(clashauto-cpp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
endif()

target_link_libraries(clashauto-cpp PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    $<$<PLATFORM_ID:Windows>:dwmapi>
    $<$<PLATFORM_ID:Windows>:shell32>   # ShellExecuteEx（runas 提权重启）
    $<$<PLATFORM_ID:Windows>:advapi32>  # OpenProcessToken/GetTokenInformation（提权判定）
    $<$<PLATFORM_ID:Windows>:wininet>   # InternetSetOption（原生设置系统代理，替代捆绑 sysproxy.exe）
)

target_compile_options(clashauto-cpp PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

# GUI 子系统：Windows 上不弹控制台窗口（CLI 子命令改由 main.cpp 附着父控制台输出）
set_target_properties(clashauto-cpp PROPERTIES WIN32_EXECUTABLE ON)

# macOS：打成「Clash Auto.app」bundle（Finder/Dock 正常显示，DMG 拖拽安装）。
# Clashr-Auto 运行时资源由 CI 放进 Contents/Clashr-Auto——AppConfigLoader 从
# exe 目录(Contents/MacOS)向上探测 ../Clashr-Auto，第一跳即命中，拖进 /Applications 也不丢。
if(APPLE)
    set_target_properties(clashauto-cpp PROPERTIES
        OUTPUT_NAME "Clash Auto"
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "Clash Auto"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.yuehongsun.auto"
        MACOSX_BUNDLE_BUNDLE_VERSION "${APP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_VERSION}")
    # CI 在配置前用 assets/icon.ico 生成 icon.icns；存在则作为应用图标打进 Resources
    set(APP_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns")
    if(EXISTS "${APP_ICNS}")
        set_target_properties(clashauto-cpp PROPERTIES MACOSX_BUNDLE_ICON_FILE "icon.icns")
        target_sources(clashauto-cpp PRIVATE "${APP_ICNS}")
        set_source_files_properties("${APP_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    endif()

    # 系统代理改经 SystemConfiguration + Security（AuthorizationRef 授权），需链接这两个框架；
    # CoreFoundation 提供 CFDictionary/CFString 等类型（Qt 已间接带上，这里显式声明保稳）。
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(SYSTEMCONFIGURATION_FRAMEWORK SystemConfiguration REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(SERVICEMANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)

    # 应用侧特权 helper 客户端（SMAppService 注册 + XPC）。ARC 省心管理 Obj-C 对象生命周期，
    # 避免 __block/NSXPCConnection 在 MRR 下的悬垂。
    target_sources(clashauto-cpp PRIVATE src/MacHelperClient.mm src/MacHelperClient.h)
    set_source_files_properties(src/MacHelperClient.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

    target_link_libraries(clashauto-cpp PRIVATE
        ${SECURITY_FRAMEWORK}
        ${SYSTEMCONFIGURATION_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${SERVICEMANAGEMENT_FRAMEWORK})

    # 特权 helper（以 root 运行的 launchd daemon）：独立可执行，只依赖 Foundation（NSXPC）。
    # 继承全局 CMAKE_OSX_ARCHITECTURES / DEPLOYMENT_TARGET，故 CI 里同样是 universal。
    add_executable(clashauto-helper helper/helper_main.mm)
    set_target_properties(clashauto-helper PROPERTIES OUTPUT_NAME "com.yuehongsun.auto.helper")
    # 复用应用生成的 Version.h（#define APP_VERSION）作为 helper 版本，避免 compile-def 的跨生成器引号坑
    target_include_directories(clashauto-helper PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/helper"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set_source_files_properties(helper/helper_main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(clashauto-helper PRIVATE
        ${FOUNDATION_FRAMEWORK}
        ${SYSTEMCONFIGURATION_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK})

    # 把 helper 可执行 + launchd plist 嵌进 app bundle（SMAppService 要求的布局）。
    # 本地构建即嵌入；CI 在 macdeployqt 之后会再覆盖一次并做 inside-out 签名（见 release.yml）。
    add_dependencies(clashauto-cpp clashauto-helper)
    add_custom_command(TARGET clashauto-cpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:clashauto-cpp>/Library/LaunchDaemons"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clashauto-helper>"
            "$<TARGET_BUNDLE_CONTENT_DIR:clashauto-cpp>/MacOS/com.yuehongsun.auto.helper"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/helper/com.yuehongsun.auto.helper.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:clashauto-cpp>/Library/LaunchDaemons/com.yuehongsun.auto.helper.plist"
        VERBATIM)
endif()

# Generate Version.h with the resolved APP_VERSION (bulletproof quoting across generators)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
    @ONLY
)
target_include_directories(clashauto-cpp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
