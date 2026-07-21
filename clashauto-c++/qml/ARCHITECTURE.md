# QML rewrite — architecture & conventions (phase 0)

This is the contract every page agent must follow so the QML rewrite stays coherent.
Phase 0 shipped the **shell + StatusPage**; the other four pages are stubs to be filled.

## Golden rules
- **Do NOT rewrite backend logic.** Reuse the existing C++ classes (`AppConfig`, `ConfigBuilder`,
  `SubscriptionStore`, `CoreController`, `ClashService`, `SubParser`, `TrayController`) as-is.
- Talk to the backend **only through `QmlBridge`** (context property `bridge`). If a page needs
  data/actions the bridge doesn't expose yet, **add a thin Q_PROPERTY / Q_INVOKABLE / list-model
  to the adapter layer** (`src/qml/`), don't reach into or edit the backend classes.
- New `.cpp` files must be added to `CMakeLists.txt` by hand; new `.qml` files must be added to the
  `qt_add_qml_module(... QML_FILES ...)` list.
- Verify config/subscription changes with the real core (`clash -t -f`) — YAML is text-manipulated,
  see the repo `CLAUDE.md`.

## File layout
```
src/main_qml.cpp          # QApplication + QQmlApplicationEngine; builds backend + bridge; loadFromModule("ClashAuto","Main")
src/qml/QmlBridge.{h,cpp} # THE facade: backend signals -> Q_PROPERTY, UI actions -> Q_INVOKABLE
src/qml/NodeListModel.{h,cpp} # QAbstractListModel of ClashService nodes (roles below)
qml/Main.qml              # shell: sidebar + StackLayout(pages) + footer
qml/Theme.qml             # pragma Singleton design tokens (dark/light)
qml/StatusPage.qml        # DONE (traffic cards + bandwidth charts + node list)
qml/{Subscriptions,Settings,Logs,About}Page.qml   # STUBS to fill
qml/{Card,NavButton,FooterSwitch,MetricCard,NodeRow,BandwidthChart}.qml   # reusable components
```
All QML lives **flat in `qml/`** (single-module layout) — the canonical Qt6 pattern that keeps type
resolution simple. The QML module URI is **`ClashAuto`**; every file just does `import ClashAuto` to
reach `Theme` (singleton) **and** all component/page types by name (`Card {}`, `NavButton {}`, …).
No directory imports. New components: drop a `.qml` in `qml/` and add it to `qt_add_qml_module`'s
`QML_FILES`. (If the file count grows unwieldy later, split into a nested module with its own
`qt_add_qml_module` + dotted URI, e.g. `ClashAuto.Components` — but not needed now.)

## How the backend is exposed (the contract)
Two context properties are set on the root context (globally visible in every QML file):
- **`bridge`** — the `QmlBridge` facade (see below).
- **`nodeModel`** — `bridge.nodeModel()` (same object), convenience for `ListView { model: nodeModel }`.

### `bridge` — properties (all NOTIFY-backed unless noted)
| property | type | meaning |
|---|---|---|
| `coreRunning` / `proxyEnabled` / `tunEnabled` | bool | status lamps (truth = `CoreController`, not the API poll) |
| `upText` / `downText` | string | formatted traffic ("1.23 MB") |
| `upBytes` / `downBytes` | real | raw bytes/s (for charts) |
| `connectionsCount` | int | active connection count |
| `totalDownText` | string | cumulative download |
| `nodeModel` | NodeListModel* | node list (CONSTANT) |
| `selectedNode` | string | active node name |
| `groups` / `selectedGroup` | stringlist / string | proxy-group selector data |
| `speedTesting` | bool | speedtest in progress |
| `mode` | string | "规则"/"全局"/"直连" |
| `lastLog` | string | latest log line (footer) |
| `version` | string | app version (CONSTANT) |
| `initialDark` | bool | theme from config at startup (CONSTANT) |

### `bridge` — invokables
`toggleCore()`, `toggleProxy()`, `toggleTun()`, `setMode(display)`, `selectNode(rawName)`,
`selectGroup(group)`, `refreshNodes()`, `runSpeedTest()`, `setNodeFilter(text)`,
`clearConnections()`, `applyMacGlass(window, dark)`.

### `bridge` — signals
`statusChanged`, `trafficChanged`, `connectionsChanged`, `nodesChanged`, `groupsChanged`,
`speedTestingChanged`, `modeChanged`, `logAppended(line)`.

### NodeListModel roles
`display` (name or "group → now"), `rawName` (for `selectNode`), `delay` (int ms),
`badgeText` ("speed/delay"), `badgeColor` (#AARRGGBB), `active` (bool). `count` property + `countChanged`.

> **Extending the bridge**: subscriptions/settings/logs will need more surface (e.g. a
> `SubscriptionListModel`, `settings` getters/setters, a full log model). Add them to `src/qml/`
> as new adapter classes or new members on `QmlBridge`, expose via context property or a bridge
> property, and document them here. Keep backend classes untouched.

## Theme tokens (`Theme.` after `import ClashAuto`)
Colors (all re-evaluate when `Theme.dark` flips): `accent`, `accentStrong`, `danger`, `shell`,
`card`, `metricBg`, `nodeRowBg`, `nodeRowActive`, `textPrimary`, `textSecondary`, `textMuted`,
`versionColor`, `inputBg`, `inputBorder`, `footerComboBg`, `switchTrackOff`, `scrollHandle`,
`hover`, `divider`.
Metrics: `radius` (5), `sidebarWidth` (120), `footerHeight` (38), `inset` (5, mac glass gap),
`iconFont` ("iconfont").
`Theme.dark` is the single source of truth for light/dark; toggle it to reskin the whole app
(SettingsPage already does). Initialize once from `bridge.initialDark` (done in Main.qml).

## Common components (all in module `ClashAuto`, use via `import ClashAuto`)
- **Card** — opaque rounded content surface (the only solid面 over the mac glass). Children go inside.
- **NavButton** — sidebar item: `label`, `current`, `clicked()`.
- **FooterSwitch** — footer toggle pill: `label`, `on`, `clicked()`.
- **MetricCard** — traffic card: `glyph` (iconfont codepoint), `title`, `value`, `accentColor`; default
  children fill a top-right corner slot.
- **NodeRow** — node list row: `display`, `badgeText`, `badgeColor`, `active`, `apply()`.
- **BandwidthChart** — self-contained Canvas sparkline: call `push(value)`.

## Light/dark
Style everything from `Theme` tokens — never hard-code hex unless it's a fixed brand color
(e.g. accent `#4898f8`, the per-metric accent colors). Both themes must look right.

## macOS frosted glass
- Window keeps the **native title bar** (not frameless) on macOS; `color: "transparent"` lets the
  glass show. `bridge.applyMacGlass(window, Theme.dark)` calls the native
  `configureMacTitleBar()` + `enableMacBlur()` (implemented in `MacWindow.mm`). It is re-applied on
  startup, on window show, and whenever `Theme.dark` changes.
- Sidebar and footer are **transparent** (glass shows through); only `Card` is opaque. On macOS the
  content column is inset `Theme.inset` (5px) from the window top/right so the card floats.
- Non-macOS: window is frameless (`Qt.FramelessWindowHint`), draggable via a `DragHandler` on the
  logo area calling `window.startSystemMove()`; `window.color` is `Theme.shell`.

## Adding a page
Pages are index-aligned 1:1 with the sidebar `Repeater` model **and** the `StackLayout` children in
`Main.qml` (order: 状态/订阅/设置/日志/关于). To add/replace a page, edit both in lock-step —
exactly like the Widgets `menu`↔`m_pages` invariant.

## Running / not breaking the live app
This same app runs live on the dev machine. The QML entry **never auto-starts the core** and never
toggles system proxy/TUN on launch — those happen only when the user clicks 核心/网页/增强.
`ClashService::start()` is read-only polling. Keep it that way in any test harness.
