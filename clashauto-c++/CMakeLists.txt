cmake_minimum_required(VERSION 3.20)

project(clashauto_cpp VERSION 0.1.1 LANGUAGES CXX)

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
    src/qml/UpdateController.cpp        src/qml/UpdateController.h
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
)

# Windows：把 assets/icon.ico 作为 exe 图标嵌入。
if(WIN32)
    set(APP_ICON_ABS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
        @ONLY)
    target_sources(clashauto-qml PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
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

# macOS：打成「Clash Auto.app」bundle；Clashr-Auto 运行时资源由 CI 放进 Contents/Clashr-Auto。
if(APPLE)
    set_target_properties(clashauto-qml PROPERTIES
        OUTPUT_NAME "Clash Auto"
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "Clash Auto"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.yuehongsun.auto"
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
    set_target_properties(clashauto-helper PROPERTIES OUTPUT_NAME "com.yuehongsun.auto.helper")
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
            "$<TARGET_BUNDLE_CONTENT_DIR:clashauto-qml>/MacOS/com.yuehongsun.auto.helper"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/helper/com.yuehongsun.auto.helper.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:clashauto-qml>/Library/LaunchDaemons/com.yuehongsun.auto.helper.plist"
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
