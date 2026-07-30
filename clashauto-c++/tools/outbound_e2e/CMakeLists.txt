cmake_minimum_required(VERSION 3.16)
project(obh LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL REQUIRED)
# 出站层源码根：默认取仓库里的 src/net（本文件在 clashauto-c++/tools/outbound_e2e/ 下）。
# 也可 -DCOAST_SRC_NET=<path> 指到别处（例如解包到 /tmp 的一份）。
if(NOT COAST_SRC_NET)
    set(COAST_SRC_NET "${CMAKE_CURRENT_SOURCE_DIR}/../../src/net")
endif()
set(B ${COAST_SRC_NET})
add_executable(obh main.cpp
  ${B}/IOutbound.h
  ${B}/core/ProxyConfig.cpp ${B}/core/ProxyConfig.h
  ${B}/core/crypto/Aead.cpp ${B}/core/crypto/Kdf.cpp
  ${B}/core/transport/TlsClient.cpp ${B}/core/transport/WsClient.cpp ${B}/core/transport/UtlsClient.cpp
  ${B}/core/proto/OutboundRegistry.cpp
  ${B}/core/proto/ShadowsocksOutbound.cpp ${B}/core/proto/TrojanOutbound.cpp
  ${B}/core/proto/VlessOutbound.cpp ${B}/core/proto/VmessOutbound.cpp ${B}/core/proto/RealityOutbound.cpp)
target_include_directories(obh PRIVATE ${B} ${B}/core)
target_link_libraries(obh PRIVATE Qt6::Core Qt6::Network OpenSSL::Crypto)
target_compile_definitions(obh PRIVATE COAST_HAVE_OPENSSL)

# ── QUIC 系（Hysteria2 / TUIC）：找到 msquic 才编入，与主工程 CMakeLists 同一套策略 ──
# 用 -DCOAST_MSQUIC_ROOT=/opt/msquic 指一份自备 SDK（含 include/ 与 lib/）。
# libmsquic 的官方 deb **只装 .so、不装头**，所以 include 目录通常要自己从对应 tag 取
# （msquic.h / msquic_posix.h / quic_platform_posix.h / quic_sal_stub.h，见 README）。
find_path(OBH_MSQUIC_INCLUDE_DIR msquic.h HINTS ${COAST_MSQUIC_ROOT} $ENV{COAST_MSQUIC_ROOT}
          PATH_SUFFIXES include msquic)
find_library(OBH_MSQUIC_LIBRARY NAMES msquic HINTS ${COAST_MSQUIC_ROOT} $ENV{COAST_MSQUIC_ROOT}
             PATH_SUFFIXES lib)
if(OBH_MSQUIC_INCLUDE_DIR AND OBH_MSQUIC_LIBRARY)
    message(STATUS "obh: msquic → ${OBH_MSQUIC_LIBRARY}；编入 Hysteria2/TUIC 用例")
    target_sources(obh PRIVATE
        ${B}/core/transport/QuicTransport.cpp ${B}/core/transport/QuicTransport.h
        ${B}/core/proto/Hysteria2Outbound.cpp ${B}/core/proto/TuicOutbound.cpp)
    target_include_directories(obh PRIVATE ${OBH_MSQUIC_INCLUDE_DIR})
    target_link_libraries(obh PRIVATE ${OBH_MSQUIC_LIBRARY})
    target_compile_definitions(obh PRIVATE COAST_HAVE_QUIC QUIC_API_ENABLE_PREVIEW_FEATURES)
    # TUIC 的 token 要 msquic 的 keying-material exporter（**v2.6+**，2.5.9 没有）。
    # 照主工程那套「真试一次编译」的办法探测，能过才定义 COAST_HAVE_QUIC_KEYING。
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES ${OBH_MSQUIC_LIBRARY})
    set(CMAKE_REQUIRED_INCLUDES ${OBH_MSQUIC_INCLUDE_DIR})
    set(CMAKE_REQUIRED_DEFINITIONS -DQUIC_API_ENABLE_PREVIEW_FEATURES)
    check_cxx_source_compiles("
        #include <msquic.h>
        int main(){ QUIC_KEYING_MATERIAL_CONFIG c{}; const QUIC_API_TABLE*t=nullptr;
                    return (int)(bool)t->ConnectionExportKeyingMaterial + (int)c.BufferLength; }
    " OBH_QUIC_KEYING_OK)
    unset(CMAKE_REQUIRED_LIBRARIES)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_DEFINITIONS)
    if(OBH_QUIC_KEYING_OK)
        target_compile_definitions(obh PRIVATE COAST_HAVE_QUIC_KEYING)
        message(STATUS "obh: msquic 带 keying exporter → TUIC 认证可测")
    else()
        message(STATUS "obh: msquic 无 keying exporter（需 v2.6+）→ TUIC 用例会因缺 token 而跳过")
    endif()
else()
    message(STATUS "obh: 未找到 msquic → 只测非 QUIC 协议（用 -DCOAST_MSQUIC_ROOT=... 指一份）")
endif()
