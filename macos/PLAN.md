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


## 2026-08-01(续八) · 节点页：顶栏多了两件 Qt 没有的、少了一件 Qt 有的

`qml/NodesPage.qml` + `qml/NodeRow.qml` 逐项核对。

### 顶栏

Qt 的顶栏只有四样：**节点 (N) | 可展开搜索 | 测速 | 帮助**。Swift 侧多了一个
**策略组下拉**和一个**「仅可用节点」勾选框**，少了 Qt 的**帮助按钮**。

- **帮助按钮**补上（打开在线文档），Qt 有这颗、Swift 一直没有。
- **「仅可用节点」勾选框去掉** —— 它对应的配置 `nodeOnlyAvailable` 在**设置页**
  「节点与订阅」卡里就有一项（Qt 就是这么放的），页面上再放一个是同一个开关的两处入口。
  改为直接读 `state.config.nodeOnlyAvailable`，功能没丢。
- ⚠️ **策略组下拉去掉了**。Qt 的 `selectedGroup` 只在 `QmlBridge` 里存在，
  **整个 qml/ 目录没有任何地方用它** —— 节点页列的就是 bridge 选定的那一组。
  按「精确匹配 Qt」去掉了，代价是 macOS 上暂时不能手动切策略组。
  **这一条比较可能是你想要留下的**，说一声就加回来（一行 Picker 的事）。

尺寸对齐：顶栏固定 30 高、间距 6、标题 18、计数 9；搜索框 28 高、半径 3、`inputBg` 底 +
1px 描边、右内距 24 给清空 ✕ 留位（✕ 距右 7、字号 14）；放大镜 16、测速图标 19（品牌色）、
帮助图标 18（muted，右边距 5）；页面内距 10、行距 8；列表行距 1。

### NodeRow

| 项 | Qt | Swift 原状 |
|---|---|---|
| 行内左内距 / 间距 | 8 / 8 | 8 / 10 |
| 行底色 | `nodeRowActive` / `nodeRowBg` | `accent.opacity(0.12)` / `metricBg.opacity(0.5)` |
| 药丸 | 半径 5、字号 12、**字色写死 `#222222`**、底为档位色 | 胶囊、字号 10、字为档位色、底为该色 15% |
| 按钮 | 宽 82、**撑满行高**、悬停才有底色 | 74×22 的玻璃胶囊、活动行还加红色调 |
| 切换在途 | 目标行转圈、**其余行文字压到 0.35 透明** | 只是 disabled，视觉上没有变化 |

药丸那条值得说：底色本身就是延迟档位的颜色（绿/黄/红），字再用同色就看不清了，
所以 Qt 写死深色字。原来的做法（底 15% 透明 + 同色字）在浅色主题下几乎读不出来。

`swift test` 306 全绿；`i18n_check.py` 237/237；persist check 7/7。本页**未做截图验证**。


## 2026-08-01(续九) · 设备页概览条：读法都不一样，还少了两项

`qml/DevicesPage.qml` 顶部那一行是
「设备(18) | 在线 N/M | 代理中 N | 今日 ↓x ↑y | ——— | 新设备提醒 + 34×18 开关 | 导出 | 网关 X」，
项间距 16、全在**同一行**。Swift 侧摊成了两行，而且几乎每一项的读法都不一样：

| 项 | Qt | Swift 原状 |
|---|---|---|
| 在线 | 「在线」12 muted + **`N/M`** 12 secondary（标签在前） | 「N」15 粗 + 「在线」12（**数值在前、大一号**） |
| 代理中 | 数值用**品牌色** | 与在线同色 |
| 今日 ↓/↑ | 有 | **没有** |
| 新设备提醒 | 手画 34×18 开关 | 系统 mini Toggle |
| 导出 | 一段**品牌色文字** | 玻璃胶囊按钮 |
| 网关 | 同一行最右 | 另起一行 |
| 仅在线筛选 | 28×28 方钮（●） | **没有** |
| 重扫 | 搜索行里的 28×28 方钮 | 概览条里的玻璃胶囊 |
| 搜索框 | 撑满、28 高、半径 3、`inputBg` + 1px 描边 | 220 定宽的系统 roundedBorder |
| 分隔线 | 无（靠 10 的行距分开） | 上下各一条 Divider |

「在线」那条差异不只是排版：Qt 给的是 **`在线/总数`** 两个数，Swift 只给了在线数，
「有几台早就搬走了」这个信息在界面上根本不存在。

补的两项功能：
- **今日全网上/下行**：新增 `HistoryStore.todayUpDown(scope:)`（`todayTotal` 只给合计）。
  跟着扫描那一拍刷新，不每帧算 —— 那是几条 SUM 聚合。
- **「仅在线」筛选**：Qt 搜索行里那颗 ● 方钮。

「导出」在 Qt 里是一段文字而不是按钮，这是有道理的：它是个低频动作，
做成按钮会和旁边的开关抢注意力。

右内距的处理也照抄了 Qt 那条讲究：**列表铺到页面最右缘**（滚动条贴窗口右侧），
概览条 / 搜索行各自补 10 回来，列表行则靠 `listRowInsets` 的 trailing 10 保持同样的右对齐。

`swift test` 306 全绿；`i18n_check.py` 235/235；persist check 7/7。本页**未做截图验证**。


## 2026-08-01(续十) · 设备行：Qt 那一行有六样东西，Swift 只有三样

`qml/DeviceRow.qml`（286 行）+ `qml/DeviceTrafficBg.qml`（123 行）对 Swift 侧那个
46 高的行 —— 缺的不是尺寸，是**整块内容**：

| 元素 | Qt | Swift 原状 |
|---|---|---|
| 行高 | 60（多了「最后访问」一行，**所有行等高**） | 46 |
| 最后访问 `→ host` | 有 | 无 |
| 实时速率两行 | 定宽 76、↓绿 ↑红、**0 也显示**、闲着只淡到 0.45 | 无 |
| 背景实时流量图 | 被代理的行整行背景是上/下行面积曲线 | 无 |
| 不可代理原因徽章 | 与开关**共用同一个 38 宽的槽位**（两者互斥） | 无（只把开关置灰） |
| 离线行 | 整行淡到 0.5 | 只淡化头像 |
| 副标题 | `IP · 厂商` | `IP · MAC · 厂商` |
| 代理开关 | 手画 38×20 半径 10 滑块 16 | 系统 mini Toggle |

几条 Qt 注释里点明、照抄下来的道理：
- **速率列宽写死 76**：速率文字每一拍都在变宽变窄（`↓ 9.77 KB/s` ↔ `↓ 1.20 MB/s`），
  列宽跟着变整行就在抖；闲着时只是淡下去，**位置和占位都不变**。
- **开关与徽章共用槽位**且槽宽写死 = 开关宽：整列的左右边缘在每一行都一样齐。
  徽章塞不下时先缩字号（9→7）再省略，完整文案挂悬停提示。
- **副标题不放 MAC**：MAC 是详情页的内容，塞进这一行只会把厂商挤没，
  而厂商才是「这是台什么设备」的线索。
- **背景流量图只画被代理的行**：其余设备的流量不经核心，画出来永远是贴底的 0 线。
  速率是 0 也照画 —— 那正是「已接管、此刻闲着」的样子，不是「没数据」。

### 数据侧：给 `DeviceTraffic` 加了定长历史

背景那张图要的是「近 N 拍速率」，而 `DeviceTraffic` 原来只留最新一拍。加 40 拍的环形历史
（≈40 秒，与状态页那张带宽图同量级），两条新用例钉住两件事：
**每拍给每台已知设备都推一个点**（没流量的推 0 —— 只给有流量的推的话，横轴就不是时间了，
一台间歇跑量的设备会画出一条时间被压缩的假曲线）、以及**必须有上限**
（挂一晚上就是几万个点，而行里只画得下几十个）。

`DeviceTrafficBg` 用 `Path` 画（Qt 那边是 Canvas），量程下限 128KB/s、曲线最高占行高 0.75
—— 两个常量都照抄：闲着时几百字节的抖动不会被放大成满屏山峰，上方留给文字。
⚠️ Qt 那张图的**连续左滑**（50ms 一帧按相位推进）没做，是每拍整条重画。

`swift test` 308 全绿（原 306 + 新增 2）；`i18n_check.py` 237/237；persist check 7/7。
本行**未做截图验证**。


## 2026-08-01(续十一) · 侧栏项与页脚开关：两个天天看见的元素，尺寸全是错的

最后一批共用组件。`qml/NavButton.qml`（46 行）与 `qml/FooterSwitch.qml`（64 行）
是整个界面里出现频率最高的两个东西，而它们的度量此前**每一项都不一样**：

| NavButton | Qt | Swift 原状 |
|---|---|---|
| 高 | 40 | 34 |
| 图标 | 17，左内距 12，**单独一种颜色**（深色主题 `#aaaaaa` / 浅色 `#666666`） | 14，与文字同色 |
| 文字 | 14，距图标 9，右留白 8 | 13，间距 10 |
| 选中底色 | **`Theme.card`**（= 右侧内容卡的颜色）+ 右侧多铺一个圆角裁掉 | 自造的 `navSelected` |
| 文字色 | 选中 `textSecondary` / 未选中 `textPrimary` | 反过来 |

选中底色那条是这个组件的**设计要点**：底色就是内容卡的颜色，加上「右角落在内容卡里」
的处理，选中项看起来是从侧栏**长进内容区**的一块，而不是一个悬在侧栏上的高亮块。
自造一个介于两者之间的灰（原来的 `navSelected`）就把这个效果整个丢了。该令牌已删除。

| FooterSwitch | Qt | Swift 原状 |
|---|---|---|
| 高 | 28 | 24 |
| 圆点 | 12×12 + **3px 外环**，启用时外环 1s 周期在蓝↔灰之间**脉动** | 8×8 实心，无环无动效 |
| 内距 | 左 8 / 右 10 | 左右各 10 |
| 标签 | 封顶 80 再省略 | 不限宽 |

圆点是这排按钮里**唯一**表达开关状态的东西（文字色只跟着变一档），所以那个脉动不是装饰：
一眼扫过去，脉动的那颗就是开着的。抽成 `BreathingDot`，页脚三个开关与模式分段共用 ——
**两处必须一起改**，否则这一排就参差不齐。

### 顺带清掉三处零引用的旧组件

`MetricCard` / `StatusDot` / `BandwidthChart`（都在 `Pages.swift` 里）是早期布局的遗留，
现在**没有任何页面引用**：状态页的六张卡各有各的实现，上传/下载卡的曲线在 `TrafficCard`
自己的 `SparkLine` 里。连同 `Theme.navSelected` 一并删除。

`swift test` 308 全绿；`i18n_check.py` 236/236；persist check 7/7。**未做截图验证**。


## 2026-08-01(续十二) · ★ 前面十一条的「未做截图验证」全是我自己造成的

前面每一条都写着「本页未做截图验证 —— 开发机上 `Coast.app` 起来后取不到窗口」。
**那个结论是错的，原因是我的启动方式。**

- 直接 exec 包内的可执行文件（`./Coast.app/Contents/MacOS/Coast`）→ **窗口数 0**；
- 用 `open -n ./Coast.app --env …` 走 LaunchServices → **窗口正常开出来**。

`.build/debug/Coast` 一直是好的（我用一个临时钩子打了 `NSApplication.shared.windows`：
debug 二进制 1 个窗口，同一份代码打成包再 exec 就是 0 个）。SwiftUI 的 `WindowGroup`
场景要经 LaunchServices 建立的会话才连得上，绕过它 exec 内层二进制不行。

**验证方法固定下来**（写在这里，别再踩）：

```bash
open -n ./Coast.app --env COAST_NO_AUTOSTART=1 --env COAST_INITIAL_PAGE=<0..6>
```

已据此补验三页，都与前面各条描述一致：
- **日志页**：两个标签（主日志选中态是卡底 + 2px 下划线）、彩色圆点（"Delay test finished."
  是绿点、其余蓝点）、时间戳在正文上方、最新置顶 —— 续一条那次重做是对的；
- **状态页**：★ 上传/下载卡显示的是 **`0 B/s`** —— 续七条修的那个「速率被显示成总量」的 bug，
  这下是眼见为实了；六张卡的 2 列布局、连接卡的眼睛/垃圾桶按钮组、延迟卡的四项都在位；
- **设置页**：4 个标签 + 右上「应用」、分组卡（图标 + 标题都是 accentStrong）、
  40 高的行 + 分隔线、手画开关、带 ＋/− 的数字框 —— 续三条那次重做是对的。

### 顺带修掉一个真 bug：窗口位置从来没有被恢复过

排查时 `defaults read com.yuehongsun.coast` 里有 **57 个** 这样的键：

```
NSWindow Frame SwiftUI.ModifiedContent<Coast.(unknown context at $10028f608).RootView, …>-1-AppWindow-1
```

`$10028f608` 是运行时地址，**ASLR 保证它每次启动都不一样**。于是：
1. 上次存的窗口位置**永远读不回来**（每次都是个全新的键）—— 而 Qt 版专门做了这件事
   （`Main.qml` 的 `restoreWindowPos`）；
2. 偏好文件**无限膨胀**，每启动一次多一个永远不会被读的键。

新增 `WindowRestore`：给主窗挂一个**固定**的 autosave 名（`CoastMainWindow`）、
启动时按它恢复位置，并把历史垃圾键清掉（只删前缀完全匹配的，不碰别的偏好）。
实测清理生效：57 → 0，且新的一次启动不再产生新的垃圾键。

（另外确认 `com.coast.Coast` 那个域里的 `window.posX/posY` 与 `geoip.lastPublished` 是
**Qt 版**写的，不是 Swift 侧写错了域。）

`swift test` 308 全绿。临时诊断钩子（`COAST_WINDOW_DEBUG`）用完即删。


## 2026-08-01(续十三) · 剩下四页的截图核对：抓到两处只有看图才发现的问题

用上一条固定下来的 `open -n ./Coast.app --env COAST_INITIAL_PAGE=<n>` 把剩下四页
（节点 / 设备 / 订阅 / 关于）都截了图。**订阅页与关于页与前面各条描述完全一致**：
订阅卡的类型徽章 + 名称 + URL + `56 / 56 节点` + 五颗圆按钮（启用态那颗是品牌色底白勾、
删除那颗是淡红底红图标）；关于页的居中 hero、绿色版本行、三颗链接药丸都在位
（那条红色的「检查失败：HTTP 403 API rate limit」正是续二条里说的**如实报错**，
不是把失败渲染成「已是最新」）。

另外两页各抓到一个问题 —— 两个都是**只有看图才会发现**的：

### 1. 节点行被我拆成了两行，Qt 是一行

Qt 的 `NodeListModel::DisplayRole` 把两段拼成**一个字符串**：
`名字 → 它此刻走到的叶子`（只有组行才有后半段），行高写死 40。
我做成了「名字 13px + `→ 叶子` 10px」两行 —— 于是**有叶子的行和没叶子的行高度不一样**，
截图上一眼就看出来列表参差不齐（实测差 4pt 左右）。而且切换节点导致某一行多出/少掉
后半段时，它下面所有行的位置都会跳一格。改回单行。

### 2. 设备副标题把厂商重复了一遍

截图上第一行是：

```
Beijing Xiaomi Mobile Software Co.
192.168.31.1  ·  Beijing Xiaomi Mobile Software Co.
```

`displayName` 的回退链是**主机名 → 厂商 → MAC**，所以查不到主机名的设备（局域网里很常见）
名字本身就是厂商，副标题再拼一次就是同一串字占掉两行 —— 而副标题本该**补充**信息。
改成「名字就是厂商时不再重复」。这条读代码时完全看不出来，因为两处各自都是对的。

★ 顺带，重截设备页时侧栏版本行第一次亮出了**红色的 "new" 角标**（这一版是
`0.0.0-loop`，比任何真实 tag 都旧）—— 续二条补的那颗角标，至此也眼见为实了。

`swift test` 308 全绿；`i18n_check.py` 236/236。


## 2026-08-01(续十四) · 更新窗做成 sheet 是错的：底部整条动作行被裁掉、按钮点不到

继续截图核对，这轮是设置页的三个子标签和三个弹窗。

**设置页的过滤 / 区域 / 规则三个标签都对**：过滤页 4 行（标签列 150 + 手画开关 + 300 宽的
可编辑下拉）；规则页「＋添加」+ 220 宽搜索框 + 右侧计数，而且**「应用」按钮在这两个标签上
确实是隐形占位**（标签栏没有左右挪动，正是 QML 注释里要的效果）。

### ★ 更新窗：sheet 装不下，按钮根本点不到

`UpdateView` 是 600×**560**，而主窗默认 900×**510**。做成 sheet 之后底部那整行
（国内代理下载 / 关闭 / 更新）**被主窗边界裁掉了** —— 截图上从「更新说明」那张卡往下
直接没了，用户**点不到任何一个按钮**。这不是尺寸偏差，是功能不可达。

Qt 那边它本来就是**独立顶层窗**（`ApplicationWindow`，600×560，最小 460×420），
正是因为不受主窗尺寸约束。改成 SwiftUI 的 `Window` scene，与 Qt 一致。

顺带踩到并记下一个坑：`Window` scene 不在主窗的 environment 链里，拿不到 `RootView`
注入的 `AppState`。加了 `AppState.sharedForWindows`（这个应用本来就只有一份状态）。
**但第一版用 `@State` 接它，窗口开出来是全空的** —— `@State` 的初值只算一次，
而 SwiftUI 会在主窗建好（也就是那个静态量被赋值）**之前**先求值一次 scene 根，
于是它永远抱着那个 nil。改成每次 body 求值时现读。这两步都是**截图才看得出来**的：
编译通过、没有任何报错，窗口标题和尺寸也都对，就是里面一片空白。

### 设备详情窗同一个毛病，先按「压得下」处理

`DeviceDetailView` 是 600×**720**，比 510 高得更多。Qt 同样是独立窗（最小 420×420）。
这轮先把它的高度从写死 720 改成「理想 720、最小 420、上限 720」，内容本来就在
`ScrollView` 里，压矮只是多滚两下、不会丢东西。**彻底做法是和更新窗一样转成独立窗**，
但它要带一个「当前是哪台设备」的参数（Qt 用的是 `devices.selectedDevice` 这个共享状态），
是个略大的改动，记在这里。

`swift test` 308 全绿；`i18n_check.py` 236/236；persist check 7/7。


## 2026-08-01(续十五) · 设备详情窗转独立窗：路上又踩到两个「编译过、但点不动/看不见」的问题

把 `DeviceDetailView` 也从 sheet 转成**独立顶层窗**（600×720，最小 420×420），
与 Qt 一致，也解掉「比 510 高的主窗装不下」那个裁切问题。

显示口径完全照 Qt：窗口显示的**永远是当前选中的那台**（`AppState.selectedDevice`，
对应 Qt 的 `devices.selectedDevice`），点列表里另一台就换内容，不用来回开关窗口。
页面那边的「点开某一行」= 先写选中、再开窗，与 Qt 的 `openFor(mac)`
（`devices.select(mac)` → show → raise）一一对应。

### ★ 两个只有真点一下才会发现的问题

1. **整行的 `.onTapGesture` 在 `List` 里根本不触发。** 点了好几次，详情窗一个都没开出来，
   **没有任何报错**。改成真的 `Button` 包整行就好了。Qt 那边同样是给整行单独铺了一个
   `MouseArea`，它的注释写的理由也是「TapHandler 抢不到」—— 两边栽在同一个坑上。
   行里那颗代理开关自己带手势，嵌在里面优先吃掉落在它上面的点击，与 Qt
   「开关的 MouseArea 压在整行的 MouseArea 之上」是同一个效果。

2. **`Window` scene 的根**如果用 `@State` 去接 `AppState.sharedForWindows`，
   窗口开出来是**全空的**（上一条已经在更新窗上栽过一次，这次照着写就没再犯）。

### 顺带修掉一个显示条件写错

第一次截图时发现：一台**可代理的在线设备**，**「策略」那一行整个不见了**。
Qt 显示策略行的条件只有 `proxyable === true`，与「台账里有没有这条记录」无关；
而我写的是 `if rejection == nil, let record` —— 于是**从没被开过代理的设备根本看不到
策略选择**。台账记录是「用户动过它」才产生的，不该反过来决定用户能不能动它。
改成台账里没有就用一份默认记录，用户选了策略再顺手建。重截确认「策略 跟随全局」已出现。

⚠️ 重截时还注意到策略那颗下拉的底色似乎没画出来（设置页里同一个 `ThemedCombo` 是有底的），
没有当场定论，记在这里下一轮查。

`swift test` 308 全绿；`i18n_check.py` 238/238；persist check 7/7。


## 2026-08-01(续十六) · `ThemedCombo` 一直没画出输入框的样子

上一条末尾记的那个疑点，查实了：**`ThemedCombo` 从来没有渲染成一个下拉框**，
一直只是一段裸文字。设置页早先的截图之所以没露馅，是因为先截到的是
`ThemedEditCombo`（另一个类型，底色画在外层）。

根因是 `.menuStyle(.borderlessButton)` 对 `label:` 的处理很霸道，前后栽了两次：

1. 装饰画在 `label:` 里 → **被整个吃掉**，只剩文字（设备详情窗「策略」那行的第一张截图）；
2. 把底色/描边挪到 `Menu` **外面**之后底是有了，但 `label:` 里的 `HStack` 仍被**居中**，
   右侧那个 ▾ 直接不见（第二张截图）。

改法：**别再试图让它按我们的排版渲染** —— 自己画一份完整的样子（文字左对齐 + 右侧 ▾ +
底色 + 1px 描边），再叠一个几乎全透明的 `Menu` 接管点击。注意那层**不能 `.opacity(0)`**：
完全透明的视图不参与命中测试，点下去没反应；用 0.001。

第三张截图确认：130 宽的框、「跟随全局」左对齐、右侧 ▾ 都在位，点开能弹出五个策略
（跟随全局 / 规则分流 / 指定节点 / 强制直连 / 禁止上网）。

这个组件用在**设置页的主题与语言、规则编辑器的类型与目标、区域编辑器的类型、
设备详情的策略**，也就是说这几处此前全是裸文字。

`swift test` 308 全绿；`i18n_check.py` 238/238；persist check 7/7。


## 2026-08-01(续十七) · 补上设备详情窗那张 90 高的带宽图，顺带发现两张图的量程公式对不上

续五条里「刻意未做」的那张图补上了：`qml/DeviceDetailWindow.qml` 里
`BandwidthChart { Layout.preferredHeight: 90; title: qsTr("下载"); lineColor: "#5bb44b" }`。
数据源用已有的 `DeviceTraffic.downHistory`（每拍推一个点，节奏天然是稳的 ——
Qt 那边专门加了个 1s 定时器，因为它把入点挂在「选中设备变了」上会让节奏乱掉、曲线一顿一顿）。

抽成共享的 `BandwidthChart`，两种用法与 QML 的 `minimal` 开关一一对应：
- **独立成图**（详情窗）：四分网格 + 右侧速度刻度（max/¾/½/¼）+ 左上标题，
  **只画折线不填充**（3px 圆角线帽、α0.70），背景是线色极淡的底（α0.03）；
- **卡片底纹**（上传/下载卡）：网格/刻度/标题一概不画，改成「一条线 + 线下的淡填充」——
  底纹要的是趋势的形状，不是能读数的图表。

### ★ 顺带抓到：两张图的量程公式根本不是同一套

Qt 的 `currentMax()` 是**128KB 基准 / 2MB 步进**。而我早先给上传/下载卡写的是
「32KB 台阶」—— 那是我自己编的，注释里还煞有介事地写着「与 Qt 卡上那组 32/64/96/128
的观感一致」。实际后果：同一份速率，详情窗那张图和状态页那两张图的刻度对不上，
上下浮动的档位也不一样。现在两处共用同一个公式。

截图确认：详情窗「实时流量」卡里那张 90 高的图在位，左上「下载」、右侧
128.00 / 96.00 / 64.00 / 32.00 KB/s 四档刻度都对。

`swift test` 308 全绿；`i18n_check.py` 238/238。


## 2026-08-01(续十八) · 曲线的「连续左滑」，以及上传/下载卡多画了 Qt 明说不要的东西

最后一件挂着的：`qml/BandwidthChart.qml` 与 `qml/DeviceTrafficBg.qml` 都是
**整条曲线连续左滑**，不是「每来一个样本整条跳一格」。两边的注释都专门写了做法：
50ms 一帧，按**距上次入点的真实经过时间**算滚动相位（0..1），画的时候整条左移
`间距 × 相位`；新点从右边缘外进入、匀速滑到位，相位归零时正好接上。

Swift 侧此前是每拍整条重画，所以是跳的。现在两处都接上了：
`TimelineView(.periodic(by: 0.05))` 算相位，顶点整体左移一格 × 相位，外层 `clipped()`。

相位要知道「上一拍是什么时候进来的」，为此加了 `AppState.pollTick`（每推进一拍 +1）。
**用计数器而不是让视图自己比对数组**：数组到上限之后长度不再变，`onChange(of: count)`
从此再也不触发；而末位的值经常连着好几拍都是 0，比值也认不出「来了新的一拍」。

### ★ 顺带发现：上传/下载卡画了四条带速率标注的网格线，而 Qt 明说不要

`BandwidthChart.qml` 的 `minimal` 模式（正是上传/下载卡这一路）注释写得很直白：
**网格、右侧刻度、左上标题全是噪音，压在卡片的数字底下只会打架，所以一概不画**，
改成「一条线 + 线下的淡填充」—— 底纹要的是趋势的形状，不是能读数的图表。
而我早先给 `TrafficCard` 自己写了一份，还把四条刻度线加了上去。

现在两种用法共用同一个 `BandwidthChart`（`minimal` 开关一一对应），
`TrafficCard` 里那份自绘的 `SparkLine` 连同刻度一起删掉。截图确认：
上传/下载卡上已经没有网格与刻度了，只剩贴底的那条线。

⚠️ 滚动是动效，**静态截图验不了**——只能确认排版没坏。另外 `TimelineView` 没有像 QML 那样
显式挂 `running: root.visible`；设备行那张图只在被代理的行上实例化，主窗遮挡时
SwiftUI 一般会自行停掉 timeline，但没有实测，记在这里。

`swift test` 308 全绿；`i18n_check.py` 238/238；persist check 7/7。


## 2026-08-01(续十九) · ★ 点一下 ✕，界面再也回不来

核对主窗 shell 的行为时试了一下红点，结果是个**死局**：

- SwiftUI 的 `WindowGroup` 窗口一关就**销毁**了；
- 而 `applicationShouldTerminateAfterLastWindowClosed` 是 false（进程照样活着，这是对的：
  ✕ 直接退会连带停掉核心与系统代理）；
- 于是点一下 ✕ 之后**窗口数恒为 0，怎么都开不回来** —— 只剩一个够不着的托盘进程。

Qt 那边专门拦了这件事：`onClosing: close.accepted = false; window.hide()`
（注释原话：「不销毁窗口，仅隐藏，供后续重开」）。Swift 侧一直没有对应物。

修复四处，缺一不可 —— 少任何一处都还是坏的：

1. **`CloseGuard`**（`NSWindowDelegate.windowShouldClose` 返回 false + `orderOut`）：
   ✕ 只隐藏、不销毁。**delegate 必须强引用**：`NSWindow.delegate` 是 weak 的，
   挂完就被释放等于没装。
2. 隐藏时**同时收掉 Dock 图标**（切 `.accessory`）—— 照 Qt 的
   `bridge.setMacDockVisible(visible)` 来：窗口一藏，回来的路只剩托盘。
3. **`showMainWindow` 要认得哪个才是主窗**。原来写的是 `NSApplication.shared.windows.first`
   —— 那可能是状态栏窗口或更新窗，order front 等于什么都没做。
4. **`applicationShouldHandleReopen` 返回 `false`**。这一位的语义是「要不要让 AppKit 再
   执行默认行为」；返回 true 时 SwiftUI 会**再建一个**主窗 —— 实测重开之后屏幕上是
   **两个** Coast 窗，一个在原位、一个在左上角。改成 false 之后就只剩一个、且在原位。

### 顺带补上「没有历史位置时落在右下角」

Qt 的 `restoreWindowPos`：有历史且仍可见就恢复，否则**贴着当前屏可用区的右下角**摆放
（可用区已扣掉菜单栏与程序坞）。Swift 侧此前是 SwiftUI 的默认落点。
现在首次启动会落到右下角，之后按上一条做的固定 autosave 名恢复。
实测：首启 (1340, 693)（屏 2240×1260，2240−900=1340 ✓），✕ 再回来仍是 (1340, 693)。

`swift test` 308 全绿；`i18n_check.py` 238/238；persist check 7/7。


## 2026-08-01(续二十) · 托盘菜单整份是裸中文，而 i18n 检查看不见它

核对最后一个 Qt UI 面：托盘。**菜单结构逐项对得上**
（控制面板 / — / UP / DOWN（禁用）/ — / 启动核心 / 打开网页代理 / 打开增强模式 / — / 退出程序，
以及 `Coast - 运行中 / 已停止` 的提示），开关态的文案也和 Qt 一样在「打开/关闭」之间翻。

但**那十条文案全是裸中文字面量，一个 `.t` 都没有** —— 对 11 种非中文语言，
整个托盘菜单直接漏中文。而 `i18n_check.py` **只扫 `Sources/Coast`**，
`TrayController` 在 `Sources/CoastKit` 里，于是它连报都报不出来。

这已经是同一类问题的第三次了（续七的「插值串不进分母」、续三的「persist 检查变成永远绿」）：
**检查看起来在把关，实际上够不着要查的东西。**

修法：
- `I18n` 从 `Sources/Coast` 移到 **`Sources/CoastKit`** —— 托盘在那一层，够不到 app 层的 `.t`，
  这正是当初写成裸字面量的原因。移过去之后十条文案全部标上 `.t`；
  查表发现**它们本来就在与 Qt 共用的表里**（Qt 用 `tr()` 翻过），所以一条译文都不用新增：
  覆盖率从 238 条涨到 **248/248**。
- `i18n_check.py` 的**覆盖率扫描**改成两个目录一起扫。
- 但「有中文却没标 `.t`」那一项**不能**照搬到整个 CoastKit：那里大量中文是**匹配用的**
  字面量，翻了反而坏事 —— `ClashService` 拿「节点 / 选择 / 代理」认策略组名、
  拿「规则 / 全局 / 直连」归一化模式，那些串要和**核心返回的数据**对得上，不是给人看的。
  一股脑扫会报出 **132 条**假阳性，检查也就没人看了。所以 app 层整层扫，
  CoastKit 只点名扫真正有界面文案的 `TrayController.swift` / `Notifier.swift`。

验证它真能抓到：临时去掉托盘「退出程序」那条的 `.t`，脚本立刻指名报错
（`TrayController.swift:64`）；补回去就绿。

`swift test` 308 全绿；`i18n_check.py` 248/248；persist check 7/7。


## 2026-08-01(续二十一) · 页脚内距、整窗背景拖动

`qml/Main.qml` 最后两处没对上的：

1. **页脚的内距。** Qt 的页脚 `RowLayout` 是 `anchors.leftMargin: 0`（注释写着
   「底部状态栏左侧内容贴左对齐」），右侧则跟着整列的 `Layout.rightMargin: Theme.inset`
   让出 5。Swift 侧是「左右各 8、右侧一路顶到窗口边缘」。改成左 0 / 右 5。
2. **整窗背景拖动。** Qt 在窗口里铺了一个 `z:-1` 的 `DragHandler`：按住任意
   **非交互**的空白/文字/卡片背景就能拖动整窗（列表、下拉这些控件会先吃掉按下事件，
   所以在它们身上拖不会移动窗口）。macOS 上的等价物是
   `NSWindow.isMovableByWindowBackground = true` —— AppKit 的判据与 Qt 那套
   「不夺取的 grabPermissions」是同一个效果。此前只有标题栏那一条能拖。

⚠️ 拖动是交互，**没有真的拖一次验证**（脚本模拟拖拽没成功），只确认了开关已打开。

★ 截图时机器正好切到了**浅色主题**，顺带把浅色下的整壳看了一遍：侧栏、选中项那块白底、
六张卡、延迟色阶、页脚玻璃按钮都正常 —— 这是之前一直没验过的一面。

`swift test` 308 全绿；`i18n_check.py` 248/248。


## 2026-08-01(续二十二) · 背景拖动真拖了一次，以及全量对照表

### 拖动验证（上一条欠的）

上一条只说「开关打开了」，这次用 `cliclick` 真发了两组鼠标事件：

| 起点 | 结果 |
|---|---|
| 状态页卡片的**空白处** | 窗口从 (1340, 693) 移到 (1060, 513) ✅ |
| 侧栏「节点」**按钮上** | 窗口**没动**，还在 (1060, 513) ✅ |

正是 Qt 要的那两条：空白处能拖整窗，控件上按住拖不会拖走窗口。

### 全量对照：`qml/` 24 个文件都有归宿

| QML | Swift |
|---|---|
| Main | `MainView` / `CoastApp` / `WindowRestore` |
| StatusPage | `Pages.StatusPage` + `TrafficCard` / `ConnLine` / `LatencyCard` |
| NodesPage / NodeRow | `Pages.NodesPage` / `Pages.NodeRow` |
| DevicesPage / DeviceRow / DeviceTrafficBg | `DevicesPage` 内三者 |
| DeviceDetailWindow | `DeviceDetailView`（独立窗） |
| SubscriptionsPage | `SubscriptionsPage`（含三个弹窗） |
| SettingsPage | `SettingsPage` + `SettingsControls` |
| RuleEditorWindow | `RulesEditor.RuleEditorSheet` / `AreaEditorSheet` |
| ConnectionsWindow | `ConnectionsView` + `ConnectionLedger` |
| UpdateWindow | `UpdateView`（独立窗） |
| AboutPage | `AboutPage` |
| LogsPage / LogTimeline | `Pages.LogsPage` / `LogTimeline` + `LogSeverity` |
| MetricCard | `TrafficCard` |
| BandwidthChart | `BandwidthChart` |
| Card / NavButton / FooterSwitch / ThemedCombo | `Components` / `SettingsControls` |
| Theme | `Theme` |
| **NpcapWindow** | **无 —— Windows 专属，macOS 正确不做** |

### 这一轮（22 条）总账

改动集中在三类，**后两类只有把 app 跑起来才发现得了**：

1. **元素与尺寸对不上** —— 每一页、每一个窗、每一个共用组件都逐项对过；
2. **编译过、检查全绿，但功能不可达** —— ✕ 之后界面再也回不来、更新窗底部整行按钮被裁掉、
   设备行整行点击在 `List` 里不触发、`Window` scene 用 `@State` 接共享状态开出全空窗、
   `ThemedCombo` 四处都只是裸文字；
3. **检查本身够不着要查的东西** —— `settings_persist_check` 变成永远绿、
   `i18n_check` 扫不到 CoastKit 里整份托盘菜单、也认不出带转义的文案。

第 3 类最值得记一笔：三次都是「看起来在把关」。修的时候一并让它们**在失效时主动失败**
（persist 检查匹配到 0 个块即报错），而不是安静地通过。


## 2026-08-01(续二十三) · 连接窗的分段按钮：整组只有一段那么宽

开始补验那几个一直没截过图的弹窗。第一个（连接窗）就抓到一个真 bug：

**「Offline」那一段被右边的 Search 框盖掉了大半，只露出一个「O」。**

根因：两段是「离线段 offset 出去」的叠放，我用 `ZStack` + `.fixedSize()` 装它们 ——
而 `ZStack` 的固有宽是**最宽的那个子项**，不是两段之和。QML 那边写的是
`Layout.preferredWidth: onSeg.width + offSeg.width - 3`，一个和式。

第一次修：照着和式把宽度显式算出来给 `ZStack`（用 `NSString.size(withAttributes:)` 量文字宽）。
**还是不对** —— 量出来比 SwiftUI 实际排版**偏窄**，这回是「Offline (0)」的**计数被裁掉**了，
只剩「Offline」。

最终改法：**根本不量**。`HStack(spacing: -3)` 让两段各自量自己，负间距天然给出
「重叠 3px」，`zIndex` 给出「在线段盖在离线段左端圆角上」—— 两个效果都拿到，
一个数都不用算。量文字宽这条路在这种「组件自己带内距」的场合本来就不该走。

截图确认：`Online (0)` 与 `Offline (0)` 完整并排、中间无缝、只有外侧是圆角。

`swift test` 308 全绿；`i18n_check.py` 248/248；persist check 7/7。


## 2026-08-01(续二十四) · 剩下三个弹窗补验完毕，都对

接着上一条把没截过图的弹窗验完。这三个**都与描述一致，没有新问题**：

- **订阅节点弹窗**（620×460）：「订阅节点 (56)」+ 计数、行高 46 的节点行（名称 12 +
  `server:port` 10）、右侧 76×30 的「停用」、底部「全选 / 全不选 / 关闭」；
  已启用的行整行用 `nodeRowActive` 打底 —— 与 Qt 一样靠**底色**表达启用态，
  一屏几十条时一眼就能扫出来。
- **规则编辑器**（460×440）：标题 16、三个说明标签 12、类型下拉（DOMAIN-SUFFIX）、
  可搜索的「值」输入、节点/策略组下拉（DIRECT）、底部「取消 / 确定」。
- **区域编辑器**（440×470）：标题、名称输入、类型下拉（url-test）、节点名正则输入、
  底部「取消 / 确定」。

至此 `qml/` 里**每一个页面、每一个窗口、每一个弹窗**都截过图核对过了
（唯一没有的是 Windows 专属的 `NpcapWindow`）。上一条那个分段按钮的 bug 是这批里唯一一个。

顺带说明：这几张都是在**浅色主题**下截的（机器当时切过去了），
所以浅色一路也一并看过了 —— 节点行的 `nodeRowActive` 在浅色下是一片偏亮的蓝，
观感偏重，但取值就是 Qt 的 `Qt.rgba(72/255, 151/255, 248/255, 0.69)`，不是这边跑偏。


## 2026-08-01(续二十五) · 把窗口拖到最小：设备页概览条整个塌了

前面所有截图都是默认的 900×510。Qt 的主窗**最小是 640×430**，而且专门为这个尺寸写了
自适应代码 —— 那说明它是会被拖到那里的。试了一下，设备页概览条**塌得很难看**：

- 「今日 ↓42.52 KB ↑7.89 KB」被压成三行竖排的碎字；
- 「新设备提醒」更是**一个字一行**竖着排下来。

Qt 的做法是按优先级把次要项收起来（顺序：**今日 → 网关 → 提醒文字 → 导出**），
而且断点**不写死像素**，拿各项自己的 `implicitWidth` 现算 —— QML 注释里讲了理由：
同一句话在 12 种语言里宽度能差一倍（德语的「新设备提醒」比中文长一大截），
写死的数字必然在某个语言上翻车。

Swift 侧用 `ViewThatFits` 摆五个变体（从最全到最省），让它按**真实排版**挑第一个装得下的
—— 同样不写死任何像素，翻译再长也不会翻车。有个坑记一下：中间那个 `Spacer` **必须给
`minLength`**，因为 `ViewThatFits` 比的是各变体的**理想宽**，而 `Spacer()` 的理想宽是 0、
怎么都「装得下」，第一个变体会被无脑选中，等于没做自适应。

重截确认：640 宽下概览条是「设备 | 在线 7/7 | 代理中 0 | ——— | 新设备提醒 + 开关 | 导出」，
今日与网关按优先级让掉了，没有一处竖排碎字。

顺带补上 `DeviceRow` 的 **`compact`**：行宽 < 250 时收起速率两列（Qt 的
`readonly property bool compact: width < 250` + `visible: !root.compact`）。
右侧那几列是定宽的，不收就只能溢出到行外面去。

`swift test` 308 全绿；`i18n_check.py` 248/248；persist check 7/7。


## 2026-08-01(续二十六) · 最小尺寸下把其余六页也走了一遍

接上一条，把 640×430（Qt 主窗的最小尺寸）下的其余各页都截了一遍。**只有设备页塌过，
其余六页都撑得住**：

- **状态页**：两列六卡，卡高不变，延迟卡四行都读得出来，其余靠滚动；
- **节点页**：顶栏「节点 (12) | 放大镜 | 测速 | 帮助」四样都在；
  ★ 顺带确认了续十四那个**单行**修复 —— 有 `→ 叶子` 的行和没有的行**高度完全一样**，
  不再参差；
- **订阅页**：顶栏三颗按钮（添加订阅 / 应用 / 更新全部）不换行、不省略；
- **设置页**：4 个标签 + 右上「应用」都在位；
- **日志页 / 关于页**：正常。

★ 这一轮关于页正好赶上更新检查**成功**，第一次看到版本行的**第三种状态**：
`v0.0.0-loop → v0.2.362 有新版本`（红色）。至此三态（检查中 / 有新版 / 已最新）
全部眼见为实，拼法也与 Qt 的三段式一致。

至此**每一页都在默认 900×510 与最小 640×430 两个尺寸下看过**。


## 2026-08-01(续二十七) · 最小尺寸下再看弹窗：连接窗横向溢出

把主窗拖到 640×430 之后再开各个弹窗，逐个看：

- **订阅节点弹窗**（620×460）：620 < 640、460 只比 430 高 30 —— SwiftUI 把它压到窗口高度内，
  底部「全选 / 全不选 / 关闭」照样够得到，**没问题**；
- **规则编辑器**（460×440）、**区域编辑器**（440×470）：都比 640 窄，没问题；
- ★ **连接窗**（720×480）：**横向溢出** —— 左边的「Online (0)」被切掉半截（截图里只剩
  「ne (0)」），整个弹窗比主窗还宽。

Qt 那边它本来就是**独立顶层窗**（`ApplicationWindow`，720×480，最小 480×320），
正是因为不受主窗尺寸约束。改成 `Window` scene，与更新窗、设备详情窗同一套做法
（`AppState.sharedForWindows` + 根视图里现读，别用 `@State`）。

重测：主窗仍是 640×430，连接窗独立开在 720×480，`Online (0)` / `Offline (0)` 完整。

**至此三个「比主窗大」的 Qt 窗口全部转成了独立窗**：更新窗（600×560）、
设备详情窗（600×720）、连接窗（720×480）。规则/区域编辑器与订阅节点弹窗因为**装得下**，
维持 sheet 不变 —— Qt 那边它们也是 `ApplicationWindow`，但这一条差异不产生任何可见后果。

`swift test` 308 全绿；`i18n_check.py` 248/248；persist check 7/7。


## 2026-08-01(续二十八) · 更新窗是个拖不动的死尺寸

三个独立窗都对着 Qt 的**最小尺寸**核了一遍：

| 窗 | Qt 最小 | Swift |
|---|---|---|
| 连接窗 | 480×320 | ✅ 已写 |
| 设备详情窗 | 420×420 | ✅ 已写 |
| **更新窗** | 460×420 | ❌ **写死了 `frame(width: 600, height: 560)`** |

写死 `width/height` 的后果是那个窗**根本拖不动** —— 而 Qt 那边它是能缩到 460×420 的。
改成只写下限（默认 600×560 由 scene 的 `.defaultSize` 给）。

拖到 460×420 重截确认：三颗竖排 tab、内容卡（当前版本 / 新版本号 / 更新说明）、
底部「国内代理下载 + 关闭 + 更新」全都在位且够得到，说明卡里的正文照常滚动。

★ 这一轮更新检查也是成功的，所以顺带看到了**更新说明正文真的渲染出来**
（一整段 release notes 在说明卡里滚），而不是之前那些空卡 —— 这一块此前一直是空的，
只验过排版没验过内容。

`swift test` 308 全绿；`i18n_check.py` 248/248。


## 2026-08-01(续二十九) · ★ 四个窗的「最小尺寸」全都是摆设

`.frame(minWidth:minHeight:)` **不构成窗口的最小尺寸**。它只约束内容布局，
窗口照样能被拖得更小，内容被裁掉。实测很直接：更新窗上明明写着 `minWidth: 460`，
一句 `set size to {300, 300}` 就把它缩到了 **300×300**。

也就是说，前面几条里我逐个「对齐 Qt 最小尺寸」的那些写法（连接窗 480×320、
设备详情 420×420、更新窗 460×420），**一个都没有真正生效** ——
包括主窗那句从一开始就在的 `minWidth: 640, minHeight: 430`。

macOS 上的对应物是 `NSWindow.contentMinSize`。新增 `.windowMinSize(width:height:)`
（一个薄薄的 `NSViewRepresentable`，挂到窗口上设 `contentMinSize`），四个窗全部接上：

| 窗 | Qt |
|---|---|
| 主窗 | 640×430 |
| 更新窗 | 460×420 |
| 设备详情窗 | 420×420 |
| 连接窗 | 480×320 |

顺带处理「上次退出时存下来的尺寸比下限还小」的情况：挂上去的那一刻就顶回去
（我自己的 `WindowRestore` 恢复过一个 460 宽的主窗帧，正是这种情况）。

实测：主窗默认 640×462（430 内容 + 32 标题栏），`set size to {300, 200}` 之后**仍是 640×462**。

★ 顺带验了节点页那个**可展开搜索**（Qt：默认只显示放大镜，点开才出输入框）：
点一下放大镜，「节点 (13)」后面确实展开出带占位符「搜索节点」的输入框。

`swift test` 308 全绿；`i18n_check.py` 248/248；persist check 7/7。


## 2026-08-01(续三十) · 上一条的写法有个副作用；另有一个**没查出来**的窗口高度问题

### 1. `WindowMinSize` 改成只设一次

上一条把 `contentMinSize` 写在了 `updateNSView` 里，每次布局都设一遍并顺手「顶回下限」。
这有副作用：`updateNSView` 一秒能跑很多次，只要**任何一拍**量到的高度偏小
（SwiftUI 布局过程中很常见），就会把窗口按下去。**下限只是下限，不该在窗口活着的时候
反复去动它**。改成挂上去那一刻设一次、顶一次，之后 `updateNSView` 什么都不做。

最小尺寸仍然生效：`set size to {300, 200}` 之后主窗仍是 640×462（430 内容 + 32 标题栏）。

### 2. ⚠️ 主窗**长不高**，原因没查出来

主窗的宽度能拖大（1000 没问题），**高度却卡在 430 内容高**，`set size` 和真的拖右下角
都涨不上去。已经排除的：

- **不是** `WindowMinSize` 引入的 —— 把它从主窗上摘掉，高度照样涨不上去；
- **不是**「反复设 min」造成的 —— 改成只设一次之后依旧；
- 显式放开 `contentMaxSize`、给根视图补 `maxWidth/maxHeight: .infinity`，**两样都无效**
  （试过即撤，没留在代码里）。

看起来是 SwiftUI 按根视图推断窗口尺寸约束时的某种行为，我这一轮没定位到。
**如实记下来**：Qt 的主窗是能自由拉高的，这一条目前对不上。
下一步可以试的方向：把 `WindowGroup` 换成 `Window` scene、或者用 `NSHostingView` 自建窗口，
再逐个排除 `.windowStyle(.hiddenTitleBar)` / `.windowGlass` / `.background(.ultraThinMaterial)`。

`swift test` 308 全绿；`i18n_check.py` 248/248。


## 2026-08-01(续三十一) · ★ 更正：上一条那个「主窗长不高」是我量错了

上一条把「主窗高度卡在 430、拖不大」记成了一个没查出来的问题。**它不是问题**。

真正的原因：那扇窗当时在 **y = 751**，而这台机器的屏幕才 1260 高。
`set size` 与拖右下角都是**保持左上角不动、往下长**，1260 − 751 再扣掉程序坞，
本来就只剩四百多点 —— macOS 把它按屏幕可见区**截住**了。窗口没有任何高度上限。

验证：先把窗口挪到 (200, 60)，再 `set size to {1000, 700}` —— 结果是
**1000×700，一次就成**。

这一轮为此排除掉的（都是好的，只是白排）：`.windowGlass(.sidebar)`、
`.windowStyle(.hiddenTitleBar)`、`contentMaxSize`、根视图的 `maxWidth/maxHeight`。
三个试探性改动上一条已经全部撤掉、没留在代码里 —— 这次庆幸撤了，因为它们修的是一个
根本不存在的 bug。

**教训记在这里**：拿脚本量窗口尺寸时，先确认窗口**离屏幕边缘够远**。
不然量到的是「屏幕还剩多少」，不是「窗口能长多大」。上一条那个结论就是这么来的。

上一条真正有价值的那一半仍然成立且已保留：`WindowMinSize` 只在挂载时设一次
（在 `updateNSView` 里反复写窗口约束本来就是错的），四个窗的最小尺寸都真的生效了。

`swift test` 308 全绿；`i18n_check.py` 248/248。


## 2026-08-01(续三十二) · 翻译第一次**真的跑起来看**：英文与德文都对

`i18n_check.py` 一路报 248/248，但那只证明**表里有这一条**，不证明界面真的换得过去。
这一轮用 `COAST_LANG=<code>` 把设置页（文案最密的一页）分别渲染成英文与德文。

**两门都完整换过去了**：侧栏（Status / Nodes / Device / Subscriptions / Settings / Logs /
About；德文 Status / Knoten / Gerät / Abos / Einstellungen / Protokolle / Über）、
标签（System / Filter / Groups / Rules）、右上「应用」→ Apply / Anwenden、
每一行设置项、页脚（Ultra / Web / Core / Rules）。

★ **德文那张最有价值** —— 它正是 Qt 注释里反复点名的「长译文」场景：
`Intervall der automatischen Abonnement-Aktualisierung` 这一行长得离谱，
而行仍然是好的：标签占满剩余宽、数字框与「Min.」纹丝不动、没有换行也没有溢出。
侧栏的 `Einstellungen` / `Benachrichtigungen` 同样在 150 宽里排得下。

这是这次移植里**第一次**端到端验翻译 —— 之前全部是「表覆盖率」。
覆盖率 100% 而界面不换，是完全可能同时成立的两件事（`.id(i18n.language)` 那条重建路径
一旦断掉就是这样），现在这条路也钉住了。

`swift test` 308 全绿；`i18n_check.py` 248/248。


## 2026-08-01(续三十三) · 自适应概览条对着 Qt 点名的那个德文串验了一遍

续二十五做的那条「按优先级收起」，是照 Qt 的注释写的，而那条注释点名的正是**德语**：
「德语 `Neues-Gerät-Hinweis` 比中文长一大截，写死的数字必然在某个语言上翻车」。
这一轮就拿德文把设备页在两个宽度下各渲染一次。

**900 宽（默认）**：中文下这个宽度是**显示**「今日」的，德文下它**已经让掉了** ——
剩「Gerät | Online 7/7 | Übernommen 0 | ——— | Neue-Geräte-Hinweise + 开关 |
Exportieren | Gateway 192.168.31.1」。断点跟着**真实文字宽**走，不是跟着像素常数走，
这正是 Qt 要的效果。

**640 宽（最小）**：再让掉「网关」与「提醒文字」，只剩开关 + Exportieren。
收起顺序与 Qt 的优先级一致（今日 → 网关 → 提醒文字 → 导出），没有一处竖排碎字或溢出。

也就是说：同一份代码，中文 900 宽显示今日、德文 900 宽不显示 —— **这个差异是对的**，
而任何写死断点的实现都会在其中一门语言上出错。

`swift test` 308 全绿；`i18n_check.py` 248/248。


## 2026-08-01(续三十四) · 德文下状态页的卡标题**换行**了 —— QML 的 Text 默认不换行

继续拿德文走其余各页。订阅页在 640 宽下三颗按钮
（Abo hinzufügen / Anwenden / Alle aktualisieren）挤得下，没问题。
**状态页有问题**：

「连接」卡的标题德文是 `Verbindungen`，在 640 宽下被折成了**两行**
（`Verbindun` / `gen`），把下面那个大号数字整体顶下去，与右边的延迟卡错开一整行。

根因是两边 `Text` 的**默认行为相反**：
- QML 的 `Text` 不写 `wrapMode` 就是**单行**（放不下就溢出/截断）；
- SwiftUI 的 `Text` 默认**换行**。

也就是说凡是「照抄 QML、没写 elide」的地方，Swift 侧都可能悄悄多出一次折行。
状态页四张卡的标题（连接 / 延迟 / 总流量 / 今日流量）全部补 `lineLimit(1)`
—— 上传/下载卡的标题早就有，所以那两张一直没露馅。

重截确认：`Verbindu…` 单行 + 下面的数字，与延迟卡回到同一条基线。

★ 这类差异**只在长译文 + 窄窗口同时出现时才看得见**，中文默认宽度下永远正常。
i18n 覆盖率、单测、编译全都拦不住它。


## 2026-08-01(续三十五) · 把「该单行的地方」一次扫干净

上一条只修了状态页那一处。既然根因是**两边 `Text` 的默认行为相反**，那就该系统性地扫，
而不是撞一个修一个。

先看 Qt 侧的口径：`qml/` 里明确写 `wrapMode` 的只有 **19 处**（说明文字、告警条、
更新说明、简介这一类），写 `elide` 的有 **46 处**；**其余全部默认单行**。
所以 Swift 侧的规则就清楚了：**横向受限的行内标签一律 `lineLimit(1)`**，
只有对应那 19 处的才让它换行。

扫出 48 处「带中文 `.t`、附近没有 `lineLimit` / `fixedSize`」的 `Text`，逐个判断后
给 **24 处**补上单行：设备详情的六个段标题与行标签、设备页的「设备 / 今日」、
延迟卡的「直连」、节点页与订阅页的页标题、状态页的「最近连接 / 用量最多」、
设置页的「分钟 / 接收测试版 / 国内加速 / 共 N 组 / 共 N 条」、
订阅节点弹窗标题与删除确认里那颗定宽按钮的「删除」、更新窗的四处。

**没动的**是空态与说明文字（暂无节点 / 未发现设备 / 只读取系统已有的邻居表…… /
打开开关即可接管…… / 关于页的简介），它们在 Qt 那边本来就是 `wrapMode: WordWrap`
或居中多行 —— 这些**该**换行。

德文 640 宽复验设置页与设备页：
「Intervall der automatischen Abonnem…」单行截断，右边的数字框与「Min.」纹丝不动；
其余各行同样规整，没有第二处折行。

`swift test` 308 全绿；`i18n_check.py` 248/248；persist check 7/7。


## 2026-08-01(续三十六) · 顺着「默认值差异」往下查：字重，和一整块没做的告警横幅

上一条修的是 `Text` 的换行默认值。同一条思路往下走 —— 还有哪些**属性的默认值**两边不同？
字重是下一个：Qt 的注释反复写着「全 UI 不加粗」。

### 字重：Qt 只加粗两处，Swift 多了三处、少了一处

`qml/` 里 `font.bold` 只出现 **2 次**：
- `Main.qml:180` logo 上那个 26px 角标里的字母（注释：不加粗在这个尺寸里看不清）；
- `DevicesPage.qml:80` 安全告警横幅的**标题**。

Swift 侧却有三处比 Qt 重：延迟卡的大数字、总流量与今日流量卡的大数字，都写成了
`weight: .medium`，而 Qt 那三处只有 `font.pixelSize: 24`（即常规字重）。已全部改回。
现在 Swift 的字重出现处与 Qt 一一对应：两处 `.bold`，别处一律常规。

### ★ 顺着 `DevicesPage.qml:80` 那处加粗，发现整块告警横幅都没做

Qt 的安全告警是页面**最上面**的一块正经横幅：半径 6、红色淡底（12%）+ 1px 红边（50%）、
内距 10、20px 警告图标顶对齐、**加粗 13px 标题**「检测到局域网内有异常代理行为」+
每条威胁一行 11px 说明、右上角一颗「知道了」小胶囊。

Swift 侧是**页面最下面**的一行光秃秃的红字，既没有卡、没有标题、图标也只有 11px，
「知道了」是个纯文字按钮。位置和形态都不对 —— 而这块横幅的用途是
「有人正在冒充网关」，它比页面上任何别的东西都该先被看见。

已按 Qt 重做并移到顶部（Qt 的顺序：告警 → 概览条 → 搜索行 → 列表）。
标题那条文案 Qt 表里没有（那边是 `qsTr` 但没进 JSON），已补齐 12 语言。

`swift test` 308 全绿；`i18n_check.py` 249/249；persist check 7/7。


## 2026-08-01(续三十七) · 把两边的十六进制色**全量对了一遍**，四张卡的主色是错的

继续「系统性对，而不是撞一个修一个」。这轮把 `qml/` 里所有 `"#rrggbb"` 字面量与
Swift 侧所有 `Color(hex:)` 做了个全量差集。

**Swift 有、Qt 没有的**：只有 `#e05a5a` 一个 —— 那是我自己编的「上传红」。
Qt 的上传卡是 `accentColor: "#a84343"`。

**Qt 有、Swift 没有的**里，两个是真问题：

1. **`#8a72c6`（×4）—— 今日流量卡的紫色。** Qt 那张卡通篇是这个紫：标题、大数字、
   24 根小时柱、Top5 的占比条。Swift 侧整张卡走的是 `accent`(蓝) ——
   于是它和「连接」卡撞色，六张卡里少了一张能一眼认出来的。
2. **`#48a5a7` —— 总流量卡的青色。** Qt 的标题、大数字、对比条的「直连」段、
   图例色点都是它（`directColor`）。Swift 侧标题与数字走的是 `accent`(蓝)，
   「直连」那一段更是画成了 `textMuted.opacity(0.4)`（灰）—— 那条对比条本该是
   「青 vs 蓝」两段有色对照，变成了「灰 vs 蓝」，一眼分不出哪段是直连。

★ 还发现一处**结构性**的漏：Qt 的上传/下载卡**文字与曲线不同色** ——
`accentColor` 管文字、`chartColor` 管曲线（上传 `#a84343` / `#b14a4a`，
下载 `#4da13e` / `#5bb44b`），曲线比文字亮一档，压在卡底上才不至于糊掉。
Swift 侧只有一个色、两处共用。`TrafficCard` 已拆成 `accent` + `lineColor` 两个参数。

重截六张卡确认：上传是 Qt 的深红、今日流量整张紫、总流量整张青且对比条两段都有色。

`swift test` 308 全绿；`i18n_check.py` 249/249。


## 2026-08-01(续三十八) · 再做一次全量对：字号直方图，抓到四张卡的图标全错

色对完，接着对**字号**：把 `qml/`（除 Windows 专属的 `NpcapWindow`）里所有
`font.pixelSize: N` 与 Swift 侧所有 `.font(.system(size: N))` 做成两张直方图比。

大部分对得上，两处不对：

**28px：Qt 用了 4 次，Swift 只有 2 次。** Qt 的状态页四张卡（连接 / 延迟 / 总流量 /
今日流量）左侧那个大图标**统一 28px、统一中性灰**（`dark ? #aaaaaa : #888888`）。
Swift 侧是：连接 26、延迟 26、总流量 **16**、今日流量 **16**，而且后两张还各自用了
**卡片的主色**（青 / 紫）。四张卡的图标既不一样大、也不一样色。

图标用中性灰是有道理的：它只是个提示符号，跟着主色走会和标题、大数字抢注意力，
而那两样才是这张卡要说的话。已加 `Theme.cardIcon` 并把四处统一；
卡头的间距也一并对回 Qt（图标与文字列 12）。

**74px vs 60px**：侧栏 logo。Qt 的地球字形是 `pixelSize: 74`（正好填满 74×74 的框），
Swift 用的是 60 的 SF Symbol。这一处**没改** —— iconfont 的 em 框与 SF Symbol 的
光学尺寸不是一回事，照搬数字反而会溢出 74×74 那个框。记在这里，是有意保留的差异。

### 顺带：一个字节都没跑时，对比条画成了满条青

改完图标重截时看出来的：`total == 0` 时那条「直连/代理」对比条被填成了**一色满条**，
看起来像「全都走了直连」，而真相是**什么都没跑**。Qt 那边 `directRatio` 在
`total == 0` 时取 0、代理段整段 `visible: false` —— 画出来是一根**空槽**。已照此修正，
并补上 Qt 那个槽底色（`#1c1c1c` / `#dddddd`，正是上一条色差集里「Qt 有、Swift 没有」的
那个 `#1c1c1c`）。

`swift test` 308 全绿；`i18n_check.py` 249/249。


## 2026-08-01(续三十九) · 圆角对完是干净的；改对**显隐条件**，抓到一个没做的元素

**圆角**：把 `radius: N` 与 `cornerRadius: N` 做了直方图比。Qt 多出的 11 / 12 / 14
三个值都是「半径 = 边长一半」——也就是圆（详情窗开关的滑块、连接窗的 ✕、订阅页的圆按钮），
Swift 侧一律用 `Circle()` 表达，**不是差异**。这一项干净，不需要改。

于是换一条风险更高的线：**显隐条件**。续十六那个「策略行整个不见了」就是这一类
（条件写错 → 元素该在的时候不在）。把 `qml/` 里所有条件式 `visible:` 列出来逐条对。

### ★ 抓到：设备行的「被争抢」标记整个没做

`DeviceRow.qml` 有两处 `contended` 相关的显隐：
- 名字旁一个**红色小标「被争抢」**（半径 3、淡红底、9px 红字，靠右不挤名字，
  悬停提示「另一台设备也在把这台设备的流量劫走」）；
- **整行描一圈红框**（1px `#e0533d`）。

Swift 侧的行**两样都没有**。数据其实一直都在 —— `ArpWatch` 的
`deviceContended` 告警里带着 `subjectIP`，上一条刚做的顶部横幅用的正是同一批告警。
也就是说：横幅会说「局域网里有异常代理行为」，但**看不出是哪一台设备**，
而那正是行上这两个标记的全部作用。已补上（页面算出被争抢的 IP 集合传给行）。

新增 2 条文案 × 12 语言。

`swift test` 308 全绿；`i18n_check.py` 251/251；persist check 7/7。


## 2026-08-01(续四十) · 显隐条件继续对：两处「条件写少了」

接着上一条的显隐条件逐条对，又抓到两处 —— 都是**条件比 Qt 少一半**，
于是元素在不该出现的状态下也出现，说的还是错话。

### 1. 详情窗：没有 helper 时显示的是**假话**

Qt 那里是**两条互斥**的提示：
- `proxyEnabled && !gatewayReady` → 琥珀色的「已标记代理此设备；透明网关模块启用后
  将自动接管其流量。」
- `proxyEnabled && gatewayReady` → 「该设备的联网由本机转发……」那一大段依赖说明。

Swift 侧只做了后者，而且**不分状态一律显示**。问题是：macOS 上 ARP 欺骗与 PF 重定向
全在特权 helper 里跑，**没装 helper 时什么都没被接管** —— 这时候跟用户说
「该设备的联网由本机转发、退出会自动交还」是**假的**。两种状态两句话，不能混。
已补上前者（`gatewayReady` 在 macOS 上就是「helper 已启用」，与 Qt 的
`LanGateway::isAvailable` 同义）。那条文案 Qt 表里本来就有，不用新增翻译。

### 2. 设备行：离线设备被扣上「其它网络」的帽子

Qt 的原因徽章条件是 `isSelf || isGateway || (**online** && !proxyable)` ——
注释写得很清楚：「其它网络」这个结论**只在设备在线时才敢下**，因为判据依赖当轮扫描
拿到的地址，而从台账加载出来、还没被本轮扫描确认的设备一律拿不到地址。
Swift 侧漏了 `online &&` 这一段，于是**刚进页面那一两秒里，每台离线设备都会被标成
「其它网络」**。已补上；本机与网关两种是恒定事实，离线也照说。

`swift test` 308 全绿；`i18n_check.py` 252/252；persist check 7/7。


## 2026-08-01(续四十一) · 显隐条件最后一处：导出结果的浮动提示

把 `qml/` 的条件式 `visible:` 清单走完，最后一处没对上的是
`DevicesPage.qml:388` 的 **`noticeBar`** —— 右下角的浮动提示，
用来回报「已导出到 <路径>」与网关类报错。

Swift 侧此前是把这条消息**塞在概览条下面**当一行常驻文字，既不在 Qt 的位置上，
也不会自己消失。已按 Qt 重做：压在整页右下角（距边各 12）、半径 5、黑底 78%、
白色 12px，**限宽 + 换行**（Qt 注释举的例子是 Npcap 权限那条报错，
单行会一路撑到窗口左边界外，长文案直接看不全），**停留 6 秒**
（Qt 注释：两三行的报错三秒半读不完）。

⚠️ **只验到弹出保存面板那一步**：点「导出」会先开一个 `NSSavePanel`
（「导出设备列表 / Save As: devices」），提示条要在**保存完成之后**才出现。
再往下走会往用户的「文稿」里真写一个文件，没有必要为截图这么做 —— 已按 Esc 取消。
代码路径是接好的（`exportCSV()` 的两个分支都写 `exportMessage`），
但**那一格浮动提示本身没有截图为证**，如实记着。

至此 `qml/` 里所有条件式 `visible:` 都逐条对过了。这一轮（续三十九～四十一）
从显隐条件这条线上一共抓出 **4 处**：设备行的「被争抢」红标与红框（整个没做）、
详情窗没有 helper 时说了假话、离线设备被标成「其它网络」、以及这条浮动提示。

`swift test` 308 全绿；`i18n_check.py` 252/252。

---

## 续四十二（2026-08-01）设备类型覆盖 + 两个把界面整个弄没的窗口 bug

### 1. 详情窗的「类型」行（Qt 有、Swift 整个缺）

对着 `onActivated:` 这一线索往下查，`DeviceDetailWindow.qml:305–320` 有一行 Swift 从来没有：
64 宽的 12px `textMuted`「类型」标签 + 150 宽的 `ThemedCombo`，12 个选项
（`unknown` 显示成「自动识别」，其余是手机/平板/电脑/…）。自动识别是拿主机名与厂商猜的，
一屋子设备的厂商全是 "Apple"，认错了却改不了，只能一直看着错图标。

已补齐，并按 Qt `DeviceStore::effectiveType()` 的口径让**详情窗头像、副标题、设备列表行**
都以「手动指定优先，否则自动识别」取类型。`device` 表加了 `type_override` 列。

### 2. ★ `last_ip` 改名没写迁移 —— 台账整个哑掉

补列时发现的：`8e3463a` 把 `device` 表的 `password` 列改名成 `last_ip`，**没写迁移**。
改名之前建的库里那一列还叫 `password`，于是之后每一条 `SELECT … last_ip …` /
`INSERT … last_ip …` 都以「no such column」失败 —— 备注名存不下、代理开关记不住、
策略选了等于没选，而 `save()` 只是返回 false，**界面上一点提示都没有**。
本机的库正是这种：`device` 表一条都写不进去，我在详情窗里改类型、改备注名、
选策略，全部「点了没反应」。

补列 + 两条回归测试（旧 schema 的库照样能读写；类型覆盖往返）。
`SQLite.swift` 全程不报错这件事本身也记一笔 —— 这类失败只能靠测试兜。

### 3. ★ 启动后主界面**根本不存在**，而且再也打不开

改完之后重启验证，撞上一个更大的：启动后只有一个 600×720 的**空白「设备详情」**窗，
主窗一个都没有。托盘的「控制面板」和点 Dock 图标都救不回来 —— 它们走
`mainWindow?.makeKeyAndOrderFront`，而 `mainWindow` 是 nil，等于什么都没做。
**在 HEAD（未改动的树）上同样复现**，与本轮改动无关；清空三个 preference 域、
`Saved Application State` 全空，照样复现，也就不是系统的窗口恢复。

修的是「保证能开出来」，三条一起：

- 主窗从 `WindowGroup` 换成 **`Window(id: "coast.main")`**。`Window` scene 会在「窗口」
  菜单里留一条打开它的菜单项（`WindowGroup` 没有），这是 AppKit 侧唯一不经 View 就能
  开窗的入口 —— 而这时候恰恰一个 View 都没有。顺带也就没有「开出第二个主窗」的问题了
  （`discardDuplicateMainWindows` 那套本来就是给 `WindowGroup` 擦屁股的）。
- `adopt()` 找不到主窗就走 `openMainWindow()` 现开一个，再认领一次（挂 autosave 名与 ✕ 拦截器）；
  认主窗时**跳过**三个附属窗（原来按 `canBecomeMain` 找第一个，抓到的正是那个空详情窗，
  于是主窗的位置也被记到了空壳上：`NSWindow Frame CoastMainWindow` 被写成 420×420）。
- 三个附属窗（更新/详情/连接）标 `isRestorable = false` + `NSQuitAlwaysKeepsWindows=false`，
  它们是**点出来的**窗，不该被摆回来。

### 4. 点 Dock 图标判据错了

`applicationShouldHandleReopen` 原来看 AppKit 传的 `hasVisibleWindows`：详情窗开着时它是
true，于是点 Dock 图标什么也不发生 —— 主窗明明隐藏着。判据改成「**主窗**可不可见」。

### 5. 台账改动的即时刷新

详情窗是独立窗口，改完备注名/类型/策略，设备页那份 `ledger` 快照要等下一轮扫描（几秒）
才更新，中间像是「点了没反应」。加了 `AppState.ledgerRevision` 这个可观察信号，
写台账的四处都发它；设备页 `onChange` 重读。顺带修好设备页开关代理时没刷新
历史库 IP→MAC 映射的问题。

截图验证：主窗恢复到 1340,693 900×510、设备列表里 192.168.31.155 那行变成打印机图标；
详情窗 600×720，「类型」行在「备注名」下面，下拉 12 项，选「打印机」后头像/副标题/列表同步变；
✕ 隐藏主窗 → 点 Dock 图标 → 主窗回来（详情窗开着也回得来）。
`swift test` 310 全绿；`i18n_check.py` 253/253；`settings_persist_check.py` 7/7。

---

## 续四十三（2026-08-01）「型号」这一格 + 几何/配色的机械核对

接着上一轮的路子往下核。这一轮走了三条线：

### 1. 数值几何逐格对（干净）

`DeviceDetailWindow.qml` / `ConnectionsWindow.qml` / `LogsPage.qml` + `LogTimeline.qml`
里所有显式的宽高/间距/圆角/字号，逐个与 Swift 的 `.frame` / `spacing` / `cornerRadius`
对了一遍 —— **全中**：详情窗 600×720、头像 48、开关 52×26 与滑块 22×22、键列宽 64、
备注框高 28、类型下拉 150、策略下拉 130、图表 90、近 7 日 56、连接行 34、空列表 96；
连接窗 720×480（最小 480×320）、徽章高 22 左右各 6、行高 42 圆角 5、关闭钮 30×30；
日志页标签高 30、下划条高 2 圆角 1 左右各让 10。

各页的**页面内距**也对了：状态页左/上/下 10 而右边不留（滚动条贴右缘，内容列自己收窄
10 补回来）、节点页与关于页四边 10、订阅/设置/日志页 0 且逐项补、设置卡内距 14/12。

`DeviceDetailWindow.qml` 里写死的颜色（#5bb44b ×3、#b14a4a ×2、#ff6b6b、#c69a54）
与 Swift 也是一一对上的。

### 2. 文案逐条对 → 抓出「型号」

把每个 QML 的 `qsTr("…")` 与对应 Swift 文件的 `"…".t` 做差集。除去口径本来就不同的几类
（策略名在 `PolicyMode.title`、窗口标题在 `CoastApp`、拒绝理由在 `RedirectTargets`、
Npcap 那几条是 Windows 专有），只剩一条真缺的：详情窗信息网格的 **「型号」**。

Qt 的网格是 IP / MAC / 厂商 / **型号** / 首次发现 / 主机名 六格，
Swift 是 IP / MAC / 厂商 / **接口** / 首次发现 / 主机名 —— 第四格被换成了别的东西。

已补：型号回到 Qt 的第四格，「接口」挪到第七格。接口是 macOS 独有的一项（本机可能同时挂着
Wi-Fi、有线和雷雳网桥，设备从哪张网卡看到直接决定 ARP 欺骗往哪儿发），留着，但**排在
Qt 六项之后**，前六格与 Qt 逐格对齐。

### 3. 型号从哪儿来 —— `DeviceModelBrowser`

Qt 是拆原始 mDNS 报文抠 TXT 的 `model=`/`md=`，另兼一路 UPnP 的 `<modelName>`
（`LanScanner.cpp:959` / `:1040`）。macOS 上不必自己拆报文：Bonjour 就是 mDNS，
`NWBrowser` 的 `.bonjourWithTXTRecord` 把 TXT 连着浏览结果一起给出来 —— 一百行搞定。

两个实测决定了实现：

- **不能只认 `_device-info._tcp`**。本机这台 iMac 压根没广播它，型号是在
  `_airplay._tcp` 的 TXT 里（`model=iMac21,1`）。所以同时浏览
  `_device-info` / `_airplay` / `_raop` / `_googlecast` 四类。
- **对号入座用两把钥匙**：Bonjour 给的是服务名（「王超's iMac」），既不是 IP 也不是主机名。
  先用 TXT 里的 `deviceid`（多数设备就是网卡 MAC，而 MAC 正是台账主键，规范化后精确匹配），
  再退回服务名与反查主机名比。都对不上就显示「-」，和 Qt 那边没人广播时一样。

⚠️ **正例没有截图为证**：这个局域网上只有本机一台在广播 Bonjour，而它的 `deviceid`
是一个派生地址、服务名也不等于主机名，所以两把钥匙都对不上 —— 屏幕上验到的是
「这一格在 Qt 的位置上、无数据时显示 -」，**没验到「真设备显示出 iPhone15,2」**。
解析与规范化那一段是单测覆盖的（`model`/`md`/`am` 三个键、只含空白等于没有、
`deviceid` 补零小写、非法值返回 nil），如实记着。

`swift test` 315 全绿；`i18n_check.py` 254/254（新增「型号」一条，12 种语言都已有 Qt 的现成翻译）；
`settings_persist_check.py` 7/7。

---

## 续四十四（2026-08-01）今日流量卡：Top5 占比条、小时柱的当前小时、以及一个恒真的判空

文案差集继续跑（`StatusPage` / `NodesPage` / `AboutPage` / `LogsPage` 四个页面对整个
Swift 树），只剩一条 Qt 有而 Swift 没有：**「今日暂无数据」**。顺着它把今日流量卡整张
重新对了一遍，抓出三处。

### 1. Top5 少了占比条，空位的处理也反了

Qt 的每条榜单下面有一根 **3pt 占比条**（圆角 1.5、槽 `#1c1c1c`/`#dddddd`、
填充 `#8a72c6`），基准是**榜首**而不是今日总量 —— Top5 之外还有长尾，按总量算五根全是细线。
Swift 只有「名字 + 字节数」两列，没有条。已补齐；行内间距 8、名字 11px、字节 10px、
两行间距 1，与 Qt 逐个对上。

另外 Qt 是 `Repeater { model: 5 }` + `visible: e !== undefined` —— QML 的
`visible: false` 会把元素**从布局里摘掉**。我第一版照「连接卡补空行」的思路写成
`opacity(0)`，那是留了空位，与 Qt 不同，已改成条件渲染。
（连接卡确实要补空行，那是 Qt 在**那张卡**上写死的行为；两张卡的规则不一样。）

### 2. 小时柱丢了「现在几点」

Qt 的 24 根小时柱**只有一种颜色**，靠透明度区分：当前小时 1.0、其余 0.55，
0 字节的小时也画 1px 底线（间隔才看得出来）。Swift 写的是「有流量才上色、没流量画成灰」——
当前小时这条信息整个没了，灰柱子还看着像坏格子。已按 Qt 改。
悬浮提示也从 `%d 点：%s` 改回 Qt 的 `H:00  <字节>`。

### 3. ★ `total > 0` 恒真 —— 空槽那条分支形同虚设

总流量卡的对比条上面写着 `let total = max(comp.totalBytes, 1)`，下面用 `total > 0`
判断有没有流量 —— **永远为真**。于是「一个字节都没跑时画一根空槽」那条修正
（续三十几轮加的）从来没生效过：截图里仍是**整整一条蓝**，看着像「全都走了代理」。
分母兜底和判空是两件事，已拆成 `divisor` 与 `hasTraffic`。

这已经是这个项目里第四次撞见「一个永远成立的检查」。

### 验证方法

今日流量卡要有数据才看得出来，而核心没在跑。用 `COAST_DATA_DIR` 指向一个临时 profile，
往它的 `coast.db` 里塞 5 条今天的 `conn`（**`ended_at` 是毫秒**，第一次按秒塞，
卡片一直显示「今日暂无数据」）—— 用户真实的数据目录一个字节都没动。
截图确认：五条榜单 + 五根长度递减的占比条、24 根小时柱都有 1px 底线、
空数据时「今日暂无数据」居中压在卡片底部、总流量为 0 时是一根**空槽**。

`swift test` 315 全绿；`i18n_check.py` 253/253；`settings_persist_check.py` 7/7。

---

## 续四十五（2026-08-01）规则表的 400 条上限 —— Qt 会说，Swift 不说

文案差集把剩下的 QML 全跑完了（订阅/设置/Main/更新窗/连接窗/规则编辑器/四个小组件）。
差出来的多数是**假阳性**：`Host`、`GeoIP`、`Search` 三个词 Qt 用 `qsTr` 包着，
Swift 是直接的英文字面量（专有名词，本来就不翻）；区域编辑器那几条
（`url（url-test/fallback…）`/`成员（每行一个…）`/`间隔秒`）是早就记过的**刻意分歧** ——
Swift 的区域编辑器编的是 `{name,type,rule}`，也就是 Qt 自己的 `ConfigBuilder` 真正消费的字段。

真的差一条：**「共 %1 条，显示前 %2 条」**。

Qt 的 `SettingsController::reloadRules()` 把规则列表**截到 400 条**（`cap = 400`），
并在右上角如实写出「共 N 条，显示前 M 条」；只有没截断时才写「共 N 条」。
Swift 既不截断，也永远只显示「共 N 条」—— 而它那句用的是 `rules.count`（全量），
列表里却是过滤后的行。也就是说：**一搜索，数字和列表就对不上，且没有任何线索**。

已按 Qt 对齐：`ruleRenderCap = 400`，标签在 `显示的 < 全量` 时换成两参数那句。
i18n 表里 Qt 的 `%1/%2` 版早就有，按本项目既有约定给 12 种语言补了 `%d` 版
（照 `%1` 版逐条换占位符，不是重译）。

截图验证：塞 500 条规则进临时 profile，右上角显示「共 500 条，显示前 400 条」。

⚠️ **过滤那一支没验到**：这台机器的拼音输入法把合成串截在候选窗里，
`cliclick` 合成的回车/空格进不了输入法的事件通道，搜索框里的字始终是未上屏状态，
过滤压根没触发。截断与过滤走的是同一行表达式，风险不大，但如实记着。
（顺带修正一条旧记录：续四十二里「输入备注名不落库」我算在了 `last_ip` 迁移头上 ——
现在看很可能同样是输入法没上屏。迁移那个 bug 本身是真的、有测试；
但**备注名输入这条路径至今没有截图为证**。）

同轮核对：设置页规则行的几何与 Qt 逐个对上（行高 36、外缩 10、内距左 10 右 6、
间距 8、三列 150/130/自适应、字号 13/12/12、两颗 30×24 药丸、列表间距 4、末项距底 10）。

`swift test` 315 全绿；`i18n_check.py` 254/254；`settings_persist_check.py` 7/7。

---

## 续四十六（2026-08-01）外壳 / 更新窗 / 规则编辑器的数值几何

这一轮把还没机械对过的三个文件的数值几何逐个核了一遍。

### 对上的（无差异）

- **`Main.qml` 外壳**：侧栏 170、页脚 38、内容列上/右内距 5 且底部贴边、
  侧栏列上 16 下 5、logo 盒 118 里居中的 74×74、状态角标 26×26 半径 7 字号 15 粗体白底、
  导航项 40 高半径 5（图标 17 距左 12、文字 14 再距 9、右缘让 8）、
  首项 topMargin 0 其余 5、左缩 20。
- **`UpdateWindow.qml`**：600×560（最小 460×420）、三颗 tab 78×46 半径 5、
  正文内距 10 间距 6、字号 16/13/15/12、说明卡内距 8 半径 4、
  进度条 22 高半径 4 内缩 1、百分比 11、取消钮 24×24 圆形悬停转红。
- **`RuleEditorWindow.qml`**：460×440、内距 16、间距 8、标题 16、字段标签 12、
  输入框/下拉 32 高半径 3、输入行左 8 右 4 间距 2；16 个规则类型的**顺序与内容**一致。
  另外截图确认：主窗缩到最小（640×430 内容）时这张 460×440 的 sheet **完整放得下**，
  三个字段与底部「取消/确定」都没被裁 —— 之前担心 440 > 430 会裁，是多虑。

### 改掉的两处

1. **页脚那个终端图标是 14，不是 12**。Qt 写的是 `pixelSize: 14`（比旁边 12 的
   日志文字大一档），Swift 写成了 12。
2. **进程候选下拉悄悄只列 200 条**。Qt 的候选是一个能滚的 220px 列表、**不截断**；
   Swift 换成了原生菜单（NSMenu 塞上千项会卡主线程），于是加了 `prefix(200)` ——
   但没有任何提示。本机 `ps` 去重后是 **431** 个进程名，也就是说一半以上的候选
   用户根本看不到、也不知道看不到。已在菜单末尾补一条禁用项，复用规则表那句
   「共 N 条，显示前 M 条」（不另造要翻 12 遍的新串）。

   ⚠️ 这条**只做到代码级**：要看到那条提示得滚到 200 项菜单的底部，
   截图与合成按键都够不着（End 键会把菜单关掉）。判断只是一个
   `filtered.count > 200` 的条件，风险很小，但如实记着。

顺手更正一句过期注释：版本行的注释还写着「更新窗在这里是 sheet」，
它早就改成 `Window(id:)` 独立窗了。

`swift test` 315 全绿；`i18n_check.py` 254/254；`settings_persist_check.py` 7/7。

---

## 续四十七（2026-08-01）菜单栏 —— Qt 在图标旁画两行速率，Swift 只有一个图标

顺着「数字怎么格式化」这条线查下去，发现 Qt 有**三个**速率格式化器，而它们各管一摊：

| 出处 | 函数 | 样子 |
|---|---|---|
| `Theme.fmtRate`（设备行/详情窗） | `fmtBytes + "/s"` | `0 B/s`、`1.20 MB/s` |
| `QmlBridge::speedText`（状态页两张大卡） | 恒两位小数、单位到 TB、**不带 `/s`** | `0.00 B`、`1.20 MB` |
| `TrayController::speedTextCompact`（macOS 菜单栏） | B 不带小数、其余**一位**、带 `/s` | `0 B/s`、`12.3 KB/s` |

由此抓出两处：

### 1. ★ 菜单栏少了两行速率（Qt 的 `MacSpeedItem.mm`）

Qt 在 macOS 上把「图标 + 两行速率」**手绘成一张恰好等于菜单栏厚度的图**：
核心没跑只画图标（宽 `iconSide + 4`）；核心在跑则图标在最左（边长 = 厚度 − 3）、
间隔 2pt、再一块**定宽右对齐**的两行文字（上行上传、下行下载，无 ↑↓ 标识，
`menuBarFont(8.5)`，宽度按最宽模板 `888.8 MB/s` 算死）。定宽是全部意义所在 ——
不定宽的话数字一长一短就把图标推来推去，菜单栏里看着在抖。

Swift 这边只有一个 SF Symbol 图标，速率只在菜单里的 UP/DOWN 两行 ——
那是 Qt 的**非 macOS** 分支。已按 Qt 补齐（同一套数值），图标也改成
`NSApp.applicationIconImage`（Qt 画的就是彩色应用图标，所以文字颜色要自己按明暗取）。

顺带把 `NSStatusItem` 的 `autosaveName` 钉成 `"CoastTray"` 并强制 `isVisible = true` ——
Qt 那边的注释记着这个坑：`visible` 会按 autosaveName 持久化，历史上写进去的 NO
会让整项在菜单栏里根本不出现。

### 2. 菜单里的 UP/DOWN 用错了格式化器

Swift 用的是 `Formatting.rate`（两位小数），Qt 用的是紧凑版。已加
`Formatting.compactRate` 并改用它 —— 菜单栏那两行与菜单里的 UP/DOWN 取的是同一个串
（Qt `macTraySetSpeed` 也是这么做的）。

### 状态页那两张大卡的 `/s`：**刻意不跟**

Qt 的 `speedText` 不带 `/s`（`MetricCard` 的默认值就写着 `"0.00 B"`），
而它自己在设备行、详情窗、菜单栏三处都带 `/s`。这是 Qt 自己的不一致，
Swift 保留 `0 B/s` 的写法 —— 一个每秒在变的数字不标单位时间，读者只能猜。
**同时更正续三十几轮的一条旧记录**：那里写着「`upText` 是 `speedText`（带 `/s`）」，
括号里那半句是错的，`speedText` 不带。绑定本身（速率而非累计量）是对的。

### 验证

`trayImage(...)` 抽成了纯函数（`NSStatusItem` 在测试进程里造不出来），
三条测试量它：核心停 → 宽度 `iconSide + 4`；核心跑 → **`0 B/s` 与 `888.8 MB/s` 画出来一样宽**；
图标画不出来时也不塌成 0 宽。另加 6 条 `compactRate` 的逐值断言。

⚠️ **两行速率那一支没有截图**：要让它出现得核心真的在跑，而启动核心会去改**用户本机的
系统代理**设置 —— 为一张截图不值得。截图验到的是「图标那一支」以及状态项确实出现在菜单栏里。

`swift test` 321 全绿；`i18n_check.py` 254/254；`settings_persist_check.py` 7/7。

---

## 续四十八（2026-08-01）设备列表的**次序** —— 少了一半规则，离线那几行还是随机的

这一轮先把 `NodesPage`/`NodeRow`/`DevicesPage` 的数值几何机械核完（**全部对上**：
节点页内距 10、间距 8、顶栏高 30、标题 18、计数 9、搜索框 28 半径 3、放大镜 16、
清除 ✕ 14、列表行距 1；节点行 40 高半径 4、徽章半径 5 左右各 5 上下各 3、右侧定宽 82；
设备页概览条间距 16、今日 ↓↑ 用 `#5bb44b`/`#b14a4a`、新设备提醒开关 34×18 滑块 14×14、
告警横幅半径 6 内距 10、图标 20、标题 13 粗、正文 11、忽略按钮半径 4 左右各 8 上下各 4）。

然后转去对**排序**——「元素的位置」在列表里就是次序。抓到一处实的：

Qt `DeviceListModel::buildTarget()` 的排序键是
**在线优先 → 今日流量（MB 档位）降序 → IP 升序 → MAC 兜底**，
注释里写明了三条理由：排序键**绝不能含实时速率**（每拍都在变，跑流量的设备会一直换位置）；
今日流量**按 MB 取档**再比（压掉两台用量相近的设备来回互超）；最后用 MAC 兜底
（次序任何时候都得是确定的）。

Swift 这边压根没有排序：在线的那批沿用 `LanBrowser.scan()` 的「网关置顶 + IP 升序」，
**离线的那批直接按 `ledger` 字典的遍历顺序追加** —— Swift 的 `Dictionary` 不保证顺序，
那几行每次刷新都可能换个位置。今日流量这一档也整个没有。

已补齐：
- `HistoryStore.todayByDevice()` —— 一次分组查询拿到今日每台设备的累计字节（不是每台查一遍）；
- `DeviceOrdering.key(online:ip:mac:todayBytes:)` 放进 `CoastKit`（挨着 `DeviceFilter`），
  因为排序错了在界面上只表现为「行怎么老在动」，很难指认，必须能单独测；
- 4 条测试：在线优先压过流量、同 MB 档不因差几百字节互换、IP 按**数值**排（.2 在 .10 前）、
  全相同时 MAC 兜底。

截图确认真机上 8 台设备是 .1/.59/.67/.155/.185/.194/.219 严格 IP 升序
（这个 profile 里今日每台流量都是 0，所以全落在同一档，正好验的是 IP 那一级）。

`swift test` 325 全绿；`i18n_check.py` 254/254；`settings_persist_check.py` 7/7。

---

## 续四十九（2026-08-01）「用量最多」答错了问题；REJECT 被算成了直连

继续对排序/聚合这条线。`ConnectionRow.recent/top` 的**排序**本身与 Qt 一致
（start 倒序 / 字节倒序），但再往上一层看**被排的是什么**，就对不上了。

### 1. ★ 「用量最多」按连接排，Qt 是按 host 累计

Qt 维护一张 `m_hostBytes`（host → {字节, 设备, 直连?}），**跨连接累加、跨会话生命周期保留**，
连接断了也还在榜上；表满 512 就只留用量最大的一半；零字节的目标不占榜位。

Swift 拿的是**当前在途连接**排前 5。两个后果：同一个域名往往同时开着十几条连接，
一个域名就能占满整张榜；而刚下完的那个大文件一断线就从榜上消失。
那答的是「此刻谁在跑」，不是「这次运行谁跑得最多」。

已按 Qt 补 `TrafficComposition.hostBytes` + `topHosts(limit:)`（含 512 上限与减半裁剪、
零字节剔除、字节相同时按 host 定序以免字典序漂移），卡片改成固定 5 行、不足补空。

### 2. ★ REJECT 的流量被算进了「直连」

Qt 明确：`if (outbound.startsWith("REJECT")) continue;` —— **两桶都不记**
（既没出网也没流量）。Swift 写的是 `chain == "REJECT"` 时计入 **directBytes**，
于是「被规则拦掉」的流量在对比条上显示成「直连出去的」，意思正好相反；
而且只匹配了 `REJECT`，漏掉 `REJECT-DROP`。已改成前缀匹配 + 跳过。

**这条 bug 有一条测试在替它站岗**：`splitsByChain` 断言的是 `directBytes == 250`
（注释还写着「DIRECT + REJECT」）—— 照着当时的实现写的断言，而实现本身是错的。
已连同改正。这是本项目第五次撞见「测试/检查在为错误行为背书」。

### 3. chains 为空的归属

Qt 是 `direct = (outbound == "DIRECT")`，空 chains 落进**代理**桶；Swift 原来落进直连桶。
已按 Qt 改，并补一条测试写明这是 Qt 的口径。

### 验证

5 条新测试：同 host 多连接合成一行、断开后仍在榜、REJECT/REJECT-DROP 两桶都不记、
空 chains 算代理、零字节不占位。截图确认空态（核心没跑）仍是「暂无流量」——
有数据的那一支要核心真在跑，与菜单栏两行速率同一个限制，没有截图。

`swift test` 330 全绿；`i18n_check.py` 254/254；`settings_persist_check.py` 7/7。

---

## 续五十（2026-08-01）「设备」这一维一直是空的；告警横幅每隔几秒闪一次

继续对聚合口径。两处，都不是显示层的问题，是**查询本身答错了**。

### 1. ★ 今日 Top 的三宗差异 —— 「设备」维几乎永远空着

Qt `HistoryStore::topGroups()` 与 Swift 的 `todayTop()` 逐行对，差三件：

1. **算上在途连接**。Qt 把 `m_live`（还没落库的在途连接）一并计入；Swift 只查库 ——
   一个还没断的大下载在榜上完全看不见，而「今天谁跑得最多」多半问的就是它。
2. **空 key 不该丢**。Swift 的 SQL 里写着 `\(column) != ''` 直接滤掉。而 macOS 上
   **绝大多数连接没有 mac**（只有走透明代理的局域网设备才有），于是「设备」这一维
   几乎永远是空的 —— 界面显示「今日暂无数据」，而同一张卡的标题上就写着 13.79 MB。
   Qt 是保留空 key 并给名字，三种维度的「空」含义还不一样：
   设备 =「本机 / 未归属」、进程 =「其它设备」（进程名只有本机连接才有）、域名 =「未知域名」。
3. **设备维度显示台账里的名字**，不是一串 MAC（`deviceNames` 由调用方喂进来，
   历史库不认识台账）。

已全部补齐。截图对照：切到「设备」以前是空的，现在是「本机 / 未归属 13.79 MB」。
`isProxied()` 也抽了出来，与 SQL 里的 `scopeClause` 互为同一判据的两种写法（注释写明）。

### 2. ★ 安全告警：这一轮没检测到 ≠ 已经停止

Qt 的 `m_secAlerts` 有 **TTL（`kSecTtlMs` = 150 秒）**：一条威胁出现后留在横幅里，
直到连续 150 秒没再观察到才被 `sweepSecurityAlerts()` 扫掉；展示按**首次出现时间**升序。

Swift 是每轮直接 `securityAlerts = alerts` 全量替换。而 ARP 争抢的表现**恰恰就是绑定来回翻**
（这一拍是攻击者的 MAC、下一拍又翻回真 MAC）—— 即时撤销会让横幅每隔几秒闪一次，
用户看到的是界面在抽搐，而不是「你正被攻击」。另外排序用的是 id（字典序），
新告警可能插到已有告警**上面**，把正在看的那条顶下去。

已补 `ArpAlertRetention`（TTL 150 秒、按首次出现时间定序、`clear()` 对应「忽略」）。
通知仍只对**本轮真的观察到**的发 —— 留存是给界面看的，不该让 TTL 内的旧告警反复弹通知。

### 验证

新增 9 条测试：空 key 三种维度的名字、设备维度取台账名/回落 MAC、`isProxied` 与 SQL 同判据；
告警「隔一拍不撤」「超 TTL 才撤」「按首次出现排」「重新观察到会续命」「clear 立刻空」。
`todayTop` 因为要用 `.t` 翻译标签而成了 `@MainActor`，几个历史库测试套件跟着标上。

`swift test` 339 全绿；`i18n_check.py` **257/257**（三条新串 Qt 的表里本来就有）；
`settings_persist_check.py` 7/7。

---

## 续五十一（2026-08-01）在途连接又漏了两处；两张表都在自己乱动

沿着上一轮的口径继续。四处。

### 1. 「常用域名」没有时间窗，也不算在途连接

Qt 的调用是 `topDomains(mac, 7, 5)` —— **近 7 天**。Swift 没有窗口，把库里全部 30 天
都算进来，「常用域名」于是变成了「一个月里的常用域名」，答的不是同一个问题。
另外同样只查库：Qt 的注释写得很直白 ——「只查库的话正开着的连接一条都算不进去，
刚打开的网页在『常用域名』里会看不见，看起来就像统计坏了」。两点都补上了。

### 2. 近 7 天柱状图的「今天」那一格同理

Qt 在填完库里的数据后，把在途连接加进今天那一格。Swift 没有 —— 今天这根柱子
要等连接断了才开始涨。已补。

### 3. 详情窗的连接表每一拍重排

Swift 是 `state.connections.filter { sourceIP == ip }`，**不排序**。而 `/connections`
是 Go map 的快照，**原始顺序随机**（Qt 的注释专门点了这件事）。于是这张表每秒重排一次。
已按 Qt 改成 start 倒序（同刻用 id 兜底）：新连接在最上面 —— 这个列表问的是
「这台设备此刻在跟谁说话」，刚建立的那条才是要看的。

### 4. ★ 连接窗的整张表随流量跳

`ConnectionLedger.merge` 每次合并都 `sort { totalBytes > }`。后果：只要有连接在跑流量，
整张表就一直在换位置 —— 想点某一行的「断开」，手还没到它就已经挪走了。

Qt 的 `ConnectionsModel::recompute()` 是一整套 reconcile（删已消失 → **原地**刷存活 →
新的**追加到末尾**），存在的全部意义就是**让行不动**：正因为核心给的顺序是随机的，
才要靠位置稳定来抵消它。已照做。要找「跑得最多」的那条，用窗口自带的搜索框。

**又是一条测试在为非 Qt 行为背书**：`sortedByTotalBytesDescending` 断言的正是那个排序。
已改写成「位置不随流量变」，并多加一拍验证：第二拍里 small 变成跑得最多的那条，
位置仍然不动、新来的排在最后。

### 验证

3 条新测试（常用域名/近 7 天算上在途、别的设备的在途不算到这台头上）+ 改写的那条。
截图确认详情窗各块位置不变、无记录时「近 7 天」整块仍然隐藏（与 Qt 的
`visible: daysTotal > 0` 一致）。有实时连接的那些分支仍受「不能启动核心」的限制。

`swift test` 342 全绿；`i18n_check.py` 257/257；`settings_persist_check.py` 7/7。

---

## 续五十二（2026-08-01）本机那一行的速率恒为 0；「仅可用节点」Qt 那边其实是死的

### 1. ★ 全机器最忙的一台，设备列表里永远显示没流量

Qt 在按设备归集流量时有一段专门处理：源地址是**本机自己的任一地址**（回环 / TUN 的
`198.18.0.1` / 任一网卡）且不是网关代理进来的，就记到「本机」那台名下。
它的注释写着：「以前这类全部落进『未归属』丢掉，于是设备列表里本机那一行的
速率/今日用量恒为 0 —— 全机器最忙的一台反而永远显示没流量」。

Swift 的 `DeviceTraffic` 直接按 `sourceIP` 分桶，而设备行认的是那台机器的**局域网 IP**。
本机自己的连接 sourceIP 多半是 `127.0.0.1`（或开增强模式后的 TUN 地址），
两者根本对不上 —— 于是本机那一行的速率与本次会话用量恒为 0，与 Qt 修掉之前一模一样。

已补 `DeviceStore.isLocalMachineIP()`（回环 / `198.18`、`198.19` / 本机全部网卡 IPv4，
网卡表缓存 30 秒，与 Qt 同期）与 `DeviceTraffic.observe(_:localIP:)`。
4 条测试：回环归本机、TUN 地址归本机、局域网设备不受影响、不知道本机 IP 时不猜。

### 2. 「仅可用节点」——**Qt 的 QML 版根本没实装**

对节点列表的过滤时发现：Qt 的 `NodeListModel::reconcile()` **只按搜索词过滤**，
`nodeOnlyAvailable` 这个设置在 QML 版里是「读了、存了、设置页有开关，但没有任何地方用」——
真正会按它过滤的是**已经不参与编译**的 Widgets 版 `MainWindow`。

Swift 是**真的**在过滤。这条**刻意不跟 Qt**：一个点了没反应的开关比行为差异更糟。
已在 `NodeFilter` 里写明这是有意为之，并记下代价（核心刚起、还没测过延迟的那一两秒
列表是空的，随后自动测延迟填上）。

顺带修掉一处**注释与代码相反**：那段注释写着「`delay == 0` 是还没测过，一律滤掉的话
列表会整个空掉，所以不能滤」，而代码与测试都正是滤掉的。三者里错的是注释。

### 另外核对过、无差异的

`ClashService` 的节点排序键（当前节点置顶 → 速度降序 → 延迟升序）与 Qt 逐条相同；
Qt 的 `connCount`（每台设备的活动连接数）虽然存在台账里，但 QML 里没有任何地方显示，
Swift 不做也对得上。

⚠️ 已知但**这次没动**：Qt 的速率是 `delta * 1000 / dt`（按**实测**的两拍间隔归一化），
Swift 直接把一拍的增量当作每秒速率。轮询晚到时（系统忙 / 睡眠唤醒）Swift 会报得偏高。
影响是数值精度而非元素缺失，留到下一轮处理，免得这一轮改动面过大。

`swift test` 346 全绿；`i18n_check.py` 257/257；`settings_persist_check.py` 7/7。

---

## 续五十三（2026-08-01）速率按实测间隔算；连接行的「本机」标注

### 1. 速率归一化（上一轮记下的那条）

Qt 是 `rate = delta * 1000 / dt`，`dt` 是**实测**的两拍间隔；Swift 直接把「一拍的增量」
当成「每秒」。轮询晚到时（系统忙、睡眠唤醒、核心卡住）攒了 3 秒的量会被报成 3 倍速率。
已改成按实测间隔归一化（首拍与间隔离谱地小时按 1 秒兜底）。
**累计量仍用原始增量**——那是「一共跑了多少字节」，与间隔无关。4 条测试。

### 2. ★ 连接行的设备列：回环应当写「本机」

Qt 的 `QmlBridge::deviceNameFor()` 四条：`dev-` 用户名 → 台账设备；源 IP 命中台账 →
显示名；**回环（`127.0.0.1` / `::1`）→ 台账里的本机记录，没有就 `tr("本机")`**；
其余原样显示 IP。

Swift 的回环分支返回的是**空串**，注释还写着「与 Qt 一致」—— 而 Qt 明明写的是
`return tr("本机")`。这一列的意义正是把本机流量与局域网设备的流量分开，
全空着等于这一列对本机行没有任何信息。已按 Qt 改（连同那条测试，它断言的正是留空）。

### 3. 匹配范围：整张台账，不只是「代理中」的

Qt 的 `deviceNameFor()` 拿**整张台账**去匹配源 IP；Swift 传进去的是
`proxiedDeviceLabels`（只有正在被代理的）。一台认得出名字、但此刻没开代理的设备，
它的连接在列表里只会显示成一串 IP。已改成全部有 IP 的台账设备。

`swift test` 350 全绿；`i18n_check.py` 257/257（「本机」表里本来就有）；
`settings_persist_check.py` 7/7。截图确认设备页与概览条正常。

---

## 续五十四（2026-08-01）进程候选的排序；「应用」不该为混合端口重启核心

### 1. 进程候选按码点排，Qt 是不区分大小写

Qt：`list.sort(Qt::CaseInsensitive)`。Swift：`seen.sorted()`（按码点）——
于是 `Xcode` 排在 `curl` 前面，大小写混排的进程名被切成两段，
想找 `chrome` 得先猜它被排到了哪一半。已改成 `localizedCaseInsensitiveCompare`。

### 2. ★ 改混合端口不该重启核心

Qt 分得很清楚：只有 **API 端口**（`external-controller`）变了才 `stopCore()/startCore()` ——
它不在 `full.yaml` 里，热重载碰不到；**混合端口**就写在 `full.yaml` 里，
`rebuildConfig()` 的热重载会让核心重新监听。

Swift 是「两个端口任一变化就重启」，白白把所有连接断一次。已按 Qt 改。
顺带补上 Qt 的**第三条分支**：核心没在跑时改 API 端口，既不能说「已重启」也不能什么都不说，
要告诉用户何时生效 —— 新增「已应用（下次启动核心生效）」，12 种语言按既有约定补齐。

### 核对过、无差异的

- `nodeAllowed()` 的允许/排除正则（含「正则非法就当这条规则不存在」）逐条相同；
- 更新检查：`/releases?per_page=20` 全量列表 + `prerelease` 按开关过滤 +
  版本号砍掉第一个 `-` 之后再比（Qt 注释里那个坑），三处都一样；
- GeoIP 源地址与 Qt 同一个（`MetaCubeX/meta-rules-dat` 的 `country.mmdb`）；
- 关于页：列宽 `min(卡宽-80, 420)`、间距 14、logo 84 / 标题 30 / 版本行 14 /
  提示 12 / 简介 13 且行距 1.35、链接行前让 6、三个链接的文案与 URL 逐条相同；
- 设置页「过滤」tab：4 行、间距 10、左右 10 距底 10、标签列宽 150 字号 13，
  两组正则预设与 Qt 的 `allowRulePresets`/`noAllowRulePresets` 逐条相同（截图确认）；
- Qt `apply()` 会写的 20 个配置键，Swift 全都写（多数经 `bool(_:key:)` 这个泛型 helper）。

### 一处**刻意不跟**（补记）

Qt 的 `apply()` 是**整份重写 `config.yaml`**，只写它认识的那 20 个键 ——
用户配置里的 `root`/`route`/`power`/`experimental`/`gateway`/`timer`/`socket`/`secret`/`beta`
等等会在保存的一瞬间**全部丢失**。Swift 是逐键就地改写、其余原样保留。
这条不跟，理由不用多说。

`swift test` 350 全绿；`i18n_check.py` 258/258；`settings_persist_check.py` 7/7。

---

## 续五十五（2026-08-01）本机那一行的「最近访问」与详情连接表，也是空的

上一轮把**速率**的归属修了（本机自己的连接源地址是 `127.0.0.1` / TUN 地址，
而设备行认的是局域网 IP）。这一轮顺着同一个判据把剩下两处一起补上：

- **设备行的「→ 最近访问」**：`lastHost(for:)` 也是直接比 `sourceIP == row.ip`，
  于是本机那一行永远没有这一行（Qt 那边 `setLastHost` 是在已经应用过本机归属规则的
  聚合循环里写的，所以它有）。
- **详情窗的连接表**：同样的比法，本机的详情窗里「连接 (0)」，而实际有几百条。

判据抽成了 `DeviceStore.connectionBelongs(sourceIP:deviceIP:isLocalMachine:)`
放进 CoastKit（3 条测试：普通设备只认同 IP、本机认回环与 TUN、
**不是本机那一行不能借这条路认领别人的流量**、空源地址一律不算）。

### 这一轮核对过、无差异的

- **主题令牌逐条对**：accent / accentStrong / danger / card / nodeRowBg / nodeRowActive /
  三档文字色 / versionColor / inputBg / inputBorder / footerComboBg / switchTrackOff /
  hover / divider / radius 5 / sidebarWidth 170 / footerHeight 38 / inset 5 —— 全中。
  `metricBg` 多了 0.82 透明度（整窗玻璃化后的刻意调整，代码里写了理由）；
  `shell` 与 `scrollHandle` 在 macOS 上分别由玻璃材质与原生滚动条取代。
- 延迟测速：URL、`timeout=5000`、`generate_204` 目标、独立连接池、
  **9 秒的请求兜底**（Qt 用 `setTransferTimeout(9000)`，Swift 用
  `timeoutIntervalForRequest = 9`）—— 逐项相同。
- 切换节点：防重入、转圈态、`clearConnections` 开关、切完重拉列表，一致。
  （Qt 另有 6 秒兜底定时器；Swift 靠 8 秒的请求超时收口，都有界。）
- Qt 的详情窗连接表也是按设备的**局域网 IP** 过滤的，所以本机详情窗在 Qt 上同样看不到
  连接 —— Swift 这次做得比 Qt 好一点，如实记着（不是「对不齐」，是补了同一处的另一半）。

`swift test` 353 全绿；`i18n_check.py` 258/258；`settings_persist_check.py` 7/7。

---

## 续五十六（2026-08-01）订阅页与日志层的逐条核对 —— 未发现差异

这一轮把订阅页与日志这两块整个对了一遍，**没有找到需要改的地方**。如实记下核对范围，
免得下一轮重复走一遍：

### 订阅页

- 顶栏：「订阅」18 + 计数 10（底对齐、下内距 3）+ 间距 6 + 上/左/右内距 10，
  三颗按钮 `添加订阅 / 应用 / 更新全部`（只有最后一颗是主色），顺序与 Qt 相同；
- 订阅卡：高 108、左右内缩 10、卡内左 12 右 10 上下 8、行距 3；
  类型徽章 11px 半径 4 左右各 6 上下各 2；名字 14 右截断；
  URL 11 **中间省略**（Qt 是 `ElideMiddle`，头尾比中间有用）；
  元信息「N / M 节点 · 每 X 分钟」拼法逐字相同；
  五颗圆钮 `启用/停用 · 查看节点 · 编辑 · 更新 ·（撑开）· 删除(danger)` 顺序与 Qt 一致；
- 节点弹窗 620×460：标题 16 + 计数 11（下内距 2）、空态「暂无节点，请先点击「更新」」13
  居中且上距 30、行高 46 半径 4、启用行用 `nodeRowActive`（那个蓝）、
  左 10 右 6、名字 12 / `server:port` 10、右侧按钮 76×30、底部三颗按钮。
  **截图确认**：主窗缩到最小（内容 430 高）时这个 620×460 的弹窗**完整显示、没有被裁**
  —— Qt 那边写的是 `min(620, page.width-40) × min(460, page.height-40)`，
  那是它的 in-window `Popup` 必须自己让位；macOS 的 sheet 可以超出父窗，不需要跟。

### 日志

- 严重级判定（错误 > 警告 > 成功 > 信息，英文查小写副本、中文查原串）逐字相同；
- 四个圆点色 `#f56c6c / #e6a23c / #67c23a / #4898f8` 相同，且都**不跟随深浅主题**；
- 时间戳格式 `yyyy-MM-dd HH:mm:ss` 相同（Swift 锁 `en_US_POSIX` + 公历）；
- 最新置顶、新日志把视图拉回顶部、空态「暂无日志」13 居中 —— 都有；
- 路由：核心日志进「主日志 + Clash 内核」两条，应用日志只进主日志 —— 与 Qt 相同。

### 一处**刻意不跟**（新记）

日志条数上限：Qt 是 **100** 条（`kMaxRows`），Swift 是 **2000**。
不跟的理由：日志页就是用来回看的，100 条在核心话多的时候几秒就冲干净了；
2000 条短字符串的内存代价可以忽略。这不影响任何元素的位置或尺寸。

### 另外核对过的

模式下拉：档位（规则/全局/直连）与回显映射相同；Swift 存的是**规范值**
（`Rule/Global/Direct`，`setMode` 进来先归一），比 Qt 存显示串更稳 ——
Qt 得在 QML 里同时认中英文两套串才能正确回显。

---

## 续五十七（2026-08-01）卡片背景那张图：少了整套刻度，线也画粗了

对 `MetricCard.qml` + `BandwidthChart.qml`。卡片本身（170 高、顶部 64 的带子、
左 14 右 10 间距 12、图标 28、标题 13、数值 24、半径 4）逐项对上；
**背景那张曲线图差了四处**：

| | Qt（minimal） | Swift（改前） |
|---|---|---|
| 线宽 | 2 | 3 |
| 线透明度 | 0.55 | 0.70 |
| 线下填充 | 实色 `rgba(线色, 0.16)` | 渐变 0.22 → 透明 |
| 刻度 | **四条 + 9px 速率标注** | **完全没有** |

刻度这件事以前**记反了**：Qt 的 `minimal` 属性注释写着「右侧刻度、左上标题全是噪音，
minimal 时一概不画」，而它的**代码**在 minimal 分支里另画了一套 —— 四条按**曲线实际高度**
（不是整高四等分）定位的淡线（`rgba(线色, 0.13)`）+ 贴右边缘、坐在线上方 3 的 9px 标注
（`#9a9a9a` / `#7a7a7a`），并且顶到卡片标题那一带（y < 18）的那一档跳过不画。
代码旁边还专门写着「等分线会全部对不上曲线，刻度就成了骗人的」。
注释与代码不一致时以代码为准 —— Swift 这边照着那句**注释**写了「不画刻度」，
于是卡片背景上那条线一直没有任何纵向参照，看得出在动、看不出跑到多少。

四处已全部对齐，绘制顺序也照 Qt：刻度线 → 填充 → 曲线 → 刻度文字
（文字最后画，否则会被填充糊掉）。顺带把 `TrafficCard` 里那句记反了的注释改正。

截图确认：上传/下载两张卡的背景上出现了 128/96/64/32 KB/s 四档淡刻度，
线色分别是红/绿的 13% —— 与 Qt 的画法一致。

`swift test` 353 全绿；`i18n_check.py` 258/258；`settings_persist_check.py` 7/7。

---

## 续五十八（2026-08-01）独立成图那一路的网格、刻度与标题也都不对

上一轮修了 `BandwidthChart` 的 **minimal**（卡片底纹）那一路。这一轮对**非 minimal**
那一路 —— 设备详情窗那张 90 高的图用的就是它，五处全不一样：

| | Qt | Swift（改前） |
|---|---|---|
| 网格线条数 | **5 条**（`gi = 0…4`，含顶边与底边） | 4 条（漏了底边） |
| 网格线颜色 | `rgba(线色, 0.10)` | `divider` 的 50% |
| 刻度字号/色 | 10px `#969696` | 9px `textMuted` 的 70% |
| 刻度基线 | `H/4*li + 12` | `y + 1`（贴着网格线） |
| 标题 | 11px、线色 70%、基线 `(10, 18)` | 10px、`textMuted`、内距 4 |

一并补了个 `topForBaseline(_:size:)`：Canvas 的 `fillText` 给的是**基线**，
而 SwiftUI 的 `offset` 定的是**顶边** —— 直接把基线值写进 offset 会整体偏低一整行。
系统字体的 ascent 约等于字号，拿字号当近似足够（上一轮 minimal 刻度那处也换成了它，
原来是随手写的 `-6`）。

截图确认设备详情窗：五条网格线（含底边）、四个 10px 右对齐刻度、
标题「下载」是 11px 的绿色 70% 且坐在左 10 / 基线 18 上。

`swift test` 353 全绿；`i18n_check.py` 258/258；`settings_persist_check.py` 7/7。

---

## 续五十九（2026-08-01）横轴少了一截：图上塞的是 60 秒，Qt 是 40 秒

继续对 `BandwidthChart`。量程（`128KB 基准 / 2MB 步进`）与滚动相位（50ms 一帧、
按**真实经过时间**算相位而不是固定步长）都与 Qt 相同。差在**点数**：

- QML：`maxPointer = 42`（40 个可见 + 2 个富余，两端各一个，滑动时右侧不留缺口），
  且 `Component.onCompleted` 就把 42 个点**预先填满**（值 1.0）——
  曲线一上来就贴着底边铺满整宽。
- Swift：状态页喂 **60** 拍进来，同样的宽度里塞了 60 秒 —— 一次流量尖峰看起来比 Qt 窄一截；
  设备那边是 40（少两个富余点，横轴跨度也差一点）；而且**不预填**，
  刚启动那几十秒曲线是从左边一小截慢慢长出来的。

已统一：`BandwidthChart.pointCount = 42` 作为唯一的口径，组件自己把不足的部分
在**左侧**补 1.0（与 Qt 的预填充同效）；状态页的采样窗口与 `DeviceTraffic.historyLength`
都改用它。2 条测试（值必须是 42、超限丢最旧且长度封死）。

截图确认：核心没跑、只有几拍采样时，曲线已经贴着底边**铺满整宽**，而不是左边一小截。

`swift test` 355 全绿；`i18n_check.py` 258/258；`settings_persist_check.py` 7/7。

---

## 续六十（2026-08-01）页脚开关的文字色、节点行的省略方向

把三个共用组件对完（`Card` / `FooterSwitch` / `NodeRow`）。

### 1. 页脚开关：关掉时文字不该变灰

Qt 的 `FooterSwitch` 里标签**恒为 `Theme.textPrimary`**，状态只由左边那颗呼吸圆点表达。
Swift 写的是 `isOn ? textPrimary : textMuted` —— 而同一段代码的注释里还写着
「Qt 也只靠圆点区分」。两处表达同一件事，浅色主题下的 `#999999` 只是让标签更难读。
已改成恒亮（截图确认三颗开关的文字现在一样深）。

其余逐项对上：圆点 12×12 + 3px 外环、启用时外环在蓝↔灰之间 1 秒一循环脉动、
间距 6、左内距 8、高 28、标签 12px 封顶 80 后省略。
（玻璃胶囊替代 Qt 的 `Theme.card` 半径 3 底色，是早就记过的 macOS 化。）

### 2. 节点行：省略方向反了

Qt 是 `elide: Text.ElideRight`（保住名字的头，丢掉「→ 叶子」那一段）；
Swift 用的是**中间省略**。中间省略能同时保住两端，看着更划算，但那是另一种取舍 ——
这里按 Qt 来，并把取舍写在注释里。

其余对上：行高 40 半径 4、左内距 8、间距 8、名字 12px `textSecondary`、
药丸半径 5（左右各 5、上下各 3）且字色写死 `#222222`、
右侧按钮定宽 82 撑满行高、悬停才有底色、切换在途时非目标行压到 0.35 透明且整行不可点。

### 一处**平台替换**（补记）

切换在途时，Qt 在按钮位置画的是 icon-font 的四帧转圈（120ms 一帧，
`bridge.spinnerGlyph`），Swift 用的是原生 `ProgressView(.small)`。
同一个位置、同一个语义，字形换成了系统控件 —— 与图标整体换 SF Symbols 是同一类替换。

### `Card`

Qt 就是「`Theme.card` 底 + `Theme.radius` 圆角」两行，Swift 同。

`swift test` 355 全绿；`i18n_check.py` 258/258；`settings_persist_check.py` 7/7。

## 同机基线：Swift 端 vs Qt 端（2026-08-04）

在**同一台机器、同一口径**下量的，两条线都本地构建（本机 macOS 26.5.2 / arm64 / 8 核 / 16 GB，
Qt 6.11.1 homebrew arm64 原生、Swift 6.3.3）。核心、配置、负载完全相同。

| 指标 | Qt 端 | Swift 端 |
|---|---|---|
| 可执行文件 | 28.0 MB | **6.9 MB**（1/4） |
| 源码行数 | 45783 | **21109** |
| 线程数 | 20 | **9** |
| **空载 App CPU** | 0.3~0.6% | **0.50%（修复后，n=20 中位；修复前 1.70%）** |
| 负载 App CPU | 1.4~1.7% | 1.1~2.8% |
| App RSS | 222 MB | **139 MB**（省 37%） |
| 负载 核心 CPU | 0.2~0.5% | 0.2~1.2% |

**结论**：Swift 端在**体积、内存、线程数**上明显更省（这几项是结构性的）。
空载 CPU 起初量到比 Qt 端高一个量级，查下来是 `startBandwidthSampling` 那条 1Hz 循环
**无条件写 @Observable 属性**导致的全树重排（详见下节），修掉后与 Qt 端同档。
负载下两者本来就接近，说明差距不在数据通路而在 UI 空转。

### CPU 分档：按**窗口可见性**，不按流量高低

产品口径（用户拍板）：**窗口打开时优先流畅**，收进托盘时才激进省电。

`AppState.startBandwidthSampling()` 的 1Hz 循环已经按这个分档：
`guard self.uiVisible else { sleep(1); continue }` —— 窗口关掉/最小化时整拍跳过；
窗口开着就**无条件**走完这一拍，不做任何"流量为 0 就跳过"的省电判断。

真机实测（macOS 26.5.2 / arm64，COAST_NO_AUTOSTART=1，n=15）：

| 状态 | CPU 中位 | 均值 | p90 |
|---|---|---|---|
| 窗口打开（每秒刷新） | 1.80% | 2.05% | 3.50% |
| 收进托盘（整拍跳过） | **0.30%** | **0.29%** | 0.40% |

托盘态省 83%，开窗态保持流畅。

### 开窗这一档的 CPU 差距：Swift 1.80% vs Qt 0.60%（3 倍），已定位到"哪条路"但还没降下来

同机、同方法（`top -l 2`，n=15~20）、同条件（`COAST_NO_AUTOSTART=1`、窗口可见）：

| 开窗 CPU | Qt 端 | Swift 端 |
|---|---|---|
| 中位 | **0.60%** | 1.80% |
| 均值 | 0.89% | 2.05% |
| p90 | 1.60% | 3.50% |

**决定性实验**：把 `startBandwidthSampling()` 整条 1Hz 循环停掉，Swift 掉到
**中位 0.60% —— 正好等于 Qt**。所以这 1.1~1.2% 全部来自这条循环。

**但逐项排除都不成立**（每项都是停掉后单独量，n=15）：

| 停掉什么 | 中位 | 结论 |
|---|---|---|
| 基线（什么都不停） | 1.70~1.80% | — |
| `pollTick &+= 1` | 1.50% | 只省 0.3%，不是主因 |
| `bandwidthSamples` 追加 | 1.80% | 没变化 |
| `syncUIVisibility()` | 2.00% | 没变化（在噪声内） |
| **整条循环** | **0.60%** | 全部开销在这里 |

→ **成本是分散的**：不是某一个属性特别贵，而是"这一拍写了若干 `@Observable` 属性"
这件事本身触发的 SwiftUI 重排就是主体。`sample` 的热点也一致 —— 100% 落在布局引擎
（`LayoutEngineBox.sizeThatFits` / `StackLayout.placeChildren` / `ViewLayoutEngine.sizeThatFits`），
没有任何业务代码上榜。

**继续排除（第二轮，n=15）**：

| 停掉什么 | 中位 | 结论 |
|---|---|---|
| 基线 | 1.80% | — |
| 拆分 `bandwidthUp/Down`（消除视图里的 map） | 1.90% | 无改善 |
| `pollNodes()`（另一条 1s 循环，70 个节点） | 1.90% | 无改善 |
| 切换到不读这些属性的页面 | 1.75% | 与页面无关 |
| **循环体全空、只留 1Hz 唤醒** | **0.50%** | ← 关键 |
| 整条循环停 | 0.60% | 同上 |

**这解开了之前所有矛盾**：循环体全空 = 0.50%，与"整条循环停"几乎一样，
说明成本**确实在循环体内**；而单独停任何一项都只省 ≤0.3%，全停却省 1.2% ——
**是各项累加的，没有单点热点**。所以"找出那个贵的属性然后省掉它"这个思路从一开始就不成立，
这也是前两轮反复扑空的原因。

**第三轮：找到了成本的真正性质 —— 是"重排面积"，不是"某个属性"**

决定性实验：**改变窗口大小**（同一二进制、同一循环，只改窗口尺寸，n=12）

| 窗口 | 中位 CPU |
|---|---|
| 420 x 320 | **0.90%** |
| 默认 | 1.70% |
| 1600 x 1000 | **1.85%** |

CPU 随窗口面积走，2 倍差 —— 这说明每拍卷进去的是**真实的布局工作量**，
而"哪个属性触发了它"完全不重要：一拍之内只发生**一次**重排，停掉任何单个属性，
其余属性照样写、重排照样发生，所以省不到东西。**这一条解释了前三轮所有的扑空。**

**这一轮试过且无效（已全部回退，不留无效改动）**：

| 改动 | 中位 | 结论 |
|---|---|---|
| 基线 | 1.80% | — |
| 刻度线 `ForEach`+`Path` → 单个 `Canvas` | 1.95% | 无改善 |
| 刻度线+刻度文字都改 `Canvas` | 2.05% | 无改善 |

即便把图内的子视图节点合并成叶子绘制节点，也没省下来 —— 说明重排的主体**不在这张图内部**，
而在更外层（卡片/页面/窗口这条链）。

**第四轮：根因锁定 —— 每拍变化的参数让整张卡片子树失效，成本是它的 layout pass**

沿着"成本 ∝ 窗口面积"继续切，这次用**整块移除**而不是逐项停（前者才有效）：

| 配置 | 中位 CPU |
|---|---|
| 基线 | 1.80% |
| 移除 `BandwidthChart`（卡片外壳保留） | 1.55% |
| **卡片内部清空、只剩外壳**（`Color.clear` + frame/background/clipShape） | **1.85%** |
| **移除两张 `TrafficCard`** | **0.70%** |
| **卡片结构不动，只把 `samples`/`tick`/`value` 换成常量** | **0.60%** |
| 空循环（理论下限） | 0.50% |

**最后一行是决定性的**：视图结构一个字没改，仅仅让传进去的参数不再每拍变化，
CPU 就从 1.80% 掉到 0.60%。而"内部清空但参数照旧变"仍是 1.85%。

⇒ **成本不是"画了什么"，而是"这棵子树被判失效"**：`TrafficCard` 只要收到新的
`samples`/`tick`/`value`，SwiftUI 就让整张卡片（含 `frame(height:170)`、`background`、
`clipShape`）重新走一遍 layout pass。图表内部怎么画、字体是什么、文字变不变，全都无关紧要
（逐项试过：换系统字体 1.85%、value 固定 2.00%、刻度线/文字改 Canvas 1.95%/2.05%，
全部无改善 —— 因为只要参数在变，这一遍 layout pass 就省不掉）。

这也解释了 Qt 端为什么只要 0.60%：QML 的 `Canvas` 重绘不参与布局，
数据变化不会让父链失效。**这是 3 倍差距的结构性来源。**

**已修复：把每拍变化的读取下沉到叶子子视图（开窗 CPU -53%）**

`TrafficCard` 原先从父视图接收 `value` / `samples` / `tick` 三个每拍都变的参数，
于是整棵卡片子树每拍失效。改法是**让卡片不再接收这些参数**：

- 卡片只收静态参数（`glyph`/`title`/`accent`/`lineColor`）+ 一个 `isUp: Bool`；
- 变化的读取放进两个 `private` 叶子子视图：`RateText` 读 `state.upText/downText`，
  `CurveLayer` 读 `state.bandwidthUp/Down` 与 `state.pollTick`。
  `@Observable` 的依赖是**按 View 的 body 记录**的，读取落在哪个 body 里，
  失效就只到哪一层 —— 卡片外壳（`frame`/`background`/`clipShape`）从此不再参与。

实测（n=20，同机同法）：

| 配置 | 中位 CPU |
|---|---|
| 改前 | 1.80% |
| **改后** | **0.85%（-53%）** |
| 参数换常量（理论最好） | 0.60% |
| 空循环（下限） | 0.50% |
| 对照：Qt 端 | 0.60% |

回归验证三段都正常（静止低 → 有流量时出现 2.1%/3.6% 的刷新峰值 → 停止后回落），
数据映射逐项核对过：上行卡 → `upText`/`bandwidthUp`，下行卡 → `downText`/`bandwidthDown`，语义无变化。

**同一手法的第二处：`ConnectionsCard` 也在从父视图收变化数据（负载 p90 -79%）**

修完 `TrafficCard` 后做负载对比，发现 Swift 端**负载下有严重 CPU 尖峰**：
n=20 时中位 2.00%、均值 6.15%、**p90 19.20%**，而 Qt 端 p90 只有 2.50%。
（注意：小样本 n=8 时曾量到"负载下 Swift 反而更省"，是假象，n=20 才现原形。）

`sample` 显示热点仍在布局引擎。查下来是状态页 body 里的
`private var recentRows: [ConnectionRow] { ConnectionRow.recent(state.connections, 5) }` ——
读 `state.connections` 落在**状态页自己的 body** 里，负载下连接列表每拍都变，
于是整张状态页失效：两张流量卡、延迟卡、页面外壳全被拉进 layout pass。
`ConnectionsCard` 明明已有 `@Environment(AppState.self)`，却仍从父视图接收算好的 `recent`。

改法与 `TrafficCard` 完全相同：把 `recent` 改成 `ConnectionsCard` 内部的计算属性，
父视图不再读 `state.connections`。

| 负载下 App CPU（n=20） | 改前 | 改后 | Qt 端 |
|---|---|---|---|
| 中位 | 2.00% | **1.00%** | 2.00% |
| 均值 | 6.15% | **1.44%**（-77%） | 2.11% |
| p90 | 19.20% | **4.10%**（-79%） | 2.50% |

回归：连接正常建立（核心侧连接数一致）、App 日志零错误。

### 系统排查：其余同类位置**实测不构成开销**，故未改动

按上面的通用规则把所有高频属性（`connections` / `composition` / `deviceTraffic` /
`logs` / `coreLogs` / `todayHourly` / `todayTop` / `securityAlerts` / `pollTick` /
`bandwidthUp/Down`）的读取点全查了一遍，形式上仍有两处"父视图算好再传下去"：

- `Pages.swift` 日志页：`LogTimeline(entries: tab == 0 ? state.logs : state.coreLogs)`
- `DevicesPage.swift` 设备行：`DeviceRow(sample: state.deviceTraffic.sample(ip:), tick: state.pollTick, …)`

**但逐页实测（n=15）各页 CPU 都在 0.80~0.90%，没有哪一页更贵**：

| 页面 | 中位 | 均值 | p90 |
|---|---|---|---|
| 状态页 | 0.90% | 1.31% | 4.10% |
| 第 2 页 | 0.80% | 1.52% | 4.20% |
| 第 3 页 | 0.90% | 0.99% | 1.40% |
| 第 4 页 | 0.80% | 0.87% | 1.10% |
| 第 6 页 | 0.90% | 1.04% | 1.40% |

原因：本机设备库有 21 台设备但**已代理 0 台**，`deviceTraffic.sample` 恒空、设备行不随拍变化；
日志空载时也不产生新条目。**形式上的反模式 ≠ 实际开销** —— 没有证据就不改，
避免为"看起来不对"付出回归风险。

★ 留给将来：**真正接管了设备之后**（设备行每拍都有新样本）这两处才会显形，
届时按同一手法下沉即可（`DeviceRow` 自己读 `state.deviceTraffic` / `state.pollTick`）。

### Qt 端也查过：`ConnectionsModel` 那处 `dataChanged` 不传 roles 是**已知且可接受**

`ConnectionsModel.cpp` 的 Step B 里 `emit dataChanged(index(i), index(i))` 不传 roles
（= 通知"所有角色都变了"），但它外面裹着 `if (!m_rows.at(i).sameFields(nw))` ——
**只在字段真变化时才发**，且连接窗默认不开、状态页只显示最近 5 条。同样不构成实测开销。

★ **通用规则**：`@Observable` 的依赖按 View 的 body 记录。**凡是变化的读取，都要落在真正
用得到它的那一层**；在父视图里算好再当参数传下去，等于把整棵父子树都绑上这个依赖。
排查手法：看某个 View 的 body 里有没有读会频繁变化的 state —— 有就往下沉。

**仍留的 0.25% 差距**（0.85% vs 0.60%）：叶子层自身的失效与重绘，属于合理开销；
若还要压，得让曲线彻底脱离 SwiftUI 的 layout（`NSViewRepresentable` + CALayer 自更新）。

★ 方法论：**逐项停无效、整块移除才有效**。原因是一拍之内只发生一次 layout pass，
停掉单项时其余参数照变，pass 照走。要定位这类成本，必须用"整块移除 / 参数换常量"
这种能真正切断失效链的手法。

★ 排除法本身的价值：先前两轮都在"猜哪个属性贵"上打转。正确做法是**先用整条循环的
开关确认总量**（0.60% vs 1.70%），再逐项做减法 —— 一旦发现逐项之和远小于总量，
就该立刻改变思路（是分散成本，不是单点热点），而不是继续逐个试。

★ **走过的弯路，别再走一遍**：曾加过一版"上下行都是 0 就跳过整拍"想在开窗时也省电，
实测界面**一卡一卡** —— 省下的那点 CPU 远不值这个观感代价，已撤掉。
分档的正确位置是**窗口可见性**，不是流量高低。要在"窗口开着"这一档继续降 CPU，
方向是让**单次刷新更便宜**（少写被观察属性、缩小重排范围），而不是降低刷新频率。

★ 那一版还带出一个更隐蔽的错：判据用 `clash.up/down`，而真机实测核心 `/traffic` 的
**瞬时字段在某些路径上恒为 0**（本机回环经代理拉 1.5 GB，`up`/`down` 全程 0，
只有 `upTotal`/`downTotal` 在涨）。所以哪怕真要判"有没有流量"，也**不能只信瞬时速率**。

★ 测可见性分档时注意：用 `osascript` 隐藏**进程**不会让 `window.isVisible` 变假，
量不出差别（我误判过一次）。必须真正**关闭窗口**才会触发 `uiVisible=false`。

### ★ 量 CPU 的坑：`top -l 1` 恒返回 0.0，会让你以为"完全不烧 CPU"

macOS 的 `top` 要**两次采样做差**才有 CPU 百分比，`-l 1` 只采一次，那一列恒为 0.0。
拿一个烧满单核的 `yes` 进程做基准验证过：

    top -l 1  ->   0.0    ← 完全测不到
    top -l 2  ->  76.4
    ps  %cpu  -> 100.0

所以：**瞬时 CPU 必须用 `top -l 2 ... | tail -1`**。
`ps -o %cpu` 也能用，但它是**进程生命周期均值**，跑得久会被稀释，不适合看当下负载。
本文件上一版写的"两条线空载 CPU 都是 0.0%、CPU 持平"就是被 `-l 1` 骗的，已按正确方法重测更正。

### 另一个坑：别跨机比

先前把 Qt 端跑在 .34（Intel i7-7567U / 4 核 / load 1.5）、Swift 端跑在本机（arm64 / 8 核），
得出的任何差异都是机器差异。本机装上 Qt 6.11.1 后重测才有意义。

## 两条线的 macOS 网关：架构对比（2026-08-04 核对）

一度以为 Swift 端**没有**网关数据面（`Sources/CoastKit/` 下只有 `ArpWatch`/`LanBrowser`/
`LanTopology`，没有 `ArpSpoofer`/`PfRules`/`LanGateway` 这些 Qt 端的同名文件）。
**是误判** —— Swift 端的网关整个在 helper 里：`Sources/CoastHelper/Redirector.swift`（864 行），
做三件事：`ip.forwarding=1`、装 PF anchor、周期发 ARP 欺骗应答，抓包走 **BPF**（`openBPF`）。

| | Qt 端 | Swift 端 |
|---|---|---|
| 数据面 | pf `rdr-to` + `/dev/pf` 的 `DIOCNATLOOK` 取原始目的 | pf anchor + BPF |
| 欺骗循环跑在 | app 进程（BPF fd 传回 app） | **helper 进程** |
| 崩溃还原 | `GatewayPanic`（SIGSEGV/BUS/ILL/FPE/ABRT）+ 下次启动 `healPending()` 自愈 | helper 的 XPC `invalidationHandler` |
| 代码位置 | `src/net/`（多文件） | `CoastHelper/Redirector.swift`（单文件） |

**关于 Swift 注释里那句对 Qt 的批评**（"像 Qt 版那样把 BPF fd 传回 app，app 被 SIGKILL 时
没有任何人来发复原包，设备就那么挂着"）—— 对**当前**的 Qt 端**已不成立**：
- `GatewayPanic` 覆盖 SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT（**SIGKILL 捕获不到，这是
  操作系统限制，任何方案都一样**）；
- SIGKILL 那个缺口由**下次启动时的 `healPending()`** 补上：把上一世还欠着复原的设备
  记在案，起来后先发复原帧（且会跳过"现在又被正常劫持着"的设备，避免把刚投的毒解掉）。

Swift 端把欺骗循环放 helper 的设计**确实更优**（XPC 连接一断就能立刻复原，不必等下次启动），
但两条线都不存在"设备永久挂着"的问题。

★ 记这一条是因为：这类跨线的架构批评注释很容易**过期**（写的时候对，对面修好了就不对了），
读到时要**先核对当前代码**再采信 —— 我这次就差点照单全收。

## 启动耗时与内存稳定性（2026-08-04 本机实测）

两项此前没量过的软件性能指标，同机、同法、两条线都本地构建。

### 启动到出窗（`COAST_NO_AUTOSTART=1`，只算 UI 起来，各 5 次）

| | Qt 端 | Swift 端 |
|---|---|---|
| 中位 | 1.26 s | **0.56 s** |
| 均值 | 1.29 s | **0.56 s** |
| 各次 | 1.24 / 1.24 / 1.26 / 1.26 / 1.45 | 0.55 / 0.56 / 0.56 / 0.56 / 0.59 |

**Swift 端快 2.3 倍**，且方差极小（±0.02s vs ±0.1s）。
量法：`osascript` 轮询进程窗口数，从 exec 到第一个窗口出现。

### 内存稳定性（带持续代理流量跑 4 分钟）

| | Qt 端 | Swift 端 |
|---|---|---|
| 起始 RSS | 231 MB | 140 MB |
| T+4min | 230 MB | 137 MB |
| 漂移 | 0 MB（中途 +4 后回落） | -3 MB |

**两条线都没有泄漏**，长时间带流量运行 RSS 稳定。Swift 端稳态少 93 MB（-40%）。

### 汇总：两条线的完整对比（截至本轮）

| 指标 | Qt 端 | Swift 端 | 优势方 |
|---|---|---|---|
| 可执行文件 | 28.0 MB | 6.9 MB | Swift 4.1× |
| 源码行数 | 45783 | 21109 | Swift |
| 线程数 | 20 | 9 | Swift |
| 启动到出窗 | 1.26 s | 0.56 s | **Swift 2.3×** |
| 稳态 RSS | 230 MB | 137 MB | **Swift -40%** |
| 开窗空载 CPU | 0.60% | 0.85% | Qt（差距已从 3× 收敛到 1.4×） |
| 托盘态 CPU | — | 0.30% | — |
| 负载 App CPU（中位/p90） | 2.00% / 2.50% | 1.00% / 4.10% | 中位 Swift 优，p90 Qt 稳 |
| 吞吐（回环经代理） | 18.9 Gb/s | 17.8 Gb/s | 相当，均远超万兆 |
| 核心 CPU（负载） | 0.40% | 0.45% | 相当 |
| 内存泄漏 | 无 | 无 | 相当 |

## `/proxies` 轮询按页分档（非节点页 0.85% → 0.55%）

`sample` 在负载下抓到的**非布局类热点只有一个**：`newJSONValue` / `newJSONObject`。
查下来是 `pollNodes()` —— 它每 **1 秒**拉一次 `/proxies` 并整份 `JSONSerialization`
解析成 `[String: [String: Any]]`（每字段装箱成 `Any`）。本机 70 个节点时该响应实测 **51 KB**。

各轮询接口的真实体量（负载下实测）：

| 接口 | 体量 | 频率 |
|---|---|---|
| `/proxies` | **51 KB** | 1s |
| `/connections` | 0 KB | 2s |
| `/configs` | 1 KB | — |

`/connections` 之所以是 0：回环传输太快，2s 采样窗口里连接早已结束
（`downloadTotal` 有 200 MB，证明流量确实过了核心）。所以**大 JSON 的唯一来源是 `/proxies`**。

改法不是全局降频（那会牺牲节点状态的实时手感），而是**按当前页面分档**：
`ClashService.nodesVisible` 由 `AppState.syncUIVisibility()` 每拍同步
（`窗口可见 && currentPage == .nodes`），看着节点页就 1s、否则 5s。
与 `uiActive` 的分工：那个管「窗口开没开」，这个管「开着时在看哪一页」。

实测（n=18，窗口停在状态页）：

| | 中位 |
|---|---|
| 改前（恒 1s） | 0.85% |
| 全局改 5s（仅作对照，未采纳） | 0.60% |
| **本次分档后（状态页）** | **0.55%** |

**两侧都已验证**（补齐了上一版欠的那一半）。

先要解决"没法在脚本里切页"：SwiftUI 的侧栏**不暴露成可访问按钮**
（`every button of window 1` 只拿得到三个无名的窗口控制按钮），而两条线原本**都没有**
切页快捷键。于是给 Swift 端补了 **⌘1..⌘7**（macOS 上这本来就是基本预期，
Finder/Xcode/浏览器都有），既补功能也让验证成为可能。

验证判据用**网络字节数**而不是 CPU —— CPU 差异只有 0.1~0.2%，落在噪声里，
而字节数是硬的（`nettop` 数 App 的入站流量，20 秒窗口）：

| 页面 | 20 秒入站 | 预期 | 判定 |
|---|---|---|---|
| 状态页（5s 档） | **187 KB** | ~200 KB（4 次 × 51KB） | 吻合 |
| 节点页（1s 档） | **1503 KB** | ~1000 KB（20 次 × 51KB） | 约 8 倍差，吻合 |

★ 教训：**验证一个"频率"改动，就该去数频率本身**（请求数/字节数），
别绕道去量 CPU —— 上一版正是因为拿 CPU 当判据，在噪声里得出过一组无效样本。

## 今日流量重算也按页分档（离开状态页 CPU -50%、p90 -66%）

排查 Swift 端空载剩下的**周期性尖峰**（p90 4.40%，Qt 只有 1.20%）：
连采 40 次标出尖峰，间隔 7/7/10/3 个采样 ≈ **10 秒周期**。
Swift 端的 10 秒任务只有两个：`LatencyMonitor` 与 `startTodayTrafficRefresh`，
后者要跑**几条 GROUP BY 聚合**，是主嫌。

对比发现两条线判据不一致：

| | 条件 |
|---|---|
| Qt（`setStatusActive`） | `m_statusActive && m_uiVisible` —— **窗口可见 且 在状态页** |
| Swift（改前） | 只判 `uiVisible` —— **不管在哪一页都跑** |

于是人在节点页/日志页时，那几条聚合照跑不误。按 Qt 的判据对齐，并补上
「切回状态页立刻补一拍」（否则用户会对着 10 秒前的旧数字）。

实测（n=20，两侧都验）：

| 页面 | 中位 | 均值 | p90 |
|---|---|---|---|
| 状态页（照跑） | 0.60% | 0.97% | 2.90% |
| 日志页（已停） | **0.30%** | **0.44%** | **1.00%** |

离开状态页 CPU 降 50%、p90 降 66%，状态页功能不受影响。

★ 这是**第三处**同一模式的问题（前两处：`/proxies` 轮询、`TrafficCard`/`ConnectionsCard`
的失效范围）。共性规律：**只给某一页看的周期性工作，判据里必须带"用户正停在那一页"**，
只判"窗口开着"是不够的。两条线互为镜子 —— 这次是 Qt 的判据更严、Swift 漏了一半，
上一次（`/proxies` 分档）反过来。**发现一条就该去对面查同名的那条。**

### Qt 端同一轮的收益

`/proxies` 按页分档后（上一轮改动）实测：

| Qt 开窗空载 | 改前（恒 1s） | 改后 |
|---|---|---|
| 中位 | 0.60% | **0.45%** |
| p90 | 1.60% | **0.90%** |

## 双向系统排查：`LatencyMonitor` 是第四处同名镜像差异

按上一节提炼的规律，把两条线**所有**周期任务列出来逐条核对守卫条件。
Swift 端 18 处周期任务里，"无守卫"的经查多数是有意为之或守卫在别处：

| 位置 | 周期 | 判定 |
|---|---|---|
| `AppState:585` ArpWatch | 30s | 注释明说**接管没开也要跑**（"有人在抢网关"与我们开没开代理无关） |
| `AppState:681` 代理地址跟随 | 5s | **只在真有设备被代理时才跑**，空载零开销 |
| `AppState:854` | 1s | 守卫在上方（`guard uiVisible`） |
| **`LatencyMonitor:31`** | **10s** | **← 真问题** |

`LatencyCard` 只出现在状态页（`Pages.swift:64`），但 Swift 端把 `latency.start()/stop()`
挂在**窗口显隐**上，于是人在节点页/日志页时仍每 10 秒探测一轮。
Qt 端是对的：`StatusPage.qml` 里 `latency.setActive(page.visible)` —— **按页**。
这是第四处同名镜像差异，方向仍是 Swift 漏了一半。

改为按页控制后（n=20）：

| 页面 | 中位 | 均值 | p90 |
|---|---|---|---|
| 状态页 | 0.50% | 0.80% | 2.30% |
| 日志页 | **0.20%** | **0.36%** | **1.00%** |
| 切回状态页 | 0.40% | 0.96% | 3.80% |

★ **CPU 下降不等于优化，也可能是功能坏了** —— 所以补了功能性验证：
在 `probe()` 处打点数**实际探测次数**，状态页 2 次 / 日志页 0 次 / 切回 2 次（25 秒窗口，
10s 周期），三段都对。

★ 打点差点给出**假的回归结论**：第一版用 `print`，stdout 重定向到文件时 Swift 的 `print`
**有缓冲、不刷盘**，三段读到的都是 0 次，看起来像"我把延迟卡整个停掉了"。
改用 `FileHandle.standardError.write` 直写才拿到真实数字。
**验证用的打点本身也会骗人**，读到"全 0"这种极端结果时先怀疑打点。

## 反方向排查：第五处镜像差异，这次是 **Qt 漏了 Swift 有的**

前四处都是 Swift 漏掉 Qt 已有的分档。这轮反过来查：把 Qt 端 25 个 `QTimer` 全列出来
逐条核对守卫。多数是对的，其中 `DevicesController::applyCadence` 尤其完善
（连接聚合 1s 停掉 / 在线态热更新只降频到 60s，并写清了「哪些不能停」的三条理由）。

真问题在 `QmlBridge::observeConnections`：它是**纯界面加工**（最近连接列表、设备归属、
流量构成），却**没有窗口可见性守卫**，托盘态下照样每 2 秒遍历全部连接。
Swift 端对应位置早就是对的 —— `AppState.onConnectionsSnapshot` 里先落历史、
再 `guard self.uiVisible` 才做界面加工，注释也写明了理由。按 Swift 的形状对齐。

实测（Qt，n=15）：

| 状态 | 中位 | 均值 |
|---|---|---|
| 窗口可见 | 0.40% | 0.47% |
| 托盘态 + 有流量 | **0.30%** | **0.31%** |

★ **关键是没有误伤历史库**：历史记录挂的是同一信号的**另一条**连接，任何时候都要落。
验证时发现"托盘态历史库新增 0 条"，本来要当成回归回滚 —— **做了对照实验**：
把改动回退、用改前的二进制跑同样流程，**同样是 0 条**。原因是回环连接寿命极短，
2 秒轮询根本抓不到（此前已实测 `/connections` 恒为空），与本改动无关。
**没有对照就会误判自己的改动**。

### 五处镜像差异汇总

| # | 位置 | 谁漏了 |
|---|---|---|
| 1 | `TrafficCard` 失效范围 | Swift |
| 2 | `ConnectionsCard` 失效范围 | Swift |
| 3 | 今日流量重算按页 | Swift |
| 4 | `LatencyMonitor` 按页 | Swift |
| 5 | **连接快照的界面加工按窗口可见性** | **Qt** |

规律已稳定：**两条线互为镜子，一边发现一处就去对面查同名的那条**；
方向不固定，两边都可能是漏的那个。

## 最终基线（全部优化落地后，2026-08-04）

同机、同法（`top -l 2`，n=18，`中位/p90`）、两条线都本地构建、都真起核心：

| 状态 | Qt 端 | Swift 端 |
|---|---|---|
| 状态页 | 0.45 / 0.80 | 0.50 / 3.90 |
| 日志页 | 0.40 / 0.50 | **0.25 / 1.10** |
| 托盘态 | 0.20 / 0.20 | **0.10 / 0.30** |
| RSS | 236 MB | **139 MB** |

三种状态下两条线都已收敛到 **0.1~0.5% 中位**。与最初相比：
Swift 开窗空载从 1.80% → 0.50%，负载 p90 从 19.20% → 4.10%；
Qt 开窗空载从 0.60% → 0.45%。

### 剩下的 Swift 状态页 p90 3.90%：**决定不再追**

连采 45 次标尖峰位置，间隔 **7 / 3 / 31 个采样 —— 不规律**，
不是固定周期任务（前几轮那种 10 秒周期的已全部修掉），
更像 SwiftUI 的偶发资源加载/调度抖动。

更重要的是**量级上无感**：
p90 3.90% ≈ 每秒 39 ms CPU 时间，分散在 60 帧里是**每帧 0.65 ms**
（一帧预算 16.7 ms）。与 Qt 的差值 3.1% ≈ 每帧 0.5 ms。**远不足以掉帧。**

★ 判断"还要不要继续优化"应当换算到**用户能感知的量纲**（帧预算、可见延迟），
而不是盯着百分比继续挖。前面几轮每一处都有明确的周期性根因和 30%+ 的收益，
这一处两者都不具备 —— 到此为止是正确的收手点。

### 设备在线态：两条线都已正确，无差异

Qt `DevicesController::applyCadence` 的 `kLivenessFastMs`/`kLivenessIdleMs` 双档，
Swift `AppState.deviceScanInterval` 按 `devicesPageVisible` 分档 —— 同一设计，均无遗漏。
镜像排查至此收敛。

## 功能回归扫描（这一系列优化全部落地后）

前面十余处改动集中在两个风险面：**页面分档**（可能让该刷的数据不刷）和
**失效范围下沉**（可能让界面不更新）。省电改动最危险的失败模式是
**"CPU 降了但功能坏了"** —— 所以专门做一轮针对性回归，而不是只看 CPU。

### 逐页巡检（两条线各 7 页）

| | 结果 |
|---|---|
| Swift 端 ⌘1..⌘7 | 七页全通，日志 `fatal/crash/assert` **0 行**，核心仍应答 `/version` |
| Qt 端 ⌘1..⌘7 | 七页全通，QML 错误/警告 **0 行** |

### 数据新鲜度（真正的风险面，用打点数**实际执行次数**）

「不崩」不等于「在刷新」。对每个分档项验证"该跑时确实在跑"：

| 分档项 | 该跑的场景 | 实测 | 不该跑的场景 | 实测 |
|---|---|---|---|---|
| Swift 今日流量重算（10s） | 状态页 | **2 次/25s** | 日志页 | **0 次** |
| ↑ 切回状态页 | 补一拍 + 恢复周期 | **3 次/25s**（多的一次正是补拍） | — | — |
| Qt 连接快照界面加工（2s） | 窗口可见 | **10 次/20s** | 托盘态 | **0 次** |
| Swift `LatencyMonitor`（10s） | 状态页 | 2 次/25s | 日志页 | 0 次 |
| Swift/Qt `/proxies` 轮询 | 节点页 1s | 20 次/20s | 其他页 5s | 5 次/20s |

全部符合预期。所有临时打点已移除并重新编译确认，工作区干净。

★ 打点一律用 **`FileHandle.standardError.write`（Swift）/ `qWarning`（Qt）**，
不用 `print` —— stdout 重定向到文件时 Swift 的 `print` 有缓冲不刷盘，
会读到全 0，看起来像"功能被改坏了"（前面吃过一次亏）。

## 真实代理性能（订阅恢复后首次测成，2026-08-04）

此前几轮测代理性能都因**订阅节点大面积不可用**而作废（一度 70 个里只有 4 个能用）。
这次订阅恢复：**52 / 52 全部可用**，最快 69ms，终于测成。

### 延迟（本机 → 核心 mixed-port → 节点 → cp.cloudflare.com，n=8）

| | 值 |
|---|---|
| 中位 | **94 ms** |
| min / max | 91 / 156 ms |

节点自身延迟 71ms（`/delay` 探测），叠加一次 HTTP 往返后 94ms —— **软件几乎不加延迟**。

### 吞吐（Cloudflare 50 MB 下载，各节点取两次峰值）

| 节点 | 峰值 | 核心 CPU |
|---|---|---|
| JP10-HY2 | **385 Mb/s** | 0.0% |
| Auto - JP | 324 Mb/s | 0.2% |
| TW-1 | 22 Mb/s | 0.0% |

★ **385 Mb/s 时核心 CPU 只有 0.0~0.2%** —— 瓶颈完全在节点侧和跨境链路，
与软件无关。对照本机回环（不出网）的 **17.8~18.9 Gb/s**：
同一个核心，数据通路本身有 50 倍的余量。

★ 一次自己造成的假数据：第一版脚本用
`curl -o /dev/null -w "    %{speed_download} B/s\n"` 再管道给 python 解析，
前导空格让解析拿到 0，**三次全读成 0 Mb/s**，看起来像"下载全失败"。
单独跑一次带完整字段的诊断才发现其实是 `code=200 / 50MB / 30.4MB/s`。
**读到整齐的 0 先怀疑解析**（与之前 `print` 缓冲那次同一类教训）。

## 网关数据面成本（macOS pf，本机实测）

端到端网关测试需要在本机做 ARP 投毒，会影响正在使用的网络（这台机器还跑着 Surge），
**本机不做**。可以安全测的是**数据面本身的成本** —— 用户点"代理这台设备"到规则生效的耗时。

### pf 自测

`COAST_PF_SELFTEST=1` → **PASS**（装载 + 挂载点核实 + 设备增删 + 幂等拆除）。
这个自测不产生真实流量、不接管任何设备，在用的机器上跑也安全。

### pfctl 各操作耗时（各 5 次）

| 操作 | 耗时 |
|---|---|
| 装载锚点规则 | 25~27 ms |
| 读回规则 | 28~30 ms |
| 清空锚点 | 23~26 ms |

固定成本 ~25ms，对"点开关到生效"完全无感。

### ★ 设备集合同步：与设备数**无关**

`PfRules::syncDevices` 用 pf table 的 `-T replace` **整体替换**，不是逐台增删。
实测证实了这个设计的价值：

| 设备数 | 同步耗时 |
|---|---|
| 10 台 | 26 ms |
| 50 台 | 25 ms |
| 200 台 | 24 ms |

**耗时与设备数完全无关**（表内条目数已核对：10/50/200 条都正确写入）。
这意味着网关侧的设备规模不构成成本 —— 200 台和 1 台一样快。
换成"逐台 pfctl 增删"的写法，200 台就是 200 × 25ms = 5 秒，差 200 倍。

## 两条线的 pf 规则组织方式不同，但**装载成本差距远小于预期**

镜像检查：Qt 端 `PfRules::syncDevices` 用 pf table 的 `-T replace`（规则恒 2 条，
设备进表）；**Swift 端 `Redirector.anchorRulesText()` 是逐设备展开**
（每台 2 条：TCP 全量 + UDP:53），设备越多规则越多。

我原以为这是个数量级差距，**实测否决了这个预期**：

| 设备数 | Swift 式（逐设备规则） | Qt 式（table） |
|---|---|---|
| 1 台 | 33 ms（2 条规则） | 23 ms |
| 10 台 | 27 ms（20 条） | 24 ms |
| 50 台 | 27 ms（100 条） | 24 ms |
| 200 台 | **34 ms**（400 条） | 25 ms |

**装载耗时几乎不随规则数增长** —— pfctl 的开销主要是进程启动 + ioctl，不是规则解析。
200 台也只比 1 台慢 1ms（34 vs 33），比 table 式慢约 9ms。

★ 所以**不改 Swift 端**：9ms 的一次性差距对"点开关到生效"无感，
而改动要动到 helper 里那段写得很细致的规则生成（`anchorRulesText` 附近有大段
真机实测得出的注释，例如"v6 不装 rdr 否则设备 IPv6 直接断网"、
"rdr pass 必须拆成 rdr + filter 两条否则本地监听 0 连接"）。
**收益 9ms、风险是碰一段有血泪注释的代码 —— 不值得。**

规则数理论上会影响每包匹配（pf 是线性匹配），但两条线的规则都装在
`com.apple/<自己的锚点>` 里，主规则集只有一条 `rdr-anchor "com.apple/*"`，
未命中锚点条件的包不会逐条走完 400 条。这一层没有实测，但也不构成改动理由。

### ★ 测试卫生：pf 被我的测试从 Disabled 变成了 Enabled

`pfctl -f` 装规则会顺带启用 pf。本机测试前 pf 是 **Disabled**，测完必须
`pfctl -d` 还原（只关 pf 本身，不动任何规则集，系统的 `com.apple` 锚点保持原样）。
锚点也要 `-F all` 清干净。**在别人正在用的机器上测网络组件，"还原到测试前状态"
和"测出结果"同等重要。**

## 第六处镜像差异：Swift 端缺"残留系统代理"自愈（已补，但本机只验证了一半）

之前给 Qt 端修过「上一次被强杀留下的系统代理永远擦不掉」——
`stopProxy()` 开头的 `guard systemProxyActive` 只看**本会话**，上一世的残留擦不掉，
用户表现为"什么都打不开"且无从得知原因。**Swift 端没有对应自愈**，本轮补上
（`CoastController.clearStaleSystemProxy()`，在 `init` 里、任何启动动作之前调用）。

### Qt 端的修复：本机实测**有效**

造残留（`networksetup -setwebproxy Wi-Fi 127.0.0.1 7890`，该端口无人监听）→
启动 Qt 端 → **`Enabled: Yes` 变成 `Enabled: No`**，web/secure 两条都被清掉。

### Swift 端：**本机构造不出可验证的场景**

两条线的判据有一处**有意的不同**：

| | 读取方式 | 取舍 |
|---|---|---|
| Qt | 遍历 `networksetup -listallnetworkservices` **每个**服务 | 能擦掉不活动服务上的残留 |
| Swift | `SCDynamicStoreCopyProxies` 读**当前生效**的代理 | 只碰真正影响上网的那份，误伤面更小 |

本机踩到了这个差异：在 Wi-Fi 服务上造残留，但**默认路由其实走 `utun4`**
（Surge 的 TUN），于是 `scutil --proxy` 是 `HTTPEnable: 0`，
Swift 判定"没有生效的代理"直接返回 —— **这是正确行为，不是漏修**。
（一开始我把它当成"修复没生效"，加打点才看清 `currentHTTPProxy()` 返回 nil 的真实原因。）

而本机的活动链路是 VPN 的 `utun4`，**没有对应的 `networksetup` 服务**，
所以无法在"当前生效链路"上构造残留 —— Swift 端这一半**本机验不了**。

**已验证的一半**：有人监听 7890 时**不动**系统代理（防误删），实测通过。
**未验证的一半**：真残留场景下能清除。逻辑与 Qt 端同构、且 Qt 端已实测有效，
但**不当作已验证** —— 待在一台系统代理走真实网口的机器上补测。

## 第七处镜像差异：Swift 端没有"收孤儿核心"，本会话开场那 9 个孤儿就是它留的

会话开场清理环境时，本机有 **9 个孤儿核心**（ppid 全是 1、Coast 没在跑、
分两批相隔 18 小时、共占 326 MB、0 个网络 fd）。当时给 **Qt 端**修了收孤儿逻辑
（`reapOrphanCore`，并把调用提到两条启动路径之前 —— macOS 走 helper root 那条会提前
return，原先放在 `m_core.start()` 边上等于从没执行过）。

这轮本机复验，两条线一起测：

| | 造 3 个孤儿后启动 | 结果 |
|---|---|---|
| Qt 端 | 只剩 1 个核心，`ppid` = App 自己 | **收掉了** |
| Swift 端（改前） | **4 个**：3 个 `ppid=1` 原样还在 + 自己新起的 1 个 | **没有任何收孤儿逻辑** |

所以开场那 9 个孤儿正是 Swift 端留下的。本轮补上 `CoreProcess.reapOrphanCores()`，
与 Qt 端同一设计：判据要求 ① 命令行含核心可执行文件绝对路径 ② `ppid == 1`，
并对自身 pid 自保；调用点放在 `start()` 里、**两条启动路径之前**（helper root 那条会
提前 return，这个坑 Qt 端踩过一次）。

验证三项全过：

| 检查 | 结果 |
|---|---|
| 3 个孤儿 | **全部收掉** |
| 剩余核心的 ppid | = App 自己（自保生效，没误杀刚起的） |
| 核心可用性 | `/version` 正常应答 v1.10.4392 |

★ 这一处的价值不只是省 326 MB 内存：孤儿占着 9191/7890 端口时，
**新核心绑不上端口却不退出** —— mihomo 只记一条 "address already in use" 然后照常运行，
于是"进程在跑、UI 显示正常、但一个端口都没监听"，代理完全不通且毫无提示。

## 第八处镜像差异：Swift 端没有单实例守卫（已补）

Qt 端的 `SingleInstance` 是之前修过的**正确性保护**（不是体验优化）。
Swift 端完全没有对应设施 —— 本机实测确认这是真缺陷：

**双实例实测（macOS 26.5.2，同一数据目录）**：

| 观察 | 结果 |
|---|---|
| 两个 App | 都活着 |
| 核心进程数 | **2 个**（各起一个） |
| 9191 端口 | 只有**先启动那个**的核心绑上 |
| 第二个核心 | 绑不上端口却**不退出**，日志里**一条冲突提示都没有** |

即"进程在跑、UI 一切正常、一个端口都没监听"，代理完全不通且毫无提示。
helper 侧更糟：`startRedirect` 会把 `redirectOwner` 覆盖成后启动的实例，
后者退出时 `clientVanished` 撤销接管 —— 而**第一个实例还活着、界面正常，
它的整个数据面已被撤销**（与 Qt 端当初那个真机复现同构）。

### 实现选择：文件锁，不是 `NSRunningApplication`

用 `flock` 而不是查同 bundle id 的运行实例 —— 后者对 `swift run` / 直接跑可执行文件
（**没有 bundle identity**）的进程无效，而开发和自动化测试恰恰都是那样跑的。
锁文件在用户数据目录，进程退出（**含被 SIGKILL**）时内核自动释放，不留残留。
拿到的 fd **故意不关**，靠进程退出回收。

验证三项全过：

| 检查 | 结果 |
|---|---|
| 第二个实例 | 被挡住并打印明确提示 |
| 第一个 `kill -9` 后 | 新实例能正常起（锁随进程释放，没泄漏） |
| Qt 端 | 不受影响（各自独立） |

### 顺带核实：崩溃还原 ARP，Swift 端**有**且设计更优

Qt 用 `GatewayPanic`（信号处理器，覆盖 SIGSEGV/BUS/ILL/FPE/ABRT，**捕获不到 SIGKILL**，
靠下次启动的 `healPending()` 补）；Swift 把欺骗循环放在 helper 里，
靠 XPC 的 `invalidationHandler` —— **app 被 SIGKILL 时连接照样断，立刻就能复原**，
不必等下次启动。这一处 Swift 更好，不需要对齐。

## 第九处镜像差异（**已修复并实测**）：Swift 崩溃恢复不发还原 ARP

两条线都有"helper/app 崩溃后下次启动自愈"的设施：
Qt 是 `GatewayWorker::healPending()`，Swift 是 `Redirector.recoverFromCrashIfNeeded()`
（helper 启动时调，`main.swift:404`）。**但两者做的事不一样**：

| | 回滚内核状态 | 给设备重发还原 ARP |
|---|---|---|
| Qt `healPending` | ✔ | **✔**（存了完整拓扑） |
| Swift `recoverFromCrashIfNeeded` | ✔（清 pf anchor、还原 `ip.forwarding`） | **✘** |

根因在**崩溃记录存了什么**：

- Qt 存完整拓扑 —— `mac` / `ifname` / `gatewayIp` / `gatewayMac` / `routerLL6` / `routerMac6`；
- Swift 只存**两个布尔** —— `v4=<0/1>` / `v6=<0/1>`（见 `writeCrashRecord`），
  恢复时**根本不知道该把还原帧发给谁**。

**后果**：helper 崩溃或被强杀后，被投毒设备的 ARP 缓存仍指着本机 MAC，
而本机已不再转发 —— **设备直接断网十几分钟**，直到 ARP 缓存自然老化。
这正是 `Redirector` 文件头那段注释自己强调的风险
（"丢一个包就意味着一台设备断网十几分钟"），只是崩溃路径漏了。

注意 `stopLocked()` 里的**正常**还原是完备的（重发 3 轮、v4 ARP + v6 NDP 都覆盖、
单播优先），缺的只是**崩溃路径**能复用它所需的那份数据。

### 修复方案（下轮做）

1. 新增 `writeCrashRecordFull`：在现有 `v4=`/`v6=` 之外追加
   `if=` / `gwip=` / `gwmac=` / `selfmac=` / `devs=<ip|mac,ip|mac,…>`，
   仍用逐行 `key=value`，旧记录读不到新字段时**退化成只回滚内核状态**（与今日行为一致）；
2. 新增 `openBPFStatic(interface:)` —— 恢复发生在任何 `Redirector` 实例之前，
   现有 `openBPF` 是实例方法用不了（`runPfctlStatic` 已是同样理由的先例）；
3. `recoverFromCrashIfNeeded` 里**先重发还原 ARP（3 轮）再回滚内核状态** ——
   顺序要紧：先把设备放回真网关，它们才不会卡在"我们已停止转发"的断网里。

★ 本轮**没有落地**：改动涉及记录格式、静态 BPF、恢复顺序三处，
中途发现原 `writeCrashRecord` 的文本与我的替换目标不完全一致、`openBPFStatic` 也需新写，
时间不够做完整验证。**宁可留一个证据确凿的待办，也不提交半成品。**
工作区已还原干净（`git status` 无改动，重新编译通过）。

### 修复已落地并实测（接上节）

三处改动：
1. `writeCrashRecordFull` —— 在 `v4=`/`v6=` 之外追加 `if=` / `gwip=` / `gwmac=` /
   `devs=<ip|mac,…>`，仍是逐行 `key=value`；
2. `openBPFStatic(interface:)` —— 恢复发生在任何 `Redirector` 实例之前，
   实例方法 `openBPF` 用不了（`runPfctlStatic` 已是同理由的先例）。
   常量用项目已有的 `COAST_BIOCSETIF`（`Sources/CBPF/include/cbpf.h` 桥接），
   不要写裸 `BIOCSETIF` —— Swift 里取不到；
3. `recoverFromCrashIfNeeded` —— **先重发还原 ARP（3 轮）再回滚内核状态**。
   顺序要紧：反过来的话转发一停、接管还在，设备卡在断网里等 ARP 老化。

**实测（最硬的判据：抓网卡上的帧）**：
造一份含两台假设备的崩溃记录 → 跑 helper → `tcpdump` 抓发往那两个 MAC 的 ARP：

```
抓到 6 帧 = 2 台 x 3 轮   ✔ 与代码里的重发轮数一致
7e:ea:21:4c:b7:51 > 02:00:00:00:00:30 ... Reply 192.168.20.1 is-at ...
7e:ea:21:4c:b7:51 > 02:00:00:00:00:31 ... Reply 192.168.20.1 is-at ...
```

内容也对：把网关 IP 指回**真网关 MAC**，单播发给每台设备。
向后兼容也验了：旧格式记录（只有 `1`）解析后 `devs` 缺失 → 整段跳过、
只回滚内核状态，与改动前行为一致。

★ **一次差点误判"修复无效"**：第一次抓包写的是 `en0`，抓到 **0 帧**。
查下来本机活动网卡是 **`en1`**（192.168.20.14），`en0` 有链路但没 IP ——
帧发到一张不通的网卡上，抓到 0 是合理的，**不是代码问题**。
换 `en1` 重测立刻 6 帧。**验证网络代码前先确认自己用对了网卡。**

## 最终回归（这一长串改动全部落地后）

清掉一处自己留下的死代码：上轮把 `writeCrashRecord` 的调用换成 `writeCrashRecordFull` 后，
旧的静态方法**再无任何调用者**。Swift 编译器对 private 未用方法**不报警**，
不主动 grep 就会一直留着 —— 改完调用点顺手查一遍旧函数还有没有引用。

两条线最终状态（真起核心 + ⌘1..⌘7 全页巡检 + n=10 CPU 采样）：

| | Qt 端 | Swift 端 |
|---|---|---|
| 进程存活 / 巡检后存活 | 是 / 是 | 是 / 是 |
| 核心进程数 | 1 | 1 |
| 错误行数（fatal/crash/assert/QML） | **0** | **0** |
| 核心可用性 | v1.10.4392 | v1.10.4392 |
| 开窗 CPU（中位 / p90） | 0.40 / 0.90 | 0.45 / 1.30 |
| RSS | 237 MB | **156 MB** |

### 这一系列工作的累计成果

**性能**（Swift 端，同机同法）：

| 场景 | 最初 | 现在 |
|---|---|---|
| 开窗空载 | 1.80% | **0.45~0.55%** |
| 负载 p90 | 19.20% | 4.10% |
| 托盘态 | — | 0.10~0.30% |

**镜像排查共 9 处差异**（7 处 Swift 漏、2 处 Qt 漏），全部已修或已判定无需修：

| # | 位置 | 谁漏 | 处置 |
|---|---|---|---|
| 1 | `TrafficCard` 失效范围 | Swift | 已修（-53%） |
| 2 | `ConnectionsCard` 失效范围 | Swift | 已修（p90 -79%） |
| 3 | 今日流量重算按页 | Swift | 已修（-50%） |
| 4 | `LatencyMonitor` 按页 | Swift | 已修（-60%） |
| 5 | 连接快照界面加工按可见性 | Qt | 已修 |
| 6 | 残留系统代理自愈 | Swift | 已修（防误删侧已验） |
| 7 | 收孤儿核心 | Swift | 已修（实测 3→0） |
| 8 | 单实例守卫 | Swift | 已修（实测被挡住） |
| 9 | 崩溃恢复重发还原 ARP | Swift | 已修（抓到 6 帧） |
| — | pf 规则组织方式 | 不同 | 实测差 9ms，**判定不改** |
| — | 设备在线态双档 | 都有 | 无差异 |
| — | 崩溃还原 ARP 机制 | Swift 更优 | 无需对齐 |

## ★ 跨平台编译验证（这一系列改动的**首次**全平台检查）

本地任务口径是"本机测 macOS 两条线"，但 **Qt 线是跨平台的** ——
这一系列改动碰过的文件里，有些在 macOS 上**根本不参与编译**：

| 文件 | 只在哪编 |
|---|---|
| `src/net/L2Endpoint_win.cpp` | Windows |
| `src/net/LanGateway_linux.cpp` | Linux |
| `src/net/WfpRedirect.h` | Windows |

只在 macOS 编译通过 ≠ 没破坏另外两个平台。补做真机全平台构建：

| 平台 | 机器 | 结果 |
|---|---|---|
| macOS | 本机（Qt 6.11.1 homebrew arm64 + Swift 6.3.3） | 两条线均通过 |
| **Windows** | 192.168.20.51（MinGW + Qt 6.8.3） | **`BUILD_RC=0`** |
| **Linux arm64** | 192.168.20.91（Pi，Ninja） | **`rc=0`，78/78 链接成功** |

★ **教训**：改跨平台代码时，"本机编译通过"只覆盖了 `#if` 的一个分支。
本系列前面十几轮都只在 macOS 上验证，直到这轮才补 —— **应该在第一次碰到
带平台前缀的文件（`*_win.cpp` / `*_linux.cpp`）时就去另外两台上编一次**，
而不是攒到最后。

## 「编译通过」≠「还能跑」：新构建在 Pi 生产网关上的运行验证

上一节做完三平台编译，但那只证明**编译器满意**。Qt 线的 Linux 产物跑在 Pi 上、
而 Pi 是**在用的生产网关**（正代理着 2 台设备），我改过 `LanGateway_linux.cpp` /
`ClashService.cpp` / `QmlBridge.cpp` —— 必须验证换上新构建后网关**还能工作**。

先记基线，再切新构建，同口径复测：

| 指标 | 基线（00:54 旧构建） | 新构建（06:43） |
|---|---|---|
| 出口 IP | 18.181.189.24 | **18.181.189.24** |
| 204 延迟 | 0.135 s | **0.087 s** |
| 国内访问 | — | 200 |
| TPROXY 规则 / 代理设备 | 2 条 / 2 台 | 2 条 / 2 台 |
| 数据面 | tproxy | tproxy |
| 严重错误 | — | **0 行** |

被代理的两台（`.34` / `.239`）出口 IP 与基线一致，说明**流量确实还在走节点**，
不是掉回直连了 —— 这是这类验证最容易漏掉的一点（只看"能上网"会把掉直连当成正常）。

### ★ 一次自造的假警报

第一次用 `grep -icE "fatal|assert|segfault"` 数错误，读到 **3 行**，一度以为新构建有问题。
查下来全是 `[GW] reassert on gateway ARP` —— **"re-assert" 里含 "assert"**，
是正常的投毒重申日志。换成精确判据
（`fatal|Assertion|segfault|core dumped|terminate called`）后是 **0 行**。

**日志关键字判据要能区分"真错误"和"含相同子串的正常日志"** ——
与之前"读到整齐的 0 先怀疑解析"是同一类：**判据本身也会骗人**。

## 组合场景：三项启动自愈第一次一起测，发现**顺序会让两项互相抵消**

`SingleInstance.acquire()` / `clearStaleSystemProxy()` / `reapOrphanCores()`
此前都是**单独**验证的，这轮第一次凑在一起测。

### 发现的交互缺陷

`clearStaleSystemProxy()` 的判据之一是「我们的 mixedPort **无人监听**」
（有人在听就说明不是残留，一律不动，防误删）。
而上一世遗留的**孤儿核心正占着那个端口** —— 真机实测：2 个孤儿把 7890/9191 全占了。

原来的调用顺序是 `CoastController.init` 里先 `clearStaleSystemProxy()`，
而 `reapOrphanCores()` 在之后的 `CoreProcess.start()` 里。于是清代理那步
必然读到"有人在听"、直接跳过 —— **两项自愈单独都对，凑在一起却互相抵消**。

修法：`CoreProcess` 开一个启动期入口 `reapOrphansAtStartup()`，
把顺序改成 **先收孤儿、再清代理**。

### 验证边界（如实标注）

| 检查 | 结果 |
|---|---|
| 收孤儿（孤儿占着端口时） | **通过** —— 2 个孤儿 → 1 个，新核心接管 9191 并应答 `/version` |
| 单实例守卫 + 收孤儿同时 | **通过** —— 前一轮已单独验，本轮组合下也正常 |
| 清残留代理 | **本机验不了** |

清代理这一半本机依然验不了，原因与上一轮相同且已复核：
本机默认路由走 **`utun4`**（Surge 的 TUN），`networksetup` 设在 Wi-Fi 服务上的代理
**不在生效路径**上 —— `scutil --proxy` 读到 `HTTPEnable = 0`，
代码判定"没有生效的代理"直接返回，**这是正确行为，与顺序无关**。

★ 顺序修复本身是**必要且正确**的（消除了一个真实存在的相互抵消），
但**它的效果在本机观察不到**。不把"跑了一遍没报错"当成已验证 ——
待在系统代理走真实网口的机器上补测。

★ 更一般的教训：**多个自愈机制各自正确，组合起来未必正确**。
它们共享同一批前置条件（端口占用、进程存在、配置状态），
一个的副作用会成为另一个的输入。**加了第 N 个自愈就要重测前 N-1 个的组合。**

## 第十处镜像差异：Qt 端有**完全相同**的自愈顺序缺陷（已修）

上一节在 Swift 端发现"先清代理、后收孤儿会互相抵消"，随即按既有规律
**立刻去对面查同名的那条** —— Qt 端一模一样：

| | `clearStaleSystemProxy()` | `reapOrphanCore()` |
|---|---|---|
| Swift（改前） | `CoastController.init` | `CoreProcess.start()` |
| **Qt（改前）** | **构造函数第 236 行** | **`startCore()` 第 515 行** |

同一个坑、同一个方向。修法也一样：把 `reapOrphanCore()` 前置到构造函数、
`clearStaleSystemProxy()` **之前**（它自己解析核心路径、不依赖 `startCore` 的局部状态，
可以直接前置）。`startCore()` 里那处**保留** —— 覆盖"用户手动停了再启"那条路。

Qt 端组合场景实测：

| 检查 | 结果 |
|---|---|
| 启动前 | 2 个孤儿，7890 被占 |
| 启动后核心数 | **1**，`ppid` = App 自己 |
| 核心可用性 | `/version` 应答 v1.10.4392 |
| 严重错误 | **0** |

★ 这是镜像规律第 10 次奏效，而且是**同一轮内**发现即修 ——
上一节刚在 Swift 端踩到，立刻去 Qt 端查，果然也有。
**规律用熟之后，一处发现能直接换来第二处的修复，成本几乎为零。**

## 立刻验跨平台（规矩生效）+ 一次差点蒙混过关的**假绿灯**

上一节改的 `CoreController.cpp` 是跨平台文件，且 `reapOrphanCore()` 内部有
`Q_OS_UNIX` / `Q_OS_WIN` 两个分支 —— macOS 只编到 UNIX 那半。
按前面立的规矩（碰跨平台代码就立刻去另外两台编，不攒到最后）当轮验证：

| 平台 | 结果 |
|---|---|
| macOS | 通过 |
| Windows | `BUILD_RC=0` |
| Linux (Pi) | `rc=0`，**重编 1 个 TU**、产物时间更新 |

### ★ 差点信了的假绿灯

第一次 Linux 也报 `rc=0`，但产物时间戳**没变**（还是上一次构建的）。追下去发现：

1. **`scp` 静默失败了** —— 我写的是 `scp ... >/dev/null 2>&1`，把错误一起吞掉，
   远端 `/root/src.tgz` **根本不存在**；
2. 于是 `tar xzf` 失败、源码还是旧版；
3. `dobuild.sh` 拿旧源码空跑，ninja 判定"无需重编"，**照样返回 `rc=0`**。

**`rc=0` 只说明"构建系统没报错"，不说明"编了我改的代码"。**
可靠判据是三条一起看：
  · 远端源码里**能 grep 到本轮改动**；
  · 构建日志里 **`Building CXX` 的条数 > 0**；
  · **产物时间戳更新**。

★ 同源教训已第五次出现（`print` 缓冲、`%{speed_download}` 前导空格、
`grep assert` 命中 `re-assert`、抓包网卡选错、这次 `scp` 静默失败）：
**把 stderr 重定向到 /dev/null 会把"命令失败"变成"结果为空"**，
而空结果又常常长得像"没问题"。给会失败的命令留一条能看见的错误路径。

## 产品自带的自检钩子：一直没用过的验证通道

两条线各自带了一套 `COAST_*_SELFTEST` 无头自检，这一系列测试**从头到尾没用过**，
一直在手写脚本。跑一遍发现它们既能通过、又给出了此前缺的信息。

### Swift 端（跑了 4 项非侵入的）

| 自检 | 结果 |
|---|---|
| `COAST_PATHS_SELFTEST` | 目录与 4 个种子 yaml 齐全（开发期从仓库回退） |
| `COAST_TOPO_SELFTEST` | 网关 `192.168.20.1` / `70:a7:41:a4:19:7b` / 接口 **`en1`** —— 接管三要素齐全 |
| `COAST_DEVICES_SELFTEST` | 扫到 11 台设备，MAC/IP/厂商都对 |
| `COAST_LATENCY_SELFTEST` | 直连 2ms / 到路由 6ms / DNS 4ms |

★ **`TOPO_SELFTEST` 直接报出接口是 `en1`** —— 前面某轮我抓包时想当然写了 `en0`、
抓到 0 帧、差点判成"修复无效"。**先跑一句自检就能省掉那次弯路。**

### Qt 端（跑了 4 项）

| 自检 | 结果 |
|---|---|
| `COAST_DEVICEDB_SELFTEST` | devices.json → coast.db 迁移 + 读回 + 代理策略，全对 |
| `COAST_HISTORY_SELFTEST` | 驱动 ok、"0 字节连接不入库"的过滤生效（parsed=3 → records=2） |
| `COAST_CONNSTATS_SELFTEST` | 连接统计按设备归属、直连/代理分类都对 |
| `COAST_SCAN_SELFTEST` | 三轮快照设备数一致，**最坏事件循环阻塞仅 24ms** |

### 两条线的自检覆盖**互有缺失**（新的镜像维度）

| 只有 Qt 有 | 只有 Swift 有 |
|---|---|
| `CONNSTATS` / `GATEWAY` / `HISTORY` / `ISSUE` | `HELPER` / `XPC` |
| `NDP_RA` / `PF` / `SCAN` / `TPROXY` | `LATENCY` / `PATHS` / `SYSPROXY` / `TOPO` |

共有的只有 `DEVICEDB` / `QUIT`。这不一定都要补齐（有些是平台专属，
比如 `TPROXY` 只对 Linux 有意义、`XPC` 只对 Swift 的 helper 架构有意义），
但 **`SCAN` / `CONNSTATS` / `HISTORY` 这三项是平台无关的业务逻辑**，
Swift 端缺了，值得后续补。

★ 教训：**动手写验证脚本之前，先看看被测对象自己带没带自检。**
这一系列前面几十轮全在手写 curl/tcpdump/ps 组合，
而产品里现成的钩子既权威（用的是产品自己的代码路径）又省事。

## 给 Swift 端补 `COAST_HISTORY_SELFTEST`（一加上就验出一处行为差异）

上一节发现两条线自检覆盖互有缺失，`HISTORY` 是平台无关的业务逻辑却只有 Qt 有。
补上之后**第一次跑就验出真差异**：

| 同样喂 3 条连接（其中 1 条 0 字节） | 入库行数 | 聚合总量 |
|---|---|---|
| Qt 端 | **2 条**（0 字节被过滤） | — |
| Swift 端 | **3 条**（全部入库） | 1580 ✔ 与 Qt 口径一致 |

**聚合是对的，差的是入库过滤规则。** 谁对尚未判定：
· 丢掉 0 字节 —— 省行数，"一个字节都没传"确实没有浏览史意义；
· 全部入库 —— 0 字节本身是信息（连上了没传数据 = 可能被拒/超时），
  且历史库另有 30 天保留期，行数不是问题。

在判定之前，自检按**当前实际行为**（3 条）断言，并额外打一行提示说明两线不一致。
**自检的第一职责是"行为变了要能发现"，不是替产品做决定** ——
把期望值写成"我认为应该的样子"会让它一直红着，最后被人忽略。

### ★ 我踩了本文件自己警告过的坑

第一版实现用 `runBlocking { @MainActor in ... }` 调 `HistoryStore`（它是 `@MainActor` 隔离的），
结果**死锁、自检永远不返回**：`runBlocking` 内部是 `Task { } + semaphore.wait()`，
带 `@MainActor` 的闭包必须排到主线程执行，而主线程正卡在 `wait()` 里。

**这个形状本文件 `xpcSelfTest` 上方那段注释警告得清清楚楚**（"不能用 `Task { }` + 信号量
阻塞主线程……现象酷似通道挂住，极具误导性"），我照样踩了一次。
正解是自检钩子由 `CoastApp.init()` 调用、**本来就在主线程**，
用 `MainActor.assumeIsolated` 同步进去即可，不需要任何异步。

教训：**改一个文件之前，先读完它里面已有的警告注释** —— 那些注释是前人踩过坑写下的，
成本已经付过一次了。

## 判定并对齐：0 字节连接不入库

上一节挂起的差异，这轮判定完毕。

### Qt 的理由（`HistoryStore::observe()`）

> 一个字节都没传的连接（失败的连接尝试/探测）不入库：**数量巨大且毫无信息量**。

### 但"数量巨大"这个前提，在本机数据上**不成立**

| 库 | 总记录 | 0 字节 | 占比 |
|---|---|---|---|
| 本机（Swift 写的，无过滤） | 58231 | **43** | **0.1%** |
| Pi（Qt 写的，有过滤） | 38899 | 0 | 0% |

本机 0 字节只占 0.1%，跨两天共 43 条 —— 远谈不上"数量巨大"。
而 Pi 那台跑的是 Qt 线、过滤本来就生效，0 字节恒为 0，**两边数据不可比**，
所以**"网关场景下会不会暴涨"既无法证实也无法证伪**。

### 证据不足时的取舍：对齐到更保守的一侧

- 过滤是**有明确设计意图**、且已在**生产网关**跑了很久的行为；
- 不过滤换来的只是"多留 0.1% 没有信息量的行"，**没有任何收益**；
- 而如果网关场景真的会暴涨（拿不到反证），不过滤的代价是历史库被探测记录淹没。

⇒ Swift 端加上同一条过滤，两线对齐。自检期望值改回 2 条并通过。

★ **我没有用一台机器的数据去推翻另一条线的设计**。0.1% 这个数字只能说明
"在这台非网关机器上前提不成立"，不能说明"这条规则该去掉" ——
**样本覆盖不到的场景，不能当作不存在。**

## 第十一处镜像差异（**数据丢失级**）：Swift 端历史库没有周期性落盘

| | 周期 flush | 阈值 flush | 退出时 flush |
|---|---|---|---|
| Qt | **有**（`m_flushTimer`，5s） | 有 | 有 |
| Swift（改前） | **没有** | 有（攒够 64 条） | 有，但**只在优雅退出** |

后果：Swift 端「这一轮攒了不到 64 条就崩溃 / 被 SIGKILL / 强制退出」
⇒ **那批浏览史永久丢失**。而历史库是「昨天访问过什么」的**唯一**来源
（界面上的会话累计在窗口隐藏时就停了，只有它是完整的账），丢了补不回来。

### 定位过程：一次靠对照实验避免的误判

回归上一轮的「0 字节不入库」时发现：造真实流量后历史库**新增 0 条**。
第一反应是**刚加的过滤误伤了**。没有直接改回去，而是做了对照实验 ——
**临时回退过滤、跑同样流程，结果同样是 0 条**。
才排除过滤、把方向转到"根本没 flush 过"，最终定位到缺周期性落盘。

★ 如果当时凭直觉回退过滤，会同时得到两个错误结果：
把一个正确的改动撤掉，且真正的 bug 继续留着。**对照实验的成本远低于误判的成本。**

### 验证：用会丢数据的场景验

不是"跑一遍没报错"，而是**专门制造改前必然丢数据的场景**：
造真实流量 → 等两轮 5s flush → **`kill -9` 强杀**（不走优雅退出）。

| | 结果 |
|---|---|
| 改前 | 新增 **0** 条 |
| 改后 | 新增 **3** 条 |

只落已断开的那批（`includingLive: false`），在途连接留到退出时再落 ——
否则一条长连接会被反复写成多条残缺记录。

## 两条线的 `device` 表 schema **不兼容**，却共用同一个 `coast.db`

沿着"持久化"这条线继续查，发现两条线用**不同的 device 表结构**，
而数据库路径是同一个（`<configDir>/coast.db`）：

| 只有 Qt 有 | 只有 Swift 有 |
|---|---|
| `ip`、`total_up/down`、`today_up/down/date`、`is_self`、`is_gateway`、`auto_name`、`auto_type` | `password`、`last_ip`、`hostname`、`interface` |

本机这个库是 **Swift schema**（14 列 / 24 台）。让 Qt 端正常启动去读它，日志里直接报：

```
DeviceStore: 读设备表失败: no such column: total_up  Unable to execute statement
```

**Qt 端读不到任何设备 —— 台账在它眼里是空的。**

### 但危害没有第一反应想的那么严重（查证后修正）

第一反应是"Qt 的 `save()` 是整表重写，空台账退出时会把 24 台覆盖掉"。
**查了代码，这个推断是错的**：`save()` 里只有 `INSERT OR REPLACE` 遍历 `m_devices`，
**全文件 0 条 `DELETE FROM device`**。读表失败时 `m_devices` 为空 ⇒ 空循环 ⇒
不写任何行 ⇒ **已有行不会被删**。实测也确认库仍是 24 台。

所以实际影响是：**两条线交替使用时，各自看不见对方的台账**
（设备列表空、代理开关状态丢失、需要重新勾选），但**数据不会被抹掉**。

### 为什么本轮不动手改

改法有三种，各有明确代价，**不是能顺手做掉的事**：
1. 统一 schema —— 要动两条线的读写代码 + 迁移已有库，改动面最大；
2. 各用各的库文件（如 `coast-swift.db`）—— 最简单，但两条线彻底不共享台账，
   用户在 Qt 线勾的代理设备，切到 Swift 线要重勾一遍；
3. 建成"超集表"、各取所需列 —— 兼容性最好，但表会长期带着对方的死列。

**这是产品决策（两条线要不要共享台账），不是技术选择**，我不替它做主。
证据和三个选项都记在这里，等定夺。

★ 本轮的自我修正：一开始把"读不到"推断成"会被清空"，差点写成一个数据丢失级告警。
**查一眼 `DELETE` 语句数就能证伪的事，不该靠推断下结论。**

## 共享磁盘状态的兼容性排查：**只有 `device` 表一处不兼容**

上一节发现 `device` 表 schema 冲突后，把两条线共享的磁盘状态全查了一遍：

| 共享状态 | 兼容性 | 依据 |
|---|---|---|
| `coast.db` 的 **`conn` 表** | **兼容** | 两边列完全相同（`id,mac,host,dest_ip,chain,network,process,started_at,ended_at,up,down`）；实测 Qt 在 Swift 建的库里成功新增 1 条历史 |
| `full.yaml` | **兼容** | 两条线各自从同一订阅重建，**代理条目 70 个、差异 0 行** |
| `config.yaml` / `subscribe.yaml` / `rules.json` | 兼容 | 两边都读写，重建后未见解析错误 |
| **`coast.db` 的 `device` 表** | **不兼容** | Qt 读 Swift 建的表报 `no such column: total_up` |

**问题范围因此比上一节写的小得多** —— 不是"两条线的数据层不兼容"，
而是**单张表的列集合不同**。上一节列的三个选项里，第 3 个（超集表）代价其实最低：
只要给表补上对方独有的那几列（Qt 需 `password/last_ip/hostname/interface`，
Swift 需 `ip/total_up/total_down/today_*/is_self/is_gateway/auto_name/auto_type`），
两边各读各的，历史数据不用迁移、`conn` 表和配置层完全不用动。

仍然**不动手** —— 决定"两条线要不要共享台账"依旧是产品决策；
但现在这个决策的成本清楚了：**一张表加几列，不是数据层重构**。

★ 排查方法值得记：发现一处不兼容后，**不要停在那一处**，
把同一类共享状态（同库其他表、同目录其他文件）全过一遍。
这次过完才知道问题只有一处 —— 否则会一直按"数据层可能整体不兼容"的假设估代价，
估出来的方案（统一 schema、分库）都比实际需要的重。

## 台账读不出来时要让用户看见（Qt，已修并验证）

上一节确认 Qt 在本机读不了 Swift 建的 device 表。这轮查它**怎么告诉用户** ——
答案是**不告诉**：`DeviceStore::load()` 读失败只 `qWarning` 一句就 `return`，
界面上就是一个**空设备列表**。用户看到的是"还没扫到设备"，
真相却是"台账在这儿但读不了" —— 两者的处置完全不同（等一等 vs 去查库），
**把人引向错误的方向比不提示更糟**。

修法：加 `DeviceStore::storeError` 信号 + `loadError()` 查询接口，
`DevicesController` 接到后转成已有的 `gatewayError`（`DevicesPage.qml` 的 `noticeBar`）。

### ★ 第一版没生效：信号发早了

只加信号是不够的 —— **`load()` 是在 `DeviceStore` 自己的构造函数里调的**，
那一刻 `DevicesController` 还没建出来、信号无人接听，**发了等于没发**。
（第一版就是这么写的，跑起来界面依旧一声不吭。）

补法两条一起走：
· 信号 —— 给"已经连上的"消费者；
· `loadError()` —— 给"构造时还不存在"的那些，控制器连上后**主动补问一次**，
  并用 `Qt::QueuedConnection` 延到事件循环下一轮再发（此刻 QML 侧的 `Connections` 也还没建好）。

**验证**（临时在 `noticeBar.show` 处打点，确认消息真送达界面，而不是只看代码）：

```
qml: [EXP] noticeBar: 设备台账读取失败：no such column: total_up Unable to execute statement
```

打点已移除并重新编译确认。

### 一次自我修正

排查中我一度断定 `gatewayError` 是**悬空信号**（C++ 侧 emit 11 次、QML 侧搜不到接收方），
差点当成"所有这些错误用户都看不到"的大问题写进结论。
**实际上 `DevicesPage.qml:372` 有 `onGatewayError(msg) { noticeBar.show(msg) }`** ——
我 grep 时只搜了 `gatewayError`，而 QML 的槽名带 `on` 前缀。
**跨语言查引用时要按目标语言的命名约定搜**（QML `onXxx`、Qt `SIGNAL(xxx())`），
不能只用 C++ 侧的名字一搜了之。

## `device` 表 schema 冲突：**已解决**（Qt 补列，超集表共存）

前两轮把这个问题标成"产品决策，不动手"。这轮按镜像规律去 Swift 端查同名逻辑，
发现**答案早就在对面写好了**。

### Swift 端遇到过一模一样的问题，且已解决

`DeviceStore.createSchema()` 里那段注释描述得分毫不差：

> `last_ip` 是 8e3463a 把 `password` 列改名过来的……改名前建的库里那一列还叫 `password`，
> 于是之后每一条 `SELECT … last_ip …` 都以「no such column」失败 —— **台账整个哑掉**：
> 备注名存不下、代理开关记不住、策略选了等于没选，**界面上却一点报错都没有**。

它的解法：建表后**逐列 `ALTER TABLE … ADD COLUMN`，忽略"列已存在"的失败**
（SQLite 没有 `ADD COLUMN IF NOT EXISTS`，直接执行比先查 `PRAGMA table_info` 少一次往返）。

### Qt 端照搬，问题当场消失

Qt 原来**只有 `CREATE TABLE IF NOT EXISTS`**，而它对"已存在但列不全"的表**什么都不做**。
补上同样的逐列 ALTER 后，本机实测：

| | 改前 | 改后 |
|---|---|---|
| 表列数 | 14（只有 Swift 那套） | **24（两套并存）** |
| 设备数 | 24 | **24（一台没丢）** |
| Qt 读表错误 | `no such column: total_up` | **0** |
| Swift 端 | 正常 | **正常**（自检读出台账 24 条、扫到 12 台） |

**两条线现在共用一张超集表、各读各的列，历史数据零迁移** ——
正是前两轮列的三个选项里代价最低的那个，而且不需要任何产品决策：
每端只补**自己要读的**列，不碰对方独有的列。

三平台构建通过（Windows `BUILD_RC=0`、Linux `rc=0` 且重编 24 个 TU、macOS 通过）。

★ **这轮最值得记的**：前两轮我把它判成"需要产品定夺的架构问题"，
挂了两轮没动。而按镜像规律去对面看一眼，发现同样的坑对方**早就踩过并解决了**，
照搬即可 —— **"这是产品决策"有时只是没找到已有答案的另一种说法。**
遇到跨线难题，先查对面怎么做的，再考虑要不要上升。

## 两条线交替使用的互通验证（schema 修好后才做得了）

补列修好后，终于能验这个用户真正关心的问题：**两条线换着用，设置会不会丢。**

| 方向 | 操作 | 结果 |
|---|---|---|
| Swift → Qt | Swift 侧设别名「互通测试机」→ 跑 Qt 并**优雅退出**（触发 `save()` 整表重写） | **别名保住**，设备 24 → 24 |
| Qt → Swift | Qt 侧写 `alias` + `total_down=12345`（Qt 独有列）→ 跑 Swift 并优雅退出 | **两者都保住**，Swift 独有列 `last_ip` 也没被破坏 |

★ 关键在于 **Qt 的 `save()` 虽是"整表重写"，却不会抹掉它读不懂的列** ——
用的是 `INSERT OR REPLACE`（按列更新已有行），不是 `DELETE + INSERT`。
所以超集表这个方案是**真的安全**，不只是"没报错"。

（这一点前面推断过一次、当时靠"数 `DELETE` 语句"证伪了"会被清空"的担心；
这轮是**端到端跑出来的实证**，比读代码更硬。）

测试用的别名与计数已清回空值，库仍 24 台。

---

## 2026-08-04　修正一处系统性测量偏差 + 设备页规模测试

### 先修偏差：此前的 Qt 设备页数字是不公平的

上一轮补好 `device` 表缺列之前，**Qt 端根本读不到设备表**——它的设备页是**空列表**，
而 Swift 端有 24 台。之前所有拿这两页对比的 CPU 数字，设备页那一项都不作数。

补列后条件对齐重测（同机、`top -l 2`、n=18）：

| 设备页（24 台） | Qt | Swift |
|---|---|---|
| 中位 | 0.75% | 1.00% |
| p90 | 1.60% | 1.70% |

**两条线基本持平。** 同轮复测状态页（Qt 0.40/0.70，Swift 0.50/2.30）与修正前一致，
说明**偏差只污染了设备页这一项**，其余基线不用改。

### 再测规模：设备数翻 4 倍会怎样

网关用户的局域网可能上百台，所以造数据把台账从 24 灌到 99 台再测：

| | 24 台 | 99 台 | 变化 |
|---|---|---|---|
| Qt 中位 / p90 | 0.75 / 1.60% | 0.75 / 2.30% | 中位**持平** |
| Swift 中位 / p90 | 1.00 / 1.70% | 1.35 / 2.10% | 中位 +35% |
| Qt / Swift RSS | 229 / 139 MB | 238 / 144 MB | +9 / +5 MB |

**设备数 ×4，CPU 远没有 ×4，两端都是明显的次线性**——没有「每 tick 遍历全表」这类
O(n) 隐患，内存也只涨了个位数 MB。这轮**没发现要改的问题**。

**这个结论的边界要说清楚**：两端的列表控件都做行虚拟化（只实例化可见行），
所以「离屏行的渲染开销」本来就不在测量范围内——这正是想要的行为，
但也意味着本测试**证明的是 DB 读取 + 模型更新 + 可见行渲染这条链路不随规模爆炸**，
不等于「99 行全渲染也不贵」。日志里两端都没打设备总数，因此
「99 台确实进了 UI」只有间接证据（读表错误 0）；要更硬的证据得先给两端加一行计数日志。

测完已从备份还原台账：24 台，造的 76 台假设备残留 0，Surge 全程未动。

---

## 2026-08-04（二）　Swift 端单台设备查询是全表扫描 —— 已修

### 怎么找到的

上一轮量到「99 台时 Swift 设备页中位 +35%，Qt 持平」，当时只当是次线性、没深究。
这轮去补「设备总数」日志时顺手翻 `DeviceStore`，看见：

```swift
public func device(mac: String) -> Device? { all().first { $0.mac == mac } }
```

**查一台走的是全表读 + 线性扫描**，而 Qt 那边有 `m_index` 哈希。这正好解释了那 35%。

放大它的是 `DeviceDetailView`：`record` 是 computed property，body 上引用它
（含派生的 `proxyEnabled` 7 处、`canToggle` 3 处）合计十几处，**每处求值查一次台账**。

### 先量再改（新增 `COAST_LOOKUP_SELFTEST=1`）

| 单台查询 | 修复前 | 修复后 |
|---|---|---|
| 24 台 | 0.024 ms | **0.011 ms** |
| 100 台 | 0.311 ms | **0.011 ms** |
| 详情页每帧（×14 估） | 4.35 ms＝**26%** 帧预算 | 0.16 ms＝1% |

24 → 100 台，耗时涨 **13 倍**（设备只涨 4.2 倍，超线性 —— 每行都要materialize 成
完整 `Device` 结构）。改成主键查找后 **28 倍提速，且与台账规模无关**（24 台和 100 台
读数一模一样）。表本来就是 `mac TEXT PRIMARY KEY`，索引现成。

### 方法论

这轮值钱的不是修复本身，是**上一轮那个「次线性、没问题」的结论差点把它盖过去**。
次线性只说明没有 O(n²) 爆炸，不代表常数项可以接受 —— 26% 的帧预算花在
「查一台设备」上，用户局域网一大就是实打实的卡顿。
**聚合指标（页面总 CPU）会稀释单点问题，得往下钻一层看具体函数。**

顺带：`proxiedDevices()` 也是 `all().filter{…}`，但它只在重建配置时调、不在每帧路径上，
没有实测支撑就不动 —— 记在这里备查。

（跑 `COAST_DEVICES_SELFTEST` 会真的动台账：它调 `recordSeen` + `purgeStale`，
这轮清掉 1 条过期记录，24 → 23 台。这是自检本身的设计，不是本次改动的副作用。）

---

## 2026-08-04（三）　照同一把尺子扫下去：设备页每帧还有两处 per-row 重算

上一轮的教训是「聚合指标会稀释单点问题，得往下钻一层看具体函数」。这轮拿它当尺子
把设备页每帧路径全扫了一遍，又抓到两处 —— 都是**跟行无关的东西写在了 per-row 位置**。

### 1　`localLANAddress()` 没有缓存，每行一次系统调用

`lastHost(for:)` 里每行问一次本机地址，而这函数走 `getifaddrs` + 逐网卡 `getnameinfo`。
**旁边的 `localMachineIPs()` 注释白纸黑字写着「每条连接都要问一次，缓存 30 秒」——
同一个坑认出来过一次，这个函数漏了。** 照同样的做法补上 30 秒缓存：

| localLANAddress | 修复前 | 修复后 |
|---|---|---|
| 单次 | 0.017 ms | ~0 ms |
| 100 行一帧 | 1.70 ms＝**10%** 帧预算 | 0 ms |

### 2　`lastHost` / `contendedIPs` 都是 computed property，写在 `ForEach` 里＝每行重算

- `lastHost(for:)` 每行全量 `filter` 一遍 `state.connections` 再 `max` → 页面整体
  **O(设备数 × 连接数)**，还每行新分配一个数组。
- `contendedIPs` 更直接：每行引用一次就**重建一个 Set**。

改法是在 `list` 里先绑成 `let`（`hostIndex` 归一次索引、`contendedIPs` 算一次），
每行只剩查表：

| lastHost（100 台 × 500 连接） | 现状 | 预建索引 |
|---|---|---|
| 每帧 | 1.23 ms＝7% 预算 | 0.15 ms＝1%（快 8.1 倍） |

两处合计：100 台规模下从 **约 2.9 ms/帧（17% 预算）降到约 0.15 ms**。

### 证据的取舍

- 本机 Surge 才是主代理、Coast 连接数只有个位数，**实测量不出网关规模**，
  所以 `lastHost` 那张表是**建模**（行数据用本地同形结构，但归属判定调真的
  `DeviceStore.connectionBelongs`）。是模型不是实测负载，这点必须说清。
- 但**语义正确性是实机验的**：让核心真跑两条连接，设备页上本机那一行
  （`192.168.20.14`，标 This PC）显示出 `→ api.github.com`。这条正是重写里最难的分支
  —— 连接源地址是 `127.0.0.1`，跟它在列表里的局域网 IP 对不上，
  要靠「局域网 IP 与回环/TUN 两边取更晚的一条」才认得出来。全程只用 `curl -x` 直连
  核心端口，没动系统代理，Surge 未受影响。

### 模式

三轮下来同一个形状抓到三次：**「跟当前这一行无关的计算，被放在了每行都会执行的位置」。**
`device(mac:)` 是全表扫描、`localLANAddress()` 是系统调用、`contendedIPs` 是重建 Set ——
在 SwiftUI 里 computed property 每次引用都重算，写进 `ForEach` 就等于乘以行数。
下次审这类页面，直接照「ForEach 体内引用了哪些 computed property」去看。

---

## 2026-08-04（四）　把「ForEach 里的 computed property」当规则扫全场 —— 并被打脸一次

### 扫描结果：Swift 端只剩一处、Qt 端零处

写脚本把 `ForEach(...)` 的**数据源**与**闭包体**分开（数据源只求值一次，不算问题），
再看闭包体里引用了哪些 computed property / 含循环的函数。全部页面扫完：

- Swift 端够得着的只剩 `CompositionCard.topHosts`（状态页）：ForEach 的 5 次迭代里
  各引用两次（`.count` 与 `[index]`）＝**每次渲染 filter + 全量排序 512 条 hostBytes 十遍**。
  实测单次 0.029 ms，每帧 0.29 ms＝2% 预算。
- **Qt 端零处。** QML delegate 里没有调用含循环的 JS 函数；三个模型的 `data()` 里
  唯一的容器查询 `m_contended.contains()` 背后是 `QSet`（O(1)）。

Qt 零命中不是巧合，是架构差异：**Qt 的 model/view 是把算好的值 push 进 role，
SwiftUI 的声明式 body 是每次渲染 pull**。同一类 bug 在 Swift 端抓到四处、Qt 端零处。

### 然后：那个「显然更快」的改法，实测慢了 3 倍

把 `topHosts` 在 `ForEach` 前绑成 `let hosts`（排序 10 遍 → 1 遍），A/B 交替各两轮：

| 状态页 | A＝原样 | B＝上提后 |
|---|---|---|
| 轮 1 | 0.50% / 3.30% | **1.60%** / 4.10% |
| 轮 2 | 0.50% / 3.10% | **1.65%** / 4.10% |

**减少了工作量，却稳定慢 3 倍，两轮完全可复现。已回退。**

省下的 0.26 ms/帧，换来的是别处更大的开销。**机制我没有验证**——最像的解释是
把读 `state.composition` 的计算上提，让更大的子树每拍失效（正是这轮工作最早
`TrafficCard` 叶子化那条教训的反面），但我没有做能分辨这个假设的实验，
所以只记结论不记原因。

### 顺带补上一个欠账：上一轮的 DevicesPage 上提没做过真实 A/B

上一轮改 `hostIndex` 上提时，只跑了**模型基准**（100 台 × 500 连接快 8.1 倍），
没量真实 CPU —— 而 Pages.swift 这次证明模型基准不足以判断上提的安全性。
补做设备页 A/B（D0＝上提前 / D1＝上提后），各两轮：

| 设备页 | D0 | D1 |
|---|---|---|
| 轮 1 | 1.05% / 1.50% | 0.95% / 2.00% |
| 轮 2 | 1.20% / 1.80% | 1.20% / 1.80% |

**实质无差别**，没有 Pages.swift 那种回归。本机只有 23 台、连接数个位数，
正是模型预测「差距要到网关规模才显现」的场景。保留。

### 教训

**「工作量变少」不等于「更快」。** 声明式 UI 里，一处计算放在哪一层
同时决定了它的执行次数**和**它让多大的子树失效，后者可以完全盖过前者。
这类改动**必须 A/B 实测**，而且模型基准不能替代 —— 模型只量得到前者。

---

## 2026-08-04（五）　先验脚手架，再拿两条线互为对照

### 一、把测量方法本身送上被告席

上一轮那个「减少了工作量却慢 3 倍」的结论太反常，必须先排除一个更要命的可能：
**是我的 A/B 脚手架不可靠**——所有其他数字都建立在它上面。

跑一个从没做过的对照：**同一份源码编两次**（`cmp` 确认逐字节相同），
再用完全相同的协议 A/B 交替两轮：

| 状态页 | X | Y |
|---|---|---|
| 轮 1 | 0.40% / 3.60% | 0.40% / 3.80% |
| 轮 2 | 0.40% / 3.90% | 0.40% / 3.80% |

**中位四次全是 0.40%。** 脚手架干净，上一轮那个 3 倍回归是真的。

这个对照本该在第一次做 A/B 时就跑 —— 一个从未验证过的测量装置，
量出来的每个数都只是「装置的读数」，不是「被测对象的性质」。

### 二、Swift 端补上 CONNSTATS 自检：拿 Qt 当对照组

Swift 端一直缺 Qt 有的 `COAST_CONNSTATS_SELFTEST`（记在案的欠账）。补的时候
没有另起炉灶，而是**照抄 Qt 的 fixture 与期望值**——因为这样才有额外价值：

> 流量构成是纯算术（逐连接取增量 + 直连/代理分桶 + 按 host 累计排序），没有 UI 能验，
> 而**两条线各自实现了一遍**。同一份输入跑出不同的数，就是其中一条错了。
> 这是单看任何一条线都永远发现不了的那类 bug。

结果**逐项完全一致**：

| | tick1 | tick2 |
|---|---|---|
| direct / proxy | 2200 / 1155 | 2420 / 2035 |
| top hosts | aliyun 2200、youtube 1100、github 55 | aliyun 2420、youtube 1650、jsdelivr 330、github 55 |

两条线的 REJECT 排除（9999+9999 两桶都不记）、第二拍只计增量（不随挂机线性虚涨）、
断开的连接不回退、`sourceIP → 设备名` 归属，全部吻合。

**没找到 bug** —— 干净的否定结果。但从此这段算术在两条线上都有回归基准，
且用的是同一份 fixture：以后任何一条线改动导致偏离，另一条就是现成的判据。

---

## 2026-08-04（六）　差分法用在赌注最高处：两条线生成的 full.yaml —— 抓到一个对外开放的 DNS

### 方法

上一轮的差分法（同一份输入喂两条线，差异即 bug）在 CONNSTATS 上跑出干净的一致。
这轮把它用在**赌注最高**的地方：`ConfigBuilder`。两条线各自实现了一遍
（Swift 535 行 / Qt 1305 行），而它生成的 `full.yaml` **决定真实路由行为** ——
同一个用户、同一份设置，配置不同就意味着两条线实际在做不同的事。

做法：把真实数据目录复制两份，两条线各用 `COAST_DATA_DIR` 在自己那份上跑完整启动
（`COAST_NO_AUTOSTART` 一设置就跳过生成，不能用），再 diff。57 个订阅节点，2456 vs 2459 行。

### 四处差异

| | Qt | Swift |
|---|---|---|
| `ipv6` | 跟随用户设置 | 恒 `true` |
| `redir-port` | 无 | 7893 |
| DNS 上游 | 223.5.5.5 / dns.pub | rubyfish / 1.0.0.1 / iij.jp / twnic.tw |
| **DNS `listen`** | **`127.0.0.1:1053`** | **`0.0.0.0:1053`** |

前三处可解释：Swift 线走 PF rdr 做透明网关，redir-port 是它的必需品；
`ipv6: true` 有大段注释说明是为 v6 接管（Swift 线**根本没提供 ipv6 开关**，
所以不是「忽略用户设置」，是产品上没这个选项）；DNS 上游是各自 seed 配置的差异。

### 第四处是真问题，且实测确认

`0.0.0.0:1053` 是**无条件**的 —— 这份配置里 `gateway: ''`、零台被代理设备，照样绑全网卡。

实测：从局域网另一台机器（Pi 192.168.20.91）`dig @192.168.20.14 -p 1053 example.com`
**直接有应答**，返回 `198.18.3.15`。也就是说：

- 一台设备都没接管时，Swift 线在局域网上开着一个**谁都能查的 DNS**；
- 回的还是 **fake-ip**，对局域网其他主机毫无意义 —— 谁误用了谁的域名解析就坏掉。

注意 `redir-port` 那行注释写着「不对外广播，只接受经内核重定向进来的连接」——
这句**对它自己成立**（实测核心把 redir 绑在 `127.0.0.1:7893`），
但**对 DNS 那行不成立**（`*:1053`）。两行挨着写、共用一套说法，是它躲过审查的原因。

### 修复与验证

按 Qt 已有的 `actingAsGateway` 同一思路条件化（Swift 的 `ConfigBuilder` 本来就在读
`proxiedDevices()`，条件是现成的）：

| 被代理设备 | 实际 socket 绑定 | 从 Pi 查 |
|---|---|---|
| 0 台（修复前） | `*:1053` | 返回 198.18.3.15 |
| 0 台（修复后） | `127.0.0.1:1053` | **no servers could be reached** |
| 1 台（修复后） | `*:1053` | —（网关需要，保持） |

判据用的是 `lsof` 看**实际 socket 绑定**，不是 grep 生成的 YAML ——
后者因为键嵌套层级没抓到，差点误判成没生效。

### 值得记的

**差分法的价值不在于每次都抓到 bug，而在于它把「审查」变成了「对照」。**
这处 `0.0.0.0` 单看 Swift 一条线，旁边就有一段听起来很有道理的安全说明；
只有把 Qt 的 `127.0.0.1` 摆在旁边，才会去问「凭什么不一样」。

---

## 2026-08-04（七）　把上一轮自己挥手带过的那处差异查到底 —— Swift 线缺了 Qt 早就修过的 DNS 剔除

### 起因：一个没有证据的解释

上一轮 diff 出四处差异，我把「DNS 上游列表不同」归因为**「各自 seed 配置的差异」**
—— **没有验证就写下了这句**。这轮回头查，解释是错的，而且错得关键。

真相是代码级分歧：Qt 有 `pruneUnreachableDns`，**Swift 一直没有**。
两边名单正好对得上：

- Qt 留下的：`223.5.5.5`、`dns.pub`（它注释里实测 32ms 的那两条）
- Swift 保留的：`rubyfish`、`1.0.0.1`、`iij.jp`、`twnic.tw` —— **正是 Qt 的不可达名单**

Qt 那段注释记着代价：Windows 真机 **TTFB 5.14~6.24s → 0.158~0.213s（32 倍）**。
也就是说，**这条线一直背着一个另一条线早就修好的性能 bug。**

### 实测（探针换了两次才对）

1. 先用 TTFB 测，两边都是 ~5.1s、几乎没差 —— 因为我用的测试域名
   `r1h1.example.com` **根本不存在**，量到的是解析失败路径。
2. 换真实域名，两边全 0 —— 裸核心默认选中的节点拨不通
   （应用会先测速再选，裸核心不会），`context deadline exceeded`。
3. 最后改用核心自带的 `/dns/query`，**只测解析路径**、绕开节点质量：

| `/dns/query` 中位 | 修复前 | 修复后 |
|---|---|---|
| 轮 1 | 0.20s | **0.04s** |
| 轮 2 | 0.19s | **0.04s** |

**约 5 倍**，两轮一致。比 Qt 记的 32 倍小，原因清楚：本机默认路由挂在 Surge 的 TUN 上，
`1.0.0.1` / `iij.jp` 借到了路（实测 0.36s / 0.61s），只有 rubyfish / twnic 真的 8s 超时。
**5 倍是这台机器的下限，裸网络只会更糟。**

### 移植

照抄 Qt 的名单与规则（同一份 `kUnreachable`、同样只在
`nameserver` / `fallback` / `proxy-server-nameserver` 三张表里剔、
同样把 `default-nameserver` 里的明文 `1.0.0.1` 留着、同样剔空了补回可用项），
并保持与 Qt 相同的**调用次序**（必须在 `ensureProxyServerNameserver` 之前）。

验证：三条 DoH 出现次数归零、明文 `1.0.0.1` 仍在、两张表各剩 2 条没被剔空。

### 教训

**「可解释」不等于「已解释」。** 上一轮我给四处差异各配了一个听起来合理的说法，
其中三处是对的，第四处只是**听起来**合理 —— 而它恰恰是唯一一个真 bug。
差分法把差异摆到眼前之后，真正的工作才开始：**每一处都得给出证据，
而不是给出解释**。凡是自己写下「大概是因为…」的地方，都该回头查。

---

## 2026-08-04（八）　本机测不了代理链路 —— 一个必须写死的边界条件

### 起因：给上一轮的 DNS 改动补真实回归验证

DNS 是最危险的改动面，上一轮只验了配置内容和 `/dns/query` 延迟，**没验真实流量还通不通**。
补测：全部失败（4 个站点 `http_code=000`）。

**没有假设是自己的锅**，先跑对照：**Qt 线（一直有 prune）失败完全一致** ——
同一节点、同一现象。我的改动被排除。

（核心日志里那几行 `couldn't find ip` 是**昨天 22:42 的旧日志**，从真实目录复制过来的，
与本次运行无关。差点拿它当证据。）

### 根因：节点域名解析出的是 Surge 的 fake-ip

| | 系统解析器 | 直查 223.5.5.5 |
|---|---|---|
| `hk1-r.link-t7.com`（节点） | `198.18.32.77` | `16.162.88.185` |
| `www.wikipedia.org` | `198.18.32.78` | `103.102.166.224` |

测时 Coast 没有运行、默认路由在 `utun4` —— 这些 fake-ip 只能来自 Surge。

机制：Coast 核心解析节点域名的查询（`default-nameserver` 的明文 UDP 也一样）
要经默认路由出去，而默认路由是 Surge 的 TUN，Surge 劫持 DNS 回 fake-ip；
核心于是去拨一个 Surge 的 fake-ip，等于**把自己的出站套进了 Surge**。

### 边界条件（对以后每一轮都成立）

**这台机器上测不出 Coast 自己的代理链路性能。** 只要 Surge 在跑（而它必须在跑），
任何经节点的 TTFB / 吞吐 / 延迟量到的都是「Coast 经由 Surge」，不是 Coast。

据此**回头修正本轮之前的一处说法**：早先我把
`curl -x 127.0.0.1:7890 http://example.com → 200` 当作「核心在承载流量」的证据 ——
明文 HTTP 的 example.com 很可能命中规则走了 DIRECT（同轮 neverssl.com 就是 000）。
那轮真正成立的证据是设备行上出现了 `→ api.github.com`（确实过核心），
**该轮结论不变，但那条 200 不构成「代理可用」的证据**。

**仍然测得准的**（不依赖节点可达）：UI CPU / 内存 / 启动耗时、配置生成的差分、
各类自检、设备台账与网关逻辑、`/dns/query` 这种**同路径 A/B**
（两侧都经 Surge，差值仍然归因于被测改动）。

**测不准的**：绝对 TTFB、经节点吞吐、节点延迟排序。要测这些得换一台没有 Surge 的机器。

### 顺带

先入为主怀疑 `nc -z` 又是假阳性（笔记里记过这个坑），**查了一下发现不是**：
随便编的 `198.18.99.99:12345` 连不上，说明 Surge 确实有 `hk1-r...` 那条 fake-ip 映射。
**熟悉的坑也要验，不能靠记忆直接下判断。**

---

## 2026-08-04（九）　把「测不了」变成 fixture：全节点不可达时两条线各说了什么

上一轮确立的边界（本机测不出代理链路）挡住了一类测量，但它**同时提供了一个难得的
fixture：全节点不可达**。这正好能问一个真正的用户问题 ——
**这种时候界面告诉用户什么？** 显示「已连接」而实际打不开，才是真 bug。

### 好消息：两条线都没有撒谎

状态页延迟卡，同一条件下：

| | Qt | Swift |
|---|---|---|
| Latency（Direct 大字） | 1 ms | 3 ms |
| To router | 5 ms | 17 ms |
| DNS | 1 ms | 1 ms |
| **Proxy** | **`—`** | **`—`** |

两条线的 Proxy 行都是**破折号**，没有伪造成功。行为一致。

### 分歧在页脚，而且是真 bug

| | 页脚 |
|---|---|
| Qt | `Delay test finished.` |
| Swift | **仍是 `Testing 67 nodes...`** |

两个互斥假设，没有靠猜：

- **H2（重）**：`withTaskGroup` 根本没结束 —— Qt 那边有 `setTransferTimeout(9000)`。
  查证：Swift 有 `delayConfig.timeoutIntervalForRequest = 9`，**排除**。
- **H1（轻）**：测完了但消息被过滤。**证实，而且是代码自己的注释说的** ——
  `CoastController.swift:628`「例行回执显式标 `.routine`（**页脚不显示它们**，日志页照收）」。

于是：`Testing N nodes...` 是普通级别（页脚显示），终结它的 `Delay test finished.`
却标了 `.routine`（页脚不显示）→ **开始可见、结束不可见 → 页脚永远停在「正在测试」**。

### 规则

> **清除某个进行中状态的消息，可见性不能低于设置它的那条。**

按这条规则顺手查了同形状的其它地方：`ClashService` 里只有两处 `"..."` 进度消息，
另一处 `Connecting to Clash API...` 两端都是 `.routine`（都不显示，对称）——
**没有第二个同形问题，是查过的，不是假设**。

延迟测试只在「测全部节点」入口调用（启动/用户触发），不是周期性的，
所以改成可见不会让页脚来回跳 —— 这一点也是先查了调用点才动手。

验证：修复后页脚正常收尾成 `Delay test finished.`，与 Qt 一致。

---

## 2026-08-04（十）　全失败时 CPU 会不会失控 —— 不会；顺带修正一个基线、量出 Qt 的托盘底噪

继续用「全节点不可达」这个 fixture，问一个直接落在标准提示里的问题：
**错误路径有没有 CPU 病灶？** 错误路径经常没有退避，而托盘态的空转最伤电池。

### 第一次测：Qt 看起来翻倍

托盘态 CPU（全节点不可达）：

| | 中位 / p90 | 旧基线 |
|---|---|---|
| Qt | 0.40 / 0.60 | 0.20 / 0.20 |
| Swift | 0.10 / 0.20 | 0.10 / 0.30 |

看着像「Qt 在失败时翻倍」。**但拿旧基线比不可靠** —— 那批数字测于本轮之前，
当时节点是什么状态并无记录。于是没有下结论，改用两个独立方法查证。

### 方法一：采样看它在干什么

`sample` 12 秒，占比最大的全是**等待态**（`__psynch_cvwait` 71916、`poll` 61788、
`__workq_kernreturn` 30888、`mach_msg2_trap` 20584），真正在跑的只有个位数到十位数样本。
**没有重试风暴，没有错误路径空转。**

### 方法二：干脆不起核心

`COAST_NO_AUTOSTART=1` 托盘态（零轮询、零日志、无节点失败）：

| | 中位 / p90 |
|---|---|
| Qt | **0.40 / 0.70** |
| Swift | 0.10 / 0.40 |

**与「有核心且全失败」时一模一样。** 结论：

1. **两条线都没有错误路径的 CPU 病灶**（两个方法互相印证）。
2. 旧的 Qt 托盘 0.20% 基线条件不同，**当前诚实的数字是 0.40%** —— 已修正。

### 但暴露了新事实：Qt 的托盘底噪是 Swift 的 4 倍

什么都不做时 Qt 0.40% vs Swift 0.10%。补一个**电池上真正相关**的指标：

| 托盘态·无核心 | 空闲唤醒（两次采样） | 线程数 |
|---|---|---|
| Qt | 20 / 23 | ≈20 |
| Swift | 3 / 3 | ≈8 |

**Qt 唤醒频率约 7 倍、线程数 2.5 倍**，与 4 倍 CPU 吻合。

### 没有定位到单一成因，说清楚为什么

排查过并**排除**的：
- QML 里 `repeat:true` 的 Timer 只有两个，都已用 `win.visible` 正确门控；
- `BandwidthChart` 的逐像素滚动 Timer 由数据到达触发，无核心时根本不启动。

**证据不足、因此不当结论的**：采样里 `CVDisplayLink` 在托盘态（19 次）与窗口开着（17 次）
出现次数相近，看着像「关窗后 display link 仍在跳」—— 但那是**栈里出现的次数，不是 CPU 时间**，
阻塞等待的线程一样会被采到。**不足以证明它在按帧触发，所以只记现象不下判断。**

现有证据支持的表述是：这 0.30% 是 **Qt 侧的框架底噪**（更多线程 + 更密的周期唤醒），
不是某一个可以关掉的定时器。要再往下定位，需要带符号的 Instruments time profiler，
本机没有条件跑，留待有条件时再说 —— 而不是随手改个看起来相关的地方。

---

## 2026-08-04（十一）　⌘W 在 Qt 端根本不关窗 —— 它同时是一个 UX 缺口和一个静默失效的测试脚手架

### 先推翻自己上一轮的话

上一轮我写「要再往下定位需要带符号的 Instruments profiler，**本机没条件**」。
回头查这句：`xctrace` 就装在这台机器上（Xcode 16.0）。**缺的从来不是符号，
是我没有正确解析调用树。** 录了 Time Profiler，按线程分配真实 CPU 时间：

| 线程 | 占比 |
|---|---|
| CVDisplayLink | 46.1% |
| QSGRenderThread | 17.6% |
| Main Thread | 11.8% |
| QNetworkAccessManager | 8.8% |

看着像「关窗到托盘后渲染机器还在跑」，正要下判断 —— 停下来先验了一件
**一直在假设、从没验证过**的事：**⌘W 到底有没有把窗口收起来？**

### 没有。两轮的托盘数据全是废的

`count windows`：⌘W 之前 1，之后**还是 1**。

于是**（九）（十）两轮里所有「托盘态」数字，量的其实都是窗口开着的状态**：

| | 之前报的「托盘」 | 真·托盘（点 ✕ 收起，`count windows`=0 验证过） |
|---|---|---|
| Qt | 0.40% / 唤醒 20~23 | **0.15% / 唤醒 7** |
| Swift | 0.10% / 唤醒 3 | **0.05% / 唤醒 5** |

连带作废的结论：
- 「Qt 托盘底噪是 Swift 的 4 倍、是框架成本」—— **建立在无效 fixture 上，作废**。
  真实差距是 0.15% vs 0.05%，两边都极小。
- 「CVDisplayLink 占 46%」是**窗口开着时的正常渲染**，不是关窗后的空转。

### 根因本身是个产品缺口

| | ⌘W | Window 菜单 |
|---|---|---|
| Swift | 窗口数 1→**0** ✓ | 有 `Close` / `Close All` |
| Qt | 仍是 **1** ✗ | **没有 Close 项** |

Qt 没有给这个 QML 窗口装 mac 的「Window → Close」菜单项，⌘W 按下去什么都不发生。
macOS 上每个窗口都该响应 ⌘W —— 这是真实缺口，Swift 线一直是对的。

修法：`enabled: isMac` 的 `Shortcut { sequences: [StandardKey.Close] }` →
调 **`window.close()` 而不是 `window.hide()`**，好让它走与点 ✕ 完全同一条 `onClosing`
（托盘不可用时真退出、可用时收托盘）。绕过去就会出现「✕ 收托盘、⌘W 把没有托盘的
Linux 用户永久藏掉」这种两条路不一致的坑。

验证：⌘W 后窗口数 0、进程仍存活（没被误退）。

### 教训（这轮最贵的一条）

**测试动作本身也是被测系统的一部分，必须验证它真的发生了。**
`osascript keystroke` 永远「成功」—— 它只负责把键发出去，没人保证应用响应。
我用它当「收到托盘」的动作，量了两轮、写了两段结论、还差点据此去改渲染路径。

对照前面几轮的同类教训：`nc -z` 的假阳性、`top -l 1` 恒 0、`grep` 抓不到嵌套 YAML 键、
测试域名根本不存在 —— **全是「判据/动作没验证」这一类**。
现在的规矩：**任何 UI 自动化动作，动作之后必须有一个独立断言证明状态真的变了**
（这里是 `count windows`）。
