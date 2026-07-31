# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**Coast** (formerly "Clash Auto") — a frameless **Qt 6 / QML (Qt Quick)** desktop client that drives an external Clash/**mihomo** core process and talks to its REST API. Cross-platform: Windows, macOS, Linux.

The git repo root tracks:
- **`clashauto-c++/`** — the app source (the only thing you edit). *Historical dir name — the product is **Coast**; the folder is left named `clashauto-c++` on purpose (renaming it would churn CI paths).*
- **`validate/`** — a Docker "download latest release + validate all platforms" harness (below).
- **`.github/workflows/release.yml`** — CI / release.

> **The app is self-contained.** It used to depend on a sibling `Clashr-Auto/` directory (the original Electron app, used as a runtime resource bundle). That dependency is **gone** — all seed resources (base `config/*.yaml`, `Country.mmdb`, Windows `wintun.dll`) are **embedded in the binary via qrc** (`clashauto-c++/assets/bundle/`, listed in `resources.qrc` + a Windows-only `resources_win.qrc`), and the mihomo core is downloaded in-app on first use. `AppConfigLoader::load()` no longer searches for any sibling directory and `AppConfig::sourceRoot` was removed. **Do not reintroduce a Clashr-Auto dependency.**

### Two UI layers — QML is shipped, Widgets is dead code

- **Shipped:** the **QML** app — `src/main_qml.cpp` + `qml/*.qml` + the `src/qml/*` C++ glue (`QmlBridge`, the `*Controller`s, the `*Model`s, `I18n`). CMake target `coast`.
- **Dead/legacy:** the older **Qt Widgets** version — `src/MainWindow.cpp`, `src/main.cpp`, `src/TrafficChart.*`. **Not compiled** (not in the target). `MainWindow.h` is still `#include`d by `TrayController.cpp` for QWidget base methods, but `MainWindow.cpp`/`main.cpp` don't build; they're **stale** (old paths, pre-Coast assumptions). The CLI test subcommands (`--build-config`, …) lived in the dead `main.cpp` — they are **not** in the shipped build.

## Build & run

**You usually can't build locally** (dev box is a GPU-less QEMU VM without the full toolchain) — verify via **CI** + the **`validate/`** Docker tool instead. When a local build *is* possible: Qt 6.8.3 + MinGW/MSVC + Ninja.

```powershell
# From clashauto-c++/. Put Qt + MinGW on PATH first.
$env:Path='C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.8.3\mingw_64\bin;' + $env:Path
cmake -S . -B build-ninja -G Ninja `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\mingw_64 `
  -DCMAKE_CXX_COMPILER=C:\Qt\Tools\mingw1310_64\bin\g++.exe
cmake --build build-ninja
.\build-ninja\Coast.exe          # OUTPUT_NAME is "Coast" on Windows (was clashauto.exe)
```

**Windows local builds need the Npcap SDK** — headers only (`L2Endpoint_win.cpp` includes `pcap.h`; `wpcap`
never enters the link line). Without it the build dies at `fatal error: pcap.h`. On this machine it lives at
`C:\Users\ultra\npcap-sdk`; set `$env:NPCAP_SDK='C:/Users/ultra/npcap-sdk'` before configuring. To fetch it
fresh: `curl -x http://127.0.0.1:7890 -o sdk.zip https://npcap.com/dist/npcap-sdk-1.13.zip` — **npcap.com is
unreachable without the local proxy** (a direct curl returns "Empty reply from server"). Do **not** keep it
under the session scratchpad: that gets cleaned and the next build breaks again (happened once).

`find_package(Qt6 … Widgets Network Qml Quick QuickControls2)`. `AUTOMOC`/`AUTORCC`/`AUTOUIC` are on, so new `Q_OBJECT` classes and `.qrc`/`.qml` changes are picked up — but **new `.cpp` files must be added by hand** to `CMakeLists.txt` (`BACKEND_SOURCES` / `QML_GLUE_SOURCES`), and new `.qml` files to the `qt_add_qml_module(... QML_FILES ...)` list. Build dirs: only `build-ninja/`, `build-qml/`, `build-release/` are in `.gitignore` — there is **no `build-*` wildcard**, so any other build dir you create shows up as untracked.

## Verifying a release — `validate/`

There is **no unit-test framework**. Verification = CI builds+packages green, then `validate/` checks the packaged artifacts:

```bash
bash validate/run.sh      # Docker: pull latest release's Coast artifacts + verify all platforms
```

It downloads every platform's Coast artifact from the latest `ClashrAuto/clashauto` release, checks sha256, runs **structural** checks (Windows flat / slimmed / self-contained, macOS `Coast.app` + `com.yuehongsun.coast` bundle id + helper, Linux flat `/opt/coast/coast`), and **actually runs the Linux build headless** under Xvfb — **twice, once per render backend** (default RHI/OpenGL, then `QT_QUICK_BACKEND=software`) — to confirm it launches on both paths and seeds its embedded config to `~/.local/share/Coast/config/`. Windows/macOS binaries can't execute in Linux Docker → structural only (real-machine run is the final word). See `validate/README.md`.

## Architecture (clashauto-c++/src)

**Backend** (shared, framework-agnostic C++):

- **`AppConfig` / `AppConfigLoader`** — config model + paths. `clashExecutable()` → the mihomo core under `userDir/command` (downloaded in-app; prefers `command/core[.exe]`, falls back to legacy `command/clash/clash-<os>-<arch>`). Seed resources come from qrc (`:/assets/bundle/*`), not any sibling dir. **`AppConfig::makeWritable()` — qrc-copied files are read-only; this restores owner-write** (config.yaml seeding + every later save depends on it, or they silently fail). Parses YAML with regex helpers (YAML note below).
- **`ConfigBuilder`** — generates `full.yaml` into `configDir`: merges base config, plugin DNS/TUN, subscription proxies, proxy groups, auto region groups. `ensureFullConfig(tunEnabled)` is the entry point. `applyCustomRules()` consumes `configDir/rules.json` (settings-page area/rule CRUD): `area` → regex custom proxy-groups (wired into the first selector), `rule` → prepended to `rules:`. A settings/rule/subscription change triggers `CoreController::rebuildConfig()` (regenerate + hot-reload).
- **`SubscriptionStore`** — owns `configDir/subscribe.yaml`: add/enable/disable subs and individual nodes, remote/local fetch + `sub`-format conversion, incremental update, allow/no-allow filtering (`nodeAllowed`).
- **`CoreController`** — launches/stops the core via `QProcess` (`-d userDir -f configDir/full.yaml`); on first run seeds `Country.mmdb` (from qrc) into userDir and, on Windows, extracts the arch's `wintun.dll` (from qrc) next to the core. Toggles system proxy **natively** (Windows WinINET `InternetSetOption` / macOS SCPreferences via a **root helper** / Linux `gsettings` — no bundled binary) + TUN. Hot-reloads via PUT `/configs`. Emits `statusChanged`/`logUpdated`.
- **`ClashService`** — polls the core's REST API on `host:uiPort` (**default `127.0.0.1:9191`** — avoids the original 9090): `/traffic`, `/connections`, `/proxies`; sets mode via `/configs`; selects nodes via `/proxies/<group>`; `DELETE /connections[/<id>]`; download speed-test. Async via `QNetworkAccessManager`; emits Qt signals.
- **`TrayController`** — system tray menu, traffic display, quick core/proxy/TUN toggles, `notify()`. (`#include`s `MainWindow.h` only for QWidget base methods.)

**QML UI layer** (`src/main_qml.cpp` + `qml/` + `src/qml/`):

- **`main_qml.cpp`** — `QApplication` + `QQmlApplicationEngine`; sets app/org name **"Coast"** (→ data dir); uses **Qt's default RHI backend** (Windows D3D11 / macOS Metal / Linux OpenGL) — it used to force the software backend, but that backend can't antialias curved edges (small circles/rounded rects get a 1-pixel hard step; `antialiasing`/`layer.smooth`/`layer.textureSize` are all no-ops there) and can't do distance-field text. GPU-less machines are still fine (D3D11 falls back to Windows' built-in WARP; Linux has Mesa llvmpipe via the `.deb`'s `libgl1`). **Don't re-add `setGraphicsApi(Software)`** — `QT_QUICK_BACKEND=software` is the escape hatch and must stay un-overridden. Also sets the default font's `PreferNoHinting` + `PreferAntialias` (unhinted glyphs; the old default was full hinting, which read as over-sharpened). Registers MiSans, `loadFromModule("Coast","Main")` (the QML module URI is `Coast`). `COAST_NO_AUTOSTART=1` skips auto-starting the core (used by headless smoke tests / local UI dev).
- **`QmlBridge`** — the thin glue exposing the shared backend to QML (status lights, traffic, nodes, mode, toggles, notifications; `persistConfigBool` writes `configDir/config.yaml`).
- **`src/qml/*Controller` + `*Model`** — `SubscriptionsController`, `SettingsController`, `UpdateController`, `AboutController`, `I18n` (12-language JSON tables in `assets/i18n/`, loaded from qrc), plus `NodeListModel` / `ConnectionsModel` / `LogModel`. Models update **incrementally** (`dataChanged`/`beginInsertRows`, deliberately **never** `beginResetModel`).
- **`qml/`** — `Main.qml` (shell: sidebar + `StackLayout` pages + footer), pages (`StatusPage`/`NodesPage`/`DevicesPage`/`SubscriptionsPage`/`SettingsPage`/`LogsPage`/`AboutPage` — sidebar order = `StackLayout` index, keep the two lists in `Main.qml` in sync), reusable components (`Card`/`NavButton`/`FooterSwitch`/`MetricCard`/`NodeRow`/`BandwidthChart`/`LogTimeline`), extra windows (`ConnectionsWindow`/`UpdateWindow`/`RuleEditorWindow`), and `Theme.qml` (singleton design tokens). `BandwidthChart.qml` is a `Canvas` realtime line chart.

Data flow: QML → `QmlBridge`/controllers → `ConfigBuilder`/`SubscriptionStore` build `full.yaml` → `CoreController` starts the core → `ClashService` polls REST → signals update QML.

### YAML is manipulated as text, not parsed

There is **no YAML library**. `AppConfig`, `ConfigBuilder`, `SubscriptionStore` read and rewrite YAML with `QRegularExpression` + manual string surgery (`setScalar`, `replaceProxyListAt`, `parseProxyList`). Preserve exact indentation and key formatting, and verify the result with `mihomo -t -f full.yaml` — malformed output isn't caught at compile time.

### Runtime data locations

Under Qt `AppDataLocation`, rebased to a flat brand dir (**no migration** from the old `%AppData%\ClashAuto\Clash Auto\clash-auto\` — a rename = fresh state; that old path is a historical fact, don't "rebrand" it). On Windows:
- **`userDir = %AppData%\Coast\`** — the core's `-d` home: `logs\`, `Country.mmdb`, cache, `command\` (downloaded core + extracted `wintun.dll`).
- **`configDir = %AppData%\Coast\config\`** — `config.yaml` (user copy, seeded from qrc on first run), generated `full.yaml`, plus `default.yaml`/`plugin.yaml`/`subscribe.yaml`/`rules.json`.
- **`configDir/coast.db`** — the app's only SQLite database (Qt6::Sql + QSQLITE, opened via `src/Sqlite.h`): `device` (the Devices-page ledger — identity/alias/proxy toggle/per-device policy/traffic counters) and `conn` (browsing history: one row per closed connection, 30-day retention). Three connections in-process: `DeviceStore`, `HistoryStore`, and a short-lived read-only one `ConfigBuilder` uses to find proxied devices (`DeviceStore::proxiedDevices`). Migrated automatically from the earlier `devices.json` + `history.db` (old ledger left as `devices.json.migrated`). Headless checks: `COAST_DEVICEDB_SELFTEST=1`, `COAST_HISTORY_SELFTEST=1`.

(macOS: `~/Library/Application Support/Coast`; Linux: `~/.local/share/Coast`.)

### Fonts — MiSans everywhere

One family, **`MiSans`**, bundled in `clashauto-c++/assets/fonts/` (committed, embedded via `resources.qrc`): `MiSans-Regular.ttf` + `MiSans-Semibold.ttf` (Semibold is the typographic-family bold face so `font.bold` maps to it instead of synthesizing). No monospace. `main_qml.cpp` sets the **global default app font to MiSans**, so every QML `Text`/control that doesn't set `font.family` inherits it — you essentially never set `font.family`. `Theme.uiFont` (`"MiSans"`) is named explicitly only where it isn't inherited: `Canvas`-drawn text (e.g. `BandwidthChart.qml`'s `ctx.font`).

## Naming, packaging & branding — per platform

Product is **Coast**; names follow each platform's convention:

- **Windows** — `Coast.exe`; installs to `%LOCALAPPDATA%\Coast`; portable zip is **flat** (exe + Qt runtime at the root, no `clashauto-c++`/`Clashr-Auto` subdirs).
- **macOS** — `Coast.app`; bundle id `com.yuehongsun.coast`; privileged root helper `com.yuehongsun.coast.helper` (**launchd Label = mach service = plist filename = codesign `-i` must all match**, see `helper/HelperProtocol.h`). **Signing/notarization is done by an EXTERNAL repo** `integemjack/schat.build` (branch `clashauto-mac`, `.github/workflows/clashauto-mac.yml`): clashauto's CI `trigger-mac` job pushes an empty commit there; it builds+signs+notarizes and clobbers the DMG onto the **same** release. clashauto's own macos job only uploads an Actions artifact. `Ireoo` can't push to `integemjack/schat.build` — needs an `integemjack` PAT.
- **Linux** — binary `coast` (lowercase, command convention); `.deb` installs flat to `/opt/coast/coast` + `/usr/bin/coast` symlink + `coast.desktop`; Debian `Package: coast`. **The `.deb` must `Depends` on `libopengl0`** — Qt6::Gui hard-links `libOpenGL.so.0` (an ELF NEEDED entry) even under the software backend; without it a clean system fails to launch.
- **The `ClashAuto` / "Clash Auto" brand string is gone** — everything is `Coast` now: release **name** (`Coast <ver>`) and asset **filenames** (`Coast-<ver>-…`), `APP_NAME`, the staged package dir, the deb package root, the QML module URI (`import Coast`, `loadFromModule("Coast","Main")`), and the CMake target (`coast`).
  Two things this rename did **not** touch, on purpose:
  - the source **directory** `clashauto-c++` — CI paths do reference it, and it is not the brand string;
  - the GitHub repo `ClashrAuto/clashauto` (note the `r` — it never matched `ClashAuto`) and the mac signer branch `clashauto-mac`.
  Two facts that made this safe, both verified rather than assumed: CI never names the CMake target (every job is a bare `cmake --build <dir>`), and the in-app updater picks assets by **extension + `portable`/`setup` keywords**, never by the filename prefix (`UpdateController::recommendedIndex`) — so renaming assets does not break one-click update for already-installed versions.
  ⚠️ **The external mac signer repo `integemjack/schat.build` (branch `clashauto-mac`) must be updated to match** — it builds/signs/notarizes the DMG and clobbers it onto the same release; if it still emits `ClashAuto-<ver>-macos-universal.dmg`, the release will carry a stale-named DMG alongside the new ones.
  Historical strings in docs (e.g. the old `%AppData%\ClashAuto\…` layout, `docs/legacy-ui-spec.md`'s description of the original Electron UI) are **left as-is** — they record what used to be true.

## Releases & CI (`.github/workflows/release.yml`)

- Version **auto-increments per commit**: `major.minor` from `project(... VERSION x.y.z)` in `CMakeLists.txt` + git commit count (`git rev-list --count HEAD`) → `major.minor.<count>`, tag `v<version>` (hence `fetch-depth: 0`). `APP_VERSION` → CMake `configure_file` → `Version.h` → shown in-app (sidebar `Ver:` + About).
- **Every push to any branch** builds + publishes a GitHub Release: Windows x64/arm64 (portable zip + NSIS setup), Linux x64/arm64 (tar.gz/zip + `.deb`), macOS universal DMG (via schat.build). PRs build artifacts but don't publish.
  - `master`/`main` → **stable** release, tag `v<version>`, `make_latest: true`.
  - any other branch → **prerelease**, tag `v<version>-beta.<sha7>` (the sha7 only disambiguates two branches landing on the same commit count; `APP_VERSION` itself stays purely numeric because asset filenames and the in-app version compare depend on it), release name `Coast <ver> (beta · <branch>)`, `make_latest: false`. **No macOS DMG** — `trigger-mac` stays master/main-only, because the external signer clobbers the DMG onto the release it targets and a beta run would overwrite the stable one.
  - The app's **`beta` setting** (`config.yaml`, Settings → 程序更新 → 接收测试版, default off) decides whether prereleases are visible: `AboutController::check()` (the update badge) skips them when off, and `UpdateWindow.qml` picks `UpdateController`'s release vs beta channel from it. Note `/releases/latest` excludes prereleases by GitHub's own definition — that's why the check reads the full `/releases` list and filters itself.
- **Self-contained CI** — no Clashr-Auto download/staging; the package is just the exe + Qt runtime (resources embedded in the exe, core downloaded in-app). `windeployqt` runs with **`--no-opengl-sw --no-system-d3d-compiler`** to drop ~24 MB of fallback DLLs — still fine after the move to the D3D11 RHI backend: WARP ships with Windows (no `opengl32sw.dll` needed unless Qt falls back to the OpenGL RHI), and Qt Quick's shaders are pre-compiled (no runtime `d3dcompiler_47.dll`). If a GPU-less machine ever fails to start, that pairing is the first thing to re-check.
- **msquic ships with Windows + Linux releases** (`MSQUIC_VERSION` in the workflow env, currently 2.5.9) so CoastCore's **Hysteria2** works in-process. Windows takes the NuGet package `Microsoft.Native.Quic.MsQuic.Schannel` (has `include/` + `lib/{x64,arm64}/msquic.lib` + `bin/*/msquic.dll`); Linux takes `libmsquic` from packages.microsoft.com — that deb ships **only the `.so`**, so headers come from the same GitHub tag. Both jobs pass **`-DCOAST_REQUIRE_QUIC=ON`**, which makes CMake **fail** instead of silently degrading, and both **bundle the library into the package** (it is not a system component anywhere) and then run the packaged binary with **`COAST_QUIC_SELFTEST=1`** — that hook does `MsQuicOpen2` + the Hy2 QPACK KAT, and is the only thing that proves the shipped package can actually load msquic (a previous attempt shipped a binary that NEEDED `libmsquic.so.2` without bundling it → it wouldn't start; `validate/validate.sh` re-checks this on the released artifacts). **macOS builds msquic from source** (`clashauto-c++/tools/build_msquic_macos.sh`, cached per version) because Microsoft ships nothing for macOS — no Release assets, no Homebrew formula, NuGet is Windows-only — and msquic **can't produce a universal binary in one pass** on Darwin (it matches `CMAKE_OSX_ARCHITECTURES` exactly when configuring quictls), so the script builds each arch and `lipo`s them; the dylib lands in `Coast.app/Contents/Frameworks/` with install name `@rpath/libmsquic.dylib`, **before** codesign. ⚠️ **The released DMG is built by the external signer repo** (`integemjack/schat.build`), which checks this repo out and runs its own build steps — it must call the same script and pass the same two `-D` flags, or the shipped DMG silently has no QUIC even though our own macOS artifact does. **TUIC stays off everywhere** until msquic 2.6 ships the keying-material exporter its token needs.
- Windows CI = **MSVC (VS 17 2022)**; Linux CI = Ninja + aqt Qt6 (bundled into the package, RPATH `$ORIGIN/lib`); macOS = aqt universal Qt6. ARM64 Windows is a cross-compile (`-DQT_HOST_PATH`).
