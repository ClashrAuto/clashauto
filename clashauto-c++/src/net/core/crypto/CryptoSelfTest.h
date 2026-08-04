#pragma once

// 加密层已知答案测试（KAT）——AEAD / HKDF / EVP_BytesToKey 的正确性自检。
//
// ★ 为什么必须有：加密写错是**静默**的 —— seal 照样吐出一串字节、程序照跑，但要么与对端(mihomo /
//   远端服务器)算出的不一致（连不通、还难定位），要么更糟：把认证/nonce 用错导致「看似加密实则可破」。
//   编译器一个字都帮不上。唯一的防线是拿公开测试向量（RFC/NIST）逐字节比对。
//
// 用法：进程启动早期若 env COAST_CRYPTO_SELFTEST 置位，就调 runCryptoSelfTest()，把逐项 PASS/FAIL
// 打到 stderr，返回值作为进程退出码（0=全过，非 0=有失败）。接线方式对齐项目里 COAST_DEVICEDB_SELFTEST
// 等既有钩子（见 main_qml.cpp）。同一个函数也被 tools/ 下的独立 KAT 驱动复用（本机整程序因 pcap.h
// 链不动，用小驱动单独验向量）。
//
// 只依赖 Aead/Kdf + Qt Core，跨平台、无 GUI、无网络。
namespace coastcore {

// 跑全部 KAT。返回 0 全过；返回失败的用例个数（>0）。结果打到 stderr。
int runCryptoSelfTest();

} // namespace coastcore
