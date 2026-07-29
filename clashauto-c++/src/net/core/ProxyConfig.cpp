#include "ProxyConfig.h"

// ProxyConfig / ProxyNode 是纯值类型，ProxyConfigStore 的方法都短到直接内联在头里了 ——
// 本 .cpp 目前没有需要外联的定义，存在的意义有二：
//   1) 给这几个类型一个独立的翻译单元，AUTOMOC 生成的 moc_ProxyConfig.cpp 会被一并编进来；
//   2) 后续单元（订阅/YAML → ProxyNode 解析、模式/选中节点的持久化等）落地时，实现放这里，
//      不必再动 CMake。
