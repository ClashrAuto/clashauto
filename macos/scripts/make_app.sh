#!/usr/bin/env bash
# 组装 Coast.app。
#
# 为什么必须打成 .app 才能验：
#   • SMAppService 的 daemon 注册要求 helper 的 plist 落在
#     Contents/Library/LaunchDaemons/ 且**可执行路径相对 bundle 根** —— 裸二进制注册不了；
#   • 版本号来自 Info.plist，不打包时界面上只会显示 "dev"；
#   • 种子资源（default.yaml/Country.mmdb 等）在 .app 里走 Contents/Resources，
#     开发期那条 #filePath 回退路径在用户机器上不存在。
#
# 用法：
#   bash scripts/make_app.sh [--version 1.2.3] [--universal] [--sign <identity>] [--no-core]
#
# 默认只构建本机架构并做 ad-hoc 签名（`-`）。发布用的签名+公证在外部仓库
# integemjack/schat.build 完成，见仓库根 CLAUDE.md。
#
# 默认**集成最新正式版内核**（fork ClashrAuto/clash 的 coast-darwin-* 产物）到
# Contents/Resources/core，首次运行由 CoreProcess.seedCoreIfMissing() 落到用户目录 ——
# 全新安装开箱即用。离线/回归自检用 --no-core 跳过（正式包不该缺内核）。
# API 有匿名限流（60 次/时，CI 共享出口 IP 极易中招）：设 GITHUB_TOKEN 可提额。
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
ASSETS="$ROOT/../clashauto-c++/assets"

# 版本号规则与 CI 完全一致：`major.minor` 取自 clashauto-c++/CMakeLists.txt 的
# project(... VERSION x.y)，末位是 git 提交数。**只有最后一位会变** ——
# major/minor 是人工决定的产品版本，不该因为本地打了一次包就跳。
#
# 写死一个 "0.1.0" 的话，本地包和 CI 包的版本对不上：升级检查按版本比大小，
# 一个 0.1.0 的本地包会认为线上任何版本都是「新版」，每次启动都提示更新。
default_version() {
    local base major minor count
    base="$(sed -nE 's/.*project[[:space:]]*\([[:space:]]*[^ ]+[[:space:]]+VERSION[[:space:]]+([0-9]+(\.[0-9]+){1,3}).*/\1/p' \
        "$ROOT/../clashauto-c++/CMakeLists.txt" 2>/dev/null | head -n 1)"
    [ -n "$base" ] || { echo "0.0.0"; return; }
    major="$(echo "$base" | cut -d. -f1)"
    minor="$(echo "$base" | cut -d. -f2)"; [ -n "$minor" ] || minor=0
    count="$(git -C "$ROOT/.." rev-list --count HEAD 2>/dev/null || echo 0)"
    echo "${major}.${minor}.${count}"
}
VERSION=""
UNIVERSAL=0
SIGN_IDENTITY="-"
BUNDLE_CORE=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --universal) UNIVERSAL=1; shift ;;
        --sign) SIGN_IDENTITY="$2"; shift 2 ;;
        --no-core) BUNDLE_CORE=0; shift ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

BUILD_ARGS=(-c release)
if [[ $UNIVERSAL -eq 1 ]]; then
    BUILD_ARGS+=(--arch arm64 --arch x86_64)
    BIN_DIR="$ROOT/.build/apple/Products/Release"
else
    BIN_DIR="$ROOT/.build/release"
fi

[ -n "$VERSION" ] || VERSION="$(default_version)"
echo "==> 构建 (version=$VERSION, universal=$UNIVERSAL)"
swift build "${BUILD_ARGS[@]}"

APP="$ROOT/Coast.app"
echo "==> 组装 $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" \
         "$APP/Contents/Resources" \
         "$APP/Contents/Library/LaunchDaemons"

cp "$BIN_DIR/Coast" "$APP/Contents/MacOS/Coast"
cp "$BIN_DIR/com.yuehongsun.coast.helper" "$APP/Contents/MacOS/com.yuehongsun.coast.helper"

# 去掉局部符号表。`swift build -c release` 产出的 Coast 里 __LINKEDIT（符号表）有 4.4 MB，
# 而 __TEXT 才 2.3 MB —— 胖包翻倍就是白搭 8 MB。符号化不受影响：调试符号在旁边的
# `.build/**/Coast.dSYM` 里单独留着，二进制里本来就没有 __DWARF。
#
# 两个讲究：
#   • 必须在 codesign **之前** —— strip 会改文件，签完再动当场破坏签名（strip 自己会警告）；
#   • 用 `-x`（只去局部符号）而不是裸 `strip`（去全部）：后者只多省 0.15 MB，
#     却把崩溃日志里的全局符号名一并抹掉，不划算。
for bin in "$APP/Contents/MacOS/Coast" "$APP/Contents/MacOS/com.yuehongsun.coast.helper"; do
    strip -x "$bin"
done

# Info.plist：把版本号替换进去
sed -e "s|<string>1.0</string>|<string>$VERSION</string>|g" \
    "$ROOT/Resources/Info.plist" > "$APP/Contents/Info.plist"

# launchd daemon plist。**文件名必须等于 Label 等于 mach service 名** ——
# SMAppService.daemon(plistName:) 就是按文件名找它的。
cp "$ROOT/Resources/com.yuehongsun.coast.helper.plist" \
   "$APP/Contents/Library/LaunchDaemons/com.yuehongsun.coast.helper.plist"

# 种子资源：仓库里只有一份，在 clashauto-c++/assets。这里拷进 Contents/Resources，
# Resources.swift 的第一条查找路径正是它。
#
# ★ 那个目录是**三平台共用**的资源池，不能整份倒进 mac 包。下面每条 exclude 都是
#   Swift 线一个字节都读不到的东西（原始 / 进 DMG 压缩后）：
#     • fonts/          15.3 MB / 10.3 MB —— MiSans。Swift 线**刻意不捆**，用系统字体 +
#                       SF Symbols（见 README「系统字体」一节、PLAN 阶段 5）；真正注册的
#                       只有 iconfont.ttf + remixicon.ttf，见 Sources/Coast/IconFont.swift。
#                       Qt 线仍然要它，所以是这里排除、不是从 assets 里删。
#     • bundle/wintun/   1.4 MB / 0.72 MB —— 四个 arch 的 wintun.dll，Windows TUN 专用。
#     • icon.ico         0.34 MB          —— Windows 图标资源。
#     • icon.icns        0.18 MB          —— 下面会从 $ASSETS 直接拷成 AppIcon.icns，
#                                            再留一份同名的就是白占一倍。
#     • app.rc.in / chevron-*.svg         —— 分别是 Windows 资源脚本模板和 Qt 下拉框箭头。
#   合计砍掉约 12 MB 压缩体积（DMG ~50 MB → ~37 MB）。
#   remixicon-LICENSE.txt **不能排除**：remixicon.ttf 是 Apache-2.0，随包分发就得带许可证。
#
#   新增资源时的判断标准很简单：Swift 侧有没有一处 Resources.asset()/seed() 会读它。
#   没有就该在这个列表里 —— 漏了只是包变胖，而误排除会被下面的清单校验当场拦住。
if [[ -d "$ASSETS" ]]; then
    echo "==> 拷贝资源 $ASSETS"
    rsync -a \
        --exclude '.DS_Store' \
        --exclude 'fonts/' \
        --exclude 'bundle/wintun/' \
        --exclude 'icon.ico' \
        --exclude 'icon.icns' \
        --exclude 'app.rc.in' \
        --exclude 'chevron-*.svg' \
        "$ASSETS/" "$APP/Contents/Resources/"
else
    echo "!! 找不到 $ASSETS —— 打出来的包缺种子配置，首次运行会没有 config.yaml" >&2
fi

if [[ -f "$ASSETS/icon.icns" ]]; then
    cp "$ASSETS/icon.icns" "$APP/Contents/Resources/AppIcon.icns"
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" "$APP/Contents/Info.plist" 2>/dev/null || true
fi

# 上面是黑名单，删错了不会编译报错、只会在用户机器上静默变成 nil（缺字体图标、
# 缺翻译、缺 OUI 表）。所以这里把 Swift 侧**真正会读的**那几项逐个点名 ——
# 清单来自全仓 Resources.asset()/seed() 的调用点，多一条 exclude 就当场断在打包这步。
# （种子 yaml 与 Country.mmdb 走 bundle/config/，CI 的 Verify bundle 另有独立检查。）
if [[ -d "$ASSETS" ]]; then
    for must in iconfont.ttf remixicon.ttf remixicon-LICENSE.txt oui.txt \
                i18n/en-US.json i18n/zh-TW.json \
                bundle/config/config.yaml bundle/config/Country.mmdb; do
        [[ -e "$APP/Contents/Resources/$must" ]] || {
            echo "!! 资源清单缺 $must —— 多半是上面的 rsync exclude 写宽了" >&2
            exit 1
        }
    done
fi

# 内核：默认集成最新**正式版**（fork ClashrAuto/clash 的 coast-darwin-* 产物）。
# 挑选规则与 CoreDownloader.pick 一字不差：走 /releases 全量列表而不是 /releases/latest
# （fork 从上游继承了一堆零资产空 tag，latest 常正好落在那上面）、跳过 draft/prerelease、
# 该 release 必须真带 darwin 产物才认；Intel 优先 -compatible（v1 基线，普通 amd64 是
# GOAMD64=v3，老 Mac 直接非法指令崩）。下载失败**打包失败**——「默认集成」的包缺了
# 内核是静默劣化，宁可当场断。
if [[ $BUNDLE_CORE -eq 1 ]]; then
    echo "==> 集成最新正式版内核"
    CORE_TMP="$(mktemp -d)"
    trap 'rm -rf "$CORE_TMP"' EXIT
    AUTH=()
    [[ -n "${GITHUB_TOKEN:-}" ]] && AUTH=(-H "Authorization: Bearer $GITHUB_TOKEN")
    curl -fsSL --retry 3 ${AUTH[@]+"${AUTH[@]}"} -H 'Accept: application/vnd.github+json' \
        'https://api.github.com/repos/ClashrAuto/clash/releases?per_page=20' \
        -o "$CORE_TMP/releases.json" || {
        echo "!! 拉取发布列表失败（403 多半是 GitHub 匿名限流，60 次/时按出口 IP 算）。" >&2
        echo "   设 GITHUB_TOKEN 提额重试，或确属离线场景时用 --no-core 跳过。" >&2
        exit 1
    }

    pick_core_url() {  # 参数 = 产物名前缀（按优先级）；release 优先、前缀次之，同 Qt CoreRelease::pick
        python3 - "$CORE_TMP/releases.json" "$@" <<'PY'
import json, sys
releases = json.load(open(sys.argv[1]))
for release in releases:  # 列表新→旧：第一个带本平台产物的正式版就是答案
    if release.get("draft") or release.get("prerelease"):
        continue
    for prefix in sys.argv[2:]:
        for asset in release.get("assets", []):
            name = asset.get("name", "")
            if name.startswith(prefix) and name.endswith(".gz"):
                print(asset["browser_download_url"])
                sys.exit(0)
sys.exit(1)
PY
    }

    fetch_core() {  # $1 = 输出文件，其余 = 前缀
        local out="$1"; shift
        local url
        url="$(pick_core_url "$@")" || { echo "!! 正式版通道里没有 $* 产物（见 $CORE_TMP/releases.json）" >&2; return 1; }
        echo "    $(basename "$url")"
        curl -fsSL --retry 3 "$url" | gunzip > "$out"
        [[ -s "$out" ]] || { echo "!! 内核下载/解压出来是空的: $url" >&2; return 1; }
        chmod 755 "$out"
    }

    if [[ $UNIVERSAL -eq 1 ]]; then
        # Go 二进制可以直接 lipo —— 和 .app 的两架构对齐，哪半跑就用哪半。
        fetch_core "$CORE_TMP/core-arm64" "coast-darwin-arm64-"
        fetch_core "$CORE_TMP/core-amd64" "coast-darwin-amd64-compatible-" "coast-darwin-amd64-v1-"
        lipo -create "$CORE_TMP/core-arm64" "$CORE_TMP/core-amd64" -output "$APP/Contents/Resources/core"
    elif [[ "$(uname -m)" == "arm64" ]]; then
        fetch_core "$APP/Contents/Resources/core" "coast-darwin-arm64-"
    else
        fetch_core "$APP/Contents/Resources/core" "coast-darwin-amd64-compatible-" "coast-darwin-amd64-v1-"
    fi
    chmod 755 "$APP/Contents/Resources/core"
else
    echo "==> 跳过内核集成（--no-core）—— 首次运行需在「设置 → 系统」手动下载"
fi

# 签名顺序有讲究：**先内层后外层**。先签 helper，再签整个 .app ——
# 反过来的话签完 app 再动里面的二进制会当场破坏外层签名。
#
# `--timestamp` 只在真签时加：公证**要求**安全时间戳，缺了会被 notarytool 直接判不合格；
# 但 ad-hoc 签名加它是无意义的网络往返（还会在离线环境下失败），所以分开。
# 注意展开写法：macOS 自带的是 **bash 3.2**，在 `set -u` 下 `"${arr[@]}"` 展开一个空数组
# 会直接报 unbound variable。`${arr[@]+"${arr[@]}"}` 是兼容 3.2 的惯用法。
TIMESTAMP_FLAG=()
if [[ "$SIGN_IDENTITY" != "-" ]]; then
    TIMESTAMP_FLAG=(--timestamp)
fi

echo "==> 签名 (identity=$SIGN_IDENTITY)"
# entitlements 只在**真签**时带：ad-hoc + entitlements 会生成一份谁都能伪造的权限，反而误导。
# 真签时它们是 hardened runtime 放行网络能力的必需品。
HELPER_ENT=()
APP_ENT=()
if [[ "$SIGN_IDENTITY" != "-" ]]; then
    HELPER_ENT=(--entitlements "$ROOT/Resources/helper.entitlements")
    APP_ENT=(--entitlements "$ROOT/Resources/coast.entitlements")
fi

# 先内层后外层。打包集成的内核也是 Mach-O —— 公证会扫到 bundle 里**每一个**可执行文件，
# 漏签它整个 .app 都过不了 notarytool。
if [[ -f "$APP/Contents/Resources/core" ]]; then
    codesign --force --sign "$SIGN_IDENTITY" \
        --identifier "com.yuehongsun.coast.core" \
        --options runtime ${TIMESTAMP_FLAG[@]+"${TIMESTAMP_FLAG[@]}"} \
        "$APP/Contents/Resources/core"
fi

# helper 单独用它自己的 entitlements。
codesign --force --sign "$SIGN_IDENTITY" \
    --identifier "com.yuehongsun.coast.helper" \
    --options runtime ${TIMESTAMP_FLAG[@]+"${TIMESTAMP_FLAG[@]}"} \
    ${HELPER_ENT[@]+"${HELPER_ENT[@]}"} \
    "$APP/Contents/MacOS/com.yuehongsun.coast.helper"

codesign --force --sign "$SIGN_IDENTITY" \
    --identifier "com.yuehongsun.coast" \
    --options runtime ${TIMESTAMP_FLAG[@]+"${TIMESTAMP_FLAG[@]}"} \
    ${APP_ENT[@]+"${APP_ENT[@]}"} \
    "$APP"

echo "==> 校验"
# `--deep --strict`：浅校验（只 `codesign -v`）在**内层二进制签坏**时照样报 OK，
# 而内层正是 helper 所在的地方 —— 那恰恰是最要命的一处。
codesign --verify --deep --strict --verbose=2 "$APP" && echo "签名校验通过"
/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist"

cat <<EOF

完成：$APP

注意：
  • 内核：$([[ $BUNDLE_CORE -eq 1 ]] && echo "已集成最新正式版（Contents/Resources/core），首次运行自动落位" \
      || echo "未集成（--no-core），首次运行需在「设置 → 系统」手动下载")
  • ad-hoc 签名（-）**装不了免密 helper** —— SMAppService 要求 helper 与主程序同属一个
    Team ID，且客户端要满足 HelperConstants.clientCodeRequirement 里那条
    "certificate leaf[subject.OU] = 6AXTRT5TV4"。本地只能验到「注册被拒」这一步。
    真正可用的 helper 必须用正式开发者证书签，走外部仓库 integemjack/schat.build。
  • .app 需放到 /Applications 或用户目录下再启动；从 .build 里直接跑，
    LaunchServices 可能不认它。
EOF
