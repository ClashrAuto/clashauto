# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**Coast** (formerly "Clash Auto") — a frameless **Qt 6 / QML (Qt Quick)** desktop client that drives an external Clash/**mihomo** core process and talks to its REST API. Cross-platform: Windows, macOS, Linux.

The git repo root tracks:
- **`clashauto-c++/`** — the app source (the only thing you edit). *Historical dir name — the product is **Coast**; the folder is left named `clashauto-c++` on purpose (renaming it would churn CI paths).*
- **`validate/`** — a Docker "download latest release + validate all platforms" harness (below).
- **`.github/workflows/release.yml`** — CI / release.
- **`clash/`** — a **git submodule** ([`ClashrAuto/clash`](https://github.com/ClashrAuto/clash)), our fork of MetaCubeX/**mihomo** renamed to `coast`. Kept a submodule so upstream stays mergeable (`upstream` remote → MetaCubeX/mihomo; take upstream changes by **merge**, never by hand-porting). CI does *not* check it out — packaging downloads the fork's **released** `coast-*` binaries (latest stable) and the app can also download them at runtime, so the submodule is source-of-truth for the fork, not a build input. Clone with `--recurse-submodules` (or `git submodule update --init`) if you need it.

> **The app is self-contained.** It used to depend on a sibling `Clashr-Auto/` directory (the original Electron app, used as a runtime resource bundle). That dependency is **gone** — all seed resources (base `config/*.yaml`, `Country.mmdb`) are **embedded in the binary via qrc** (`clashauto-c++/assets/bundle/`, listed in `resources.qrc`), and the core is **bundled at package time by default** (CI's "Bundle latest stable core" steps / `macos/scripts/make_app.sh` put the latest stable `coast-*` release binary in the package; `CoreController::seedBundledCore()` — Swift: `CoreProcess.seedCoreIfMissing()` — seeds it to `userDir/command/core[.exe]` on first run, never overwriting an installed one). In-app download in「设置 → 系统」remains the update path and the fallback for dev builds. `AppConfigLoader::load()` no longer searches for any sibling directory and `AppConfig::sourceRoot` was removed. **Do not reintroduce a Clashr-Auto dependency.**

### Two UI layers — QML is shipped, Widgets is dead code

- **Shipped:** the **QML** app — `src/main_qml.cpp` + `qml/*.qml` + the `src/qml/*` C++ glue (`QmlBridge`, the `*Controller`s, the `*Model`s, `I18n`). CMake target `clashauto-qml`.
- **Dead/legacy:** the older **Qt Widgets** version — `src/MainWindow.cpp`, `src/main.cpp`, `src/TrafficChart.*`. **Not compiled** (not in the target). `MainWindow.h` is still `#include`d by `TrayController.cpp` for QWidget base methods, but `MainWindow.cpp`/`main.cpp` don't build; they're **stale** (old "Clash Auto" names, old paths). The CLI test subcommands (`--build-config`, …) lived in the dead `main.cpp` — they are **not** in the shipped build.

## Build & run

**The dev box now has a working local toolchain** (installed 2026-08-05; it used to have none, and older notes saying "you can't build locally" are stale). What's there:

| | |
|---|---|
| Qt 6.8.3 mingw_64 | `C:\Qt\6.8.3\mingw_64` (aqtinstall) |
| MinGW 13.1 / Ninja / CMake | `C:\Qt\Tools\{mingw1310_64,Ninja,CMake_64}` |
| Rust | `rustup`, default toolchain **`stable-x86_64-pc-windows-gnu`** |
| Npcap SDK | `C:\Users\ultra\npcap-sdk` |

⚠️ **rustup's default host is MSVC and that does not work here.** `rustup target add x86_64-pc-windows-gnu` alone is not enough: cargo builds each dependency's *build script* for the **host**, so an MSVC host goes looking for `link.exe` and the build dies with `error: linker 'link.exe' not found` on `heapless`/`smoltcp`. Install the GNU **toolchain** and make it default.

There is also a **Linux build box** at `root@192.168.20.239` (Ubuntu 24.04, 8 cores) with Qt 6.8.3 under `/opt/Qt`, a source copy in `/root/coast`, and the mihomo core at `/root/core/coast` for `-t` config validation. Use it for the POSIX side and for anything that needs a real `core -t -f` run.

Still true: **CI is the final word** for packaging, and the `validate/` Docker tool checks the released artifacts.

```powershell
# From clashauto-c++/. Put Qt + MinGW on PATH first.
$env:Path='C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.8.3\mingw_64\bin;' + $env:Path
$env:NPCAP_SDK='C:\Users\ultra\npcap-sdk'   # Windows only: L2Endpoint_win.cpp needs pcap.h
cmake -S . -B build-ninja -G Ninja `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\mingw_64 `
  -DCMAKE_CXX_COMPILER=C:\Qt\Tools\mingw1310_64\bin\g++.exe
cmake --build build-ninja
.\build-ninja\Coast.exe          # OUTPUT_NAME is "Coast" on Windows (was clashauto.exe)
```

**Windows builds also need `cargo` on PATH** — the transparent gateway's TCP data plane is a Rust staticlib (`rust/coaststack`, smoltcp; see `src/net/coaststack.h`). It's Windows-only: Linux uses TPROXY and macOS uses pf rdr, so neither needs a userspace stack (`NetStack.cpp` is `if(WIN32)`; the other platforms link `NetStack_stub.cpp`, whose `init()` just fails). `-DCOAST_RUST=OFF` skips cargo entirely and falls back to that same stub — the gateway is then unavailable on Windows too, but the app builds and runs. There is **no lwIP** any more (removed 2026-08; the whole 23-round evaluation and the removal record are in `clashauto-c++/docs/lwip-alternatives.md`).

`find_package(Qt6 … Widgets Network Qml Quick QuickControls2)`. `AUTOMOC`/`AUTORCC`/`AUTOUIC` are on, so new `Q_OBJECT` classes and `.qrc`/`.qml` changes are picked up — but **new `.cpp` files must be added by hand** to `CMakeLists.txt` (`BACKEND_SOURCES` / `QML_GLUE_SOURCES`), and new `.qml` files to the `qt_add_qml_module(... QML_FILES ...)` list. Build dirs: `.gitignore` has a `build-*/` wildcard (it used to be four hardcoded names, and the one dir not on that list nearly got `git add -A`'d into a commit), so any `build-<whatever>/` you create is ignored at any depth.

## Verifying a release — `validate/`

There is **no unit-test framework** for the C++ side (the Rust crate has `cargo test`: 19 tests, run in CI). Verification = CI builds+packages green, then `validate/` checks the packaged artifacts. Headless self-test hooks worth knowing (all are env vars on the built binary, exit 0 = pass): `COAST_RUSTSTACK_SELFTEST` (Rust C-ABI round trip), `COAST_SMOLGW_SELFTEST` (whole gateway data plane: synthetic frames → SYN-ACK → SOCKS CONNECT with the right per-device user), `COAST_NDP_RA_SELFTEST` (RA parsing — the only guard against the silent "dual-stack device leaks IPv6" failure), `COAST_TPROXY_SELFTEST` / `COAST_PF_SELFTEST` (kernel rule layers; need root), `COAST_ARPPARSE_SELFTEST` (system `arp` output → per-interface IP→MAC; all three platforms' formats, dispatched at runtime so Linux CI covers the Windows one too), `COAST_NICEGRESS_SELFTEST` (the per-NIC listener generation: builds a throwaway device ledger, really runs `ensureFullConfig`, asserts on the product, and prints the `full.yaml` path), `COAST_TOPO_DUMP` (not a test — a live dump of what this machine's topology actually resolved to: primary NIC, per-NIC gateway + gateway MAC, the ARP table split per interface, cross-interface conflicts). ⚠️ The TPROXY/pf **data planes** have no end-to-end self-test — a known gap, not a solved problem.

★ **Which uplink a proxied device leaves through is decided by the *inbound*, not by any rule.** Coast emits **one listener per NIC on each datapath**, each carrying `interface-name`: SOCKS `coast-gw-<i>` on `kGatewayPort + i` (Windows/userspace stack — `LanGateway` dials the port matching the device's NIC) and TPROXY `coast-tproxy-<i>` on `kTproxyPort - i` (Linux — `TproxyRules` dispatches by `iifname`, since a forwarded packet already carries its ingress NIC; that beats keying on device IP, which changes on DHCP renewal). The two port series grow in opposite directions so they can't collide. The core-side support for that key is ours (`clash` submodule: `component/dialer/egress.go` — listener → `Metadata.EgressInterface` → ctx → dialer, slotted into the existing precedence chain between an outbound's explicit `interface-name` and the global `DefaultInterface`). Don't try to express this with rules or outbounds: the config language can only bind an interface to an *outbound object*, so per-device egress there means duplicating the whole node table and proxy-group set per NIC — and it can only ever retarget `DIRECT`, which real subscriptions don't use for public destinations. Old cores simply ignore the unknown listener key, so no version gate is needed.

★ **The one check that matters most for anything touching config generation**: let the core itself judge the YAML.

```bash
/root/core/coast -t -f <full.yaml> -d <dir>   # <dir> must contain Country.mmdb, else it tries to download GeoIP and times out
```

`COAST_NICEGRESS_SELFTEST` prints a path made for exactly this. Every YAML in this repo is hand-assembled from strings, so **a broken product is invisible at compile time and costs the whole config** (the core refuses to load it — not "one feature is off", but "the proxy doesn't start"). Gotchas learned the hard way:

- **`-t` exits 0 either way.** Failure prints `configuration file … test failed` and *still* returns rc=0. Grep the message; never gate on the exit code.
- **`-t` does not resolve `interface-name`.** A listener bound to a nonexistent NIC passes `-t` cleanly. Only a real dial can falsify that binding (below).
- Without the mmdb in `-d <dir>` the run fails on a *download timeout* that reads like a config error.
- On Windows the product is CRLF, so any test asserting across lines must normalise `\r\n` first — the product itself is fine, a CRLF `full.yaml` passes `-t`.
- **Don't hand-copy the seed `config/*.yaml` from a Windows checkout to a Mac/Linux box.** `core.autocrlf=true` means the working tree has CRLF while the blob is LF; `mergePlugin`'s `^dns:$` match then misses and the plugin block gets **appended instead of replacing**, producing a duplicate `dns:`/`tun:` that the core rejects outright. It looks exactly like a generator bug and is not one. `git clone` on the target box, or strip the CRs after copying.

**Verifying per-NIC egress for real** (the `-t` pass proves nothing here): point the listener at a **numeric destination IP** and watch `netstat -ibn` byte deltas per interface before/after. Use a numeric IP because a machine running any other TUN VPN resolves names to **fake-ip** (198.18.x.x) through the system resolver — those are routable only inside *that* VPN, so a correctly-bound listener times out and looks broken. Real result on the dual-uplink Mac (2026-08-05, core v1.10.4393): `interface-name: en0` → en0 +2344 B / en1 **+0**; `interface-name: en1` → en0 **+0** / en1 +2392 B; unbound → follows the default route. Same shape as the Pi's Linux run.

```bash
bash validate/run.sh      # Docker: pull latest release's Coast artifacts + verify all platforms
```

It downloads every platform's Coast artifact from the latest `ClashrAuto/clashauto` release, checks sha256, runs **structural** checks (Windows flat / slimmed / self-contained, macOS `Coast.app` + `com.yuehongsun.coast` bundle id + helper, Linux flat `/opt/coast/coast`), and **actually runs the Linux build headless** under Xvfb — **twice, once per render backend** (default RHI/OpenGL, then `QT_QUICK_BACKEND=software`) — to confirm it launches on both paths and seeds its embedded config to `~/.local/share/Coast/config/`. Windows/macOS binaries can't execute in Linux Docker → structural only (real-machine run is the final word). See `validate/README.md`.

## Architecture (clashauto-c++/src)

**Backend** (shared, framework-agnostic C++):

- **`AppConfig` / `AppConfigLoader`** — config model + paths. `clashExecutable()` → the mihomo core under `userDir/command` (downloaded in-app; prefers `command/core[.exe]`, falls back to legacy `command/clash/clash-<os>-<arch>`). Seed resources come from qrc (`:/assets/bundle/*`), not any sibling dir. **`AppConfig::makeWritable()` — qrc-copied files are read-only; this restores owner-write** (config.yaml seeding + every later save depends on it, or they silently fail). Parses YAML with regex helpers (YAML note below).
- **`ConfigBuilder`** — generates `full.yaml` into `configDir`: merges base config, plugin DNS/TUN, subscription proxies, proxy groups, auto region groups. `ensureFullConfig(tunEnabled)` is the entry point. `applyCustomRules()` consumes `configDir/rules.json` (settings-page area/rule CRUD): `area` → regex custom proxy-groups (wired into the first selector), `rule` → prepended to `rules:`. A settings/rule/subscription change triggers `CoreController::rebuildConfig()` (regenerate + hot-reload).
- **`SubscriptionStore`** — owns `configDir/subscribe.yaml`: add/enable/disable subs and individual nodes, remote/local fetch + `sub`-format conversion, incremental update, allow/no-allow filtering (`nodeAllowed`).
- **`CoreController`** — launches/stops the core via `QProcess` (`-d userDir -f configDir/full.yaml`); on first run seeds `Country.mmdb` (from qrc) into userDir. (It used to also extract a per-arch `wintun.dll` next to the core on Windows — **removed**: sing-tun `//go:embed`s the driver into the core and loads it from memory via `memmod`, never from disk. Verified byte-for-byte against a shipped `coast-windows-amd64-compatible.exe`. Don't add it back.) Toggles system proxy **natively** (Windows WinINET `InternetSetOption` / macOS SCPreferences via a **root helper** / Linux `gsettings` — no bundled binary) + TUN. Hot-reloads via PUT `/configs`. Emits `statusChanged`/`logUpdated`.
- **`ClashService`** — polls the core's REST API on `host:uiPort` (**default `127.0.0.1:9191`** — avoids the original 9090): `/traffic`, `/connections`, `/proxies`; sets mode via `/configs`; selects nodes via `/proxies/<group>`; `DELETE /connections[/<id>]`; download speed-test. Async via `QNetworkAccessManager`; emits Qt signals.
- **`TrayController`** — system tray menu, traffic display, quick core/proxy/TUN toggles, `notify()`. (`#include`s `MainWindow.h` only for QWidget base methods.)

**QML UI layer** (`src/main_qml.cpp` + `qml/` + `src/qml/`):

- **`main_qml.cpp`** — `QApplication` + `QQmlApplicationEngine`; sets app/org name **"Coast"** (→ data dir); uses **Qt's default RHI backend** (Windows D3D11 / macOS Metal / Linux OpenGL) — it used to force the software backend, but that backend can't antialias curved edges (small circles/rounded rects get a 1-pixel hard step; `antialiasing`/`layer.smooth`/`layer.textureSize` are all no-ops there) and can't do distance-field text. GPU-less machines are still fine (D3D11 falls back to Windows' built-in WARP; Linux has Mesa llvmpipe via the `.deb`'s `libgl1`). **Don't re-add `setGraphicsApi(Software)`** — `QT_QUICK_BACKEND=software` is the escape hatch and must stay un-overridden. Also sets the default font's `PreferNoHinting` + `PreferAntialias` (unhinted glyphs; the old default was full hinting, which read as over-sharpened). Registers MiSans, `loadFromModule("ClashAuto","Main")` (the QML module URI is kept **`ClashAuto`** internally — invisible to users). `COAST_NO_AUTOSTART=1` skips auto-starting the core (used by headless smoke tests / local UI dev).
- **`QmlBridge`** — the thin glue exposing the shared backend to QML (status lights, traffic, nodes, mode, toggles, notifications; `persistConfigBool` writes `configDir/config.yaml`).
- **`src/qml/*Controller` + `*Model`** — `SubscriptionsController`, `SettingsController`, `UpdateController`, `AboutController`, `I18n` (12-language JSON tables in `assets/i18n/`, loaded from qrc), plus `NodeListModel` / `ConnectionsModel` / `LogModel`. Models update **incrementally** (`dataChanged`/`beginInsertRows`, deliberately **never** `beginResetModel`).
- **`qml/`** — `Main.qml` (shell: sidebar + `StackLayout` pages + footer), pages (`StatusPage`/`NodesPage`/`DevicesPage`/`SubscriptionsPage`/`SettingsPage`/`LogsPage`/`AboutPage` — sidebar order = `StackLayout` index, keep the two lists in `Main.qml` in sync), reusable components (`Card`/`NavButton`/`FooterSwitch`/`MetricCard`/`NodeRow`/`BandwidthChart`/`LogTimeline`), extra windows (`ConnectionsWindow`/`UpdateWindow`/`RuleEditorWindow`), and `Theme.qml` (singleton design tokens). `BandwidthChart.qml` is a `Canvas` realtime line chart.

Data flow: QML → `QmlBridge`/controllers → `ConfigBuilder`/`SubscriptionStore` build `full.yaml` → `CoreController` starts the core → `ClashService` polls REST → signals update QML.

### YAML is manipulated as text, not parsed

There is **no YAML library**. `AppConfig`, `ConfigBuilder`, `SubscriptionStore` read and rewrite YAML with `QRegularExpression` + manual string surgery (`setScalar`, `replaceProxyListAt`, `parseProxyList`). Preserve exact indentation and key formatting, and verify the result with `mihomo -t -f full.yaml` — malformed output isn't caught at compile time.

### Runtime data locations

Under Qt `AppDataLocation`, rebased to a flat brand dir (**no migration** from the old `%AppData%\ClashAuto\Clash Auto\clash-auto\` — a rename = fresh state). On Windows:
- **`userDir = %AppData%\Coast\`** — the core's `-d` home: `logs\`, `Country.mmdb`, cache, `command\` (downloaded core; older installs may still have an orphaned `wintun.dll` here — harmless, nothing reads it).
- **`configDir = %AppData%\Coast\config\`** — `config.yaml` (user copy, seeded from qrc on first run), generated `full.yaml`, plus `default.yaml`/`plugin.yaml`/`subscribe.yaml`/`rules.json`.
- **`configDir/coast.db`** — the app's only SQLite database (Qt6::Sql + QSQLITE, opened via `src/Sqlite.h`): `device` (the Devices-page ledger — identity/alias/proxy toggle/per-device policy/traffic counters) and `conn` (browsing history: one row per closed connection, 30-day retention). Three connections in-process: `DeviceStore`, `HistoryStore`, and a short-lived read-only one `ConfigBuilder` uses to find proxied devices (`DeviceStore::proxiedDevices`). Migrated automatically from the earlier `devices.json` + `history.db` (old ledger left as `devices.json.migrated`). Headless checks: `COAST_DEVICEDB_SELFTEST=1`, `COAST_HISTORY_SELFTEST=1`.

(macOS: `~/Library/Application Support/Coast`; Linux: `~/.local/share/Coast`.)

### Fonts — MiSans everywhere

One family, **`MiSans`**, bundled in `clashauto-c++/assets/fonts/` (committed, embedded via `resources.qrc`): `MiSans-Regular.ttf` + `MiSans-Semibold.ttf` (Semibold is the typographic-family bold face so `font.bold` maps to it instead of synthesizing). No monospace. `main_qml.cpp` sets the **global default app font to MiSans**, so every QML `Text`/control that doesn't set `font.family` inherits it — you essentially never set `font.family`. `Theme.uiFont` (`"MiSans"`) is named explicitly only where it isn't inherited: `Canvas`-drawn text (e.g. `BandwidthChart.qml`'s `ctx.font`).

## Naming, packaging & branding — per platform

Product is **Coast**; names follow each platform's convention:

- **Windows** — `Coast.exe`; installs to `%LOCALAPPDATA%\Coast`; portable zip is **flat** (exe + Qt runtime at the root, no `clashauto-c++`/`Clashr-Auto` subdirs).
- **macOS — two product lines ship side by side, distinguished only by a `-qt` in the asset name:**
  - **Swift** (`macos/`) — the primary one. Deployment target **macOS 26** (`Package.swift` `platforms` + `Resources/Info.plist` `LSMinimumSystemVersion`); the whole UI is built on Liquid Glass, so below 26 there is nothing to fall back to. **arm64 only** (since 2026-08 — `make_app.sh` without `--universal`; an Intel slice on a macOS-26-only line just doubles the package). Asset `Coast-<ver>-macos-**arm64**.dmg`.
  - **Qt** (`clashauto-c++/`, CI job `macos`) — the compatibility line for machines that can't run macOS 26. `CMAKE_OSX_DEPLOYMENT_TARGET=13.0`, **universal** (Intel + Apple Silicon) — this is the line Intel machines get. Asset `Coast-<ver>-macos-universal-**qt**.dmg`.
  - **Neither is signed or published by this repo's CI** — it has no Developer ID private key and shouldn't. Both mac jobs only build (as a compile/structure self-check) and upload an unsigned Actions artifact. Signing + notarization + release upload happen in the EXTERNAL repo `integemjack/schat.build`, branch `clashauto-mac`: `clashauto-mac.yml` (Swift) and `clashauto-mac-qt.yml` (Qt). Both check out the *same* clashauto sha and rebuild from source, so their build steps must stay in sync with the corresponding job here. `Ireoo` can't push to `integemjack/schat.build` — needs an `integemjack` PAT.
  - The **`trigger-mac`** job is what kicks both off: one empty commit whose message carries `release-tag` + **both** `product: coast-swift` and `product: coast-qt`. It is deliberately a single job pushing a single commit — two jobs pushing to the same branch concurrently would collide on a non-fast-forward, and the failure is silent (one line just never gets signed).
  - Both are `Coast.app` / bundle id `com.yuehongsun.coast` / helper `com.yuehongsun.coast.helper` (**launchd Label = mach service = plist filename = codesign `-i` must all match**, see `helper/HelperProtocol.h`).
  - ★ **The `-qt` marker is a hard contract, not cosmetics.** Both DMGs live on the same release, and both updaters pick assets by extension/keyword. Drop or rename the marker and each line starts silently pushing users onto the other app — which, since a one-click update deletes the old `.app` before installing the new one, leaves them with an app their OS can't launch and no way back.
  - **Both sides fail closed — neither ever falls back to the other line's package.** Swift: `UpdateChecker.isQtLine` filters `-qt` out, and `macAsset`/`AppUpdater.pickAsset` return nil rather than offer one. Qt: `isQtLineAsset` (in `UpdateController.cpp`) drops non-`qt` mac assets in `fetchReleases` so they're never even listed, and `recommendedIndex`'s macOS branch returns `-1` + a status message instead of picking any `.dmg`. The reachable case is "one line's signing job failed, so the newest release only has the other line's package" — and clients only ever look at the *newest* release, so falling back there is always wrong.
- **Linux** — binary `coast` (lowercase, command convention); `.deb` installs flat to `/opt/coast/coast` + `/usr/bin/coast` symlink + `coast.desktop`; Debian `Package: coast`. **The `.deb` must `Depends` on `libopengl0`** — Qt6::Gui hard-links `libOpenGL.so.0` (an ELF NEEDED entry) even under the software backend; without it a clean system fails to launch.
- **Release assets are `Coast-<ver>-<platform>-<arch>-…`** (renamed from `ClashAuto-*` on 2026-08, together with the release **name** `Coast <ver>`). Renaming the prefix is safe *because* the in-app updaters match on platform/arch/extension, never on the brand prefix (`UpdateController::fetchReleases` + `recommendedIndex`, `UpdateChecker.macAsset`) — keep it that way.
- **Still `ClashAuto` on purpose** (do not "fix"): the QML module URI `ClashAuto`, the CMake target/dir names, the repo dir `clashauto-c++`, the GitHub repo `ClashrAuto/clashauto`, and the schat.build trigger branch `clashauto-mac`.

## Releases & CI (`.github/workflows/release.yml`)

- Version **auto-increments per commit**: `major.minor` from `project(... VERSION x.y.z)` in `CMakeLists.txt` + git commit count (`git rev-list --count HEAD`) → `major.minor.<count>`, tag `v<version>` (hence `fetch-depth: 0`). `APP_VERSION` → CMake `configure_file` → `Version.h` → shown in-app (sidebar `Ver:` + About).
- **Every push to any branch** builds + publishes a GitHub Release: Windows x64/arm64 (portable zip + NSIS setup), Linux x64/arm64 (tar.gz/zip + `.deb`) straight from this CI, plus macOS universal DMG ×2 (Swift + `-qt`) that arrive **asynchronously** from schat.build. PRs build artifacts but don't publish.
  - `master`/`main` → **stable** release, tag `v<version>`, `make_latest: true`.
  - any other branch → **prerelease**, tag `v<version>-beta.<sha7>` (the sha7 only disambiguates two branches landing on the same commit count; `APP_VERSION` itself stays purely numeric because asset filenames and the in-app version compare depend on it), release name `Coast <ver> (beta · <branch>)`, `make_latest: false`. Betas get both macOS DMGs too — `trigger-mac` passes the **target release tag** to schat.build, so a beta's signed DMGs land on the beta release instead of clobbering the stable one. (That clobbering — from an older trigger that pushed a bare empty commit and let the signer guess `master` — is why beta mac packages were disabled for a while.)
  - The app's **`beta` setting** (`config.yaml`, Settings → 程序更新 → 接收测试版, default off) decides whether prereleases are visible: `AboutController::check()` (the update badge) skips them when off, and `UpdateWindow.qml` picks `UpdateController`'s release vs beta channel from it. Note `/releases/latest` excludes prereleases by GitHub's own definition — that's why the check reads the full `/releases` list and filters itself.
- **Self-contained CI** — no Clashr-Auto download/staging; the package is the exe + Qt runtime + a bundled `core[.exe]` (resources embedded in the exe; the "Bundle latest stable core" step downloads the latest **stable** `coast-<os>-<arch>` release asset from `ClashrAuto/clash`, same pick rules as `CoreRelease.h`, and fails the build if it can't). `windeployqt` runs with **`--no-opengl-sw --no-system-d3d-compiler`** to drop ~24 MB of fallback DLLs — still fine after the move to the D3D11 RHI backend: WARP ships with Windows (no `opengl32sw.dll` needed unless Qt falls back to the OpenGL RHI), and Qt Quick's shaders are pre-compiled (no runtime `d3dcompiler_47.dll`). If a GPU-less machine ever fails to start, that pairing is the first thing to re-check.
- Windows CI = **MSVC (VS 17 2022)**; Linux CI = Ninja + aqt Qt6 (bundled into the package, RPATH `$ORIGIN/lib`); macOS = aqt universal Qt6. ARM64 Windows is a cross-compile (`-DQT_HOST_PATH`).
