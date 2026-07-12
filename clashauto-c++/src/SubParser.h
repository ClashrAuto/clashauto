#pragma once

#include <QString>

// 本地订阅解析：把订阅原始内容转换成 Clash/mihomo 的 `proxies:` YAML 块。
// - 若内容已是 Clash YAML（含 proxies:）→ 原样返回；
// - 否则按需 base64 解码，逐条解析 ss/ssr/vmess/trojan/vless/hysteria/hysteria2/tuic 链接，
//   以「块式 + 单行 flow 值」输出（嵌套项如 ws-opts 写成一行，兼容现有扁平化管线）。
// 解析不了的单条链接直接跳过；无任何可解析节点时返回空串。
namespace SubParser {
QString toClashProxies(const QString &rawContent);
}
