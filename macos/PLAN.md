# Coast for macOS — Swift 重构路线图

> **新接手者请先看 [`README.md`](README.md)** —— 那是入口文档(架构/构建/设计决策)。
> 本文是**过程账本**:逐步的进度与踩过的坑。
>
> **这份文件是「可续跑」的进度锚点。** 每次迭代先读它，找到第一个未打勾的条目继续做，
> 做完当场把勾打上并追加「实现要点/坑」。不要凭记忆判断进度。

目标：在 `macos/` 下用 **Swift 6 + SwiftUI** 完全重写 `clashauto-c++/` 的 macOS 端，
不再依赖 Qt。行为对齐现有 Qt/QML 版（同一套 `~/Library/Application Support/Coast` 数据目录、
同一个 mihomo 核心、同一套 REST 端口与配置文件格式），UI 用原生 macOS 观感重做。

## 基线分支

**本重构基于 `master`（本仓库的主干，没有叫 `main` 的分支）。** `macos` 分支已于
2026-07-31 从 `master` 重建。此前它挂在 `feat/coastcore-outbound` 那条线上（比 master 多 101
个提交，全是 CoastCore 进程内数据面 + TUN 的工作），那些提交原样保留在
`feat/coastcore-outbound`（本地与 origin 都有），并额外打了 `macos-before-rebase` 标签。

**这条基线的直接后果**（别照着 CoastCore 分支的印象写代码）：
- `AppConfig` 里**没有** `coastcore` / `coastcore_strict` / `coastcore_inbound` 三个键；
- `ClashService` 里**没有** `groupLeafMap`（那是给进程内规则解析用的）；
- `src/net/**` 在 master 上只有 26 个文件 / 约 9.5k 行，是**局域网网关**那套
  （ARP/NDP 欺骗、L2 端点、lwIP、Socks5 客户端），**不包含**进程内出站引擎
  （`net/core/**`、`net/inbound/**`、QUIC/TLS 传输层等在 master 上根本不存在）。

## 环境

- Swift 6.3.3 / Xcode 26.6 / macOS 26.5（本机可直接 `swift build`，与 Qt 端不同，**本地可编译可运行**）
- 构建：`cd macos && swift build`；打包 .app：`bash macos/scripts/make_app.sh`

## 范围划线

| Qt 端模块 | Swift 端处置 |
|---|---|
| `AppConfig` / `AppConfigLoader` | ✅ 端口，保持同样的 YAML 文本读写语义 |
| `ClashService`（REST 轮询/测速） | ✅ 端口为 async/await + URLSession |
| `CoreController`（进程/系统代理/TUN） | ✅ 端口，系统代理走 SCPreferences + root helper |
| `ConfigBuilder` / `SubscriptionStore` / `SubParser` | ✅ 端口（YAML 仍按文本手术，不引 YAML 库） |
| `DeviceStore` / `HistoryStore`（SQLite） | ✅ 端口到 GRDB-free 的原生 sqlite3 封装 |
| `TrayController` | ✅ 端口为 `NSStatusItem` |
| QML 页面（Status/Nodes/Devices/Subs/Settings/Logs/About） | ✅ 用 SwiftUI 重做 |
| `helper/`（root helper，XPC） | ✅ 复用同一 mach service 名与协议，用 Swift 重写 |
| `src/net/**`（局域网网关：ARP/NDP、L2 端点、lwIP、Socks5，约 9.5k 行） | ⛔ **暂不移植**。这套是「把局域网设备的流量引到本机」用的，与桌面端自用无关；等 DevicesPage 真要联动时再评估。 |
| Widgets 死代码（`MainWindow.cpp` 等） | ⛔ 不移植 |

## 阶段与进度

### 阶段 0 — 脚手架
- [x] 建 `macos/` 骨架、`Package.swift`、PLAN.md
- [x] `AppPaths`（userDir/configDir，与 Qt 端同路径）
- [x] `YAMLText`（正则式 YAML 读写工具，对齐 `AppConfigLoader` 的 5 个 helper）
- [x] `AppConfig` + 加载/落盘（含首次 seed、mini 归一化、secret 随机生成）
- [x] 种子资源打包（config.yaml/default.yaml/plugin.yaml/subscribe.yaml/Country.mmdb）
- [x] 空壳 SwiftUI App 能 `swift build` 通过

### 阶段 1 — 后端：核心进程与 REST
- [x] `ClashAPI`：`/traffic` 常开流、`/connections`、`/proxies`、`/configs`、`DELETE /connections`
- [x] `ClashService`：轮询编排、组/叶子映射、模式同步、节点选择、清连接
- [x] 延迟测速 + 下载测速（并发 5、选组串行锁，对齐 C++ 语义）
- [x] `CoreProcess`：启停 mihomo（`-d userDir -f configDir/full.yaml`）、日志转发
- [x] 核心下载器（首次运行拉 mihomo 二进制）

### 阶段 2 — 后端：配置生成与订阅
- [x] `SubParser`：ss/ssr/vmess/vless/trojan/hysteria2/tuic 链接解析（含 sing-box JSON）
- [x] `SubscriptionStore`：`subscribe.yaml` 读写、远程/本地拉取、增量更新、允许/排除过滤
- [x] `ConfigBuilder`：`full.yaml` 生成（base + plugin DNS/TUN + 节点 + 策略组 + 地区分组）
- [x] `applyCustomRules`（`rules.json` → 自定义组与规则）

### 阶段 3 — 系统集成
- [x] 系统代理开关（SCPreferences；优先 root helper，回退 Authorization）
- [x] root helper（`com.yuehongsun.coast.helper`）Swift 重写 + SMAppService 注册
- [x] TUN 开关（依赖 helper 以 root 起核心）
- [x] `NSStatusItem` 托盘（流量显示、快捷开关、退出）
- [x] 开机自启（SMAppService.mainApp）

### 阶段 4 — UI 骨架
- [x] `Theme`（对齐 `qml/Theme.qml` 全部设计令牌，深/浅两套）
- [x] 主窗口：无边框 + 侧栏 + 页面切换 + 页脚开关条
- [x] `AppState`（`@Observable` 中枢，取代 `QmlBridge`）

### 阶段 5 — UI 页面
- [x] StatusPage（流量卡、带宽图、状态点、今日流量卡）
- [x] NodesPage（节点列表、延迟/测速、分组切换）
- [x] SubscriptionsPage（订阅 CRUD、节点启停）
- [x] SettingsPage（设置项 + 内核下载 + helper 安装 + 代理自检 + 规则/地区编辑器）
- [x] LogsPage
- [x] AboutPage + 更新检查（只打开发布页，**不做自动替换 .app**，理由见变更日志）
- [x] DevicesPage — **只读的局域网设备浏览**（不含代理开关，理由见下）。带台账/策略的完整版仍阻塞于网关决策
- [x] RuleEditorWindow（规则/分组编辑器）；ConnectionsView（连接查看器,状态页连接卡点开）；UpdateWindow 并入关于页

### 阶段 6 — 数据库与设备
- [x] `SQLite` 轻封装（原生 sqlite3）
- [~] `DeviceStore`（`device` 表）— **阻塞于阶段 9 的网关决策**，理由见下方 2026-07-31 条目
- [x] `HistoryStore`（`conn` 表，30 天保留）
- [~] `LanScanner`（ARP/邻居表扫描）— **同上，阻塞于网关决策**

### 阶段 7 — 本地化与资源
- [x] `I18n`（复用 `assets/i18n/*.json`，12 语言）—— **全部 125/125 已对齐**
- [x] 字体 —— **确认不用 MiSans**（已拍板），用系统字体 + SF Symbols
- [x] 应用图标

### 阶段 8 — 打包与 CI
- [x] `scripts/make_app.sh`：产出 `Coast.app`（含 helper、daemon plist、种子资源、签名）
- [x] 签名/公证接入（在**本仓库**的 CI 里做，recipe 取自 schat.build；见下方说明）
- [x] CI workflow 增加 Swift macOS job

### 阶段 9 — 可选：局域网网关
- [x] **评估完成**，结论见 [`docs/gateway-evaluation.md`](docs/gateway-evaluation.md)。
- [x] **方案改为 macOS 原生做法**（2026-07-31 拍板：网关按 macOS 特有方式做、能用 Swift 就用
      Swift）。不移植 C/C++、不做二层投毒、不引 lwIP。详见评估文档末尾的「最终方案」。
  - [x] `DeviceStore`（`device` 表：别名/代理开关/策略/最近 IP）
  - [x] `ConfigBuilder`：`redir-port` + `dns.listen` + 每设备 `SRC-IP-CIDR` 规则
  - [x] DevicesPage：**一个开关 + 一个策略选择**，设备端零配置
  - [x] 特权 helper 的 `Redirector`：ARP 欺骗 + `ip.forwarding` + PF anchor，全 Swift
  - [x] 复原路径：XPC 连接一断（app 崩/被杀）就自动复原；`ARPPacket` 复原字节有单测钉住
  - [x] `LanTopology`：取默认网关三要素，真机自检通过（COAST_TOPO_SELFTEST）
  - [x] `CoastController.syncRedirect`：开关/策略变更后下发给 helper，退出时先复原
  - [ ] **真机验证**：拿一台真设备验「开开关即接管、关开关即恢复」——
        需正式签名的 helper（ad-hoc 装不了），只能等签名构建，本地到此为止

## 变更日志

- 2026-07-31：建立 `macos/` 骨架与本文件。
- 2026-07-31：**阶段 0 完成**。`swift build` + `swift test`（7 用例）本机全绿。要点：
  - 种子资源**不复制**进 `macos/`，仍用 `clashauto-c++/assets` 那唯一一份（`Resources.swift`
    先找 `.app/Contents/Resources`，回退仓库相对路径）—— 免得 5.5 MB 的 Country.mmdb 进两次仓。
  - `YAMLText.setValue` 必须对替换内容做 `escapedTemplate`：secret 是随机十六进制，
    但订阅名/规则里出现 `$1` 会被 NSRegularExpression 当反向引用吃掉。已有用例覆盖。
  - `AppConfig.coastcore` 在 Swift 版**默认 false**（Qt 版是 true）：`src/net/**` 还没移植，
    开着也没有实现。字段保留只为读写 config.yaml 时不丢用户在 Qt 版设过的值。
- 2026-07-31：**阶段 1 完成 4/5**（只剩核心下载器）。`swift build` + 18 个用例全绿。要点：
  - 下载测速**不能**用 `URLSession.bytes` 那个逐字节 AsyncSequence —— 20 MB 等于两千万次
    await 恢复，那点开销会直接算进被测速度。改成 `URLSessionDataDelegate` 按块收
    （`SpeedProbe.swift`，一次测速一个 session，因为 delegate 只能建 session 时挂）。
  - 「选组 + 建连」的串行锁用 `AsyncSemaphore`（自己写的，见同名文件）而不是
    `DispatchSemaphore` —— 后者 `wait()` 阻塞线程，5 个并发下载就能把协作线程池饿光。
  - C++ 版的 `m_connectionsInFlight` / `m_nodesInFlight` 两个「在途」标志在 Swift 侧**不需要**：
    轮询写成 `while { await poll(); sleep }`，await 天然保证上一拍没回来就不会发下一拍。
  - 节点排序的权重公式 `speed > 0 ? speed : 10000 - delay` 把「字节/秒」和「毫秒」挤在同一根
    标尺上。这是 C++ 版的原样，**照搬不改**（换公式会让老用户的列表顺序无缘无故变样），
    已用一条专门的用例把这个怪癖钉住，免得后来者当 bug 顺手「修掉」。
  - `CoreProcess` 留了 `PrivilegedCoreLauncher` 这个接缝给阶段 3 的 root helper。
    没有 helper 而 TUN 开着时**必须明确记一条日志** —— 用户那边的表象只是「增强灯亮着却不全局」。
  - ⚠️ 欠账：`seedGeoIP` 只做了首次落地，C++ 版还有「暂存库换上 + 线上库体检」
    （`MmdbFile::applyStaged` / `validateFile`）。坏掉的 GeoIP 库核心能正常 Load、只是查什么都
    返回空 → `GEOIP,CN` 静默失配、国内流量集体出海。随「更新 GeoIP」入口一起在阶段 5 补。
- 2026-07-31：**阶段 1 完成**（22 个用例全绿）。要点：
  - `CoreDownloader` + `FileDownloader`：下载同样避开逐字节 AsyncSequence，改用
    `URLSessionDownloadTask` 的 `didWriteData` 报进度（内核包十几 MB）。
  - Intel 机器**优先 `-compatible` 资源**：普通 amd64 构建的指令集基线较新，老 Mac 上直接
    非法指令崩掉。精确名失配时有正则兜底（上游改过 tag 写法）。
  - macOS 的内核资源是**裸 gzip**、不是 tar.gz，`gzip -dc` 解出来直接就是可执行文件；
    装完必须 chmod 0755，否则表现是一句含糊的「启动失败」。
- 2026-07-31：**阶段 2 起步 —— `SubParser` 完成**（41 个用例全绿）。要点：
  - 不用 `URLComponents` 解分享链接，手写了 `ProxyURI`：订阅链接的 fragment 常是**未编码的
    中文节点名**、密码里带 `/ ? #` 也很常见，`URLComponents(string:)` 对这些直接返回 nil，
    整条节点就没了。Qt 那边用的是 `QUrl::TolerantMode`，这里等价地宽松处理。
  - 百分号解码**不把 `+` 当空格**（不是表单编码）—— 那会破坏 base64 密码。
  - `buildProxy` 对端口做 1..65535 校验后才输出。宁可少一个节点，也不能写出一条让核心
    **整份配置**加载失败的记录。
  - sing-box 的 `selector`/`urltest`/`direct`/`block`/`dns` 一律跳过：策略组由
    `ConfigBuilder` 自己生成，订阅里的组结构不采纳。
- 2026-07-31：**`macos` 分支从 `master` 重建**（原基线是 `feat/coastcore-outbound` 那条线）。
  Swift 代码随之修正三处对不上的假设：
  - `AppConfig` 去掉 `coastcore` / `coastcore_strict` / `coastcore_inbound` —— master 的
    `AppConfig.h` 里没有这三个键。（顺带说明：去掉后我们既不读也不写它们，所以老用户
    config.yaml 里若有这几行会被**原样保留**，`YAMLText.setValue` 只动我们写的键。）
  - `ClashService` 去掉 `groupLeaf` 映射 —— master 的 `ClashService.h` 没有 `groupLeafMap`，
    那是给 CoastCore 进程内规则解析用的，这条基线上没有消费者。
  - PLAN 的范围表改写 `src/net/**` 一行：master 上它是**局域网网关**（9.5k 行），
    不是进程内出站引擎（25k 行，那些文件在 master 上不存在）。
- 2026-07-31：**`SubscriptionStore` 完成**（63 个用例全绿）。要点：
  - 把 `subscribe.yaml` 的「切块」抽成 `SubscribeDocument`：C++ 侧每个操作各写一遍
    「扫描 + 记住行号」的循环（七八处），这里切一次、各操作在块上动。输出逐字节一致，
    但少了七八处手写扫描各自跑偏的机会。
  - 用户对节点的启停按 **`server:port`** 认人，**不按节点名** —— 机场经常改名，
    按名字认会让用户禁用过的节点在下次更新后全部复活。已有用例钉住。
  - `setAllNodesEnabled` 必须**从后往前**改：缺 `use:` 键时要插行，从前往后会让后面所有
    节点区间整体位移。
  - 更新中间那条订阅时替换的是 `listLine..<block.upperBound`，而中间块的 upperBound 正是
    下一块的起始行 —— 错一格就把后一条订阅整条吞掉，专门有一条用例守着。
  - `SubscriptionStore` 的目录**可注入**（`init(config:directory:)`）。它每个写操作都真的落盘，
    不隔离的话跑一次单测就把开发者自己的订阅覆盖了。
  - 正则写错时当作没设 —— 不能因为用户在设置里敲错一个字符就让所有节点消失。
- 2026-07-31：**阶段 2 完成 —— `ConfigBuilder` 落地**（96 个用例全绿）。
  验证方式不只是单测：用**真实种子** default.yaml(8 万行) + plugin.yaml 生成了一份 1556 行的
  full.yaml，再用真正的 YAML 解析器（PyYAML）解 —— 解析通过、8 个策略组、1392 条规则、
  **零悬空引用**、私网规则确实在 rules 最顶部、`tun.enable` 从 plugin 正确合并。
  要点：
  - **私网直连必须最后前插**。它和自定义规则都插在 `rules:` 顶部，后插的在上面；私网要赢，
    否则「访问自己家路由器后台 / 内网 NAS」会被指向代理的自定义规则捞走。有专门用例守着。
  - `ensureProxyServerNameserver` 不是可选项：开 TUN 后核心要先解析代理服务器域名才能拨代理，
    走境外 DoH 的话那条连接会被自己的 TUN 路由捞回代理 → 死循环，表现为「所有节点无延迟、
    境外全打不开」。注入境内明文 DNS 打破环路。
  - 策略组**刻意不写 `lazy: false`**：那会让核心启动时同步健康检查，节点不可达就卡住启动、
    REST API 迟迟不监听。延迟改由应用起来后异步测速填充。
  - `autoGroups` 返回值按组名排序：C++ 用 QMap（天然按键排序），组的先后顺序直接体现在 UI 上。
  - 缺失的顶层标量要**前置**到文件开头而不是追加到末尾 —— 追加会掉进最后一个块的缩进范围，
    变成那个块的子键。
  - `Resources` 加了一条 `#filePath` 兜底根：`swift test` 时 argv[0] 是 Xcode 的 xctest 工具
    （在 /Applications 下），原来那条「从可执行文件往上找仓库根」一路什么都找不到，
    单测里读种子全是 nil。打包后 .app 里该路径不存在，不会误命中。

  ⚠️ 两处**刻意未移植**，都记在这里免得以为是漏了：
  - `applyDevicePolicies`（设备台账 → 网关 SOCKS listener + IN-USER 规则）：依赖 `DeviceStore`，
    随阶段 6 一起做。
  - `dns.listen: 127.0.0.1:1053`：那是给局域网网关做 DNS 劫持转投用的（端口须与
    `NetStack::kDnsHijackPort` 一致）。网关本身在阶段 9 才评估移植，现在开这个监听端口没有消费者，
    故不写。**若将来移植网关，这一行必须一并加回。**
- 2026-07-31：**阶段 3 前两项完成**（系统代理 + root helper）。96 个用例仍全绿。
  - `SystemProxy`：走 SystemConfiguration 直接改网络配置，一次 Lock → 逐服务改 → Commit → Apply。
    授权用 `AuthorizationCopyRights` 预授权 `system.services.systemconfiguration.network`，
    `AuthorizationRef` **整进程复用** —— 开代理弹一次密码，之后关代理/改端口都不再弹。
  - 改代理时在**现有字典上改**，不是新建：FTP/PAC 等我们不管的键要原样保留，整份替换会抹掉
    用户自己配的东西。关代理只翻 `*Enable` 位、保留 host/port，下次秒开。
  - `SystemProxy.selfTest()`：读当前值 → 设测试值 → 读回核对 → **还原**。这条路只有在真实桌面
    会话里才验得了（CI/无头机跑不了），而出错的后果是「本机上不了网」，所以给一个几秒钟的钩子。
  - helper 单独一个 `CoastHelperProtocol` target：以 root 运行的进程只链接这一小块，
    而不是整个 CoastKit —— root 进程里的每一行代码都是攻击面。
  - `setCodeSigningRequirement` 是 helper **唯一的门**：没有它，任何本机进程都能连上一个 root
    服务改网络配置、以 root 起任意程序。macOS 13 以下直接拒绝连接，不降级放行。
  - `isEnabled` 用 `SMAppService.status == .enabled` 判据，**不是** ping 得通：冷启动的 daemon
    首个 XPC 偶发慢/超时，用 ping 当门槛会让 helper 明明装好却被判不可用，表现是
    「装了 helper、开了增强、核心却仍非 root」且毫无线索。
  - helper 停核心先 **SIGTERM** 再 SIGKILL：直接 KILL 会把系统留在「默认路由指向已消失的 utun」
    上 —— 用户直接断网。
  - 打包相关（已就位，等阶段 8 的脚本用）：`Resources/` 下三份 plist。SPM 产出的 helper 是
    **裸 Mach-O 不是 bundle**，Info.plist 只能靠链接期 `-sectcreate __TEXT __info_plist` 嵌进去；
    已用 `plutil` 验证嵌入内容可解析。可执行产物名正好是 `com.yuehongsun.coast.helper`
    （SPM 用 product 名命名可执行产物），与 launchd plist 的 `BundleProgram` 对得上。
  - ⚠️ `Package.swift` 因此用了 `.unsafeFlags`，代价是**本包不能再被别的 SwiftPM 包当依赖引用**。
    这是个 app 不是 library，可以接受；将来若要拆库，把 helper 移到独立 package 即可。
- 2026-07-31：**阶段 3 完成**（100 个用例全绿）。新增 `CoastController`（C++ `CoreController`
  的编排层对应物）、`TrayController`、`LaunchAtLogin`。要点：
  - **macOS 上翻 TUN 必须重启核心，不能热重载** —— 两条原因叠加，缺一不可：
    ① 建/拆 utun + 改默认路由必须 root，而 root 只在核心由 helper **冷启动**时才有；
    ② mihomo 的 `PUT /configs` 默认**不重载 general/tun 段**（要 `?force=true`），
    热重载改 `tun.enable` 核心根本不理会。两者叠加的现象就是「开了 TUN 却不全局」。
  - 热重载前先跑 `mihomo -t` 校验：不过就不 PUT，保留当前正在跑的好配置。否则一份坏配置会把
    核心打到失效，而用户只看到「突然全断网」。核心不在时跳过校验（别因缺核心反而不重载）。
  - 托盘只在状态**真的变了**时才写图标/标题：`NSStatusItem` 的赋值是到 window server 的往返，
    每秒无脑重设会让图标肉眼可见地闪。
  - 开机自启用 `SMAppService.mainApp` 而不是往 `~/Library/LaunchAgents` 写 plist：后者在
    签名+公证的应用上会被系统当成「未托管的登录项」，用户在系统设置里关掉后我们无从知晓，
    两边状态长期不一致。`SMAppService` 的状态就是系统真值，另外单独认出 `requiresApproval`
    —— 那种情况下再调 `register()` 不生效，只能引导用户自己去开。
- 2026-07-31：**阶段 4 完成 + 阶段 5 起步**（Status/Nodes/Logs 三页）。100 个用例仍全绿。
  **不只是编译通过 —— 真的把 app 跑起来截了图看**：窗口开出来了、侧栏 7 项、流量卡、带宽图、
  状态点、页脚三开关 + 模式下拉都在位，菜单栏托盘项也注册成功（`COAST_NO_AUTOSTART=1` 跑的，
  没动真实系统代理）。要点：
  - `Theme` 的颜色**逐条对齐** `qml/Theme.qml` 的十六进制值，不是重新设计一套。用
    `Color(hex: 0x4898F8)` 这种写法就是为了能和那边逐行核对。
  - 设备类型图标从 Remix Icon 私用区码点换成 **SF Symbols**：系统自带、自动跟随字重与明暗，
    省掉一个 300KB 字体文件和它的版权/打包负担。
  - 日志是**有上限的环形缓冲**（2000 条）：核心在 debug 级别下每秒能刷几十条，无界数组跑一晚上
    就是几百 MB，而日志页只看得到最近几屏。带宽图同理只留 60 拍。
  - 页脚模式下拉按**规范值**（Rule/Global/Direct）算档位，不拿本地化显示串去 indexOf ——
    否则切语言后会回显错档。
  - ✕ **不退出程序**只隐藏窗口，`applicationShouldTerminate` 里等 `shutdown()` 跑完再退
    （`.terminateLater`）：否则用户退出后整机仍指着一个没人监听的端口，表现为
    「关了 Coast 就上不了网」。
  - 状态页在「TUN 开着但核心非 root」时**当场打一条红色告警** —— 这正是「增强灯亮着却不全局」
    的根因，不写出来用户完全无从查起。
- 2026-07-31：**阶段 5 主体完成**（订阅/设置/关于三页），113 个用例全绿。
  **在真机上把三页都截图看过**，不是只看编译通过。加了 `COAST_INITIAL_PAGE=<0..6>` 这个开发钩子
  —— AppleScript 点不到 SwiftUI 的按钮，没它就没法可复现地验「某一页渲染对不对」。

  ★ **看截图时抓到一个真 bug**：关于页显示「已是最新版本」，但本地版本是 `dev`，
  按版本比较**必然**比任何真实 tag 旧 —— 说明检查其实失败了却被渲染成「最新」。
  查下来是 GitHub API 从这台机器返回 **403（限流）**，而限流响应是个带 `message` 的**对象**、
  不是发布数组，原来的 `as? [[String: Any]]` 失配后**返回 nil**，被调用方当成「没有更新」。
  已改为**抛 `CheckError.unexpectedResponse`**，界面现在如实显示
  「检查更新失败：HTTP 403：API rate limit exceeded…」。并把解析抽成
  `parseReleases(data:status:includePrerelease:)` 单独可测，补了 5 条用例把限流、
  非 JSON、空列表、prerelease 频道、draft 跳过都钉住 —— 这条路径**只在真实网络上才会走到**，
  不测就永远不知道它坏没坏。

  其它要点：
  - 设置页编辑的是**本地草稿**，点「应用」才落盘：端口这类改一半的中间值直接生效会把核心打断
    （用户把 7890 改成 1080，输到 "108" 时就已经重启过一次核心了）。
  - **端口变更必须重启核心**，热重载改不了监听端口 —— 否则核心还 bind 在旧端口、UI 已按新端口去连，
    表现为「应用之后一切都断了」。
  - 开机自启不等「应用」，立刻同步并**把系统的真实结果读回来** —— 它是系统状态，配置文件里那份
    只是我们的一厢情愿。
  - 新增订阅后**立刻拉一次** —— 否则用户看到的是一条 0 个节点的空记录，会以为没加上。
  - 订阅内容没变（`changed == false`）就不重建配置：自动更新周期短时这是常态，白重载没意义。
  - 关于页**不做自动下载安装 .app**：自动替换要处理签名、公证和「正在运行的自己」，
    风险远大于省下的两步点击。只打开发布页。
- 2026-07-31：**打包脚本完成**（阶段 8 第一项）。`bash scripts/make_app.sh --version 0.1.0`
  产出可运行的 `Coast.app`，签名校验通过，版本号从 `dev` 变成真实值。
  顺带加了三个**无头自检钩子**（沿用 Qt 版 `COAST_*_SELFTEST` 的做法）：
  `COAST_PATHS_SELFTEST` / `COAST_HELPER_SELFTEST` / `COAST_SYSPROXY_SELFTEST`。
  存在理由是这些路径**只在打包后的真实 .app 里才走得到**，单测一行都验不了。

  验证结果：
  - 5 份种子全部**从 `.app/Contents/Resources` 解析到**，没有悄悄回退到开发期那条仓库相对路径
    （那条在用户机器上根本不存在）—— 这正是 `COAST_PATHS_SELFTEST` 要答的问题。
  - helper：daemon plist 与可执行文件都在位，`SMAppService` 认出了它。

  ★ **踩到并记下一个极具误导性的坑**：**同一个包**，放在构建目录里跑 `SMAppService` 报
  **`notFound`**（看起来就像「plist 没打进去」），拷到 `~/Applications` 再跑就变成
  **`requiresApproval`**（说明它其实找到并认了这份 plist）。在构建目录里排查 `notFound`
  会让人去找一个根本不存在的打包 bug。已在 `helperSelfTest` 里对非标准位置**主动打提示**。

  - 签名顺序：**先内层后外层**（先签 helper 再签 .app）。反过来签完 app 再动里面的二进制，
    会当场破坏外层签名。
  - ad-hoc 签名（`-`）**装不了免密 helper**：`TeamIdentifier=not set`，而
    `HelperConstants.clientCodeRequirement` 要求 `certificate leaf[subject.OU] = 6AXTRT5TV4`。
    本地只能验到「包打对了、系统认出了 daemon」这一步；真正可用的 helper 必须用正式开发者
    证书签，走外部仓库 `integemjack/schat.build`。
- 2026-07-31：**阶段 6 做了能做的那一半**（`SQLiteDatabase` + `HistoryStore`），125 个用例全绿，
  并把状态页的**今日流量卡**接上了（之前标为「随阶段 6 补」的那块）。

  ★ **一个需要说明的范围判断**：阶段 6 剩下的 `DeviceStore` / `LanScanner` / `DevicesPage`
  我**没有做**，因为它们全部依赖阶段 9 那个还没决定要不要移植的局域网网关。查证依据：
  - `DevicesController` 的构造函数直接吃一个 `LanGateway *`；
  - `ConfigBuilder::applyDevicePolicies` 生成的是「网关专用 SOCKS listener + IN-USER 规则」，
    这套东西的唯一用途就是分流**局域网设备**的流量；
  - 而设备的流量要先经 ARP/NDP 欺骗引到本机，才谈得上被分流、被计数。
  没有网关，设备页只能列出一张不能操作的设备清单，代理开关点了不会有任何效果、流量计数恒为 0。
  与其半做一个骗人的页面，不如把它和网关决策绑在一起。PLAN 里这三项已标 `[~]` 而非 `[ ]`。

  `HistoryStore` 则**不依赖网关**（记的是本机连接），所以照做，且它正好解掉了今日流量卡的欠账。

  要点：
  - 连接**结束时才落一条**，不是每秒逐条写：每拍比对快照，某个 id 消失即视为断开。
    退出时 `flush(includingLive:)` 把在途的长连接也落下来 —— 否则一条挂几小时的连接永远进不了库。
  - `host` 会**迟到**（sniffer 嗅出域名后才填上），所以每拍都跟一次；但**空值不覆盖已有值**，
    否则会把已经拿到的域名又抹回 IP。有专门用例。
  - `proxyOnly` 口径排除 `DIRECT`/`REJECT`，**也排除 chain 为空的** —— 状态不明的不该被算进代理流量。
  - 今日区间必须按**本地时区**算：用 UTC 的话东八区用户在早上 8 点前看到的「今日」其实是昨天。
  - 批量写走**一个事务**：逐条 commit 每条一次 fsync，几万条记录是分钟级。
  - `sqlite3_bind_text` 必须传 `SQLITE_TRANSIENT`：默认的 STATIC 只存指针，等 step 时 Swift 那边的
    临时 String 早已释放，写进去的是垃圾且不一定当场崩。
  - 今日流量卡**不跟着每秒轮询刷**（那是几条 GROUP BY 聚合），10 秒一次足够。
  - 旧库迁移改名时 `-wal`/`-shm` **必须跟着一起搬**：只搬主文件会让 sqlite 读到
    「主库是旧的、WAL 里还有没并回去的事务」的组合，直接判定库损坏。
- 2026-07-31：**阶段 7**（i18n / 图标 / 字体决策），125 个用例全绿。

  i18n 沿用 Qt 版那套聪明做法：**中文源串直接当 key**，翻译表是扁平的
  `中文 → 目标语` 映射，与 Qt 端**共用同一批 JSON**，不复制一份到 macos/。
  好处是不需要 lupdate/.ts/.qm 工具链，代码里写的就是最终中文，读代码即读界面；
  漏翻时回落到中文而不是显示 key 或空白。
  - 界面字符串的 `.t` 是用一个**带字符串状态机的脚本**批量加的，不是正则硬替：
    要跳过注释行（注释里全是中文）、跳过行内 `//` 之后的部分（但 URL 里的 `//` 不算）、
    跳过**带插值**的串（插值后的结果永远匹配不上扁平表）。共处理 112 处。
  - 语言切换靠给根视图挂 `.id(i18n.language)` 整体重建，等价于 QML 的 `retranslate()`。
  - 语言选择器列**各语言的自称**（English / 日本語 / Русский…），不是用当前界面语言去翻译
    语言名 —— 用户切到看不懂的语言后要靠这个列表切回来。
  - `COAST_LANG=<code>` 可覆盖语言，验各语言排版时不必改用户配置。

  ★ **验证时发现的实情，必须说清楚**：翻译**没有全覆盖**。截图看到英文界面仍是中文后我查了
  一下，engine 是好的（上传→Up、下载→Down、连接→Connections 都对），问题是**我写的 106 条
  界面串里只有 37 条存在于 Qt 版的翻译表**里 —— Swift 版的说明文字比 Qt 版详细得多，多出来的
  69 条那些表里根本没有。
  处置：**只补了 en-US 这一门**（69 条，我能可靠地写对），现在 en-US 是 106/106。
  其余 11 种语言仍是 37/106，缺的那些会回落显示中文 —— **我不会为看不懂的语言编造译文**。
  为此加了 `scripts/i18n_check.py`，跑一下就能看到每种语言的覆盖率，
  `--missing <lang>` 列出待翻条目，交给会那门语言的人补。

  ★ **字体：刻意不移植 MiSans**（PLAN 里标 `[~]` 而非打勾）。Qt 版捆 MiSans 是因为 Qt 在三个
  平台上的默认字体渲染不一致，需要一个统一的。而这是一次**原生 macOS 重写**，用系统字体
  （SF / 苹方）才是对的：自动光学字号、自动跟随字重与明暗、系统级 CJK 回退、少 10 MB 包体。
  图标同理用 SF Symbols 取代 remixicon。若将来产品上要求三平台字体完全一致，再把 MiSans
  加回来也只是打包脚本 + 一行注册的事。
  - 应用图标复用 `assets/icon.icns`，打包脚本已拷进 `Contents/Resources/AppIcon.icns`
    并写入 `CFBundleIconFile`。
- 2026-07-31：**规则编辑器完成**，137 个用例全绿。

  这一项补的是一个**我自己造出来的缺口**：阶段 2 就写好了 `ConfigBuilder.applyCustomRules`
  去读 `configDir/rules.json`，但 Swift 端一直没有任何东西**写**这个文件 —— 消费者有了、
  生产者没有，那段代码等于是死的。现在 `RulesStore` + `RulesEditor` 把闭环接上了。

  要点：
  - `RulesStore` 手写字典而不是用 `JSONEncoder`：这个文件**与 Qt 版共用**，键名和结构必须
    逐字对齐，交给 Encoder 推导容易在某次重构里悄悄改掉字段名。专门有一条用例直接读回
    JSON 核对 `type/node/value` 与 `name/type/rule` 这六个键。
  - **保存前逐条校验**。坏规则会让核心**整份配置**加载失败，而用户只会看到「突然全断网」，
    完全无从关联到自己刚加的那条。重点拦的是**逗号** —— 它会把一行规则切成更多段，直接
    写坏整张规则表。另外 IP-CIDR 要求带 `/`、端口要在 1..65535。
  - 区域分组的正则**当场校验**：`ConfigBuilder` 遇到非法正则会静默跳过那个组，
    用户加了组却什么都没发生，不如在这里说清楚。
  - 「目标策略」是从**生成好的 full.yaml** 里读现有策略组与节点名做成下拉，不让用户手敲 ——
    敲错一个字就是一条指向不存在目标的死规则。已存在但不在候选里的值也照样显示出来，
    否则 Picker 会是空白，用户以为规则丢了。
  - 筛选返回的是**真实下标**而不是过滤后的副本：过滤视图里改第 0 行，改的必须是原数组里
    那一条，不是第 0 条。
  - 有一条**端到端用例**：编辑器写出的 rules.json → `ConfigBuilder.applyCustomRules` 消费 →
    断言规则确实前插到 `rules:` 顶部、分组确实被生成并加进了主选择组。
- 2026-07-31：**阶段 8 完成** —— CI job + 签名 + 公证。137 个用例全绿。

  `.github/workflows/release.yml` 追加了一个 **`macos-swift` job，完全独立于 Qt 那条流水线**：
  不共用 env、不 `needs` 任何 job、不参与 Release 发布，只产出 Actions artifact。
  步骤：选 Xcode → 解析版本（与 Qt 线**同一套编号**，免得同一 commit 出两个版本号）→
  build → test → i18n 覆盖率报数 → 导入证书 → 打包 universal → **验包** → zip/dmg + 公证 → 上传。

  ★ **签名/公证不必再等外部仓库**。用户把 `integemjack/schat.build` 放进目录后我读了它 ——
  注意**当前 checkout 是 `main` 分支**（那是 `Ireoo/Secret-Chat` 的 CI 仓库，不是 clashauto 的；
  clashauto 的在 `clashauto-mac` 分支，而这个 submodule 的 git dir 缺失、切不过去）。
  但 `main` 上的 `qt-desktop.yml` 里**已经有一条跑通的「原生 SwiftUI mac 客户端 → Developer ID
  真签 + 公证 → .dmg」**流程，直接照搬即可，不需要动那个外部仓库：
  - 证书导进**用完即弃的钥匙串**；`MACOS_CERT_P12` 没配就退回 ad-hoc，**fork 照样能构建**，
    而不是整条流水线红掉；
  - 公证**一旦开做就必须成功**：失败即整步失败、artifact 不上传 —— 政策是「绝不发未公证的包」，
    不能悄悄给用户一个双击会被 Gatekeeper 拦下的东西；
  - 更新用的 zip 必须 `ditto -c -k --keepParent`：普通 `zip` 不保留 bundle 的符号链接与扩展属性，
    解出来的 .app 签名当场失效。

  需要在仓库设置里加的 Secret：`MACOS_CERT_P12` / `MACOS_CERT_PW` /
  `MACOS_NOTARY_KEY` / `MACOS_NOTARY_KEY_ID` / `MACOS_NOTARY_ISSUER`。都没配也能跑（ad-hoc）。

  ★ **刻意不发到 Release**：Qt 那条线已经通过 schat.build 往同一个 Release 传 macOS DMG，
  而 app 内的更新检查是**按扩展名挑资源**的（`UpdateChecker.macAsset` 取第一个 `.dmg`）。
  两个 macOS 包落在同一个 Release 上，老版本会挑到哪个纯看运气。要发布必须先定
  「谁是正牌 macOS 包」—— 那是产品决定，不该由 CI 悄悄决定。

  验证过的两件事（都不是「看着对」）：
  - `--universal` 这条路**真跑了一遍**：两个二进制都是 fat（x86_64 + arm64），
    `.build/apple/Products/Release` 这个路径假设成立。
  - **抓到一个真 bug**：macOS 自带的是 **bash 3.2**，`set -u` 下 `"${arr[@]}"` 展开空数组会直接
    报 unbound variable，`--timestamp` 那个可选数组正好踩中。已改用 `${arr[@]+"${arr[@]}"}`
    并用 `/bin/bash` 显式复验通过 —— CI 上跑的就是 3.2，这个不修就是必炸。
- 2026-07-31：**阶段 9 评估完成**（PLAN 上最后一个待办）。写在
  [`docs/gateway-evaluation.md`](docs/gateway-evaluation.md)。

  ★ **评估过程中推翻了我自己先前的一个错误假设**。我在阶段 6 说设备页依赖网关时，
  顺带以为「macOS 上网关本来就是 stub」。查 `CMakeLists.txt` 才发现**不是**：

      if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE OR WIN32)   # ← APPLE 在真实现这一支

  macOS 走的是真实现（二层用 `L2Endpoint_mac.cpp` 的 BPF）。所以移植它属于**恢复对等**，
  不是「新增一个 macOS 从来没有的功能」—— 这直接改变了这件事的性质与优先级。

  查到的硬数字：macOS 实际需要 ≈ **7,100 行 C++**（总 9,001 减去 Win/Linux 的二层端点），
  外加 **135,473 行纯 C 的 lwIP（零 Qt）**；而那 7,100 行**Qt 耦合很深**
  （11 个头文件里 7 个是 `QObject` 子类，约 1,000 处 Qt 符号）。

  结论：**去 Qt 化后当 SPM 的 C/C++ target 链进来**，而不是用 Swift 重写（包级别代码已调通，
  重写只买风险）、也不是原样链（那会把 Qt 拖回来，这次重构就白做了）。
  敢这么推荐的关键依据是 **macOS 上已有 headless 自检**（`GatewaySelfTest` 用 feth +
  真实 BPF 端点），意味着这次重构**可验证**，不是「改完只能祈祷」。

  另外确认了**没有 macOS 原生替代方案**：NetworkExtension 接管的是**本机**流量，
  而这功能的本质是把**别的设备**的流量骗过来（二层 ARP/NDP 投毒），两者不是一回事。
- 2026-07-31：**只读设备页完成**（155 个用例全绿）。这是评估里点名的那件
  「不依赖任何拍板、现在就能做」的事。

  切分方式是刻意的：**设备发现本身是个完整的小功能**（读系统邻居表 + OUI 厂商表 + 反查主机名，
  不发包、不需要 root），而「代理某台设备」没有二层网关就只能是个点了没反应的假开关。
  所以这一版**没有那个开关**，并在页脚直接写明「本页仅浏览，代理局域网设备需要二层网关支持，
  macOS 版尚未提供」—— 留空白让人猜比写清楚更糟。

  ★ **两个 bug 都是看真机输出抓到的，不是想出来的**：
  1. **组播条目被当成了设备**。第一版在真实网络上列出了 `mdns.mcast.net / 224.0.0.251 /
     01:00:5e:00:00:fb` —— 那是 mDNS 组播组，不是一台设备。修法用以太网标准的判据：
     **首字节的 I/G 位**（最低位）为 1 即组播，一条规则同时盖住 IPv4 组播 `01:00:5e:*`、
     IPv6 组播 `33:33:*` 和广播 `ff:ff:*`；IP 侧再拦一道 224.0.0.0/4 做双保险。
     修完在真机上复验，列表已以真实设备收尾。
  2. 修完之后**我自己的一条用例挂了** —— 我编的 MAC 例子 `1:2:3:4:5:6` 首字节是 `01`，
     本身就是个组播地址，被新规则正当地拒掉。改的是**用例**不是代码（换成偶数首字节）。

  其它要点：
  - macOS 的 `arp -an` **不补前导零**（打印成 `3c:84:6a:1:2:3`），而 OUI 表是定长 `3C846A`。
    不补零的话厂商永远查不到，且**不报错**，只是所有设备的厂商栏莫名其妙全空。
  - 反查主机名并发做：逐个串行时一台超时就拖住整轮。
  - 排序按 IP 的**数值**：字符串序会把 `.10` 排在 `.2` 前面。
  - 用子进程读 `arp -an` 而不是自己 `sysctl(NET_RT_FLAGS)`：后者要手工走
    `rt_msghdr` + `sockaddr_dl` 变长结构，是在不同 macOS 版本上出过变动的地方，
    而这个调用几秒才一次，短命子进程的代价完全可接受。
- 2026-07-31：**四项决定已拍板**，逐条落实：
  1. **要**做局域网设备代理 → 阶段 9 转为待办四步，见上。
  2. **Release 只发 Swift 版** → `macos-swift` job 补上 Release 上传，`trigger-mac` 停用
     （`if: false`，整个 job 保留并在注释里写明恢复所需的原条件）。二者必须二选一的理由：
     app 内一键更新按扩展名挑第一个 `.dmg`，同一 Release 上两个 macOS 包会让老版本挑到哪个
     纯看运气。
  3. **不用 MiSans** → 维持系统字体 + SF Symbols，PLAN 里那项从「主动搁置」改为已确认。
  4. **其余语言对齐** → 12 种语言全部 **125/125**。日语已截图验过真的渲染出来。
     机器翻译质量，欢迎母语者复核；`scripts/i18n_check.py` 可持续把关。

  另外修了一个查证出来的差距：**`useMirror` 只镜像下载、不镜像 GitHub API**
  （实测 ghfast.top 对 `api.github.com` 同样返回 403，它不代理 API）。这一点与 Qt 版相同、
  不是本次移植引入的；但我的版本原先比 C++ 差一截 —— C++ 的 `applyDownloadProxy` 会在
  **核心已在跑**时把版本查询也丢给它出网，我一直是直连。现已补上
  （`CoreDownloader.proxyPort` / `UpdateChecker.proxyPort`），解决的是「更新内核 / 查程序更新」
  这个常见场景；首次安装时核心还没有，那种情况仍需用户自己能访问 GitHub。
- 2026-07-31：**网关方案转向 macOS 原生做法**（167 个用例全绿）。

  按「网关按 macOS 特有方式做、能用 Swift 就用 Swift」的指示重新设计。核心判断：
  **ARP/NDP 投毒 + lwIP 用户态栈，全部只是为了做「设备端零配置」这一件事**；
  而 macOS 上让设备直接指向 `本机IP:7899` 是条直白得多的路 —— 不投毒、不要 root、
  不要用户态 TCP/IP 栈、**零行 C++**。

  ★ **关键发现：per-device 策略一点没损失**。`applyDevicePolicies` 生成的
  「带每用户认证的 listener + `IN-USER` 规则」在两条路上是同一套东西 ——
  投毒只是把流量*送到*这个口的其中一种方式罢了。所以换方案换掉的只有「设备端零配置」，
  换来的是少一整个子系统。

  两处**必须**与 Qt 版不同（都有用例钉住）：
  - listener 绑 `0.0.0.0` 而非 `127.0.0.1` —— 不劫持就得让设备够得着；
  - **每设备随机密码**而非 Qt 版的固定字面量 `coast` —— 端口现在暴露在局域网上，
    固定密码等于开放代理。密码首次开启时签发一次并落库，之后复用：每次重建都换的话，
    已配好的设备会在下一次热重载后集体掉线。

  转向之前我已经把 lwIP 立成 SPM 的 C target 并**成功编过全部 32 个源文件**
  （符号链接过去、源码一份不复制）。那条路技术上走得通，现在不走了，但已撤掉不留死代码 ——
  评估文档里记着它「已验证可行」，将来真要做透明代理不必重新趟。
- 2026-07-31：**纠正一次方向偏差 + 敲定零配置透明代理方案**（164 用例全绿）。

  ★ **我先设计错了一版**：把「局域网代理」做成了「让用户去每台设备上手动填代理地址」。
  那等于没做这个功能 —— **零配置正是它的全部意义**。用户当场指出，已整个换掉。

  最终方案（macOS 原生，零行 C++、不引 lwIP）：
  ARP 欺骗让设备把本机当网关 → `net.inet.ip.forwarding=1` 让内核转发 →
  **PF 的 `rdr` 规则**把 TCP 重定向到 mihomo 的 `redir-port`、UDP :53 重定向到它的 DNS 口。

  ★ **为什么这条路不需要 lwIP**：Qt 版背那 13 万行用户态 TCP/IP 栈，是因为它在**二层**
  自己收发帧、自己重组 TCP。而内核转发 + PF 重定向把这一整层全省了 —— 包由内核按正常
  网络栈处理，我们只负责「让流量来」（ARP）和「让它拐弯」（PF）。同一个问题的 macOS 原生答案。

  由此产生的身份口径差异：透明重定向**看不到任何凭据**，设备只能按**源 IP** 认，
  所以策略是 `SRC-IP-CIDR,<ip>/32,<目标>` 而不是 Qt 版的 `IN-USER`。
  上一版设计里的每设备随机密码、SOCKS 用户名全部作废并已删除 —— 零配置下它们没有位置。

  `lastIP` 落库而不是只放内存：设备暂时不在邻居表里（睡眠、刚重启）时规则不能凭空消失 ——
  那会让它在恢复的一瞬间直连出去。
- 2026-07-31：**透明代理的接管与复原全部落地**（168 用例全绿，全 Swift、零 C++）。

  ★ **复原是这个功能的命门，我把它设计成第一位**：被欺骗的设备把本机当网关，一旦停止转发
  就直接断网，且 ARP 缓存要十几分钟才过期。为此：
  - **整个欺骗循环跑在 helper 里，不把 BPF fd 传回 app**（Qt 版是后者）。这样 app 无论怎么死
    （正常退出 / 崩溃 / 被 SIGKILL），XPC 连接一断，helper 的 `invalidationHandler` 就复原。
    fd 传给 app 的话，app 被 SIGKILL 时没有任何人来发那几个复原包。
  - 复原发的是「网关 IP → **真网关 MAC**」，发三遍加冗余（ARP 不可靠，丢一个包=一台设备断网
    十几分钟）。`ARPPacket` 的复原字节有专门单测逐字节钉住。
  - `stopCore` 里**第一件事**就是 stopRedirect —— 哪怕后面出错，设备也已经放回去了。
  - `ip.forwarding` **只回滚我们开的那一次**：用户可能自己开着它干别的。
  - `LanTopology.defaultGateway` **三要素缺一就返回 nil**，让调用方根本不开始接管，
    而不是带着残缺信息硬上（没有网关 MAC = 发不出复原包）。

  组件：`ARPPacket`（报文构造，纯字节可测）、`Redirector`（helper 里的 root 侧实现）、
  `CBPF`（BIOCSETIF 等 _IOW 宏的 C shim，Swift 导入不进来）、`LanTopology`（网关三要素）、
  `MacHelperClient.startRedirect/stopRedirect`、`CoastController.syncRedirect`。

  ⚠️ **真机联调还没做**：需要正式签名的 helper（ad-hoc 装不了，见阶段 8 的结论），
  本地只能验到「网关三要素取得到、配置规则生成对、报文字节对」。真设备上的
  「开开关即接管、关开关即恢复」得等签名构建。这一点如实记着，没假装跑通。
- 2026-07-31：**打包收尾：entitlements + helper 的 redirect 能力进包**（168 用例全绿）。
  - `CBPF` / `Redirector` 是**静态链进 helper 二进制**的（单 Mach-O 可执行文件），
    所以打包无需额外拷贝 —— 已核对 helper 二进制里有 52 个 redirect/bpf/pfctl 符号。
  - 补了两份 hardened-runtime entitlements（公证的硬要求，没有它们签名包一跑就被杀）：
    · `coast.entitlements` —— 主程序：network.client（REST/下载/查更新）+ server（留一手）；
    · `helper.entitlements` —— helper：同样两条网络能力。
    **BPF/PF/sysctl 不在 entitlement 里** —— 它们靠的是 root 身份，hardened runtime 不拦这些
    系统调用，为它们声明 entitlement 是没有意义的。这一点想清楚了才没往里塞多余权限。
  - entitlements **只在真签时带**：ad-hoc + entitlements 会生成一份谁都能伪造的权限、反而误导。
  - 已验证：两份 plist `plutil -lint` 通过、codesign 接受并真的把网络能力嵌进签名、
    重打后 deep-strict 校验通过、种子仍从 `.app` 解析。
- 2026-07-31：**对抗性审查发现并修掉一个严重安全 bug：ARP 欺骗必须单播**（169 用例全绿）。

  ★ 上一轮我把 `deviceMACs` 初始化成**全广播、且从未填真实 MAC**，于是欺骗包和复原包都发
  **广播 ARP reply**。广播 gratuitous ARP 会被**整个局域网**上缓存里有网关条目的设备处理 ——
  用户只选了一台设备，却可能把**全网**流量都引到本机。这远超用户意图，是个安全问题。

  修复贯穿四层:协议 `startRedirect` 加 `deviceMACsCommaSep`（与 IP 一一对应）→
  `MacHelperClient` 传 `(ip, mac)` 对 → helper 解析并 zip 回去 → `Redirector` 用**真实 MAC
  单播**欺骗与复原。MAC 解析不了的那一台**直接跳过、不代理**，宁可漏一台也绝不退回广播。
  PF 规则也改用「实际接管的这批 IP」而非原始入参 —— 否则会给一台并不欺骗、流量根本不到本机的
  设备装死 rdr 规则。

  新增用例逐字节钉住「以太头目的 MAC = 目标设备本人，不是广播」。

  这条是自审出来的，不是用户报的 —— 透明代理这条链路最新、最危险、且从没在真实 root 环境
  跑过，值得单独对抗性审一遍。
- 2026-07-31：**继续对抗性审查:堵住 helper 两处纵深防御缺口**(173 用例全绿)。

  审 helper 里所有以 root 执行的路径,发现两处直接信任 XPC 裸字符串:
  1. **PF 规则注入**:`installPF` 把 `interface` 原样拼进 PF 规则喂给 pfctl。设备 IP 上一轮
     已过 ARPPacket 解析器洗过(安全),但 interface 零校验 —— 带换行的串能**另起一条 PF 规则**。
  2. **路径遍历**:`startCore` 的 `executable`/`config`/`userDir` 是裸字符串,helper 以 root
     起 executable、按 userDir 拼日志路径,没做 `..` 卫生。

  codesign 门是主防线,但 root 服务的原则是**不信任任何输入,即便来自"自己的 app"** ——
  app 一旦被注入,这些就是直通 root 的路。加 `InputValidation`(网卡名白名单 + 路径卫生),
  放**共享层** CoastHelperProtocol 而非 helper 内部:可单测(executableTarget 不能 @testable
  import),且 app 侧下发前也能用同一套 fail-fast。新增 4 条用例把「什么该拒」钉死。
- 2026-07-31：**实证审查 ConfigBuilder 的 YAML 注入面,抓到并修掉一个真注入**(176 用例全绿)。

  不靠推理 quote 是否正确,而是**构造含恶意字符的节点名**(换行/引号/冒号/井号/逗号/YAML
  锚点等 11 种),跑完整 ConfigBuilder,验证有没有越权。**抓到真漏洞**:含真实换行的节点名
  (机场可控),经 subscribe.yaml 往返后重新拼进 full.yaml 时,`YAMLSurgery.quote` 用单引号
  包裹但**没处理真实换行**,于是标量跨行,ConfigBuilder 自己的正则提取(`proxyNames`)把续行
  当成新的 `- name:` 条目 —— 机场由此能凭空注入 proxy/组引用。

  修在**唯一咽喉点**:三个 quote 函数(`YAMLSurgery.quote` / `SubParser.yq` /
  `YAMLScalar.quote`)在引用前统一**剥控制字符**(折叠成空格)。节点名/规则值里的换行没有任何
  合法用途,一处堵死整类问题。dump 验证:恶意名折叠成单行、proxies 数量精确等于输入数、
  EVIL 只作为某个合法节点名的子串存在,不再是独立条目。

  ★ 过程中我的**测试断言一度写错**(把「名字含 EVIL 子串」当成注入),dump 出实际 YAML 才
  看清:一个字面叫 `...EVIL...` 的节点是无害的,危险的是它变成**独立的一行 `- name:`**。
  修正为「proxy 数量 == 输入数」这个正确的不变式。**先 dump 看真相、再改断言**,没有为了
  让测试变绿而放松它。
- 2026-07-31：**实证审最后一个外部数据入口:核心 REST 返回的 JSON**(187 用例全绿)。

  这是唯一还没实证过的注入/健壮性面。先确认 **SQL 注入不成立**:`HistoryStore` 的动态 SQL
  片段(`column`)来自封闭枚举、`scopeClause` 是固定字面量,所有数据值一律参数绑定。
  但「不成立」也用实证钉死,而非看一眼下结论:
  - `x'; DROP TABLE conn; --` 作为 host → **原样存入、表不被破坏、可原样查回**(参数绑定 =
    payload 只是数据);
  - host 含单双引号/换行/制表/井号、字节数是错类型或缺失、id 缺失、chains 空、3000 条批量
    —— 全部不崩、口径正确。

  `ProxyTree`(解析 `/proxies` 的沿链递归)的健壮性:自环、互指成环、超长链(>16 步)、
  now/history 是错类型 —— 全部**安全终止**(16 步上限 + 访问集),排序对 `Int.max`/负值不崩。

  结论:三个外部数据入口(用户输入的订阅/规则、局域网 helper 输入、核心 REST JSON)都已
  实证审过。**连续五轮对抗性审查发现并修掉 3 个真安全 bug**(ARP 广播、helper 注入面、
  YAML 换行注入),另确认 2 处(SQL、代理图)本就安全但补齐了实证防线。
- 2026-07-31：**并发安全审查:用 Swift 6 模式当权威检查器,结论=无活的数据竞争**。
  见 [`docs/concurrency-audit.md`](docs/concurrency-audit.md)。

  ★ 一次**方法上的自我纠错**:先试「摘掉 `@unchecked Sendable` 看还编不编得过」,但意识到
  本项目用 Swift **5** 语言模式、对 Sendable 检查宽松,「编过」根本不证明安全 —— 那个试验
  没有说服力。改用真正权威的手段:把 CoastKit 切到 Swift **6** 模式跑一遍,那是真正的
  数据竞争检查器。(试完即还原,不留改动。)

  v6 报了 14 处,全部集中在两个边界、都不是活 bug:
  - `ClashAPI`(actor)→ MainActor 传 `[String: Any]`(12 处):JSON 全是值类型、actor 不保留
    引用,当前可证安全,只是类型没表达;
  - `MacHelperClient.withProxy` 的 sending 闭包(2 处):`OnceFlag` 保证只跑一次。

  决定**维持 v5**:彻底迁 v6(把 ClashAPI 返回类型结构体化)是中等 refactor,为「今天已安全」
  的东西冒引入 bug 的风险不划算。文档把「v6 会标记什么、正解是什么」记清,将来照此迁。
  11 个 `@unchecked Sendable` 类型的安全依据逐个核对入表。**承诺逐个兑现,无活竞争。**
- 2026-07-31：**功能对等审计:补上唯一漏掉的页面——实时连接查看器**(191 用例全绿)。

  逐一核对 Qt 的每个 QML 页面/窗口在 Swift 侧是否都有对应,发现一个**真缺口**:
  Qt 有 `ConnectionsWindow`(每条活动连接的 host/进程/出口链/上下行,可逐条关闭、可清空),
  而我的 Swift 侧后端钩子齐全(`ClashService.closeConnection`/`clearConnections`/
  `onConnectionsSnapshot`)、状态页也显示连接数,**却没有查看/管理连接的 UI**。

  补法:
  - `ConnectionRow`(CoastKit):把 `/connections` 的裸 `[[String: Any]]` 在进 UI 前解析成
    类型化结构,UI 不碰裸字典。配 4 条单测(字段解析、host 回退 IP、按流量降序、跳过缺 id)。
  - `AppState.connections`:复用每拍的 /connections 快照解析,不额外发请求。
  - `ConnectionsView`:sheet,状态页「连接」卡可点打开。**真机截图验过渲染**——
    三条假连接的 host/进程/出口链徽标(代理品牌色、DIRECT 灰)/上下行/关闭按钮都在位。

  其它 QML 逐一核对结果:`NpcapWindow`=Windows 专属(macOS 正确不做);`DeviceDetailWindow`
  =依赖网关流量聚合(随网关);其余页面全部已有对应。**至此页面级功能对等达成。**

  截图用的两个临时钩子(COAST_FAKE_CONNS/OPEN_CONNS)已移除,不留调试代码。
- 2026-07-31：**行为对等审计:接上三个「存了值却没接上」的空开关**(191 用例全绿)。

  逐一核对需要运行时行为支撑的 config 开关,发现三个**空开关**——用户拨了但运行时什么都不
  发生(比缺功能更糟,用户以为它在工作):
  - **`autoUpdateMinutes`**(定时自动更新订阅):接上定时 Task,内容变了才重建配置
    (周期短时「没变就不重建」很关键,否则每次到点白重载一次核心);
  - **`autoTheme`**(跟随系统深浅色):启动对齐一次 + 监听 `AppleInterfaceThemeChangedNotification`;
  - **`closeToTray`/mini**(启动静默到托盘):启动即 orderOut 窗口、只留托盘。

  验证都走真机、不只看编译:
  - closeToTray:临时设 mini:true 启动,**在屏主窗数 = 0**、托盘项仍在(用 optionOnScreenOnly
    核实,`optionAll` 会把已隐藏窗口也算进来,差点误判);
  - autoTheme:自检读到「深色」不崩。

  ★ **自检抓到一个真崩溃 bug**:`systemIsDark` 原用全局 `NSApp`,而它在 app 启动完成前是 nil,
  早启动路径读它直接崩。改用恒有效的 `NSApplication.shared` + `AppleInterfaceStyle` 兜底。
  正常流程虽从 `start()` 调(NSApp 已就绪),但依赖这个时序脆弱 —— 自检把它逼出来了。
  临时钩子用完即移除。
- 2026-07-31：**行为对等审计续:又抓到一个死开关 + 补节点切换反馈**(191 用例全绿)。

  比对 Qt `QmlBridge` 暴露的属性 vs 我的 `AppState`,发现:
  - **`nodeSwitchNote` 又是个死开关**:配置存了「切换节点时弹出通知」,但**没有任何代码发通知**,
    而且 `TrayController.notify` 零调用者 —— 整个通知功能没接上。接上:切节点成功 → 按开关弹通知。
  - **切节点无即时反馈**:`selectNode` 发 PUT 后 UI 没任何变化,用户不知道点没点中(Qt 有转圈)。
    加 `ClashService.switchingTo`(PUT 在途时非空)+ NodesPage 在该节点转圈 + **在途忽略重复点**。

  顺带把通知样板抽成 `Notifier`(CoastKit):切节点(AppState)和托盘(TrayController)都要发,
  免得那段 UNUserNotificationCenter 授权+投递抄两遍。

  ★ 连续两轮「行为对等审计」共抓到 **4 个死开关**(autoUpdate/autoTheme/closeToTray/nodeSwitchNote)
  —— 它们的共性是「config 存了值、UI 有开关、运行时却没接上」,比缺功能更隐蔽(用户以为在工作)。
  这类只能靠**逐个核对 config 键是否有运行时消费者**才查得出,看功能列表看不出来。
- 2026-07-31：**补上最后一块数据对等:会话流量构成(直连 vs 代理)**(197 用例全绿)。

  这是 Qt `QmlBridge` 里我唯一还没做的数据(`directBytes`/`proxyBytes`)。核心的
  `downloadTotal` 只有一个总数、答不了「多少走了代理」,得**按连接攒增量**:每拍对每条连接
  算「比上拍多了多少」,按出口链累进直连桶或代理桶。

  抽成 `TrafficComposition`(CoastKit,纯值类型可测),6 条用例钉住几个易错点:
  - **攒的是增量不是每拍重复计总数**;新连接全部字节算增量;断开后不再累加但总数保留;
  - **核心把已有连接计数清零重来时 delta 取 0、不倒扣**(否则出现负流量)。

  StatusPage 加一根占比条 + 图例。**真机截图验过**:59.60 MB(代理 47.68 / 直连 11.92),
  数字与喂入的假数据精确吻合(累加逻辑对)。临时钩子已移除。

  **至此 `QmlBridge` 的数据面全部对等** —— 连续三轮行为/数据审计把页面、config 开关、
  会话统计三块补齐,期间抓到 4 个死开关 + 2 个真 bug(NSApp 早启动崩、连接查看器缺失)。
- 2026-07-31：**★ 第一次用真正的 mihomo 核心校验生成的 full.yaml —— 通过**(199 用例)。

  此前 `full.yaml` 的正确性只靠 PyYAML(仅证明「是合法 YAML」)和我自己的正则反解。
  但**核心有自己的 schema**(规则类型、代理字段、组结构),一份合法 YAML 完全可能被它拒绝 ——
  真正的消费者才是权威判据。

  弄来 `Mihomo Meta v1.19.29`(经 ghfast 镜像;GitHub API 直连仍 403),用 `mihomo -t` 校验一份
  **完整场景**的配置:4 种协议的订阅节点(trojan/vless/ss/hysteria2)、自定义规则
  (DOMAIN-SUFFIX/IP-CIDR/PROCESS-NAME)、区域组、设备策略(SRC-IP-CIDR)、redir-port、
  DNS、sniffer、profile —— **TUN 开与关两种都通过**。

  这是对 `ConfigBuilder` 这条链路最有分量的一次验证:它证明的不只是「语法对」,而是
  「真核心愿意加载」。测试在 `COAST_TEST_MIHOMO` 未设时**自动跳过**(不做硬依赖,别人机器
  和 CI 上未必有核心),`scripts/regression.sh` 会明确报告启用与否。
  核心存到 `~/.local/share/coast-devtools/mihomo` 供以后回归复用。
- 2026-07-31：**★ 端到端:ClashService / CoreProcess 对真实运行的核心跑通**(204 用例)。

  此前 `ClashService` 只对**假数据**验过(ProxyTree/排序/模式归一那些纯函数),
  `CoreProcess` 只验过「核心不存在」的路径。这轮让它们对**真跑着的 mihomo** 说话,5 条:
  - `ClashService` 轮询真核心:解析出策略组与节点、模式从核心读回;
  - **模式切换真的落到核心**并读回;
  - **选节点真的生效**(核心的 `now` 变了);
  - **secret 鉴权真的在起作用**(错 secret 拿不到数据 —— 证明我们发的 Bearer 头被校验);
  - **`CoreProcess` 起停真核心**:能起来、REST 通、能干净停掉。

  隔离做得很小心:非常规端口(193xx/178xx)、`allow-lan:false`、临时目录;
  `CoreProcess` 那条要把核心放到 `AppPaths` 期望的位置,**用户已装核心时直接跳过**(不覆盖他的),
  用完即删。跑完核对过:`command/` 空、无遗留 mihomo 进程、系统代理 `HTTPEnable:0` 未被动过。

  代价与取舍:带真核心跑全套约 87 秒(7 条测试真的在起进程),不带时 0.14 秒。
  所以维持 `COAST_TEST_MIHOMO` 开关 —— 日常与 CI 不受影响,要深验时才开。

  **至此除「helper 以 root 起核心 + ARP 接管/复原」外,所有链路都对真实核心验过了。**
  那一段仍卡在正式证书签名。
- 2026-07-31：**延迟测速对真核心验通,并实证了「独立连接池」这个设计的必要性**(207 用例)。

  测速是 `ClashService` 里最复杂的一段(并发闸门 + 选组串行锁 + 首字节计时),此前只验过
  纯排序逻辑。补三条对真核心的:
  - `ClashAPI.delay` 拿到**真实延迟**(DIRECT→gstatic 实测 ~98ms);**连不通的节点返回 `nil`**
    而不是 0 或崩(这个区分很重要:0 会被排序当成"有延迟");
  - 测完延迟**真的注入节点列表**(testNodeDelays → pollNodes → NodeInfo.delay);
  - ★ **40 个并发延迟请求(全部会超时)期间,常规轮询 `/configs` 依然秒回**。

  最后一条最有价值:`ClashAPI` 给延迟测速单开 `delaySession` 这个设计,是从 C++ 版继承的
  ——防「几十个测速请求占满 6 连接/主机的池,把 pollNodes/selectNode 挤到排队卡死」。
  这个设计此前**从没在真并发下验过**,现在有实证了(洪水期间 /configs 耗时 <3s 且拿得到数据)。

  207 用例:不带真核心 0.13 秒,带真核心 47 秒。


## 2026-07-31 · 用真开发者证书跑通 helper —— 卡了十几轮的阻塞解开

用户把证书放到了下载目录。实际上钥匙串里**早已**有可用身份
`Developer ID Application: yuehong sun (6AXTRT5TV4)`，Team ID 正是
`HelperConstants.clientCodeRequirement` 里硬编码要求的那个 —— 不必导入 p12 就能真签。
于是第一次真正装上并连通了 helper：

```
SMAppService 状态: enabled → helper 版本: 0.8.0 → ✅ XPC 通道可用
```

双向代码签名鉴权用 `codesign -v -R=<requirement>` 分别验过两端，都满足。

### 由此暴露并修掉的四个真问题

1. **`.app` 被原地替换后，以 root 运行的仍是旧 helper —— 且可能长期如此。**
   daemon 被拉起后 `RunLoop.main.run()` 常驻，重新注册**不会**换掉它。
   实测：替换成新版、重注册成功，`getVersion` 仍返回旧版本，PID 也没变。
   影响面是每一次程序更新。修法：协议加 `terminate`，helper 先还原 ARP 接管、
   再停核心、才退出，launchd 下次按需拉起新版。
   验证：`0.7.0 / PID 22210` → 自愈后 `0.8.0 / PID 22302`。

2. **第一版自愈把事情弄得更糟。** 写成「先 unregister 再 register」，实测注销成功、
   紧接着的注册报 `Operation not permitted`（launchd 的注销是异步的），用户从
   「有一个陈旧但能用的 helper」变成「完全没有 helper」。
   改成：先只 `register()`（对已 enabled 的服务重复注册是安全的），
   用**行为**（XPC ping）而非 `status()` 判定成败，不行才注销 + 退避重试。
   —— `status()` 会说谎：报 `.enabled` 而 XPC 无人应答的情况实测存在。

3. **helper 版本号恒为 1.0，探针失灵。** 它嵌在 `__TEXT,__info_plist`，是链接期产物，
   而那个 plist 只作为一条 `-Xlinker` 参数存在，**不在 swift build 的依赖图里** ——
   改了构建系统照样判定无需重建（连 touch 源码强制重编都不保证重链）。
   改成运行时读所在 `.app` 的 `Info.plist`。`getVersion` 是唯一能问出
   「现在跑的到底是哪一份 helper」的探针，恒定值等于没有。

4. **特权操作零日志。** 一个以 root 改系统代理、起进程、往局域网发 ARP 接管别人流量的
   daemon，没终端没界面，出问题时除系统日志无处可查。五个操作全部加 os_log 审计，
   带调用方 PID（`NSXPCConnection.current()`）。只记 IP/MAC/端口/路径，不记凭据 ——
   系统日志是全机可读的。
   注意 `os_log` 的 `CVarArg...` 无法转发数组，那样编译过但输出全错。

### 顺带记下的坑

- zsh 里 `log` 被同名函数截掉，`log show ...` 报 “too many arguments” 或静默返回空。
  查系统日志必须用 `/usr/bin/log`。这让我先前几次查询假阴性。
- 自检探针的超时必须**长于**被测组件自身的超时，否则屏蔽掉真实错误。
- `SelfTests` 是 `@MainActor` 隔离的，里面写 `Task { } + 信号量阻塞主线程` 会死锁
  （任务体继承主 actor，排在被阻塞的主线程上，永不开始）。要用 `Task.detached` + 跑 runloop。

### 仍未验证

- **真机接管（ARP 欺骗 + PF rdr）本身**没有实跑。跑它会改动局域网状态与
  `net.inet.ip.forwarding`，需要用户明确同意后再做。
  （附带确认：本机 `ip.forwarding` 现为 1，但不是我们设的 —— `Redirector.stop()` 在
  `!active` 时提前返回，根本不碰它。）
- 11 种机翻语言仍待母语者校对。


## 2026-07-31(续) · 核心意外死亡:一个会让用户彻底断网的空洞

`CoreProcess.terminationHandler` 只把 `isRunning` 置 false 并打一行日志，**没有任何人监听**。
后果是复合的：

- `CoastController.isCoreRunning` 是本类的存储属性，只在 `startCore`/`stopCore` 里更新 ——
  核心自己死掉后它**一直是 true**。于是 `startCore()` 的 `guard !isCoreRunning` 永远提前
  返回，**核心再也起不起来**；`toggleCore()` 也会走成 `stopCore()`，用户第一次点击像没反应。
- 系统代理仍指向一个没人监听的端口 —— 那不是「代理失效」，是**彻底断网**，
  而界面还显示「运行中」。
- 被接管的设备仍在把流量发给一个不再转发的网关，同样直接断网。

修法：`CoreProcess` 区分「我方要求停止」与「它自己死了」（`stopRequested` 标志），
后者触发 `onUnexpectedExit`；`CoastController` 收到后撤销设备接管、关掉系统代理、
置回状态，并通知 UI 弹一条系统通知。

**回归测试证明非空转**：摘掉修复后精确地在两条断言上失败
（`isCoreRunning → true` / `notified`），恢复后通过。

### 连带修掉的两个测试基础设施问题

1. **测试在往用户真实的 config 目录里写 `full.yaml`。** 加了 `COAST_DATA_DIR` 数据根覆盖，
   由 `scripts/regression.sh` 在进程启动时设一次。
   ★ 第一版是让测试自己 `setenv` —— 结果 `RealCoreE2ETests` 单跑全过、全量跑必挂：
   环境变量是进程级共享可变状态，而 swift-testing 默认**并行**跑套件。
   这正是我在同一个提交的注释里刚警告过的顺序依赖，转头就自己踩了。

2. **`RealCoreValidationTests` 一直依赖外网。** 它在临时 `-d` 目录里跑 `mihomo -t`，
   而生成的规则含 `GEOIP,CN` —— 目录里没有 `Country.mmdb`，mihomo 就去**联网下载**。
   之前能过纯粹是当时下得动；这次下不动，等满 90 秒超时后报
   `can't download MMDB: context deadline exceeded`，看起来像是我们生成的配置有问题。
   改成把内置 mmdb 拷进去（正是 app 的 `seedGeoIP()` 做的事）：90 秒联网失败 → 0.16 秒本地通过。

裸跑 `swift test` 与隔离跑现在都是 212 全绿；缺运行器配置时**显式打印跳过原因**，
不静默、也不误判为失败。

### 顺带记下

- 把 `static let` 改成计算属性后，若该符号被用作 public 默认参数，增量构建会残留旧的
  默认参数生成器符号 → `ld: symbol(s) not found`。必须 `rm -rf .build` 整清。


## 2026-07-31(续二) · 零配置透明代理**从来就不可能工作过**

有了能真装的 helper 之后，用审计日志查连接生命周期，撞上一个**致命且此前完全不可见**的设计冲突：

- helper 把「撤销设备接管」绑在连接失效上（`invalidationHandler` / `interruptionHandler`
  → `redirector.stop()`）。这是命门：app 崩了必须把被 ARP 欺骗的设备放回去，
  否则那几台设备会一直把本机当网关、直接断网十几分钟。
- 而客户端 `MacHelperClient.withProxy` 是**每次调用建一条连接、用完即弃**
  （`defer { connection.invalidate() }`）。

两条单看都合理，合起来是：`startRedirect` 一返回，那条连接就 invalidate，
helper 立刻把刚建立的接管撤掉。**接管从来维持不住一秒。**

实证（在真 helper 上跑一次最普通的 `getVersion`）：

```
audit] 客户端连接 invalidated → 撤销接管
audit] stopCore [caller pid=24950]
audit] 客户端连接 invalidated → 撤销接管
```

任何一次普通调用之后都跟着一条撤销 —— 这条日志是这一轮才加的，
在此之前这个冲突**没有任何可观测的痕迹**，且只会在真机接管时暴露
（而真机接管一直被「没有可用签名」挡着）。

**修法**（两端都要动，缺一不可）：

- helper：`redirectOwner` 只记**发起接管的那条**连接，且只在 `redirector.start` 真的成功
  之后才记。只有它断开才撤销接管，普通调用的连接来去自如。
  （失败也记的话会留一个悬空 owner，挡住下一次真正的接管认领。）
- 客户端：接管期持有一条**长连接**（`redirectConnection`），从 `startRedirect` 活到
  `stopRedirect`；`withProxy` 加 `on:` 参数复用它且不在返回时 invalidate。
  启动失败则立即释放，不留空连接。

验证：修好后再跑普通调用，日志里不再出现撤销；`startRedirect`/`stopRedirect`
都走 `CoastController` 持有的**同一个** `MacHelperClient` 实例（`private let helper`），
长连接挂在正确的对象上。

### 仍需真机确认

「发起方连接断开 → 撤销接管」这一半**没法在不真做 ARP 欺骗的前提下验证**。
handler 会可靠触发是已证实的（正是靠它才发现这个 bug），新增的只是 owner 过滤。
真机接管测试要改动局域网状态，等你明确同意再做。


## 2026-07-31(续三) · 接管的两个「进程死了但状态还在」缺口

沿着上一条（连接生命周期）继续查**镜像方向**，找到两个：

### 1. helper 崩溃后，内核状态永远回滚不掉

`Redirector` 的内进程回滚很完整（start 每一步失败都回滚前面的），但 **PF anchor 和
`ip.forwarding` 是内核里的东西，活得比进程久**。helper 被 SIGKILL 时它们原样留着；
launchd 按需拉起的新实例 `active == false`，`stop()` 走 `guard active` 直接提前返回 ——
于是这两样**永远收拾不掉**。后果不是「功能失效」，是 rdr 规则继续把流量重定向到一个
没人监听的端口，即被接管设备持续断网。

修：接管建立时往 `/var/db/com.yuehongsun.coast.helper.redirect` 落一份记录（内含接管前
`ip.forwarding` 的原值），正常停止时删除；helper 启动时若发现记录还在，就说明上次死在
接管中间 —— 清空**我们自己命名的** anchor（不碰别人的 PF 规则）、把 forwarding 恢复成
记录里的原值、删记录。

ARP 不在回滚之列：欺骗随进程消失，设备缓存会自行老化（十几分钟），而我们已经不知道
当时接管的是哪几台、真网关 MAC 是什么，无从定向复原。

**实证**（`COAST_REDIRECT_RECORD` 覆盖路径后直接跑 helper 二进制）：

```
audit] 检测到上次接管未正常收尾(helper 疑似崩溃/被强杀)，正在回滚内核状态
audit] 已清空 PF anchor coast.redirect;ip.forwarding 恢复为 0
audit] Coast helper 启动 版本=1.0 路径=...
```

顺序正确（恢复在启动行之前），记录被清除；无记录时不产生任何多余动作。

### 2. app 不知道 helper 在接管期间没了

上一条修的是「helper 误以为 app 没了」，这条是它的镜像。接管连接被中断时
`activeRedirectIPs` 仍挂着，界面继续显示那几台设备「已代理」，实际上接管早随进程消失。

修：客户端给接管连接装 `interruptionHandler` → `onRedirectLost`；`CoastController`
收到后**重新接管一次**（设备侧开关没变，用户当初打开它就是授权了接管；而新 helper
启动时已经把遗留内核状态收拾干净了）。`stopRedirect` 里先摘掉 handler —— 我方主动停
不该被当成「丢了」。

### 仍需真机

这两条的「触发路径」都验过了（崩溃恢复端到端跑通、handler 装拆逻辑通过全量测试），
但**真正的接管过程**仍未在真机跑过 —— 那会改动局域网状态，等你明确同意。


## 2026-07-31(续四) · 「进程活着 ≠ 核心可用」——一个算出来却没人用的信号

接管那条链在没有真机的前提下已经挖到头，换到 `ClashService`。

**好消息**：`/traffic` 的看门狗（断流 2s 后重连）本来就写对了。补上端到端验证 ——
真核心 kill -9 再拉起，`coreReachable` 走完 true → false → true，2.2s 内自动接回。
这条路径是上一轮修好「核心崩溃后能重启」之后**才真正成为常态**的，之前没测过。

**问题**：`coreReachable` 被一路维护着，却**从没被任何地方使用** —— 是个死属性。
于是界面判断核心是否正常靠的是「进程还活着」（`isCoreRunning`）。而进程活着不等于核心可用：
mihomo 卡死、REST 端口被别人占住导致绑定失败、热重载崩在半路 —— 这些情况下进程都在，
界面照样显示「运行中」，实际上什么都不通，用户完全无从查起。

修：加 `coreUnresponsive`，在状态页点一条红字（沿用「增强灯亮着却不全局」那条已有先例）。

★ **不能直接照搬 `coreReachable`**：它在刚启动、第一帧数据到达之前天然是 false，
直接拿去点亮警告会让用户每次启动都看到一闪而过的红字。改用**连续失败轮数**去抖
（看门狗一轮 2s，两轮≈4s）。
**这个去抖不是想当然**：把阈值临时改成 1 重跑，误报测试立刻失败
（`coreUnresponsive → true` 出现在启动空窗期），改回 2 就过。

两条新测试：重启后自动接回、启动空窗期不误报。214 全绿。

### i18n

新增文案让覆盖率掉到 128/138。补齐 10 条 × 12 种语言，全部回到 138/138，
并确认它们真的进了 app bundle（ja/ru/zh-TW 抽查渲染值，不是只写进源 JSON）。
其中 7 条是更早几轮遗留的（连接查看器、设备页），一并补上。


## 2026-07-31(续五) · 把「算出来却没人用」变成一次系统性排查

上一轮 `coreReachable` 是死属性这件事值得推广：写脚本扫全部 214 个对外属性，
找出产品代码里零引用的 9 个。多数是误报（`description` / `errorDescription`
是协议实现，插值时隐式调用），但有一个是**真问题**：

### `RulesStore.Rule.ruleLine` 与 ConfigBuilder 各写了一份

同一个三元表达式写了两遍：

```swift
// RulesStore.swift:30
type == "MATCH" ? "\(type),\(node)" : "\(type),\(value),\(node)"
// ConfigBuilder.swift:366  ← 一模一样，独立的一份
```

`ruleLine` 的注释还写着「与 `ConfigBuilder.applyCustomRules` 的拼法一致」——
把一致性当成约定，却没有任何东西保证它。

而致命的是**测试测的是 `ruleLine`，产品代码里没人调用它**；真正写进 `full.yaml` 的是
ConfigBuilder 里那一份，无人看守。改坏它，测试照样全绿。甚至有一个测试就叫
「生成的规则行与 ConfigBuilder 的拼法一致」—— 它靠硬编码期望串来断言，
只能守住死代码那一侧。

修：ConfigBuilder 直接调 `RulesStore.Rule(...).ruleLine`，拼法只此一处。

**验证合并确实起作用**：把 `ruleLine` 改坏（`type,node,value` 顺序调换）后重跑 ——
现在会连带打挂 **ConfigBuilder 的测试**和 rules.json 端到端测试（合并前只会挂那个
测死代码的）。恢复后 47/47 通过。

### 顺带清掉的死 API

- `AppConfig.clashConfig` —— `AppPaths.fullConfig` 的纯别名，注释说「保留成员形式便于
  对齐 C++ 调用点」，但没有任何调用点。同一个东西两个名字。
- `Theme` 的 `shell` / `inputBg` / `inputBorder` / `scrollHandle` —— Qt 时代的遗留。
  确认过 `MainView` 上设了 `.preferredColorScheme(theme.dark ? ...)`，且带输入框的三个
  视图全是同一个 `WindowGroup` 内的 `.sheet`（继承环境），所以原生控件本来就跟着 app
  主题走。留着这几个硬编码色反而会引诱后来者绕过主题体系、和系统外观打架。

214 全绿，打包签名正常。


## 2026-07-31(续六) · 有界面、无落盘：四个设置改完重启就丢

继续「两处该一致却没人保证」的排查。端口这一项**通过**（`ConfigBuilder` 直接用
`DeviceStore.redirPort`，单一来源）。但把「设置页可编辑的字段」与「`persist` 调用」
对起来一比，露出 4 个：

```
❌ allowRule / allowRuleEnabled / noAllowRule / noAllowRuleEnabled
```

用户在设置页填节点过滤正则、点应用 —— `state.applyConfig(draft)` 更新内存，
节点**当场**被过滤，看起来完全正常；重启后全丢。

**根因**：`YAMLText` 只有嵌套读（`nestedValue`/`nestedBool`），**没有嵌套写**。
这四个键在 `use_rule:` 段下，而 `persist(key:)` 只能写顶层。界面接上了，
写入这条管道压根不存在。

补 `setNestedValue`/`setNestedBool`，与读侧解析约定严格对称（段块 = `^section:` 后每行
恰两格；段内键 = `^  key:`），三种情形：键在→只改那行；段在键不在→插到段首；段不在→追加整段。

### 写往返测试时又抓到一个读侧真 bug

用带 `#` 的正则试往返，`机场#1` 读回来成了 `机场`：

```swift
// 旧：引号内的 # 也会截断
"['\"]?([^'\"\r\n#]*)"
```

顶层的 `value()` 用的是同一个模式，两处都中。用户写一条含 `#` 的正则会被**静默截断成
另一条规则** —— 而且改完当场生效、重启才露馅，等于两层静默叠加。

改成 YAML 本来的读法：引号内允许 `#`，只有裸值遇 `#` 才当注释。两个读取共用同一个
`scalarTail` 与 `scalar()`。

### 把一次性排查固化成常驻检查

单元测试只能锁住**管道**（`persist` 存了能读回来），锁不住**接线**（SettingsPage 有没有调）。
所以把这轮用来发现问题的扫描做成 `scripts/settings_persist_check.py`，接进 `regression.sh`：
每个被 `$draft.X` 双向绑定的字段都必须出现在某个 `persist(...)` 调用里，
豁免必须写进 `EXEMPT` 并说明理由。

**验证它真能抓到**：临时删掉一行 `persist(... allowUse ...)`，脚本立刻指名报错、退出码 1。

`regression.sh` 全绿：220 用例 · 6 PASS · 0 FAIL · 1 SKIP（系统代理需正式授权，开发机验不了）。


## 2026-07-31(续七) · 「100% 覆盖率」是虚的

`i18n_check.py` 统计的是**标了 `.t` 的**中文串。从没标 `.t` 的串根本不进分母 ——
于是它永远显示 100%，而那些串对所有非中文用户直接漏出中文。

扫下来 17 处真·用户可见文案就这样躲着：「有新版 %@」「下载中 %d%%」
「%d 台设备已接管」「更新说明（%@）」…… 全是**插值串**（`"…\(x)…"`），
而我最初手写的扫描正则把含反斜杠的字面量排除了 —— **扫描工具自己也有同一个盲区**。

全部改成 `String(format: "…%@…".t, …)`，补 18 条 × 12 语言 → 156/156。

### 翻译一上线就会犯的一个 bug

`RulesEditor` 用 `message.hasPrefix("已")` 判断这条消息是成功还是失败（决定灰色还是红色）。
成功文案是 `"已保存并应用".t` —— 翻成英文 “Saved and applied”、日文「保存して適用しました」
都不以「已」开头，于是**每一种非中文语言里，保存成功都会显示成红色错误**。
改成由设置方直接给出 `messageIsError`。

这类「靠文案内容做判断」的写法在单语言下永远正确，翻译一开就全错，而中文开发机上
永远复现不了。

### 把排查固化成两道新检查

都会让 `i18n_check.py` 非零退出，并接进 `regression.sh`：

1. **有中文却没标 `.t` 的界面串** —— 堵住上面那个盲区。`SelfTests`/`I18n` 在豁免名单里
   （控制台输出、语言自称，本就不该翻译）。新检查比我手写的扫描更严，又多抓出 4 处。
2. **译文与源串的格式占位符是否一致** —— 对不上时 `String(format:)` 会按错误类型读参数，
   **只在那一种语言下崩**，中文开发机上永远复现不了。
   实测把 `ja` 的 `%@` 改成 `%d`，立刻指名报错、退出码 1。

`regression.sh`：7 PASS · 0 FAIL · 1 SKIP；220 用例全绿；
并确认格式串真的进了 app bundle 且占位符完好（ja/ru/fr 抽查）。


## 2026-07-31(续八) · 与 Qt 版的对等性：逐项核对而不是「看起来差不多」

重构的本职是「完全替代 Qt 版」，那就得逐项对。三个维度都过了一遍：

### 1. 配置项 —— 完全对等

Qt `AppConfig` 27 个字段 vs Swift 23 个：**0 缺失、0 多余**
（差的 4 个是 `clashConfig`/`clashExecutable`/`configDir`/`userDir`，路径类，已迁到 `AppPaths`）。
24 个 YAML 键名映射也**逐个对上**，老用户的 `config.yaml` 不会被误读。

### 2. 生成的 full.yaml —— 三处差异，都清楚

| 键 | 情况 |
|---|---|
| `redir-port` | 只有 Swift 写 —— 透明代理（macOS 特有）|
| `listeners` | 只有 Qt 写 —— **有意不做**，见下 |
| `speedtest` / `list` | 订阅文件字段，不是配置键（正则误报）|

`listeners:` 是 Qt 版的 LAN 代理做法：给每台设备发一组用户名密码，配 `IN-USER` 规则。
你明确否掉了（「不要用户配置任何信息，只要在软件上点开关就行」），Swift 改成
ARP + PF 透明重定向。这条分歧是设计决定，不是遗漏。

`speedtest` 在 Qt 里也是只写不改的遗留字段（永远写 `false`，没有界面能改），
只影响节点显示名的 `[speedtest]` 后缀。不实现。

### 3. 数据文件迁移 —— 新增测试锁住

这次是**替换**掉 Qt 版，用户的数据目录里就是 Qt 写出来的文件。解析器在某个不认识的字段上
翻车，用户升级后订阅全丢，而且丢得毫无征兆 —— 界面只会显示「没有订阅」。

新增 `QtMigrationTests`，直接拿 Qt 源码里**逐字段抄下来**的格式喂给 Swift：

- `subscribe.yaml`（含 Swift 未声明的 `speedtest` / `proxy` / `updateTime`）：
  两条订阅、节点、订阅级与节点级的 `use:` 都读得出来；
- **回写时不丢未知字段** —— 用户若装回 Qt 版，这些信息还在。文本手术的意义正在于此；
- `rules.json`（Qt schema `{area:[{name,type,rule}], rule:[{type,node,value}]}`）：
  区域组与自定义规则都读得出来，`MATCH` 的两段拼法也对。

224 用例全绿。


## 2026-07-31(续九) · UI 对等性扫出一个真功能缺口：ARP 欺骗检测

拿 Qt 的 i18n 表当地图：表里 401 条、Qt 界面在用 287 条、Swift 只用 156 条。
差集里大多是 Windows 专属（`wintun.dll`、Npcap）和文案碎片，但捞出一个**真功能**：

```
%1 也在劫持你代理的设备「%2」
%1 正在冒充网关或本机，可能在监听/代理你的流量
```

Qt 的 `DevicesController::onSecurityAlert` 会告警两类情形，Swift 版完全没有
（`劫持` 在 Swift 侧 0 处命中）。而本程序自己就在做 ARP 欺骗，「有没有**别人**也在做」
不是可选项：

- **设备被争抢**：两边每秒对着同一台设备发欺骗应答，它的网关指向来回翻 ——
  表现为那台设备网络时好时坏，而用户只会怪我们。
- **网关被冒充**：教科书式中间人，本机出网流量先经过它。做代理的更没理由视而不见。

### 实现取舍

判据只用**本机的 ARP 表**（`arp -an`），不额外抓包：检测要在「没开接管」时也生效，
而 BPF 嗅探要 root、要常驻，代价与收益不成比例。粒度粗一些，但两类最要紧的情形都覆盖。

`ArpWatch.evaluate` 写成**纯函数**（输入是一份显式 `Snapshot`）—— 否则它只能在
「恰好有人正在欺骗」的机器上被验证，等于验不了。

### 测到的几个会咬人的边界

- **本机自己的 MAC 占着某个 IP 不算欺骗** —— 不豁免的话接管期间会自己报自己；
- **大小写**：`arp -an` 与我们记录的大小写常常不同，不统一就会把正常状态误判成攻击；
- **取不到网关真值时不猜**：宁可漏报也不能误报；
- **节流**：同一威胁 30 分钟一次（与 Qt 一致）。每 30 秒弹一次只会让用户关掉通知权限，
  然后连真正要紧的那条也看不到。

### 数据源单独在真机上核过

`LanTopology.localMACs()` 手工算 `sockaddr_dl` 的偏移 —— 算错会**静默返回空集**，
于是「是我们自己」的豁免失效，接管期间把自己报成攻击者。这种错编译器不管、纯逻辑测试
也照过，所以专门写了一条对着 `ifconfig` 的 `ether` 行核对的测试。
`arpMap()` 的 MAC 规范化（补前导零、小写）同样单独核过 —— 不补零的话比对永远不相等。

告警显示在设备页顶部（排在「已接管 N 台」之前）+ 系统通知；4 条新文案 × 12 语言，160/160。
`regression.sh`：7 PASS · 0 FAIL · 1 SKIP。


## 2026-07-31(续十) · GeoIP 更新：一个自己注释里承认欠着的功能

继续拿 Qt 的 i18n 表当地图。这次命中 `GeoIP 数据库` / `Country.mmdb 是「按 IP 归属地区分流」…`。
Swift 侧只**种**内置库，没有任何更新入口 —— `CoreProcess.seedGeoIP` 的注释自己写着
「随「更新 GeoIP」功能一起在阶段 5 补 —— 现在还没有下载入口」。

后果是内置库只会越来越旧：`GEOIP,CN` 依赖 IP 归属，库过期就分流错。

### 移植的是一份事故复盘，不是一段代码

Qt 的 `MmdbFile.h` 记着 2026-07-29 09:37 的真机事故：下载路径只判
`error == nil && !data.isEmpty` 就原地截断覆盖线上库，于是半截 body / 错误页 / CDN 截断
都会被写下去。而**核心完全不报错** —— maxminddb 打得开、不 fatal，只是每次查询返回空。
`GEOIP,CN` 是 `MATCH` 之前最后一条、负责把国内流量留在直连的规则，它静默失效之后，
所有嗅探不出域名的裸 IP 目的地全部出海。表现是「国内国外都很慢」，日志里一个字都没有。

所以照搬了那套判据（`MmdbFile.swift`）：

- 体积下限（挡 HTML 错误页）；
- 末尾 metadata 段能解出 `node_count` / `record_size` / `ip_version` / 格式主版本；
- 由 `node_count × record_size` 算出的搜索树尺寸与文件长度自洽；
- **搜索树末尾那 16 字节分隔符必须全零** ★ 真机那个坏文件正是栽在这一条；
- 拿几个知名 IP 走一遍树，终止记录必须落在合法的数据指针区间内。

以及落盘策略：`stage()` 只写 `<target>.new`，**绝不碰线上库** —— 核心把它 mmap 着，
而且 mmdb 只加载一次，原地覆盖从来就没有「立刻生效」过，唯一效果是有概率毁掉好文件。
真正换上发生在起核心前的 `applyStaged()`；同一时刻还给线上那份做体检，坏了退回内置种子
（Qt 那次事故里**没有**这一步：版本戳已记成最新，坏库会一直用到上游发下一个 release）。

### 好文件那一侧的测试是必需的

八条测试里，坏文件五条一开始就全绿 —— 但 **真库解不出 metadata**。原因是标记常量：

```swift
Array("\u{AB}\u{CD}\u{EF}MaxMind.com".utf8)   // → C2 AB C2 CD … 是 UTF-8 编码后的字节
[0xAB, 0xCD, 0xEF] + Array("MaxMind.com".utf8)  // → 规范要求的裸字节
```

只测坏文件的话，一个永远返回「不合法」的实现也能全绿。拿真的 5.6 MB 内置库当已知-good
才是这组测试的支点。

界面：设置页「更新」区加按钮 + 说明；核心在跑时经本机代理下载（GitHub 直连不通时唯一能成的路径）。
3 条新文案 × 12 语言，163/163；`regression.sh` 7 PASS · 0 FAIL · 1 SKIP。


## 2026-08-01 · 日志页：Qt 那边有两个标签和一条彩色时间线，Swift 这边什么都没有

按「逐个元素对齐 Qt」继续，这轮是**日志页** —— 它是差距最大的一页，
之前基本就是一个 `List`，与 `qml/LogsPage.qml` + `qml/LogTimeline.qml` 几乎没有对应关系：

| Qt 有 | Swift 原状 |
|---|---|
| 「主日志 / Clash 内核」两个标签 | 无，只有一条流 |
| 按严重级别上色的圆点（红/橙/绿/蓝） | 无 |
| 时间戳在正文**上方**一行，`yyyy-MM-dd HH:mm:ss` | 与正文同行，只有时分秒 |
| **最新置顶**，新条目滚回顶部 | 最旧在上，滚到底 |
| 时间线是一张嵌套的 Card | 直接铺在页面卡上 |

尺寸逐项照抄 QML，没有「差不多就行」：标签栏上/左/右内距 10、标签间距 6、标签高 30、
左右内距 16、字号 13、选中态卡底 + 底部 2px 强调条（宽 = 标签宽 - 20）；标签栏与时间线间距 8；
时间线卡贴页面左/右/下边缘；列表上内距 6、下内距 10，行左右内距 10、圆点槽宽 14、
圆点 8×8 落在 (3,5)、正文列上内距 4 下内距 10、行内间距 2。

### 严重级别抽成了 CoastKit 的纯函数

`LogSeverity.of()` 是 Qt `LogModel::severityFor` 的移植，**判定顺序不能动**：
错误 > 警告 > 成功 > 信息。一条「已更新失败」两边关键字都命中，顺序错了就会显示成绿点，
而绿点的含义正好相反。放 CoastKit 而不是塞进视图里，是因为这是唯一能对着判定表逐条测的形态
（新增 5 组用例，含 `" ok"` 带前导空格这个**不是笔误**的细节 —— 没有它，
token / lookup / broken 里的 "ok" 都会被判成成功）。

### 两条流分开存，不在渲染时 filter

与 Qt 一样维护 `logs` 和 `coreLogs` 两个数组。`logs.filter` 要扫 2000 条、每帧一次，
而分开存每条日志只多写一次。路由口径与 Qt 完全一致：`ClashService` 的日志只进主线，
`CoastController` 的（它自己的编排消息 + 核心进程 stdout）两边都进 ——
对应 Qt 的 `CoreController::logUpdated` 整条路由进「Clash 内核」。

时间戳在 append 时定型（锁 `en_US_POSIX` + 公历：跟随区域设置的话，
和历/佛历用户看到的年份会是 R8、2569，而这一栏的用途正是和核心日志对时间）。

### 验证到哪一步，说清楚

- `swift build` + `swift test` 286 用例全绿（原 281 + 新增 5）；`i18n_check.py` 199/199
  —— 三条新文案「主日志 / Clash 内核 / 暂无日志」本就在与 Qt 共用的翻译表里，未新增条目。
- ⚠️ **这一页没有截图验证**。开发机上 `Coast.app` 起来后 `window 1` 取不到（无窗口），
  设 `COAST_DATA_DIR` 时连 config.yaml 都没 seed 出来 —— 是打包产物在开发机上的启动问题，
  与本次改动无关，但意味着「渲染出来长什么样」这一步**没验**，只验到编译与逻辑。
- ⚠️ **`regression.sh` 在本分支 HEAD 上本来就是红的**（与本次改动无关，已 stash 复现）：
  `CoreCrashE2ETests.swift:132` 与 `MmdbFileTests.swift:148` 两条在 bash 环境下必挂、
  在 zsh 下连跑 11 次全过。是环境相关的既有问题，单开一件事处理。


## 2026-08-01(续一) · 关于页：Qt 是居中 hero，Swift 是一张设置式的分组表

继续逐元素对齐。关于页原来是「更新 / 更新说明 / 路径」三张左对齐的卡片摞在
`ScrollView` 里 —— 和 `qml/AboutPage.qml` 那个**居中 hero**（84px logo → 30px 标题 →
版本行 → 一次性提示 → 简介 → 三颗链接药丸）没有任何元素对得上。整页换掉，尺寸照抄 QML：
卡片内距 10、列宽 `min(卡宽-80, 420)` 居中、列间距 14、链接行前多让 6、
药丸高 30 / 半径 15 / 宽 = 文字宽+26 / 1px 描边、字号 12。

版本行的三态与配色逐条对齐：检查中=灰「正在检查更新…」、有新版=红
`v当前 → v最新  有新版本`（点击开发布页）、其余=绿 `#4da13e`「版本 X · 点击检查更新」
（点击查更新）。绿色是 QML 里的字面量、不是主题令牌 —— 那边也写死。

文案的**拼法**照抄（三段各自是翻译表里的一条），没有合并成一条格式串：
`"版本 "` / `"  ·  点击检查更新"` / `"  有新版本"` 在与 Qt 共用的表里就是分开的三条，
合并会让三条同时失配。

### 顺带补上一个 Qt 有、macOS 完全没有的元素：侧栏版本行的更新角标

`Components.swift` 里那个 `UpdateBadge` 一直**零引用** —— 写了但没接。Qt 的
`Main.qml` 侧栏底部是「`Ver: x.y.z` + 右上角红色 "new" / "core" 角标 + 有更新时版本转红，
点击打开更新窗」，Swift 侧只有一行灰字。现在接上了（角标贴右上角的做法与 QML 一致：
角标组的**垂直中心**对齐版本文字的**顶边**，即一半浮在文字上方）。点击跳关于页
——Swift 没有独立更新窗（更新内容并入关于页，是既有决定）。

为此把更新检查从关于页的 `@State` **提到 `AppState`**：侧栏和关于页两处都要读它，
各查各的会显示出不一致的结论。Qt 那边同样是一个共享的 `about` 控制器同时喂两处。
启动路径上延后 3 秒查一次 —— 角标的意义正是「不进关于页也看得见」，而核心刚起来时
经它出网的成功率远高于直连（直连 api.github.com 常年 403）。

### "core" 角标要先知道本地内核是哪一版 → 新增 `CoreVersion`

移植 Qt `AboutController::checkCore` 的前半段：跑 `<core> -v`，用**同一条正则**
`\bv\d+[0-9A-Za-z.\-]*` 抠版本。解析写成纯函数单独测（5 条）——
正则是这段唯一容易写错的地方，而抠错版本的后果是角标常亮或常灭，
两种都没有报错、也没法从界面上追查。探测失败一律 nil → **不置角标**，
Qt 的注释写得明白：「宁可不误报」。

### 两处 Qt 侧没有的东西一并去掉，说清楚

- **路径卡**（数据目录 / 配置目录 / 内核 + 在 Finder 里显示）：Qt 的关于页与设置页
  都没有这块，是 Swift 自己加的。按「精确匹配 Qt」去掉。
- **更新说明正文 + 内核最新版提示**：Qt 把它们放在独立的 `UpdateWindow.qml` 里，
  不在关于页上。内核与 GeoIP 的更新**入口**本来就在设置页，没有随之丢失。

⚠️ `UpdateWindow.qml`（程序/内核/GeoIP 三个竖排 tab + 说明卡 + 进度条 + 底部动作行）
在 Swift 侧仍**没有对应窗口**，是下一轮该补的。

`swift test` 291 用例全绿（原 286 + 新增 5）；`i18n_check.py` 197/197。
本页同样**未做截图验证**，原因与上一条相同（开发机上 `Coast.app` 起来后取不到窗口）。


## 2026-08-01(续二) · 更新窗：Qt 有一整个窗，Swift 一个元素都没有

`qml/UpdateWindow.qml`（351 行）在 Swift 侧此前**完全没有对应物** —— 上一条把
「更新说明正文」从关于页拿掉时就记着这笔账。现在补上 `UpdateView`：
左侧三颗竖排 tab（程序 / 内核 / GeoIP）+ 右侧内容卡（版本 + 更新说明）+
下载进度条（带 ✕ 取消）+ 底部动作行（国内代理下载勾选 / 关闭 / 更新）。

尺寸逐项照抄：窗 600×560、内距 10、行距 8；tab 78×46 / 半径 5 / 图标 16 / 文字 13 /
选中态品牌色底白字；内容卡半径 5 + 1px 描边 + 内距 10 + 行距 6；说明卡半径 4 +
内距 8 + 正文 12px 行距 1.3；进度条高 22 / 半径 4 / 进度内缩 1；✕ 24×24 圆、
悬停转红底白字；勾选框 16×16 半径 3；关闭 80×30、更新 100×30、均半径 5。

Qt 那边是独立顶层窗，这里沿用本项目对 `ConnectionsWindow` / `RuleEditorWindow`
的既有做法以 sheet 呈现，尺寸取 Qt 的 600×560。入口也回到 Qt 的位置：
**点侧栏版本行打开更新窗**（上一条临时指向关于页，现已改正）。

★ **一处刻意的行为差异**：「程序」页的「更新」不做自动下载安装，只打开发布页
（自动替换 .app 要处理签名、公证与「正在运行的自己」——既有决定，不是这次漏了）。
按钮与 Qt 同名同位，点完在状态行如实写明「已打开发布页，请手动下载安装」。
**内核与 GeoIP 两页是真的在下载安装**，进度条与 ✕ 取消都接的是真任务。

顺带 `UpdateChecker` 加了 `latestCoreRelease()`（版本号 + 说明正文一次请求拿回）：
角标只要版本号、更新窗要说明，为一段说明再打一遍 GitHub 不划算 ——
匿名调用每小时只有 60 次。`latestCoreTag()` 保留为它的薄封装。

### 修了 `i18n_check.py` 的一个盲区：带转义的文案永远报缺翻译

GeoIP 那段说明里有 `\n\n`。脚本是直接从**源码文本**里抠 key 的，抠到的是
「反斜杠 + n」两个字符，而与 Qt 共用的 JSON 表里存的是一个真换行 ——
于是这条**明明在表里**却被报成 12 种语言全缺。加了一层 `unescape()`：
查表用的 key 必须是**运行时**那个串，不是源码里那串字符。

新增文案只有一条（「已打开发布页，请手动下载安装」，Qt 没有这个行为所以表里没有），
已补齐 12 种语言。`i18n_check.py` 209/209。

`swift test` 291 全绿；`settings_persist_check.py` 14/14。本窗同样**未做截图验证**
（开发机上取不到窗口，原因见前两条）。


## 2026-08-01(续三) · 设置页：Qt 是四个标签，Swift 是一张从上滚到底的表

`qml/SettingsPage.qml`（829 行）是剩下差距最大的一页。Qt 那边是
**顶部 4 个标签（系统 / 过滤 / 区域 / 规则）+ 右上「应用」**；Swift 侧是一列
网络/行为/规则/节点过滤/外观/更新/系统的 section 从上滚到底，没有标签、没有分组卡，
控件全是系统原生的（高度随 macOS 版本变，尺寸根本对不上）。整页换掉。

### 手画了一整套控件

Qt 那批 inline component 逐个复刻到 `SettingsControls.swift`：
`SettingCard`（左右内距 14 / 上下 12 / 头部图标 15 + 标题 13 均 accentStrong / 与首行间距 6）、
`SettingRow`（行高 40、标签靠左撑满省略号、控件靠右）、`CardDivider`（1px、透明度 0.6）、
`ThemedSwitch`（外框 46×24、轨道 40×20 半径 10、滑块 16 白色内缩 2、120ms 过渡）、
`ThemedField` / `ThemedCombo` / `ThemedEditCombo`（30 高、半径 3、获焦描边转品牌色）、
`ThemedSpin`（右侧上下各半格的 ＋/−）、`PillButton`（高 30、半径 4、主/次两态）、
`SettingTab`（84×32、14px、选中底部 2px 条）。

**不用系统原生控件**是有原因的：Qt 每一个都是手画的、尺寸写死成具体数字，
而 `Toggle`/`TextField` 的高度由系统决定、随 macOS 版本变 —— 和「精确匹配 Qt」直接冲突。
为此把 `inputBg` / `inputBorder` 两个令牌加回 `Theme`（早先按「无人引用」删过一次，
那时设置页还在用原生控件）。

### 落盘语义也按 Qt 分成两类

不再是「全部等点应用」：**开关 / 下拉即时生效**，**Host / 端口 / 过滤正则等文本输入等「应用」**
（改一半的中间值直接生效会打断核心：把 7890 改成 1080，输到 "108" 时就已经重启过一次核心）。
Qt 的「应用」按钮同样只在系统 / 过滤两个标签上出现，区域 / 规则是改完即生效。
「应用」在区域/规则标签上用的是**透明占位**而不是隐藏 —— 隐藏会被布局移除、
标签栏随之变宽，切标签时整条顶栏跳一下（QML 注释里专门写了这一点）。

### 区域 / 规则从 sheet 搬回页内标签

原来它们挤在一个叫 `RulesEditor` 的 sheet 里（620×460 的双段选择器 + 两张内联可编辑表），
Qt 是**设置页的两个标签 + 每条一个独立编辑窗**。现在按 Qt 来：
列表行高 36 / 左右内缩 10 / 半径 3 / `nodeRowBg` / 内容左 10 右 6，列宽逐个对齐
（区域 150+90+fill，规则 150+130+fill），行尾 ✎ ✕ 各 30×24。
编辑器改成 `RuleEditorSheet`（460×440，对齐 `qml/RuleEditorWindow.qml`）与
`AreaEditorSheet`（440×470，对齐 Qt 设置页里那个 `areaEditor` 窗）。
规则编辑器补上了 Qt 有而 Swift 没有的**进程规则可搜索下拉**（类型选 PROCESS-NAME /
PROCESS-PATH 时，「值」变成系统进程列表，`ProcessChoices` 走 `ps -Axo comm=`）。

★ **区域编辑器的字段与 Qt 那个不同，是有意的。** Qt 的编辑器写
`{name, type, url, interval, proxies}`，而 **Qt 自己的 `ConfigBuilder::applyCustomRules`
读的是 `{name, type, rule}`，且 `rule` 为空就整组跳过** —— 用 Qt 那个编辑器建出来的分组
会被 Qt 自己的配置生成器静默丢掉。那是 Qt 侧两代 schema 并存留下的 bug，不是该照搬的行为。
这里按**真正被消费的字段**做：名称 / 类型 / 节点名正则。

### `settings_persist_check.py` 悄悄变成了一个永远绿的检查

改完页面后它报「可编辑字段 0 个 ✅」—— 它查的是 `$draft.X` 双向绑定，而新页面
**根本没有那个 `draft` 结构**了，正则匹配到 0 个字段，于是永远通过。
**一个永远通过的检查比没有检查更危险**，因为它看起来还在把关。

换成与写法无关的判据：**每个含 `state.applyConfig(` 的块，必须在同一块里含
`AppConfigLoader.persist(`**（写内存不落盘 = 重启即丢，正是要拦的那件事），
并在**匹配到 0 个块时主动失败**，免得同样的事再发生一次。
验证它真能抓到：临时删掉 `setThemeLight` 里那行 persist，脚本立刻指名报错、退出码 1。
现在 7 个写配置的块全部落盘。

`swift build` + `swift test` 291 全绿；`i18n_check.py` 208/208
（新增 5 条文案，其中「共 %d 条 / 组」直接沿用 Qt 已有译文只换占位符）。
本页同样**未做截图验证**，原因与前几条相同。


## 2026-08-01(续四) · 连接窗：Qt 记得断掉的连接，Swift 只有一份活快照

`qml/ConnectionsWindow.qml`（283 行）对 Swift 侧那个 83 行的 sheet，缺的不只是排版：
**整个「离线」概念都没有**。核心的 `/connections` 只返回当前还活着的连接，
断掉的下一拍就从快照里消失 —— 而这个窗口存在的意义正是回答
「刚才那条连到哪去了」。Qt 用 `ConnectionsModel` 把消失的条目留下来标成离线，
Swift 侧一直只有一份活快照。

新增 `ConnectionLedger`（CoastKit，纯值类型）：合并每拍快照，消失的标离线并
**保留最后一次的数值**（清零的话用户会以为它压根没跑过流量），断掉又回来的重新算在线。
9 条用例钉住合并、排序、reset 与三处筛选。

窗口按 Qt 重做：720×480（原 620×460）、顶栏 Online(N)/Offline(N) **分段按钮**
（26 高、离线段左端塞到在线段底下 3px、开=#4898f8 关=#909399）+ Search 框
（同高、左侧前缀标签 + 1px 竖线）；列表行高 42 / 半径 5 / 间隔 1 / 左右内距 10 /
元素间距 10；● 圆点 10px（在线 #67c23a、离线 #999999）；`[type] host` 14px；
四枚徽标各 22 高 / 半径 5 / 宽 = 内容 + 12 / 图标与文字间距 4；✕ 30×30 半径 3、
悬停转红底白字、离线置灰不可点。速率**不带空格**（`1.50KB`）—— 四枚徽标横排，
多一个空格就多挤掉一截 host。

补上 Qt 有而 Swift 没有的**右键「添加规则」**：用本行地址预填 value，
开的是上一轮做的 `RuleEditorSheet`（Qt 那边是 `openForValue`，同一个共享编辑器）。

### 顺带修了两处 `ConnectionRow` 与 Qt 对不上的地方

1. **行首的 `[type]` 取错了字段**。Qt 是 `metadata.type`（HTTP / Socks5 / Redir…），
   空了才退回 `metadata.network`；Swift 只有 `network`，于是永远显示 tcp/udp。已补 `type` 字段。
2. **透明网关的连接会把进程标成 Coast 自己**。局域网设备经透明代理进来的连接从
   127.0.0.1 发起，`find-process-mode` 查到的必然是 Coast 本身 ——「某台手机在访问 X」
   被标成「Coast 在访问 X」比留空更糟。按 Qt 的判据（`inboundUser` 以 `dev-` 开头）强制留空。
   另外出口链为空时显示 `-` 而不是空白（徽标空着像渲染坏了）。

进程名是**迟到**的（find-process-mode 头几拍常常还是空的），所以账本合并时
**空的进程名不覆盖已有的**，否则那枚徽标会一闪一闪地出现又消失。Qt 那边同一句注释。

`swift test` 300 全绿（原 291 + 新增 9）；`i18n_check.py` 206/206；persist check 7/7。
本窗同样**未做截图验证**。


## 2026-08-01(续五) · 设备详情窗：Qt 有 567 行，Swift 一个元素都没有

`qml/DeviceDetailWindow.qml` 此前在 Swift 侧**完全没有对应物**。PLAN 早先把它标为
「依赖网关流量聚合（随网关）」—— 但网关早已落地，这条注记过期了。

Swift 侧原本是设备行下面**内联展开**一小块（备注名 + 策略）。Qt 是
「页面只留列表 + 一个 600×720 的详情窗」，内容自上而下：头部（48 头像 / 18 名字 /
52×26 大开关）→ 依赖说明 / 不可代理的原因 → 2 列信息网格（列距 24、行距 6、键列宽 64）
→ 备注名（28 高）→ 实时流量卡（半径 6、内距 10）→ 近 7 天柱状（高 56、柱间距 6、
日期 9px）→ 策略（下拉 130）→ 常用域名 → 该设备的实时连接（行高 34、半径 4、内距 8）。
按 Qt 重做，内联展开随之删掉。

### 补数据比补界面难：三块数据此前根本不存在

1. **历史库的 `mac` 列一直是空的。** 建表时就有这一列、`Dimension.device` 也在查它，
   但**没有任何地方写它** —— 于是「这些流量是哪台设备跑的」在界面上永远查不出东西。
   补上 `sourceIP` → MAC 的解析，且**在落盘那一刻解析**（与 Qt 同）：查询时再解析的话，
   设备换了 IP（DHCP 续租）就会把历史流量算到别的设备头上。映射由 `AppState` 从台账喂进来，
   没喂时 `mac` 留空 —— 宁可查不到，也不写出错误数据。顺带加 `conn(mac, ended_at)` 索引。
2. **按设备的历史查询**：`recentDays(mac:)`（**恰好 7 项、缺天补 0** —— 缺天不补的话
   横轴会跳着走）、`topDomains(mac:)`、`total(mac:)`。
3. **每设备实时速率与会话累计**：核心不按设备统计任何东西，只能像 `TrafficComposition`
   那样每拍对每条连接算增量、按 sourceIP 归桶。抽成 `DeviceTraffic`（纯值类型，6 条用例）。
   钉住的几个易错点：攒的是增量不是每拍重复计总数；核心计数清零时按新连接算**绝不倒扣**
   （倒扣会显示成「↓ -3.00 MB/s」）；这一拍没流量的设备速率**归零**而不是维持上一拍
   （否则一台早就静默的设备会永远显示着「正在以 3MB/s 下载」）。

### 一处刻意未做

Qt 详情窗里那张 **90px 高的实时带宽曲线**没有做。它需要每台设备一条独立的采样序列
（Qt 用一个 1s 定时器往图里推点），而三个数字 + 近 7 天柱状已经把「这台设备在跑多少」
说清楚了。记在这里，不算漏。

### 顺带修了 `i18n_check.py` 的一个误报

语言选择器里那串语言**自称**（简体中文 / 繁體中文 / 日本語）被判成「漏标 `.t`」。
它们**故意不翻译** —— 用当前界面语言去翻译语言名的话，用户切到看不懂的语言后
就再也找不回来了。把这张表从设置页移到 `I18n.swift`（脚本对该文件本就免检，
且那才是它的正确归属），而不是给整个设置页加文件级豁免 —— 那会把这一页的检查全关掉。

`swift test` 306 全绿（原 300 + 新增 6）；`i18n_check.py` 231/231（新增 3 条文案 × 12 语言，
其中两条长文案 Qt 表里本来也没有）；persist check 7/7。本窗**未做截图验证**。


## 2026-08-01(续六) · 订阅页：Qt 是单列卡片 + 三个弹窗，Swift 是左右分栏

`qml/SubscriptionsPage.qml`（656 行）与 Swift 侧那个 336 行的 `HSplitView`
（左订阅、右节点）从结构上就不是一回事。Qt 是**单列 108 高的订阅卡** +
「查看节点 / 添加编辑 / 删除确认」三个弹窗。整页换掉。

Swift 侧此前缺的**元素**：类型徽章、URL 行、「每 N 分钟」元信息、顶栏的「应用」按钮、
五颗圆形动作按钮里的「查看节点」、节点弹窗、编辑弹窗里的类型分段与自动更新周期、
删除确认弹窗。缺的**功能**是后两样：
- **「应用」按钮**：拿当前订阅重建一次配置。用户手改过 `subscribe.yaml` 或想强制重来时
  此前没有任何入口。
- **每订阅的自动更新周期**：`setSubscriptionUpdateTime` 一直在 store 里，
  但没有任何界面调它 —— 又一个「后端有、界面没接」的口子。

尺寸照抄：卡高 108 / 左右内缩 10 / 卡内左 12 右 10 上下 8 / 行距 3；类型徽章半径 4；
名称 14、URL 11（**中间省略** —— 订阅链接的头尾都比中间那段有用）、元信息 11；
圆按钮 28×28 半径 14（选中态品牌色底白图标、危险态淡红底红图标）；
文字按钮高 30 / 最小宽 84 / 半径 `Theme.radius`；
节点弹窗 620×460、行高 46 半径 4、右侧「使用/停用」76×30、底部「全选 / 全不选 / 关闭」；
编辑弹窗宽 420、类型分段 60×26、周期框宽 90；删除确认弹窗宽 340。

节点行的启用态跟 Qt 一样靠**整行底色**（`nodeRowActive`）表达，不是一个小勾 ——
一屏几十条时，底色一眼就能扫出启用了哪些。

`swift test` 306 全绿；`i18n_check.py` 238/238（新增 1 条文案 × 12 语言）；persist check 7/7。
本页**未做截图验证**。


## 2026-08-01(续七) · 状态页逐尺寸核对：抓到一个把速率读成总量的真 bug

`qml/StatusPage.qml`（641 行）的**结构**此前已经对上了（2 列 × 3 行共六张卡、
曲线画在上传/下载卡的背景里、两张列表卡共用 `ConnLine`、今日卡有口径与维度两组切换）。
这一轮是逐个尺寸核对，结果抓到一个真 bug 加一串对不上的度量。

### ★ 真 bug：上传/下载卡显示的是**总量**，不是速率

Qt 用的是 `bridge.upText`，而它是 `speedText(up)` —— **带 `/s`**。
Swift 侧写的是 `Formatting.bytes(state.clash.up)`，于是把「1.2 MB/s」显示成「1.2 MB」。
`AppState.upText`（`Formatting.rate`）一直存在、也一直没人用 —— 页面绕过它自己格式化了一遍。
这个错在界面上没有任何异样：数字在跳、单位也对，只是**少了 /s**，
读起来像「一共传了 1.2 MB」。改成用 `state.upText` / `state.downText`。

### 对不上的度量，逐条改回 Qt

| 项 | Qt | Swift 原状 |
|---|---|---|
| 四张数据卡高 | 268 | 230 |
| 卡片圆角 | **4**（比 `Theme.radius`(5) 小一档） | `Theme.radius` |
| 卡内内距 | 12 | 10 |
| 页面内距 | 左/上/下 10、**右 0**（滚动条贴右缘） | 四周 14 |
| 卡间距 | 10 | 12（竖向） |
| 上传/下载卡头部 | 顶部 64 高的带子、左 14 右 10、间距 12、图标 28、标题 13、数值 24 | 整卡内距 12、间距 8、图标 18、标题 12、数值 22 |
| 小时柱高 | 34 | 44 |

上传/下载卡头部那条 **64 高的带子**不是随手写的：卡片会随窗口长高，若按整卡居中，
图和字会在卡片中央撞在一起，且窗口越高字越往下漂（Qt 的注释原文）。

### 一处保留的差异

今日卡的口径（全部/仅代理）与维度（进程/设备/域名）两组切换，Qt 是扁平色块 + 下划线，
Swift 用的是 `GlassSegmented`（液态玻璃分段）。**保留玻璃版** —— 那是本分支早先
按你的指示做的 macOS 原生观感，不是这次移植的疏漏。

`swift test` 306 全绿；`i18n_check.py` 238/238；persist check 7/7。本页**未做截图验证**。
