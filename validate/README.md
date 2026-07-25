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
| **Windows** | 扁平（`Coast.exe` 在包根、无 `clashauto-c++`/`Clashr-Auto`）、瘦身（**无** `opengl32sw.dll`/`d3dcompiler_47.dll`）、`Qt6Quick.dll` 已部署 |
| **macOS** | `Coast.app`、`Info.plist` bundle id = `com.yuehongsun.coast`、`MacOS/Coast` + `com.yuehongsun.coast.helper`、无 `Contents/Clashr-Auto`（DMG 用 7z 解，best-effort） |
| **Linux .deb** | 扁平装到 `/opt/Coast/Coast` + `/usr/bin/coast` + `coast.desktop`、无旧子目录 |
| **Linux 运行** | 装 `.deb` → `Xvfb` 无头跑 `Coast`（软件后端）：**能启动不崩** + **内嵌配置从 qrc 落地到 `~/.local/share/Coast/config/`**（验证自包含种子 + 只读补权） |

`COAST_NO_AUTOSTART=1` 跳过「自动下载并启动 mihomo 内核」，只验 UI 启动 + 配置落地。

## 说明 / 限制

- **只有 Linux 是真跑**：Windows/macOS 二进制没法在 Linux Docker 里执行，故只做**结构检查**
  （在真机上跑才是最终确认）。
- macOS DMG 用 `p7zip` 解，对 UDZO/HFS 支持有限；解不出时该项标 `⏭️ 跳过`，请在 Mac 上挂载手动核对。
- 退出码：全过 `0`，有 `❌` 则 `1`。
