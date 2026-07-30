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
