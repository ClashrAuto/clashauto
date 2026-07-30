#pragma once

// Clash 风格 `proxies:` YAML  →  CoastCore 的 ProxyNode 列表 / ProxyConfig 快照。
//
// 定位（#10 激活的基础子件 10a）：本文件**只**做「文本 → 结构」这一步纯解析，**不接线到任何运行路径**
//（NetStack / LanGateway / 拨号侧的接线是后续子件的事）。它把 app 侧既有的两种 proxies YAML 形状——
//   (A) SubParser::toClashProxies 产出的「块式 + 单行 flow 值」（列表项 2 空格、字段 4 空格，
//       嵌套项如 ws-opts / reality-opts / alpn 一律写成一行 inline flow）；
//   (B) SubscriptionStore::parseProxyList 产出的「重缩进 + 追加 use:」（列表项 4 空格、字段 6 空格，
//       多一条 `use: true/false`）——
// 都能吃进来，逐条映射成 ProxyNode。字段名（realityPublicKey/wsPath/... ）与拨号侧各 *Outbound 从
// ProxyNode 取用的字段一一对齐（见 proto/RealityOutbound.cpp 等）。
//
// 约束：纯 Qt Core（QString/QRegularExpression 手工切分，全程当文本处理，无 YAML 库——与 CLAUDE.md
// 「YAML is manipulated as text」的既有风格一致），无新依赖。

#include "ProxyConfig.h"

#include <QString>
#include <QVector>

#include <memory>

namespace coastcore {

// 逐条解析 proxies 列表项 → ProxyNode。
//   · proxiesYaml 可带 `proxies:` 表头（形状 A / 完整 clash yaml），也可是无表头的裸列表（形状 B）。
//   · 单条解析不出（缺 name/server/port，或整体畸形）→ **跳过该条**，绝不因一条坏节点丢整表；
//     warn 非空时把「skipped N node(s)」之类的计数/摘要写进去。
// 返回不含内建 DIRECT（DIRECT 由 buildProxyConfig 追加），顺序保持 YAML 内顺序。
QVector<ProxyNode> parseClashProxies(const QString &proxiesYaml, QString *warn = nullptr);

// 便捷：parseClashProxies + 追加内建 DIRECT（若列表里没有同名节点）+ 组装成一份不可变 ProxyConfig 快照。
//   selected 为当前选中节点名；mode 为分流模式。返回的 shared_ptr 可直接喂给 ProxyConfigStore::reload。
std::shared_ptr<const ProxyConfig> buildProxyConfig(const QString &proxiesYaml, const QString &selected,
                                                    ProxyConfig::Mode mode, QString *warn = nullptr);

// 正确性自测（COAST_PROXYCFG_SELFTEST=1，见 main_qml.cpp）：用内置样例 YAML（ss / vmess / vless /
// vless+reality / trojan / hysteria2 / tuic 各一条，含 ws-opts、sni、reality-opts、alpn，两种缩进形状）
// 解析并逐字段断言。全过返回 true；有失败项打到 stderr 并返回 false。不建 GUI、不碰网络。
bool proxyConfigSelfTest();

} // namespace coastcore
