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

#include <QHash>
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

// 从 full.yaml 的 `proxy-groups:` 解析出「策略组 → 叶子」映射。**mihomo 不可用时的兜底。**
//
// ★ 为什么需要：Rule 模式的规则 target 绝大多数是**组名**（🎯 全球直连 / 🚀 节点选择 …），
//   没有这张表就 nodeByName 必失配、整类回退核心。平时这张表来自 mihomo 的 REST
//   （ClashService::groupLeafMap，那是**权威**的——它含用户手选、并由核心的 store-selected 持久化）。
//   但那正是数据面最后一条隐藏的 mihomo 依赖：核心一停，Rule 模式就解析不出目标、全部回退到
//   一个已经不在的核心。本函数从我们**自己生成**的 full.yaml 里把组结构解析出来顶上。
//
// 解析规则（对齐 mihomo 在「用户没手选过」时的行为）：组的叶子 = 其 `proxies:` 列表的**第一项**；
// 该项若还是组名就继续往下走，直到落到具体节点名或内建 DIRECT/REJECT。带环检测（组互相引用时
// 该组判为不可解析，直接不收录 → 调用方回退核心，安全）。
//
// ⚠️ 局限（务必知情）：它给的是**默认选择**，不是用户手选。所以只在 mihomo 的权威表拿不到时才用。
QHash<QString, QString> parseProxyGroupsLeaf(const QString &fullYaml);

// 正确性自测（COAST_PROXYCFG_SELFTEST=1，见 main_qml.cpp）：用内置样例 YAML（ss / vmess / vless /
// vless+reality / trojan / hysteria2 / tuic 各一条，含 ws-opts、sni、reality-opts、alpn，两种缩进形状）
// 解析并逐字段断言。全过返回 true；有失败项打到 stderr 并返回 false。不建 GUI、不碰网络。
bool proxyConfigSelfTest();

} // namespace coastcore
