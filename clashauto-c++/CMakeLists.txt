cmake_minimum_required(VERSION 3.20)

project(clashauto_cpp VERSION 0.1.1 LANGUAGES CXX)

# Version string shown in the app UI. Defaults to the project version for
# local builds; CI overrides it with -DAPP_VERSION=<major.minor.commitcount>.
if(NOT APP_VERSION)
    set(APP_VERSION "${PROJECT_VERSION}")
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
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.clashrauto.clashauto"
        MACOSX_BUNDLE_BUNDLE_VERSION "${APP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_VERSION}")
    # CI 在配置前用 assets/icon.ico 生成 icon.icns；存在则作为应用图标打进 Resources
    set(APP_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns")
    if(EXISTS "${APP_ICNS}")
        set_target_properties(clashauto-cpp PROPERTIES MACOSX_BUNDLE_ICON_FILE "icon.icns")
        target_sources(clashauto-cpp PRIVATE "${APP_ICNS}")
        set_source_files_properties("${APP_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    endif()
endif()

# Generate Version.h with the resolved APP_VERSION (bulletproof quoting across generators)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
    @ONLY
)
target_include_directories(clashauto-cpp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
