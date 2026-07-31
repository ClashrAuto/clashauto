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
