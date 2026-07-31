# Coast for macOS — Swift 重构路线图

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
- [x] RuleEditorWindow（规则/分组编辑器）；ConnectionsWindow/UpdateWindow 的功能已并入页面内，不另开窗

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
- [ ] **已拍板要做**。按评估的四步落地：
  - [ ] 1. lwIP 单独立成 SPM 的 C target，跑通 `swift build`（零风险，纯配置）
  - [ ] 2. 给 `src/net/**` 剥 Qt，每剥一个文件跑一次 `COAST_GATEWAY_SELFTEST`
  - [ ] 3. Swift helper 补 `openBpfForInterface`（`/dev/bpf*` 需 root）
  - [ ] 4. 然后才做 `DeviceStore` / `LanScanner` 台账版 / DevicesPage 的代理开关

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
