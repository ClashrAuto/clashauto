#pragma once

// 分流路由：给「目的地 + 设备身份」定一个出站节点名。**CoastCore 的唯一路由实现。**
//
// ★ 为什么要单独一个文件：这段逻辑原本是 `LanGateway_linux.cpp` 里的一个 lambda，只有局域网网关
//   用得到。现在本机入站（`net/inbound/MixedInbound`）也要按同一套规则分流 —— 而它是跨平台的，
//   够不着那个 Linux-only 的文件。复制一份是最糟的选择：分流规则一旦有两份实现，
//   「本机走了 A 节点、设备走了 B 节点」这类问题会极难查，而且两边永远在慢慢漂移。
//   所以把它提到这里，两个入口共用一份。
//
// 语义（**保持与网关原实现逐字一致**，改动务必同时改两个调用方的期望）：
//   返回节点名（"DIRECT" 或具体节点名）= 走进程内出站；
//   返回**空串** = 「判不了/不该由我们判」→ 交回退工厂（现状是 mihomo）。
//   严格模式下 CoreDialerFactory 会把「空串」变成明确失败而不是静默回退，见其 setStrict。
#include "CoreDialerFactory.h"

#include <memory>

class ProxyConfigStore;
class RuleEngine;

namespace coastcore {

// 造一个可直接喂给 CoreDialerFactory::setRouter 的路由函数。
// store/rules 以 shared_ptr 捕获，保证在数据面线程上被同步调用时始终存活。
// rules 可为空（非 Rule 模式用不到它；Rule 模式下为空则一律回退）。
// debugLog：打开后每条连接打印判定链路（命中的 target → 解析出的节点），排查
//「为什么回退 / 为什么走了这个节点」全靠它。网关侧接的是 COAST_GATEWAY_DEBUG。
CoreDialerFactory::Router makeRouter(std::shared_ptr<ProxyConfigStore> store,
                                     std::shared_ptr<RuleEngine> rules, bool debugLog = false);

// 自测：用合成的 ProxyConfig + RuleEngine 钉住上面那套「什么时候回退」的判定。
// 这段逻辑一旦松掉，后果是**误路由**（本该走节点的走了直连，或反过来），比崩溃更难发现。
bool routerSelfTest();

} // namespace coastcore
