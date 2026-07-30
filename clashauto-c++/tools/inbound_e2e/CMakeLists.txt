cmake_minimum_required(VERSION 3.16)
project(ibh LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL REQUIRED)

# 源码根：默认取仓库里的 src/net（本文件在 clashauto-c++/tools/inbound_e2e/ 下）。
if(NOT COAST_SRC_NET)
    set(COAST_SRC_NET "${CMAKE_CURRENT_SOURCE_DIR}/../../src/net")
endif()
set(B ${COAST_SRC_NET})

# 注意：这里链的是**生产代码本身**，不是任何桩。CoreDialerFactory 会 include OutboundRegistry，
# 后者引用全部协议实现，所以协议源文件都要编进来（即使本测试只走 DIRECT）。
add_executable(ibh main.cpp
  ${B}/IOutbound.h
  ${B}/inbound/MixedInbound.cpp   ${B}/inbound/MixedInbound.h
  ${B}/GatewayDiag.cpp            ${B}/GatewayDiag.h
  ${B}/core/ProxyConfig.cpp       ${B}/core/ProxyConfig.h
  ${B}/core/CoreDialerFactory.cpp ${B}/core/CoreDialerFactory.h
  ${B}/core/CoreRouter.cpp        ${B}/core/CoreRouter.h
  ${B}/core/RuleEngine.cpp        ${B}/core/RuleEngine.h
  ${B}/core/DnsResolver.cpp       ${B}/core/DnsResolver.h
  ${B}/core/DirectOutbound.cpp    ${B}/core/DirectOutbound.h
  ${B}/core/crypto/Aead.cpp ${B}/core/crypto/Kdf.cpp
  ${B}/core/transport/TlsClient.cpp ${B}/core/transport/WsClient.cpp ${B}/core/transport/UtlsClient.cpp
  ${B}/core/proto/OutboundRegistry.cpp
  ${B}/core/proto/ShadowsocksOutbound.cpp ${B}/core/proto/TrojanOutbound.cpp
  ${B}/core/proto/VlessOutbound.cpp ${B}/core/proto/VmessOutbound.cpp
  ${B}/core/proto/RealityOutbound.cpp)
target_include_directories(ibh PRIVATE ${B} ${B}/core)
target_link_libraries(ibh PRIVATE Qt6::Core Qt6::Network OpenSSL::Crypto)
target_compile_definitions(ibh PRIVATE COAST_HAVE_OPENSSL)

# QUIC 系（Hysteria2/TUIC）不编入：本台子只走 DIRECT，msquic 是可选依赖。
# 需要连 QUIC 一起验时用 tools/outbound_e2e（那边有完整的 msquic 探测逻辑）。
