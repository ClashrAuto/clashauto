# Coast release 下载验证（Docker）

从 `ClashrAuto/clashauto` 最新 Release 下载 **Coast** 全平台产物，校验并（对 Linux）无头真跑一遍。
用来在本机 Docker 里快速确认一次发布是否 OK —— 尤其那些本机编不了、只能靠产物验的改动
（改名 Coast、瘦身、自包含、扁平化）。

## 跑

需要 **Docker Desktop 运行中**。GitHub 需走代理时，容器用宿主机 `host.docker.internal:7890`
（即本机 mihomo 混合端口，需在跑）。

```bash
# git-bash / WSL：
bash run.sh
# 能直连 GitHub：
PROXY= bash run.sh
```

PowerShell 等价：

```powershell
docker build -t coast-validate .
docker run --rm -e https_proxy=http://host.docker.internal:7890 -e http_proxy=http://host.docker.internal:7890 coast-validate
```

## 验什么

| 平台 | 检查 |
|---|---|
| **全部** | 资产齐全 + `sha256` 边车校验 |
| **Windows** | 扁平（`Coast.exe` 在包根、无 `clashauto-c++`/`Clashr-Auto`）、**全静态**：包里一个 `.dll` 都没有、一个子目录都没有（Qt 与 MSVC 运行库全进 exe） |
| **macOS** | `Coast.app`、`Info.plist` bundle id = `com.yuehongsun.coast`、`MacOS/Coast` + `com.yuehongsun.coast.helper`、无 `Contents/Clashr-Auto`（DMG 用 7z 解，best-effort） |
| **Linux .deb** | 扁平装到 `/opt/coast/coast` + `/usr/bin/coast` + `coast.desktop`、无旧子目录、**无自带 Qt 运行时**（`lib/` `plugins/` `qml/` `qt.conf` 都不该在） |
| **Linux 运行** | 装 `.deb` → `ldd` 断言**不依赖任何 `libQt6`** → `COAST_STATICDEPS_SELFTEST` 验插件真在二进制里 → `Xvfb` 无头跑两遍（默认 RHI / `QT_QUICK_BACKEND=software`）：**能启动不崩** + **内嵌配置从 qrc 落地到 `~/.local/share/Coast/config/`**（验证自包含种子 + 只读补权） |

> ★ 静态 Qt 之后，「插件在不在」这件事在**干净容器**里验才有意义：开发机上 TLS 后端 / QSQLITE /
> 图片格式插件多半从别处也能加载到，只有这里能暴露。而它们缺失时**不会崩**，只是订阅拉不到、
> 上网历史不记、窗口空白 —— 所以专门有 `COAST_STATICDEPS_SELFTEST` 这条断言，退出码即结论。

`COAST_NO_AUTOSTART=1` 跳过「自动下载并启动 mihomo 内核」，只验 UI 启动 + 配置落地。

## 说明 / 限制

- **只有 Linux 是真跑**：Windows/macOS 二进制没法在 Linux Docker 里执行，故只做**结构检查**
  （在真机上跑才是最终确认）。
- macOS DMG 用 `p7zip` 解，对 UDZO/HFS 支持有限；解不出时该项标 `⏭️ 跳过`，请在 Mac 上挂载手动核对。
- 退出码：全过 `0`，有 `❌` 则 `1`。
