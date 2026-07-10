# UI 实现差异表 + Qt 样式常量

> 对照对象：`legacy-ui-spec.md`（旧项目实测尺寸） ↔ 现有 `src/MainWindow.cpp`（`appStyle()` + 各 `buildXxxPage()`）。
> 图例：✅ 符合 · ⚠️ 数值/样式不符 · ❌ 未实现或结构缺失 · ➖ 不适用/无法在 Qt 复刻。
> 「结构性」= 需改布局/控件代码，非改 QSS 能解决；「样式性」= 可直接在 `appStyle()` 里改。

---

## 一、总览（按页面）

> **本轮已实现更新（2026-07）**：批 A/B/C/D 已合入 `MainWindow.cpp`，并给 `SubscriptionStore` 加了 `editSubscription`/`removeSubscription`（CLI `--edit-subscription` / `--remove-subscription` 已验证）。下表「本轮后」为当前状态。

| 页面 | 初评 | 本轮后 | 说明 |
|---|---|---|---|
| 窗口/外壳 | ✅ | ✅ | 外圆角已改 10；毛玻璃 blur 仍未做（➖） |
| 标题栏 header | ⚠️ | ✅ | 按钮已换字形（－/❐/✕）；宽 42 与 mini 略差（可忽略） |
| 侧边栏 sider | ⚠️ | ✅ | 菜单 5px 间距 + hover 右移 + **选中蓝左条**；logo 透明底 + **状态变色已接线**（TUN红/核心黄/关白） |
| 页脚 footer | ✅ | ✅ | 加在线 ◉ / 箭头 › 图标 + 圆点 **1s 呼吸动画**（QPropertyAnimation 透明度） |
| 状态页 status | ⚠️ 结构 | ✅ | 左右 **50:50**、卡片图标 30、间距 10、徽标 120、节点选中蓝左条；⚡ 测速按钮；折线图**按 tubiao.js**（平滑左滚 50ms/点步 1s + 四分网格 + 右侧速度刻度 + 阶梯自适应最大值）；**节点列表宽度自适应**（搜索框占满、名称随列宽伸缩、无横向滚动条） |
| 订阅页 sub | ❌ 结构 | ✅ | **3 列卡片网格** + 「+」新增卡 + 6 圆按钮；启用/查看/更新/编辑/删除/**测速**全功能（测速调 `/proxies/{name}/delay`，需核心运行） |
| 设置页 setting | ❌ 结构 | ✅ | 5 tab + 系统分组(h1+虚线) + **区域/规则表格真 CRUD**（持久化 `rules.json`，增/改/删对话框）；**区域/规则已注入 full.yaml**（`ConfigBuilder::applyCustomRules`，改动后自动 `rebuildConfig` 热重载）；系统 tab 新增「托盘/启动」分组：**关闭到托盘**(`mini`)、**开机自启**(`sys`，Win 注册表 Run) |
| 日志页 logs | ❌ 结构 | ✅ | 双 tab(主/Clash) + 打开日志目录 + **圆点竖轴时间线**（自绘 rail，按级别着色，最新在顶） |
| 会员页 vip | ❌ 结构 | ➖ 已移除 | **按需求整页删除**：`buildVipPage`、侧栏「VIP」菜单项、`#vip*`/`#machineId` 样式、设备码/分享逻辑及 `QClipboard`/`QSysInfo` include 全部移除；菜单与 `m_pages` 索引保持对齐（状态/订阅/设置/日志/关于 = 0–4） |
| 关于页 about | ❌ 结构 | ✅ | 版本(绿粗) + 红色可点链接 + Exe/Root 路径 + **检查更新按钮** |
| 主题 | ❌ | ✅ | **深/浅双主题 + 切换**（启动读 config，设置保存实时切换） |
| 更新弹窗 update | ❌ | ✅ | 独立 **QDialog 600×660**：左侧 tabs 正式/测试版 + 版本说明 + 资源列表 + 进度条(26) + 底部；拉 GitHub releases，「打开下载页」 + **双击资源应用内下载**（进度条 + 跟随重定向 + 存至「下载」目录并打开所在文件夹） |

### 仍未做（明确清单）
- **区域/规则 → full.yaml 路由注入**：✅ 已完成。`ConfigBuilder::applyCustomRules` 读取 `userDir/rules.json`：`area` 项按正则匹配节点名生成自定义 `proxy-group`（原名，非旧项目的「 自定义」后缀，因 C++ 规则目标为自由文本、需与组名一致）并加入首个选择组；`rule` 项按 `MATCH,<node>` / `<type>,<value>,<node>` 前插到 `rules:` 顶部。**验证**：本机无 Qt/核心可跑，改用 PyYAML 对等移植的字符串手术在真实 `default.yaml` 上跑通（区域分组/去重/选择组接线/规则前插均断言通过、输出可被 YAML 解析）。
- **会员页已按需求整页移除**（不再实现会员/VIP 相关任何功能）。
- 更新弹窗的**自更新（下载后自动替换重启）**：当前为「应用内下载到『下载』目录 + 打开文件夹」，未做静默替换；标题栏按钮宽度 42 vs mini（可忽略）。

### 节点测速（本轮新增，已实现）
- `ClashService::testDelays()`（测当前分组）+ `testNodeDelays(names)`（测指定名单），逐个 GET `/proxies/{name}/delay?timeout=5000&url=...`，全部返回后 `pollNodes()` 刷新延迟色。
- 状态页节点区加 ⚡ 按钮（测当前分组）；订阅卡 ⚡ 测该订阅节点。
- 前置条件：Clash 核心运行、REST API 可达；离线时请求静默失败后仍会刷新列表。**未做真机联测**（需真实订阅节点产生外连，未在本机触发）。

---

## 二、外壳 / 标题栏 / 侧边栏 / 页脚

### 2.1 窗口外壳

| 项 | 规格 | 现状 (`MainWindow.cpp`) | 状态 |
|---|---|---|---|
| 主窗尺寸 | 900×510 min | `MainWidth/Height=900/510`+`setMinimumSize` | ✅ |
| 无边框/透明 | frame:false | `FramelessWindowHint`+`WA_TranslucentBackground` | ✅ |
| 外层圆角 | body 10px、#main 5px | `#root border-radius:5px` | ⚠️ 外圆角应 10 |
| 页脚预留 | `#main bottom:38px` | footer 固定 38 在 VBox | ✅ 等价 |
| 内容内边距 | `.main.two padding:10px` | `rightPane margins(10,10,10,0)`+页面 `(0,0,0,10)` | ✅ 等价 |
| 背景毛玻璃 | `filter:blur(100px);margin:-30px` | 无 | ➖ Qt 难复刻 |

### 2.2 标题栏（28px）

| 项 | 规格 | 现状 | 状态 |
|---|---|---|---|
| 高度 | 28 | `TitleHeight=28` | ✅ |
| 背景 | #222，顶圆角 5 | `#titleBar #222` 顶圆角 5 | ✅ |
| 标题 span | 12px #ccc，padding-left 10 | `#windowTitle 12px #ccc`+layout margins(10,0,0,0) | ✅ |
| 按钮 | mini，`height:28 padding:0 10px radius:0`，图标 minus/copy-document/close | `fixedSize(42,28) radius:0`，文本 `-`/`[]`/`X` | ⚠️ 图标缺、宽度 42 |
| 关闭 hover | 红 | `#closeButton:hover red` | ✅ |

### 2.3 侧边栏（120px）

| 项 | 规格 | 现状 | 状态 |
|---|---|---|---|
| 宽度/背景 | 120 / #303032 | `SidebarWidth=120` / `#sidebar #303032` | ✅ |
| logo 圆底 | 80×80 radius 80 | `#logo 80×80 radius:40` | ⚠️ radius 应=40? 见注 |
| logo 图标 | font 70，**透明底**，状态变色（TUN红/代理黄/白） | 实心黄圆 `#ffff00`+暗云字 font 54 | ⚠️ 造型不符、无状态色 |
| 菜单项尺寸 | 115×?，`padding:10 0 10 35`，radius `5 0 0 5`，font 14 | `#menuButton fixedSize(115,40)`,padding-left 35,radius `5 0 0 5`,font14 | ✅ |
| 菜单项间距 | margin-bottom **5px** | layout `setSpacing(0)` | ❌ 缺 5px |
| 菜单 hover | bg rgb(62,62,62) + **padding-left 39**（右移4） | bg rgb(62,62,62)，无右移 | ⚠️ 缺右移 |
| 菜单选中 | bg#000 #ccc + **box-shadow inset 3px 0 0 #4898f8**（蓝左条） | bg#000 #ccc，**无蓝左条** | ❌ 缺蓝条 |
| 版本号 | 底部 width120 font12 #666 | `#version #666 12` 底部 | ✅ |

> 注：logo 规格 `border-radius:80px` 与 80×80 等效于全圆，现 `radius:40` 同样是全圆（半径=边长一半），视觉一致；真正差异是**填充造型**与**状态变色**。

### 2.4 页脚（38px）

| 项 | 规格 | 现状 | 状态 |
|---|---|---|---|
| 高/背景/内边距 | 38 / #303032 / `0 5 0 120` | `FooterHeight=38` `#footer #303032` margins(120,0,5,0) | ✅ |
| 底圆角 | 5 | ✅ | ✅ |
| 在线徽标 | radius3 padding`3 5` | `#usersBadge radius3 padding3 6` | ⚠️ padding 6→5 |
| 开关块 openOrClose | `padding:5 10 4 5` radius3 font12 bg#000 | `#switchButton radius3 font12 bg#000` padding-l22 r8 | ✅ 近似 |
| 圆点 quan | 外16(含padding4) 内8；off内#666 on#4898f8 | `#switchDot 16×16 border4`；off `rgba(102,102,102,.15)` on #4898f8 | ⚠️ off 无实心灰内点 |
| 圆点动画 | `color_black 1s infinite` 呼吸 | 无 | ➖ 可选 QPropertyAnimation |
| 模式选择 | width120 mini | `mode fixedWidth(120)` | ✅ |
| 在线/箭头图标 | icon-zaixian1 / icon-arrow-2 | `-` / `|` | ⚠️ 图标缺 |

---

## 三、各页面差异

### 3.1 状态页 status

| 项 | 规格 | 现状 | 状态 |
|---|---|---|---|
| 左右分栏 | 左 50% / 右 50% | `addWidget(left,5)+ (right,1)` ≈83:17 | ❌ 比例应 1:1 |
| 流量卡网格 | 2×2，gutter 10 | `QGridLayout` H/V spacing **16** | ⚠️ 间距 16→10 |
| 卡片图标 | font **30** padding-top5 | `#metricIcon font 44` | ⚠️ 44→30 |
| 卡片标题/数值 | 12 / 20 | 12 / 20 | ✅ |
| 卡片体内边距 | `8px 10px` | `metricCard margins(14,18,14,18)` | ⚠️ |
| 卡片配色 | up/down/process/download 四色 | 完全一致 | ✅ |
| 卡片背景 | #222 | #222 | ✅ |
| 折线图区 | 上下各 50% 高 | 两 `TrafficChart` stretch 1 | ✅ |
| 节点标题 | h1 height30 line30；计数 span 9px | `#sectionTitle 18`，计数 `<span 9px>` | ✅ 近似 |
| 搜索框 | 内联 width 80% + append 按钮 | `m_nodeSearch fixedWidth(180)` | ⚠️ 定宽 |
| 分组选择 | 下拉 dropdown | `m_nodeGroupSelector combo 130` | ✅ 等价 |
| 节点行 | 名 span19 / 徽标 width120 / 按钮 span5 | `nodeRow minH56` `delayBadge width104` `nodeButton 82×38` | ⚠️ 徽标 120→104 |
| 节点选中 | 蓝左条 inset3 #83bdff，底 #4897f8b0(.69) | 整块 `rgba(72,151,248,0.86)` | ⚠️ 无左条、透明度不符 |
| 节点行间距 | margin-bottom10 | spacing 10 | ✅ |
| 节点全览弹窗 | dialog 80% 两列 | 无（仅订阅有节点弹窗） | ❌ |

### 3.2 订阅页 sub

| 项 | 规格 | 现状 | 状态 |
|---|---|---|---|
| 列表布局 | **3 列卡片网格** span8，卡体 height 87 | 单列 `createSubscriptionRow` nodeRow minH56 | ❌ 结构 |
| 「+新增」卡 | height87 line82 font80 居中 | Add 按钮 82 宽 | ❌ |
| 行操作按钮 | 6 个 circle（启用/测速/查看/编辑/更新/删除）margin-left3 | 3 个方按钮(Nodes/Update/Use)82×38 | ❌ |
| 底部栏 | height30，左右 mini 按钮 | 顶部 Add/Reload/Apply 82 宽 | ⚠️ 位置/构成不符 |
| 新增/编辑对话框 | 70%，radio(sub/clash)+3 输入框，选文件按钮 | `QInputDialog` 连续三次输入 | ❌ |
| 节点详情弹窗 | 80% **两列** 卡，延迟徽标 width50 | `dialog 620×420` 单列 | ⚠️ 单列 |

### 3.3 设置页 setting

| 项 | 规格 | 现状 | 状态 |
|---|---|---|---|
| Tab 数 | 5（远程/过滤/区域/规则/系统） | 3（UI/System/Node filter） | ❌ |
| 系统表单项 | `width 300` inline-block padding`0 10` mb10 | `QFormLayout` 默认 | ⚠️ |
| 分组标题 | h1 `font 24 bolder` + `border-bottom 1px dashed` | 无分组 | ❌ |
| 下拉/端口宽 | select/port width **200** | 默认 | ⚠️ |
| 区域/规则表格 | `el-table` 列宽 180/180/auto/100 + 分页 page-size5 | 无 | ❌ |
| 应用按钮 | 右上绝对 `top5 right5` mini text | 底部右对齐 `Save 120` | ⚠️ 位置 |
| Tab 样式 | item #999，选中 #fff + `border-bottom 2px #4898f8` | `QTabBar::tab` 同 | ✅ |

### 3.4 日志页 logs

| 项 | 规格 | 现状 | 状态 |
|---|---|---|---|
| 结构 | 双 tab(main/clash) + `el-timeline` | 单 `QTextEdit #logView` bg#111 #ccc | ❌ 结构 |
| 打开目录按钮 | 右上 mini text | 无 | ❌ |
| 滚动高度 | `dom.height-73` | 自适应 | ➖ |

### 3.5 会员页 vip —— ➖ 已按需求移除，不再对照

### 3.6 关于页 about

| 项 | 规格 | 现状 | 状态 |
|---|---|---|---|
| 结构 | 版本+链接列表 `line-height40` | 居中 `#aboutTitle 28` + desc | ❌ 结构 |
| 版本号 | green bold；链接 red；New/Beta 角标 | 无 | ❌ |

### 3.7 主题

| 项 | 规格 | 现状 | 状态 |
|---|---|---|---|
| 深色 | 全套 black.css | `appStyle()` 深色 | ✅ 主要覆盖 |
| 浅色 | 全套 white.css | 无 | ❌ 未实现 |
| 主题切换 | 挂 `#main.black/.white` | 无切换 | ❌ |

---

## 四、可直接套用的 Qt 样式常量（供粘贴）

> 以下修正**只针对「样式性」差距**（⚠️ 中能靠 QSS 解决的部分）。带 `/* NEW */` 为新增或改动行。结构性 ❌ 项在 [4.4](#44-需改布局代码的项非-qss-能解决) 单列，不在此。

### 4.1 尺寸常量（替换 `MainWindow.cpp` 顶部匿名 namespace）

```cpp
namespace {
constexpr int TitleHeight  = 28;
constexpr int SidebarWidth = 120;
constexpr int FooterHeight = 38;
constexpr int Radius       = 5;
constexpr int OuterRadius  = 10;   /* NEW: body 外圆角，规格 10 */
constexpr int MainWidth    = 900;
constexpr int MainHeight   = 510;

constexpr int MenuItemW    = 115;  /* NEW */
constexpr int MenuItemH    = 40;   /* NEW */
constexpr int MenuSpacing  = 5;    /* NEW: 菜单项 margin-bottom */
constexpr int LogoSize     = 80;   /* NEW */
constexpr int MetricGutter = 10;   /* NEW: 规格 gutter，替换现 16 */
constexpr int NodeBtnW     = 82;   /* NEW */
constexpr int NodeBtnH     = 38;   /* NEW */
constexpr int DelayBadgeW  = 120;  /* NEW: 规格速度/延迟列宽，替换现 104 */
}
```

配合的布局改动（一行级）：
```cpp
// buildSidebar(): 菜单项之间加 5px
layout->setSpacing(0);            // 保持；改为逐项 addSpacing 或统一 setSpacing(MenuSpacing)
// —— 推荐：把 logo 与菜单分区，仅菜单区 setSpacing(MenuSpacing)

// buildStatusPage(): 流量卡间距 16 → 10
metrics->setHorizontalSpacing(MetricGutter);
metrics->setVerticalSpacing(MetricGutter);

// buildStatusPage(): 左右 50:50（规格），替换 5 / 1
layout->addWidget(left, 1);
layout->addWidget(right, 1);

// createNodeRow(): 徽标宽 104 → 120
delay->setFixedWidth(DelayBadgeW);
```

### 4.2 深色 `appStyle()`（修正版，补齐蓝左条 / hover 右移 / logo / 圆角）

```cpp
QString MainWindow::appStyle() const
{
    return R"(
        #root { background:#242425; border-radius:10px; }                       /* 外圆角 5→10 */
        #titleBar { background:#222; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#ccc; font-size:12px; padding-left:0; }
        #titleButton, #closeButton { color:#ccc; background:#222; border:0; border-radius:0; }
        #titleButton:hover { background:#333; }
        #closeButton:hover { background:red; color:white; }
        #body, #rightPane, #page { background:rgba(0,0,0,0); }
        #sidebar { background:#303032; }
        /* logo：透明底 + 状态变色（默认代理黄）。红色态用 setProperty("state","tun") 切换 */
        #logo { color:#ffff00; background:transparent; min-width:80px; max-width:80px; min-height:80px; max-height:80px; font-size:70px; }
        #logo[state="tun"]   { color:#ff0000; }
        #logo[state="proxy"] { color:#ffff00; }
        #logo[state="off"]   { color:#ffffff; }
        /* 菜单：radius 5 0 0 5，hover 右移到 39，选中蓝左条 */
        #menuButton { color:#fff; background:transparent; border:0; border-left:3px solid transparent; border-radius:5px 0 0 5px; text-align:left; padding-left:32px; font-size:14px; }
        #menuButton:hover   { background:rgb(62,62,62); padding-left:36px; }        /* 39-3(border) 视觉右移 */
        #menuButton:checked { background:#000; color:#ccc; border-left:3px solid #4898f8; }   /* 蓝左条 */
        #version { color:#666; font-size:12px; }
        #metricCard { background:#222; border:0; border-radius:4px; min-height:70px; }
        #metricIcon { font-size:30px; color:#aaa; }                              /* 44→30 */
        #metricTitle { color:#bfbfbf; font-size:12px; }
        #metricValue { color:#bfbfbf; font-size:20px; }
        #metricCard[kind="up"] QLabel { color:rgb(168,67,67); }
        #metricCard[kind="down"] QLabel { color:rgb(77,161,62); }
        #metricCard[kind="process"] QLabel { color:rgb(70,110,168); }
        #metricCard[kind="download"] QLabel { color:rgb(72,165,167); }
        #sectionTitle { color:#eee; font-size:18px; font-weight:400; }
        QScrollArea { background:transparent; border:0; }
        QScrollBar:vertical { background:#242425; width:10px; margin:0; }
        QScrollBar::handle:vertical { background:#555; border-radius:5px; min-height:24px; }
        QLineEdit, QComboBox, QTextEdit { color:#fff; background:#444; border:1px solid #333; border-radius:3px; min-height:28px; padding:0 8px; }
        QLineEdit:focus, QComboBox:focus { border:1px solid #4898f8; }            /* 规格 focus 蓝框 */
        QTabWidget::pane { border:0; }
        QTabBar::tab { color:#999; background:transparent; padding:8px 16px; }
        QTabBar::tab:selected { color:#fff; border-bottom:2px solid #4898f8; }
        QCheckBox, QLabel { color:#ccc; }
        #iconButton { color:#4898f8; background:transparent; border:0; font-size:18px; }
        /* 节点/订阅行：选中改用蓝左条 + 半透明底(#4897f8b0=0.69) */
        #nodeRow, #plainCard { background:#252525; border:0; border-left:3px solid transparent; border-radius:0; min-height:56px; }
        #nodeRow[active="true"] { background:rgba(72,151,248,0.69); border-left:3px solid #83bdff; }
        #nodeName { color:#ccc; font-size:12px; }
        #delayBadge { color:#222; border-radius:5px; padding:3px 5px; font-size:12px; }
        #nodeButton { color:#ccc; background:transparent; border:0; border-left:1px solid #000; border-radius:0; font-size:12px; }
        #nodeButton:hover { background:#333; }
        #primaryButton { color:white; background:#4898f8; border:0; border-radius:4px; min-height:30px; }
        #footer { background:#303032; border-bottom-left-radius:10px; border-bottom-right-radius:10px; }
        #usersBadge { color:#fff; background:#000; border-radius:3px; padding:3px 5px; font-size:12px; }   /* 6→5 */
        #footerLog { color:#ccc; font-size:12px; }
        #switchButton { color:#eee; background:#000; border:0; border-radius:3px; min-height:28px; padding-left:22px; padding-right:8px; font-size:12px; }
        #switchDot { background:rgba(102,102,102,0.15); border:4px solid rgba(102,102,102,0.15); border-radius:8px; }
        #switchDot[on="true"] { background:#4898f8; border-color:rgba(72,152,248,0.30); }
        #logView { background:#111; color:#ccc; border:0; }
        #aboutTitle { color:#eee; font-size:28px; }
    )";
}
```

> 说明：QSS 的 `:hover { padding-left }` 会随光标切换，视觉上等价规格的「+4px 右移」；蓝左条用 `border-left:3px solid transparent → #4898f8/#83bdff` 实现（规格是 `box-shadow inset`，Qt QSS 不支持 inset shadow，故用等宽 border 顶替，占位透明避免选中时整体位移）。logo 状态色需在状态回调里 `m_logo->setProperty("state", ...)` 后 `unpolish/polish`（同现有 `#switchDot` 的做法）。

### 4.3 浅色主题 `lightStyle()`（新增，规格 white.css）

新增方法（`MainWindow.h` 声明 `QString lightStyle() const;`），按 `config.theme` 选择 `setStyleSheet(theme==light ? lightStyle() : appStyle())`：

```cpp
QString MainWindow::lightStyle() const
{
    return R"(
        #root { background:#ffffff; border-radius:10px; }
        #titleBar { background:#eee; border-top-left-radius:10px; border-top-right-radius:10px; }
        #windowTitle { color:#333; font-size:12px; }
        #titleButton, #closeButton { color:#333; background:#eee; border:0; border-radius:0; }
        #titleButton:hover { background:#fff; }
        #closeButton:hover { background:red; color:white; }
        #body, #rightPane, #page { background:rgba(0,0,0,0); }
        #sidebar { background:#fafafa; }
        #logo { color:#ffff00; background:transparent; min-width:80px; max-width:80px; min-height:80px; max-height:80px; font-size:70px; }
        #logo[state="tun"] { color:#ff0000; } #logo[state="proxy"] { color:#ffff00; } #logo[state="off"] { color:#333; }
        #menuButton { color:#333; background:transparent; border:0; border-left:3px solid transparent; border-radius:5px 0 0 5px; text-align:left; padding-left:32px; font-size:14px; }
        #menuButton:hover   { background:rgb(210,210,210); padding-left:36px; }
        #menuButton:checked { background:#fff; color:#333; border-left:3px solid #4898f8; }
        #version { color:#666; font-size:12px; }
        #metricCard { background:#eee; border:0; border-radius:4px; min-height:70px; }
        #metricIcon { font-size:30px; color:#888; }
        #metricTitle { color:#3d3d3d; font-size:12px; }
        #metricValue { color:#3d3d3d; font-size:20px; }
        #metricCard[kind="up"] QLabel { color:rgb(168,67,67); }
        #metricCard[kind="down"] QLabel { color:rgb(77,161,62); }
        #metricCard[kind="process"] QLabel { color:rgb(70,110,168); }
        #metricCard[kind="download"] QLabel { color:rgb(72,165,167); }
        #sectionTitle { color:#111; font-size:18px; font-weight:400; }
        QScrollArea { background:transparent; border:0; }
        QScrollBar:vertical { background:#fff; width:10px; margin:0; }
        QScrollBar::handle:vertical { background:#ccc; border-radius:5px; min-height:24px; }
        QLineEdit, QComboBox, QTextEdit { color:#333; background:#eaeaea; border:1px solid #ccc; border-radius:3px; min-height:28px; padding:0 8px; }
        QLineEdit:focus, QComboBox:focus { border:1px solid #4898f8; }
        QTabWidget::pane { border:0; }
        QTabBar::tab { color:#999; background:transparent; padding:8px 16px; }
        QTabBar::tab:selected { color:#333; border-bottom:2px solid #4898f8; }
        QCheckBox, QLabel { color:#333; }
        #iconButton { color:#4898f8; background:transparent; border:0; font-size:18px; }
        #nodeRow, #plainCard { background:#eee; border:0; border-left:3px solid transparent; border-radius:0; min-height:56px; }
        #nodeRow[active="true"] { background:rgba(72,151,248,0.69); border-left:3px solid #1f6fd2; }
        #nodeName { color:#333; font-size:12px; }
        #delayBadge { color:#333; border-radius:5px; padding:3px 5px; font-size:12px; }
        #nodeButton { color:#333; background:transparent; border:0; border-left:1px solid #fff; border-radius:0; font-size:12px; }
        #nodeButton:hover { background:#c1c1c1; }
        #primaryButton { color:white; background:#4898f8; border:0; border-radius:4px; min-height:30px; }
        #footer { background:#fafafa; border-bottom-left-radius:10px; border-bottom-right-radius:10px; }
        #usersBadge { color:#333; background:#fff; border-radius:3px; padding:3px 5px; font-size:12px; }
        #footerLog { color:#333; font-size:12px; }
        #switchButton { color:#333; background:#eee; border:0; border-radius:3px; min-height:28px; padding-left:22px; padding-right:8px; font-size:12px; }
        #switchDot { background:rgba(255,255,255,0.15); border:4px solid rgba(255,255,255,0.15); border-radius:8px; }
        #switchDot[on="true"] { background:#4898f8; border-color:rgba(72,152,248,0.30); }
        #logView { background:#fff; color:#333; border:0; }
        #aboutTitle { color:#111; font-size:28px; }
    )";
}
```

> 浅色 `#nodeRow[active]` 左条用 `#1f6fd2`（规格 white 的 `inset 3px 0 0 #1f6fd2`）；深色用 `#83bdff`。其余色值取自 `white.css` 实测（侧栏 #fafafa、卡 #eee、输入 #eaeaea/#ccc 等）。

### 4.4 需改布局代码的项（非 QSS 能解决）

| 项 | 需要的改动 |
|---|---|
| 状态页 50:50 | `layout->addWidget(left,1); addWidget(right,1)`（见 4.1） |
| 侧栏菜单 5px 间距 | 菜单区独立布局 `setSpacing(5)` |
| 圆点呼吸动画 | 给 `#switchDot[on]` 加 `QPropertyAnimation`（可选） |
| 订阅页 3 列网格 | `QGridLayout` 每行 3 卡；卡体 87px；6 个 circle 按钮 |
| 订阅新增/编辑对话框 | 自定义 `QDialog`：radio(sub/clash)+name/url/updateTime |
| 设置页 5 tab + 表格 | 加「区域/规则」`QTableWidget`（列 180/180/auto/100）+ 分页；系统页分组 `h1+虚线` |
| 日志页双 tab 时间线 | `QTabWidget` main/clash + 时间线样式 + 打开目录按钮 |
| 关于页 | 按 include/about.vue 结构重搭（会员页已移除） |
| logo 状态变色 | 状态回调里 `m_logo->setProperty("state", tun?"tun":proxy?"proxy":"off")` + unpolish/polish |
| 主题切换 | 读 `config.theme`，`setStyleSheet(light?lightStyle():appStyle())` |

---

### 建议落地顺序

1. **样式性快赢**（4.1+4.2）：菜单蓝左条 / hover 右移 / 卡片图标 30 / 状态页 50:50 / 徽标宽 120 / 外圆角 10 —— 改动小、观感提升明显。
2. **浅色主题**（4.3）：加 `lightStyle()` + 按 `config.theme` 切换。
3. **结构性**（4.4）：按 订阅网格 → 设置表格 → 日志时间线 → 关于 的顺序补齐（会员页已移除）。
