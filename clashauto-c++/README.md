# clashauto-c++

`clashauto-c++` 是对 `Clashr-Auto` 的 C++ / Qt Widgets 重构版本，目标是在桌面 UI 上尽量 1:1 复刻原 Electron + Vue 客户端，同时把核心控制逻辑迁移到原生 Qt。

## 已完成

- 复刻主窗口结构：无边框标题栏、左侧菜单、状态页、订阅页、设置页、日志页、会员页、关于页、底部状态栏。
- 复刻深色主题、节点列表、流量卡片、实时折线图、底部 TUN/代理/核心开关和模式选择。
- `AppConfigLoader` 读取原项目配置，并定位同级 `Clashr-Auto` 资源目录。
- `CoreController` 支持启动/停止 Clash 核心、系统代理、TUN 开关、生成配置并热重载 `/configs`。
- `TrayController` 支持系统托盘菜单、流量显示、核心/代理/TUN 快捷开关。
- `ConfigBuilder` 生成 `full.yaml`，合并默认配置、插件 DNS/TUN、订阅节点、代理组和自动分组。
- `SubscriptionStore` 支持订阅读取、添加、启用/禁用、远程/本地 YAML 更新、`sub` 转换、增量更新、规则过滤、节点明细和单节点启用/禁用。
- 状态页节点列表已接入 Clash REST API：读取当前分组节点、应用节点、清理连接、刷新节点。
- 模式切换已通过 Clash REST API 写入 `Rule` / `Global` / `Direct`。
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
