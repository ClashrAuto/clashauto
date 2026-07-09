cmake_minimum_required(VERSION 3.20)

project(clashauto_cpp VERSION 0.1.0 LANGUAGES CXX)

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
    resources.qrc
)

target_link_libraries(clashauto-cpp PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

target_compile_options(clashauto-cpp PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
