# clashauto-c++

`clashauto-c++` 是对 `Clashr-Auto` 的 C++ / Qt Widgets 重构版本，目标是在桌面 UI 上尽量 1:1 复刻原 Electron + Vue 客户端，同时把核心控制逻辑迁移到原生 Qt。

## 已完成

- 复刻主窗口结构：无边框标题栏、左侧菜单、状态页、订阅页、设置页、日志页、关于页、底部状态栏。
- 复刻深色主题、节点列表、流量卡片、实时折线图、底部 TUN/代理/核心开关和模式选择。
- `AppConfigLoader` 读取原项目配置，并定位同级 `Clashr-Auto` 资源目录。
- `CoreController` 支持启动/停止 Clash 核心、系统代理、TUN 开关、生成配置并热重载 `/configs`。
- `TrayController` 支持系统托盘菜单、流量显示、核心/代理/TUN 快捷开关。
- `ConfigBuilder` 生成 `full.yaml`，合并默认配置、插件 DNS/TUN、订阅节点、代理组和自动分组。
- `SubscriptionStore` 支持订阅读取、添加、启用/禁用、远程/本地 YAML 更新、`sub` 转换、增量更新、规则过滤、节点明细和单节点启用/禁用。
- 状态页节点列表已接入 Clash REST API：读取当前分组节点、应用节点、清理连接、刷新节点、搜索过滤、切换代理分组。
- 模式切换已通过 Clash REST API 写入 `Rule` / `Global` / `Direct`。
- 设置页支持保存 API 地址、端口、主题、TUN、系统代理、增量更新、节点过滤规则，并写回用户目录 `config.yaml`。
- 设置页「区域 / 规则」CRUD 已注入 `full.yaml`：`ConfigBuilder::applyCustomRules` 读取 `rules.json`，`area` 按正则生成自定义代理组并接入选择组、`rule` 前插到 `rules:`；编辑后自动重建并热重载配置。
- 更新弹窗支持应用内下载：双击资源即带进度条下载到「下载」目录（跟随重定向），完成后打开所在文件夹。
- 系统集成：设置页新增「关闭到托盘」（`mini`，✕ 隐藏到托盘而非退出）与「开机自启」（`sys`，Windows 注册表 `Run` 项）；退出时（含托盘退出）经 `aboutToQuit` 停止核心并还原系统代理。
- 连接管理：状态页节点栏「🔗」按钮打开连接对话框，接 Clash `/connections`，展示主机/网络/代理链/上下行/规则，支持关闭选中、关闭全部、手动刷新与 1.5s 实时刷新（选中行时暂停）。
- 日志页新增「清空」按钮，清空当前时间线视图（日志文件不受影响）。
- 单实例锁（`QLockFile`）：避免多开争抢核心端口；CLI 子命令不受限。
- 节点切换通知（`note`，设置「切换通知」可关）：自动/手动换节点时弹系统通知（跳过启动首次）。
- 关于页「导出节点」按钮：把状态页当前分组节点（名称+延迟）导出为 JSON 到「下载」目录。
- 关于页新增「打开配置目录」按钮；侧栏与关于页版本号统一取自编译期 `APP_VERSION`（与 Release 版本一致）。
- 日志页已从占位页变成真实日志视图，并同步写入用户目录 `logs/qt-main.log`。
- 提供 CLI 辅助命令，便于脚本测试订阅和配置生成。

## 构建

本机已验证环境：

- Qt `6.8.3`
- MinGW `13.1.0`
- Ninja

```powershell
$env:Path='C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.8.3\mingw_64\bin;' + $env:Path
cmake -S . -B build-ninja -G Ninja `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\mingw_64 `
  -DCMAKE_CXX_COMPILER=C:\Qt\Tools\mingw1310_64\bin\g++.exe
cmake --build build-ninja
```

## 运行

```powershell
$env:Path='C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.8.3\mingw_64\bin;' + $env:Path
.\build-ninja\clashauto-cpp.exe
```

只生成配置、不打开 UI：

```powershell
.\build-ninja\clashauto-cpp.exe --build-config
```

订阅调试命令：

```powershell
.\build-ninja\clashauto-cpp.exe --update-subscription 0 C:\path\to\subscription.yaml
.\build-ninja\clashauto-cpp.exe --print-subscription-path
.\build-ninja\clashauto-cpp.exe --print-effective-url 0
.\build-ninja\clashauto-cpp.exe --list-subscriptions
.\build-ninja\clashauto-cpp.exe --list-nodes 0
.\build-ninja\clashauto-cpp.exe --set-node-use 0 1 false
.\build-ninja\clashauto-cpp.exe --edit-subscription 0 名称 http://url clash
.\build-ninja\clashauto-cpp.exe --remove-subscription 1
```

生成的 Clash 配置默认位于：

```text
C:\Users\Administrator\AppData\Roaming\ClashAuto\Clash Auto\clash-auto\full.yaml
```

可用原项目内置 Clash 核心验证：

```powershell
G:\clashauto\Clashr-Auto\command\clash\clash-windows-amd64.exe -t -f "C:\Users\Administrator\AppData\Roaming\ClashAuto\Clash Auto\clash-auto\full.yaml"
```

## 当前验证

- `cmake --build build-ninja` 通过。
- `--build-config` 可生成 `full.yaml`。
- 原项目 Clash 核心 `-t -f full.yaml` 校验通过。
- Qt 程序启动 smoke test 通过。
