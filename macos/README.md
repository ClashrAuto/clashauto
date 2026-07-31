# Coast for macOS —— Swift/SwiftUI 版

`clashauto-c++/` 那个 Qt/QML 客户端的 macOS 端**原生重写**:Swift 6 + SwiftUI,不依赖 Qt。

与 Qt 版**并存、共享同一套用户状态** —— 同一个 `~/Library/Application Support/Coast`
数据目录、同一个 mihomo 核心、同一套配置文件格式与 i18n 表。两版可以互相接管,
不会让老用户的订阅/设备/历史凭空消失。

> 逐步的设计决策与踩过的坑记在 [`PLAN.md`](PLAN.md) 的「变更日志」里 —— 那是过程账本;
> 本文是**给新接手者的入口**,只讲「是什么、怎么跑、为什么这么设计」。

## 一分钟上手

```bash
cd macos
swift build            # 编译
swift test             # 单元测试(168 个用例)
swift run Coast        # 开发期直接跑(会开 GUI)

bash scripts/make_app.sh --version 0.1.0   # 打包成 Coast.app(ad-hoc 签名)
bash scripts/regression.sh                 # 一键回归:测试 + 打包 + 全套自检
```

**与 Qt 版最大的不同:本地就能编、能跑、能测。** Qt 版得靠 CI + Docker `validate/`;
这边 `swift build`/`swift test` 直接来。

## 架构

四个 SPM target,依赖自内向外:

```
CoastHelperProtocol   —— app 与 helper 共享的 XPC 契约 + ARP 报文构造(纯字节,可测)
        ↑
     CoastKit         —— 全部后端逻辑,与 UI 无关(配置/订阅/REST/存储/系统集成)
        ↑
      Coast           —— SwiftUI 可执行程序(窗口/托盘/七个页面)

  CoastHelper         —— 特权 helper(root daemon),只链 CoastHelperProtocol + CBPF
  CBPF                —— 几个 BPF ioctl 常量的 C shim(_IOW 宏 Swift 导入不进来)
```

**为什么 helper 单独成 target、只链最小面**:它以 root 跑,链进去的每一行代码都是攻击面。
所以它不碰 CoastKit,只依赖那份共享契约。

### CoastKit —— 后端(约 5900 行)

| 领域 | 关键文件 | 职责 |
|---|---|---|
| 配置 | `AppConfig` `AppPaths` `YAMLText` | 读写 config.yaml;数据目录与 Qt 版**逐字一致** |
| REST | `ClashAPI` `ClashService` `SpeedProbe` | 轮询核心、组/叶子解析、延迟与下载测速 |
| 核心进程 | `CoreProcess` `CoreDownloader` | 启停 mihomo、按需下载内核 |
| 订阅 | `SubParser` `ProxyURI` `SingBoxConverter` `SubscriptionStore` `SubscribeDocument` | 分享链接解析、`subscribe.yaml` 读写 |
| 配置生成 | `ConfigBuilder` `YAMLSurgery` `LocalIPv6Prefixes` | 生成 `full.yaml`(YAML 按文本手术,见下) |
| 存储 | `SQLite` `HistoryStore` `DeviceStore` `RulesStore` | 原生 sqlite3;上网历史、设备台账、自定义规则 |
| 局域网 | `LanBrowser` `LanTopology` | 读邻居表发现设备、取默认网关三要素 |
| 系统集成 | `SystemProxy` `MacHelperClient` `LaunchAtLogin` `TrayController` `CoastController` | 系统代理、helper 客户端、开机自启、托盘、编排 |

### Coast —— UI(约 2400 行)

`AppState`(`@Observable` 中枢,取代 Qt 版的 `QmlBridge`)+ `Theme`(设计令牌,颜色逐条对齐
`qml/Theme.qml`)+ 七个页面(状态/节点/设备/订阅/设置/日志/关于)+ `SelfTests`(无头自检)。

## 五个刻意的设计决定

新接手者最容易「顺手改掉」的地方,逐条讲清为什么不能:

1. **YAML 当文本手术,不引 YAML 库**(`YAMLText` / `YAMLSurgery`)。
   `default.yaml` 是 8 万行带注释的模板,用库往返一次会把注释和格式全抹掉,生成的
   `full.yaml` 与现在完全不同。所以逐条对齐 Qt 版的正则语义,而不是"现代化"它。

2. **系统字体 + SF Symbols,不捆 MiSans**。Qt 版捆 MiSans 是因为 Qt 三平台字体渲染不一致;
   原生 macOS 用系统字体才对(光学字号、跟随明暗、CJK 回退、少 10 MB)。

3. **i18n 用中文源串直接当 key**,翻译表是扁平的 `中文 → 目标语` 映射,与 Qt 版**共用同一批
   JSON**。漏翻时回落到中文,不是显示 key 或空白。加新文案后跑 `scripts/i18n_check.py` 看覆盖率。

4. **局域网设备代理是零配置透明代理**,不是让用户去设备上填代理地址。
   链路:ARP 欺骗 → 内核转发 → PF 重定向到 mihomo 的 redir-port。**不需要 lwIP**——
   Qt 版那 13 万行用户态 TCP/IP 栈,在 macOS 上被内核转发 + PF 省掉了。详见
   [`docs/gateway-evaluation.md`](docs/gateway-evaluation.md)。

5. **复原优先**。被 ARP 欺骗的设备把本机当网关,一旦停止转发就断网(ARP 缓存十几分钟才过期)。
   所以整个欺骗循环跑在 helper 里,XPC 连接一断(app 崩/被杀)就自动复原;复原发真网关 MAC、
   发三遍加冗余;`ARPPacket` 的复原字节有单测逐字节钉住。这是全项目最危险的一处。

## helper(特权 daemon)

`com.yuehongsun.coast.helper`,以 root 跑,做四件需要 root 的事:设系统代理(免密)、
以 root 起核心(TUN 依赖它)、ARP 欺骗 + PF 重定向(设备代理)、复原。

- **名字四者一致**:launchd Label = mach service 名 = helper bundle id = codesign `-i`。
- **唯一的门是 `setCodeSigningRequirement`**:没有它,任何本机进程都能驱动一个 root 服务。
- **ad-hoc 签名装不了它**(`TeamIdentifier=not set`,不满足客户端签名要求)。本地开发只能验到
  「包打对了、系统认出了 daemon」,真正可用的 helper 要正式开发者证书签 —— 走外部仓库
  `integemjack/schat.build`,或本仓库 CI 配好 `MACOS_CERT_*` secret 后自签。

## 打包与发布

- `scripts/make_app.sh` 产出 `Coast.app`:嵌 helper、daemon plist、种子资源(从
  `clashauto-c++/assets` 拷)、hardened-runtime entitlements、签名。`--universal` 出双架构,
  `--sign <id>` 真签(不带则 ad-hoc)。
- CI:`.github/workflows/release.yml` 的 `macos-swift` job **完全独立于 Qt 流水线**。
  macOS 发布包**只出 Swift 版**(`trigger-mac` 已停用)—— 因为 app 内一键更新按扩展名挑第一个
  `.dmg`,两个 macOS 包并存会让老版本挑到哪个纯看运气。

## 自检钩子(`COAST_*_SELFTEST`)

有些路径**只在打包后的真实 .app 里才走得到**,单测验不了。这几个钩子跑完即退,不建 GUI:

| 环境变量 | 验什么 |
|---|---|
| `COAST_PATHS_SELFTEST=1` | 种子资源从 `.app/Contents/Resources` 解析(不是回退到仓库路径) |
| `COAST_TOPO_SELFTEST=1` | 默认网关三要素(IP/MAC/接口)取得到 —— 接管与复原的全部依赖 |
| `COAST_HELPER_SELFTEST=1` | helper 的 daemon plist + 可执行文件在位、SMAppService 认出它 |
| `COAST_SYSPROXY_SELFTEST=1` | 系统代理读写机制(需授权;ad-hoc 下判 SKIP) |
| `COAST_NO_AUTOSTART=1` | 启动时**不自动起核心**(本地调 UI 不想动真实系统代理) |
| `COAST_INITIAL_PAGE=<0..6>` `COAST_LANG=<码>` | 指定起始页/语言,便于复现地截图验证 |

`scripts/regression.sh` 把它们串成一条命令,分 **PASS / FAIL / SKIP** 三类 ——
「环境验不了」(如 ad-hoc 装不了 helper)判 SKIP 不算失败,只有真失败才退非 0。

## 现状与欠账

- **本地能验的全绿**:单测 + 打包 + 全套自检(`bash scripts/regression.sh`)。
- ⚠️ **真机联调未做**:零配置设备代理的「开开关即接管、关开关即恢复」需要**正式签名的
  helper**(ad-hoc 装不了),得等签名构建 —— 本地只验到「网关三要素取得到、配置规则对、
  ARP 报文字节对」。
- ⚠️ **翻译**:en-US 与 zh-CN(源语言)完整,其余 11 种语言机器翻译,欢迎母语者复核。
