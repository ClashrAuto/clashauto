# 旧项目 UI 尺寸规格（Clashr-Auto Electron/Vue → C++/Qt 1:1 复刻参考）

> 来源：`Clashr-Auto/src/renderer/**` 与 `Clashr-Auto/src/main/**`。所有数值均从原项目 `<style>` / 内联 `style` / element-ui 属性中**逐字**摘录，未做取整或近似。
> 单位默认 `px`。`!important` 已保留。被源码注释掉（不生效）的值标注为「(注释)」。
>
> 目录：
> 1. [窗口尺寸](#1-窗口尺寸-browserwindow)
> 2. [全局共享外壳（每个页面都有）](#2-全局共享外壳)
> 3. [侧边栏 sider / 页脚 footer / 标题栏 header](#3-侧边栏-页脚-标题栏)
> 4. [主题色 black / white](#4-主题色)
> 5. 各页面专属：[状态页](#51-状态页-statusvue) · [订阅页](#52-订阅页-subvue) · [设置页](#53-设置页-settingvue) · [日志页](#54-日志页-logsvue) · [连接页](#55-连接页-connectionsvue) · [会员页](#56-会员页-includevipvue) · [关于页](#57-关于页-includeaboutvue) · [托盘](#58-托盘-trayvue) · [更新弹窗](#59-更新弹窗-updatevue)

---

## 1. 窗口尺寸 (BrowserWindow)

| 窗口 | 宽 | 高 | min 宽 | min 高 | 备注 |
|---|---|---|---|---|---|
| 主窗口（status/sub/setting/logs/vip/about） | **900** | **510** | 900 | 510 | `useContentSize:true`、`frame:false`、`autoHideMenuBar:true`、`backgroundColor` 深色 `#00000000`/浅色 `#ffffffff` |
| 托盘窗口 tray | **350** | **612**（macOS **625**） | 350 | 612/625 | `frame:false` |
| 连接窗口 connections | **900** | **600** | 900 | 600 | 独立窗口 |
| 更新窗口 update | **600** | **660** | — | — | `frame:false`；脚本按状态改尺寸：有更新 `600×540`、无更新 `350×150`、下载中 `400×150` |

- 标题栏拖拽：整窗 `-webkit-app-region: drag`，交互控件区 `no-drag`。
- Windows 顶栏用 `el-header height=28px`；macOS 无顶栏，改为 `el-aside padding-top:28px`（给红绿灯让位）；Linux 顶栏隐藏且窗口无圆角。

---

## 2. 全局共享外壳

> 以下规则在 `status.vue` 的**非 scoped** `<style>` 中定义，并被**逐字复制**进 `sub / setting / logs / vip / about` 各页面。即这 6 个主页面共享同一套外壳样式。`connections`、`tray`、`update` 是独立窗口，样式各自独立（见各自小节）。

### 2.1 窗口/根容器

| 选择器 | 属性值 |
|---|---|
| `body, html` | overflow: hidden !important; background-color: RGBA(0,0,0,0) !important; **border-radius: 10px !important**; position: fixed; top/bottom/left/right: 0 |
| `div#main` | position: fixed; top:0; left:0; right:0; **bottom: 38px**; overflow: hidden; **border-top-left/right-radius: 5px**；(注释 height:600px) |
| `div#main.linux` | border-radius: 0px !important |
| `.el-container.main` | overflow: hidden; padding: 0; height: 100%; position: relative; bottom: 0 !important |
| `.el-container.main::before` | content:''; 绝对定位铺满; **filter: blur(100px); margin: -30px**（背景毛玻璃） |
| `.el-container.main.two` | **padding: 10px** |
| `.el-container` | -webkit-app-region: no-drag |
| `section.el-container` | position: relative |
| `.div` | position: absolute; top/right/bottom/left: 0（主内容绝对定位铺满层） |
| `.div > .el-row` | height: 100% |
| `.el-main` | padding: 0 !important; margin-bottom: 10px |
| `.v-modal` | display: none（禁用 element-ui 遮罩） |

### 2.2 顶栏 header（`.el-header`，高 28px）

| 选择器 | 属性值 |
|---|---|
| `.el-header`（第一处） | margin-bottom: 0 !important; -webkit-app-region: drag; display: flex; justify-content: space-between; align-items: center |
| `.el-header`（第二处） | padding: 0 !important; margin-bottom: 10px |
| `.el-header > span` | font-size: 12px; padding: 0 0 0 10px |
| `.el-header .el-button` / `.is-disabled` | **height: 28px**; padding: 0 10px !important; **border-radius: 0 !important**; -webkit-app-region: none |
| `.el-header .el-button.is-disabled(:hover)` | cursor: default |

### 2.3 卡片 / 表单 / 通用组件

| 选择器 | 属性值 |
|---|---|
| `*` | font-family: 'Noto Sans SC', sans-serif !important; margin: 0; padding: 0; user-select: none |
| `.el-card` | **margin-bottom: 10px**; border: none !important; box-shadow: none !important |
| `.el-card__body` | **padding: 8px 10px !important** |
| `.el-card.nodesChoose:last-child` | margin-bottom: 0 |
| `.el-card.use > .el-card__body > .el-row > .el-col > .el-button` | padding: 12px 10px !important |
| `.el-card.tag …:last-child > .el-row > .el-col:first-child` | font-size: 12px（卡片小标题） |
| `.el-card.tag …:last-child > .el-row > .el-col:last-child` | font-size: 20px（卡片数值） |
| `.el-card.tag … > .el-col > i` | **font-size: 30px**; padding-top: 5px; display: block（卡片图标） |
| `.el-form-item` | position: relative |
| `.el-form-item .icon-warm` | display: inline-flex; align-items: center; **font-size: 24px**; color: #8c8c8c |
| `.el-pagination` | margin-top: 10px |
| `.settingBox .el-dialog` | margin: 10px auto !important |
| `.el-scrollbar` | width: 100% |
| `.el-timeline` | padding-left: 3px |
| `.box-card.use` | font-size: 12px |
| `.el-collapse` | border-top: none !important |
| `.el-collapse-item__header` / `__wrap` | border: none !important；`__wrap` padding:0；`__content` padding-bottom:0 !important |
| `.el-checkbox.li` | display: block !important |
| `.el-input-group__append/__prepend` | border: none !important |
| `.el-dialog__wrapper.look .el-dialog__body` | padding: 10px !important |
| `h1.pointer` | font-size: 12px; height: 24px; line-height: 24px; margin-right: 60px; 省略号溢出 |
| `.el-tabs h1` | font-weight: normal; font-size: 14px; margin-bottom: 10px |
| `.sub`（文本） | overflow/text-ellipsis/nowrap |
| `.sub button` | font-size: 9px; padding: 10px 10px !important |
| `.windows.close` | position: fixed; left:8px; top:8px; font-size:16px; **width:16px; height:16px; border-radius:20px**; z-index:1000; background:red |

---

## 3. 侧边栏 · 页脚 · 标题栏

> `sider.vue` / `footer.vue` / `header.vue` 自身几乎无 CSS（空 scoped），实际样式来自第 2 节共享外壳 + 主题色。以下是它们对应的关键尺寸。

### 3.1 侧边栏 sider（`el-aside width=120px`）

| 选择器 | 属性值 |
|---|---|
| `el-aside`（属性） | width: **120px**；darwin 内联 `padding-top: 28px` |
| `.el-aside` | padding-right: 0; padding-top: 5px; -webkit-app-region: drag |
| `.center` | text-align: center; margin-bottom: 10px |
| `.center-end` | margin-bottom: 0 |
| `div.logo` | padding-top: 16px; margin-bottom: 16px |
| `div.logo > div`（圆形 logo 底） | margin: auto; **width: 80px; height: 80px; border-radius: 80px**; flex 居中; -webkit-app-region: drag |
| `div.logo > div > i`（图标） | **font-size: 70px** |
| `div.logo.global > div > i` | color: #ff0000 !important; text-shadow: 0 0 5px #8b2222 !important（TUN 开启=红） |
| `div.logo.proxy > div > i` | color: #ffff00; text-shadow: 0 0 5px #baba36（代理开启=黄） |
| `ul.menu` | margin: auto; **width: 115px**; border-radius: 5px; list-style: none; padding: 5px 0 5px 5px; -webkit-app-region: no-drag |
| `ul.menu > li` | text-align: left; **border-radius: 5px 0 0 5px**; **padding: 10px 0 10px 35px**; margin-bottom: 5px; font-size: 14px; cursor: pointer |
| `ul.menu > li:hover` | 深色 background rgb(62,62,62) / 浅色 rgb(210,210,210)；**padding-left: 39px**（悬停右移 4px） |
| `ul.menu > li.on` | 深 bg #000 / 浅 bg #fff；**box-shadow: inset 3px 0 0 #4898f8**（选中左侧蓝条） |
| `ul.menu > li:last-child` | margin: 0 |
| `li` 过渡 | transition: background .18s, color .18s, padding-left .18s |

菜单项顺序：status / sub(仅 127.0.0.1) / setting / log(仅 127.0.0.1) / vip / about。图标 `<i class="logo iconfont icon-wangluo">`。

### 3.2 页脚 footer（`.el-footer`，高 38px）

| 选择器 | 属性值 |
|---|---|
| `.el-footer` | **padding: 0 5px 0 120px !important**; **height: 38px !important**; font-size: 14px; position: fixed; left/bottom/right: 0; **border-bottom-left/right-radius: 5px**; display: inline-flex; align-items: center; justify-content: space-evenly; -webkit-app-region: drag |
| `.linux .el-footer` | border-bottom-left/right-radius: 0 |
| `.el-footer div.log`（内联 width:100%） | line-height: 38px; font-size: 12px; margin-right: 5px; 省略号 |
| `div.log div`（在线数徽标） | border-radius: 3px; padding: 3px 5px; margin-right: 10px |
| `div.log span` | margin-left: 10px |
| `div.log i` | font-size: 14px; font-weight: 100 |
| `.openOrClose`（TUN/代理/核心开关块） | display: flex 居中; **font-size: 12px !important**; **padding: 5px 10px 4px 5px**; border-radius: 3px; margin-right: 5px; cursor: pointer; -webkit-app-region: no-drag |
| `.openOrClose > span` | display: block; margin-bottom: 2px; white-space: nowrap |
| `.quan`（状态圆点外圈） | **width: 8px; height: 8px; padding: 4px; border-radius: 24px**; margin-right: 5px |
| `.quan > div`（内点） | **width: 8px; height: 8px; border-radius: 8px** |
| `.quan.on` | 同上；开启态 background rgba(72,152,248,0.3)；**animation: color_black 1s infinite**（呼吸） |
| `.quan.on > div` | background: #4898f8 |
| 模式选择器 `el-select`（内联） | **width: 120px**; size="mini"; -webkit-app-region: no-drag |
| `.copy`（版本号） | position: fixed; bottom: 10px; left: 0; **width: 120px**; text-align: center; font-size: 12px; font-weight: 100; cursor: pointer |
| `.boxCenter .el-col` | height: 38px; line-height: 38px; font-size: 12px |

`@keyframes color_black`（呼吸动画）：`from/to` rgba(72,152,248,0.5)，`50%` 深色 rgba(102,102,102,0.15) / 浅色 rgba(200,200,200,0.15)。

### 3.3 标题栏 header

`.header` → width: 100%; margin-bottom: 0 !important; -webkit-app-region: drag; flex 两端对齐居中。三个按钮 `el-button size=mini`：最小化 `el-icon-minus`(info)、最大化 `el-icon-copy-document`(info)、关闭 `el-icon-close`(danger)。

---

## 4. 主题色

> `.black`（深色，默认）/ `.white`（浅色）。挂在 `#main` 上（`div#main.black` / `div#main.white`）。核心强调色 **#4898f8**（蓝）。

### 4.1 关键背景/文字

| 元素 | 深色 black | 浅色 white |
|---|---|---|
| `#main` 背景 | #242425 | #fff |
| `.el-container.main` | rgba(0,0,0,0.8) | rgba(255,255,255,0.8) |
| `.el-aside` 侧栏 | #303032 | #fafafa |
| `.el-footer` 页脚 | #303032 | #fafafa |
| `.el-header` 顶栏 | #222（span #ccc） | #eee（span #333） |
| `.el-card.tag` 流量卡 | #222 !important | #eee !important |
| `.el-card__body` | #333 / 文字 #ccc | #333 / 文字 #333 |
| `.el-input__inner` 输入框 | 文字#fff bg#444 border#333；顶/底 border:none | 文字#333 bg#eaeaea border#ccc |
| `.el-footer .el-input__inner` | bg #111 border #222 | （继承通用） |
| `.el-input__inner:focus` | border-color #4898f8 | border-color #4898f8 |
| `.el-button--default` | bg#444 文字#ccc；hover bg#333 | bg#ccc 文字#333；hover bg#c1c1c1 |
| `.el-dialog` | #333 !important（标题#fff，输入#555） | #fff !important（标题#333，输入#ccc） |
| `.el-dropdown-menu` | 文字#ccc bg#000 border 1px #333 box-shadow 0 2px 12px rgba(255,255,255,.1) | — |

### 4.2 流量卡片配色（`.el-card.tag.*` 文字色，两主题相同）

up `rgb(168,67,67)` · down `rgb(77,161,62)` · process `rgb(70,110,168)` · download `rgb(72,165,167)`。卡内 `.el-row` padding: 20px 0（深色 !important）。

### 4.3 节点卡片 `.box-card.use`（状态页节点行）

| | 深色 | 浅色 |
|---|---|---|
| 默认 bg | #252525 | #eee |
| `.on` 选中 | bg #4897f8b0；box-shadow inset 3px 0 0 **#83bdff** !important | bg #4897f8b0；box-shadow inset 3px 0 0 **#1f6fd2** !important |
| hover | transform: translateY(-1px) | translateY(-1px) |
| 过渡 | background .18s, transform .18s | 同 |
| 分隔 apply 按钮 | border-left 1px #000；width:100%；font-size:12px；左圆角0 | border-left 1px #fff；同 |

### 4.4 订阅卡片 `.sub`（订阅页）

深色：`.sub .el-card__body` #222；`.sub.on .el-card__body` rgba(72,152,248,0.5)；`.sub.mini .el-card__body` rgba(72,152,248,0.5)。
浅色：`.sub .el-card__body` #eee；`.sub.on .el-card__body` #4898f8；`.sub.mini .el-card__body` #4898f8。
过渡 background .18s。

### 4.5 表格 / 分页 / 时间线（主要深色）

`.el-table th/tr` bg#333，边框色 **#464646** !important，文字#fff，hover 行 #222（浅色 hover `#f5f8ff`）；`.el-pagination` 按钮 bg#444 文字#ccc；`.el-timeline-item__tail` border-left 2px #333；`.el-timeline-item__content` #ccc；`.el-tabs__item` #999，`.el-tabs__nav-wrap::after` #333；`.el-input-number` 增减按钮 bg#333 border#000。

> 注：两个主题文件里第 86–174 行附近的一批 `box-shadow` 规则均被**注释**，不生效。`.el-textarea__inner` 在浅色下仍为深色（bg#111）——源码如此。

---

## 5. 各页面专属

### 5.1 状态页 status.vue

布局：`.tubiaoBox` 绝对定位分三块。

| 区域 | 定位/尺寸（内联） |
|---|---|
| 左上·流量卡区 | position: absolute; left:0; right:50%; top:0; **height:200px**; padding: 0 5px 0 0 |
| ├ 卡片栅格 | `el-row :gutter=10`，四张 `el-col :span=12`（2×2）；图标列 span6 / 文本列 span18 |
| ├ process 卡右上按钮 | `.look`：position:absolute; top:0; right:0; border-radius: 0 0 0 5px; font-size:12px |
| │ `i.el-icon-view` | padding: 5px 7px; background RGBA(0,0,0,0.3) |
| │ `i.el-icon-circle-close` | padding: 5px 7px; background rgba(255,0,0,0.3) |
| 左下·实时折线图区 | position: absolute; left:0; right:50%; bottom:0; **top:210px**; padding: 5px 5px 0 0 |
| ├ 标题 h1 | padding: 10px 0 5px 0 |
| ├ canvas 容器 | position: absolute; **top:45px**; left:0; right:5px; bottom:0 |
| └ `canvas ref=up/down` | width:100%; **height:50%**（上下各半） |
| 右侧·节点列表区 | position: absolute; **left:50%**; right:0; top:0; bottom:0; padding: 5px 0 0 5px |
| ├ 标题 h1 | padding: 0 0 5px 0; **height:30px; line-height:30px**；节点计数 span font-size:9px |
| ├ 搜索框 el-input(mini) | margin-left:10px; **width:80%**; float:left；append 按钮 **height:28px**; border-radius: 0 5px 5px 0 |
| ├ `.testNodes i` | color:#4898f8; font-weight:bold |
| ├ `.question i` | color:#7b7b7b; font-size:18px |
| └ 节点 scrollbar | position: absolute; **top:40px**; left:5px; right:15px; bottom:0 |
| 节点行 | `el-row :gutter=10`：名列 `el-col span19`，按钮列 `el-col span5`；速度/延迟外 span width:120px 右对齐，徽标 padding:3px 5px; border-radius:5px；当前节点名 `border-left: 3px solid green; padding-left:5px` |
| 节点全览弹窗 | `el-dialog.look width=80%`；`:top` win32=18px/其它0px；scrollbar height=`dom.height-100`；卡片 `el-col span=12`（2 列） |

图标：`icon-shangchuan1`(上传) `icon-xiazai1`(下载) `icon-ai70`(连接) `icon-xiazai`(总下载)。

### 5.2 订阅页 sub.vue

- 主体 `div.div` padding-bottom: 40px；列表 scrollbar height:100%，内 div padding: 0 5px。
- 订阅卡栅格：`el-row :gutter=10`，卡片 `el-col :span=8`（**3 列**）。
- 「+新增」卡内 div：**height: 87px**; line-height: 82px; **font-size: 80px**; text-align: center。
- 订阅卡内 div：**height: 87px**；标题 h1 font-size:16px（省略号，margin-bottom:5px）；节点数 span font-size:12px; margin-bottom:10px；全选链接 color #4898f8。
- 行内操作按钮 `.buttons .el-button`（全 `circle`）：margin-left: 3px !important，首个 0 !important。顺序：启用(check)/测速(check)/查看(view)/编辑(s-tools)/更新(refresh-right)/删除(delete)。
- 底部栏 `el-row`：position:absolute; bottom:0; left:0; right:0; **height:30px**；左右各 `el-col span=12`（右侧 text-align:right），按钮 size="mini"。
- 弹窗宽度：新增 **70%**、编辑 **70%**、节点详情 look **80%**；`:top` win32=18px/其它0px。
- 弹窗内：`el-radio-group` margin-bottom:20px；`el-input(small)` margin-bottom:20px；选文件 append 按钮 height:32px; padding:0 20px; margin:0 -20px; border-radius: 0 5px 5px 0；错误 span float:left color:red。
- 节点详情卡 `el-col :span=12`（2 列），卡片 position:relative；延迟徽标 position:absolute; right:3px; top:3px; bottom:3px; **width:50px**; padding:3px 5px; border-radius:5px; font-size:12px; 背景=`color(delay,0.3)`。

> 本页**无 el-table**，订阅/节点均为 el-card 栅格。

### 5.3 设置页 setting.vue

- `el-tabs` 五个 tab：远程 / 过滤(127) / 区域 area(127) / 规则 rule(127) / 系统。
- 各 tab 内 `el-scrollbar` height = **`dom.height - 73`**（`dom.height`：win32 = `#main.height()-28`，否则 `#main.height()`）。
- 「应用」按钮 `el-button size=mini type=text`：position:absolute; **top:5px; right:5px**。
- 远程/过滤：`el-col` margin-bottom:10px；`el-select` width:100%。
- area/rule 表格 `el-table size=mini border` width:100%：
  - area 列宽：name **180** / type **180** / rule（自适应）/ 操作 fixed=right **100**。
  - rule 列宽：type **180** / node **180** / value（自适应）/ 操作 fixed=right **100**。
  - 操作列按钮 `el-button type=text size=small`（s-tools / delete-solid）。
  - `el-pagination background layout="prev,pager,next" :page-size=5` text-align:center。
- 系统 tab 表单 `el-form.setting`（无 label-width 属性，靠 CSS）：
  - `.setting .el-form-item` → **width: 300px**; display: inline-block; padding: 0 10px; margin-bottom: 10px。
  - `.setting h1` → padding-left: 10px !important; **font-size: 24px !important**; font-weight: bolder !important（深色文字 #333，浅色 #ccc）。
  - `.setting > div` → **border-bottom: 1px dashed #ccc**; padding: 0 0 10px 0; margin-bottom: 20px（分组虚线，深色边 #333）。
  - theme/language `el-select` width:200px；port `el-input` width:200px；GeoIp `el-progress` width:100px margin-left:10px；`i.icon-warm` font-size:24px。
- 设置弹窗 `el-dialog.settingBox width=40%`（`:top` win32=18px/0px），内 `el-form label-width="80px"`，每项 div margin-bottom:5px，底部按钮 size=mini。

### 5.4 日志页 logs.vue

- `el-tabs` 两个 tab：main / clash。
- 每 tab `el-scrollbar` height = **`dom.height - 73`**（同设置页算法）。
- `el-timeline :reverse=true`，`el-timeline-item placement=top`（时间线内容色见 4.5）。
- 「打开日志目录」按钮 `el-button size=mini type=text`：position:absolute; top:5px; right:5px。

### 5.5 连接页 connections.vue

> 独立窗口（900×600），**不含** sider/主外壳，样式基本走内联。

| 元素 | 值 |
|---|---|
| `#main`（scoped） | bottom: 0 !important; border-radius: 5px !important |
| `.connections .el-tabs__header` | margin-bottom: 0px !important |
| 顶栏 `el-header` | height=28px；darwin 版 `-webkit-app-region:drag`，标题 span margin-left:60px |
| 内容 `el-container` | padding: 5px; flex-direction: column |
| 工具条 `el-form` | margin-bottom: 5px; padding: 0 5px |
| Online/Offline 按钮 | `el-button-group` size=mini |
| 搜索 `el-input` | size=mini; **width: 50%** |
| 滚动区 `el-scrollbar` | height = linux `dom.height-40` / 其它 `dom.height-68`（`dom.height=$(window).height()`） |
| 列表 `el-row :gutter=0` → `el-col :span=24`（单列） | 卡片 `el-card.sub.look` |
| 卡片行 div | flex nowrap 两端对齐居中 |
| 标题 h1.pointer | margin-right: 10px；离线时 color:#999 !important；在线有 `el-icon-loading` |
| chains 徽标 span | background rgba(0,0,0,0.35); border-radius:5px; padding:3px 5px; font-size:12px; margin-right:10px；图标 margin-right:5px |
| download 徽标 span | background rgba(0,255,0,0.5); color:#333; border-radius:5px; padding:3px 5px; font-size:12px; margin-right:10px |
| upload 徽标 span | background rgba(255,0,0,0.5); color:#333; border-radius:5px; padding:3px 5px; font-size:12px; margin-right:10px |
| 关闭按钮 el-button(mini) | **padding: 7px !important**; icon=el-icon-delete |

### 5.6 会员页 include/vip.vue

> 外壳同第 2 节；主体在 `include/vip.vue`。`el-container.main.two` 内联 `-webkit-app-region:drag`。

| 选择器 | 属性值 |
|---|---|
| 根 div（内联） | position: relative; width: 100% |
| `.el-col` | font-size: 14px; line-height: 40px; user-select: text（深色文字#fff / 浅色#333） |
| `.integration` | 深色 #fff / 浅色 #000 |
| `.machineId` | font-size: 12px; text-align: center; color: #999; text-shadow: 0 0 1px #fff; position: absolute; bottom: 10px; left: 10px; right: 10px |
| `.vip` | -webkit-app-region: drag |
| `.el-button/.el-input/.el-input-number` | -webkit-app-region: no-drag |
| `.item` | display: flex; nowrap; 两端对齐居中 |
| `.item .content` | nowrap; overflow: hidden; 省略号 |
| `.vip .el-card__body` | padding: 10px !important |
| `.vip .tag > div` | flex nowrap 两端对齐居中 |
| `.vip .tag > div > div.icon` | flex 居中; **width: 80px** |
| `.vip .tag > div > div.icon i` | **font-size: 36px** |
| `.vip .tag > div > div.content` | width: 100% |
| `.vip .tag > div > div.content p` | line-height: 24px |

- 卡片栅格：积分/会员 `el-col :span=12`（2 列），兑换/绑定/分享 `el-col :span=24`（整行）。`el-row.vip :gutter=10`。
- `el-input-number size=mini :step=500 :min=500`；兑换按钮 padding-top/bottom: 8px。
- info 按钮 `el-button type=text icon=el-icon-info` display:inline-block。
- 图标：`el-icon-timer / el-icon-user / el-icon-goods / el-icon-lock`。

### 5.7 关于页 include/about.vue

> 外壳同第 2 节；`components/about.vue` 的 `el-container.main.two` 内联 `height: ${dom.height}px`。主体在 `include/about.vue`：

| 选择器 | 属性值 |
|---|---|
| `el-scrollbar`（内联） | height = **`dom.height - 20`**; overflow-x hidden |
| `.el-col` | font-size: 14px; line-height: 40px; user-select: text（深色文字#fff） |
| `a` | color: red |
| `a.update`（New/Beta 角标） | text-decoration: none; margin-left: 10px |
| `.version` | color: green; font-weight: bold |

内容行：Clash Auto 版本 / Clash 版本 / TG 群 / GitHub / ReadMe / Exe path / Root path。

### 5.8 托盘 tray.vue

> 窗口 350×612。导入 `trayBlack.css` / `trayWhite.css`（另一套主题）。

| 选择器 | 属性值 |
|---|---|
| `#main` | bottom: 0 !important; border-radius: 5px |
| `#main *` | font-size: 14px |
| `.el-row` | padding-bottom: 10px；`:first-child` margin-top: 0 |
| `h2` | padding: 0 10px 5px 10px |
| `.el-col` | margin-top: 10px |
| `canvas`（注释未渲染） | width: 100%; **height: 60px**; border-radius: 5px |
| `li` | padding: 5px |
| `li.button`（面板/退出） | display: block; margin: 0 10px; border-radius: 5px; text-align: center !important |
| `li.node`（节点行） | margin: 0 5px 5px 5px; border-radius: 5px; flex nowrap 两端对齐居中 |
| `li.node span` | font-size: 12px !important; text-align: left |
| `li.node span.speed` | **width: 120px**; text-align: right |
| `li.node span.icon` | **width: 16px**；`i` color:#4898f8；`i.no` color:RGBA(0,0,0,0)（隐藏勾） |
| `li.node span.name` | 省略号; margin: 0 5px; width: 100% |
| `li.node span.speed span` | color:#000; border-radius:5px; padding:3px 5px; nowrap |
| 节点列表 scrollbar（内联） | **height: 400px**; width: 100%; overflow-x hidden |
| 当前节点名（内联） | border-left: 3px solid green; padding-left: 5px |

结构：面板按钮 → 节点列表(h2)→ 模式(h2: TUN/核心)→ 退出按钮。勾选图标 `iconfont icon-duigou1`。

### 5.9 更新弹窗 update.vue

> 窗口 600×660（脚本动态调整，见第 1 节）。导入 `updateBlack.css` / `updateWhite.css`。

| 选择器 | 属性值 |
|---|---|
| `#main` | border-radius: 5px; bottom: 0; -webkit-app-region: drag |
| `#main .title` | font-size: 12px; padding: 5px |
| `#main .main` | padding: 10px |
| `#main .buttons` | position: fixed; **bottom: 10px**; left:0; right:0; flex 两端对齐居中; padding: 0 10px |
| `#main span.bt` | font-size: 10px |
| 交互控件 | -webkit-app-region: no-drag; font-size: 12px |
| `.version` | padding: 10px |
| `h1` | font-size: 20px |
| `h2` | padding-top: 10px; font-size: 16px; margin-bottom: 5px |
| `li.asset`（下载资源行） | border-radius: 5px; margin-bottom: 5px; padding: 5px; font-size: 12px；勾 `i` #4898f8 / `.no` 透明 |
| `.update .el-tabs__item` | -webkit-app-region: no-drag |
| 左侧 tabs `el-tabs.update`（内联） | **height: 450px**；`:tab-position=left` |
| 更新说明 scrollbar（内联） | **height: 200px**; overflow-x hidden |
| 资源列表 scrollbar（内联） | **height: 130px**; overflow-x hidden |
| 进度条 `el-progress` | `:text-inside=true` **`:stroke-width=26`**；外层 div margin-top: 10px |
| 状态文字 div | text-align: center; font-size: 12px |
| noUpdate h3 | text-align: center; padding-top: 20px |
| 按钮 | 全 size=mini；更新按钮 type=primary，`address===''` 时禁用 |

---

## 附：复刻要点速记

- **骨架三固定值**：顶栏 `28`、侧栏 `120`、页脚 `38`；主窗 `900×510`。窗体圆角 body `10`、`#main` 顶角 `5`、页脚底角 `5`。
- **强调色** `#4898f8`；选中左条 `inset 3px 0 0 #4898f8`；悬停菜单右移 `35→39`（+4px）。
- **状态页**左右五五分（`right:50% / left:50%`），左列上 200 图卡、`top:210` 下折线图。
- **滚动区高度**都用 JS 动态：设置/日志 `dom.height-73`、关于 `dom.height-20`、连接 `-68/-40`、节点全览弹窗 `-100`、托盘固定 `400`、更新说明 `200`/资源 `130`/左 tabs `450`。
- 深/浅两主题切换类挂 `#main`；托盘、更新各有独立主题文件。
