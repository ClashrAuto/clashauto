# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository layout — two projects, one tracked

This workspace contains two distinct projects. **Only `clashauto-c++/` is tracked by this git repo** and is where active development happens.

- **`clashauto-c++/`** — a C++ / Qt Widgets rewrite of the original client. All commits, CI, and releases target this. This is what you edit.
- **`Clashr-Auto/`** — the *original* Electron + Vue app (electron-vue, Node/npm). It is **untracked here** (has its own `.git`) and functions purely as a **runtime resource bundle** for the C++ app: it supplies the Clash/mihomo core binaries (`command/clash/*`), the `sysproxy` binaries (`command/sysproxy/*`), the base `config/config.yaml`, and `language/`. Do not treat it as part of the C++ build; do not modify it to fix C++ behavior.

The C++ app **finds `Clashr-Auto/` at runtime as a sibling directory**: `AppConfigLoader::load()` walks up to 8 parent directories from the executable looking for `../Clashr-Auto/config/config.yaml` (`src/AppConfig.cpp`). If you run the built exe somewhere without a sibling `Clashr-Auto/`, config loading, core startup, and system proxy will all fail. In CI these resources are downloaded from the `ClashrAuto/Clashr-Auto-Desktop` GitHub releases, not built.

## Build & run (clashauto-c++)

Locally verified toolchain: Qt 6.8.3, MinGW 13.1.0, Ninja. CMake finds Qt6 or Qt5 (`Widgets`, `Network`).

```powershell
# From clashauto-c++/. Put Qt + MinGW on PATH first.
$env:Path='C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.8.3\mingw_64\bin;' + $env:Path
cmake -S . -B build-ninja -G Ninja `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\mingw_64 `
  -DCMAKE_CXX_COMPILER=C:\Qt\Tools\mingw1310_64\bin\g++.exe
cmake --build build-ninja
.\build-ninja\clashauto-cpp.exe
```

CMake has `AUTOMOC`/`AUTORCC`/`AUTOUIC` on, so new `Q_OBJECT` classes and `.qrc`/`.ui` changes are picked up automatically — but **new `.cpp` files must be added to the `add_executable(...)` list in `CMakeLists.txt`** by hand. Build dirs (`build-ninja`, `build-release`) are gitignored.

## Testing — there is no unit-test framework

Verification is done through **CLI subcommands in `src/main.cpp`** that short-circuit before the GUI opens. These are the "tests" — use them to exercise config/subscription logic headlessly:

```powershell
.\build-ninja\clashauto-cpp.exe --build-config                 # generate full.yaml, print its path
.\build-ninja\clashauto-cpp.exe --print-subscription-path
.\build-ninja\clashauto-cpp.exe --list-subscriptions
.\build-ninja\clashauto-cpp.exe --print-effective-url 0
.\build-ninja\clashauto-cpp.exe --list-nodes 0
.\build-ninja\clashauto-cpp.exe --set-node-use 0 1 false        # <subIdx> <nodeIdx> <true|false>
.\build-ninja\clashauto-cpp.exe --update-subscription 0 C:\path\to\subscription.yaml
```

Validate a generated config against the real core (its `-t -f` is the source of truth for correctness):

```powershell
G:\clashauto\Clashr-Auto\command\clash\clash-windows-amd64.exe -t -f "C:\Users\Administrator\AppData\Roaming\ClashAuto\Clash Auto\clash-auto\full.yaml"
```

## Architecture (clashauto-c++/src)

The app is a frameless Qt Widgets client that drives an external Clash/mihomo core process and talks to that core's REST API.

- **`AppConfig` / `AppConfigLoader`** — the config model plus the sibling-`Clashr-Auto` discovery described above. Resolves per-OS/per-arch core and sysproxy binary paths. Parses `config.yaml` with regex helpers (see YAML note below).
- **`ConfigBuilder`** — generates `full.yaml`: merges the base config, plugin DNS/TUN blocks, subscription proxies, proxy groups, and auto-generated region groups. `ensureFullConfig(tunEnabled)` is the entry point. `applyCustomRules()` then consumes the settings page's `userDir/rules.json`: `area` entries become regex-matched custom proxy-groups (wired into the first selector), and `rule` entries are prepended to the `rules:` block. MainWindow triggers `CoreController::rebuildConfig()` (regenerate + hot-reload) whenever settings or a rule/area row change.
- **`SubscriptionStore`** — owns the subscriptions YAML: add/enable/disable subscriptions and individual nodes, remote/local fetch + `sub`-format conversion, incremental update, and allow/no-allow rule filtering (`nodeAllowed`).
- **`CoreController`** — lifecycle glue: launches/stops the core via `QProcess`, toggles system proxy (via the bundled `sysproxy` exe) and TUN, rebuilds config, and hot-reloads by PUTing to the core's `/configs` endpoint. Emits `statusChanged` / `logUpdated`.
- **`ClashService`** — polls the running core's REST API on `host:uiPort` (default `127.0.0.1:9090`): `/traffic`, `/connections`, `/proxies`, sets mode via `/configs`, selects nodes/groups via `/proxies/<group>`, clears via `DELETE /connections`. All async via `QNetworkAccessManager` + callbacks. Emits Qt signals consumed by the UI.
- **`MainWindow`** — the whole UI (frameless titlebar with custom drag handling, sidebar, status/subscriptions/settings/logs/about pages, footer with TUN/proxy/core switches). The sidebar `menu` list is index-aligned 1:1 with the `m_pages` stack (`createMenuButton(menu[i], i)`), so adding/removing a page means editing both lists together. (The VIP/会员 page was intentionally removed.) Owns a `ClashService` and pointers to `CoreController`, `TrayController`, `SubscriptionStore`. All styling is inline via `appStyle()`. `closeEvent` honours the `mini`/`closeToTray` config: when on, ✕ hides to tray instead of quitting. On real quit (incl. tray "退出程序"), a `QCoreApplication::aboutToQuit` handler calls `CoreController::stopCore()` to kill the core and restore the system proxy. The `sys`/`autoStart` setting writes the Windows `HKCU\...\Run` key via `QSettings` (applied on settings save).
- **`TrayController`** — system tray menu, traffic display, quick core/proxy/TUN toggles, plus `notify()` for balloon messages.
- **`TrafficChart`** — custom-painted realtime line chart widget.

Data flow: `MainWindow` builds config through `ConfigBuilder`/`SubscriptionStore` → `CoreController` starts the core with `full.yaml` → `ClashService` polls the core's REST API → signals update the UI.

### YAML is manipulated as text, not parsed

There is **no YAML library**. `AppConfig`, `ConfigBuilder`, and `SubscriptionStore` read and rewrite YAML with `QRegularExpression` and manual string surgery (e.g. `setScalar`, `replaceProxyListAt`, `parseProxyList`). When touching config/subscription generation, preserve exact indentation and key formatting, and verify the result with `clash -t -f` — malformed output won't be caught at compile time.

### Runtime data locations

User-writable state lives under the Qt `AppDataLocation`, i.e. `%AppData%/ClashAuto/Clash Auto/clash-auto/` on Windows: the user's `config.yaml` (copied from the bundled one on first run), the generated `full.yaml`, and `logs/qt-main.log`. The user copy takes precedence over the bundled `Clashr-Auto/config/config.yaml`.

## Releases & CI (`.github/workflows/release.yml`)

- The release version **auto-increments per commit**: the "Resolve build version" step takes `major.minor` from `project(... VERSION x.y.z)` in `clashauto-c++/CMakeLists.txt` and uses the git commit count (`git rev-list --count HEAD`) as the patch/build number → `major.minor.<count>` (e.g. `0.1.15`), tag `v<version>`. Bump `major`/`minor` in CMakeLists to start a new line; the build number rises on its own each commit. This is why both checkout steps use `fetch-depth: 0` (full history is needed for the count), and both jobs compute the same version from the same `github.sha`. The resolved `APP_VERSION` is passed to CMake (`-DAPP_VERSION=`), which `configure_file`s `src/Version.h.in` → `Version.h` (`#define APP_VERSION`), so the **app UI (sidebar `Ver:` + About page) shows the exact packaged version**. Local builds default `APP_VERSION` to `PROJECT_VERSION`.
- Any **push to `master`/`main`** builds and publishes a GitHub Release (Windows x64 + ARM64 portable zip & NSIS installer; Linux x64 + ARM64 tar.gz/zip & `.deb`). PRs build artifacts but do not publish.
- CI does **not** build `Clashr-Auto/`; it downloads the runtime bundle from `Clashr-Auto-Desktop` release `v2.5.7` and, for ARM64, pulls the matching mihomo core. It then stages `clashauto-cpp.exe` (via `windeployqt`) alongside a trimmed `Clashr-Auto/` resource dir, preserving the sibling-directory layout the app expects.
- Windows CI builds with **MSVC (Visual Studio 17 2022)**, not MinGW; Linux CI uses Ninja + system Qt6. ARM64 Windows is a cross-compile and passes `-DQT_HOST_PATH`.
