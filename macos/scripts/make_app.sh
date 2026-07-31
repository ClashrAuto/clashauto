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
#   bash scripts/make_app.sh [--version 1.2.3] [--universal] [--sign <identity>]
#
# 默认只构建本机架构并做 ad-hoc 签名（`-`）。发布用的签名+公证在外部仓库
# integemjack/schat.build 完成，见仓库根 CLAUDE.md。
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
ASSETS="$ROOT/../clashauto-c++/assets"

VERSION="0.1.0"
UNIVERSAL=0
SIGN_IDENTITY="-"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --universal) UNIVERSAL=1; shift ;;
        --sign) SIGN_IDENTITY="$2"; shift 2 ;;
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

# Info.plist：把版本号替换进去
sed -e "s|<string>1.0</string>|<string>$VERSION</string>|g" \
    "$ROOT/Resources/Info.plist" > "$APP/Contents/Info.plist"

# launchd daemon plist。**文件名必须等于 Label 等于 mach service 名** ——
# SMAppService.daemon(plistName:) 就是按文件名找它的。
cp "$ROOT/Resources/com.yuehongsun.coast.helper.plist" \
   "$APP/Contents/Library/LaunchDaemons/com.yuehongsun.coast.helper.plist"

# 种子资源：仓库里只有一份，在 clashauto-c++/assets。这里整份拷进 Contents/Resources，
# Resources.swift 的第一条查找路径正是它。
if [[ -d "$ASSETS" ]]; then
    echo "==> 拷贝资源 $ASSETS"
    rsync -a --exclude '.DS_Store' "$ASSETS/" "$APP/Contents/Resources/"
else
    echo "!! 找不到 $ASSETS —— 打出来的包缺种子配置，首次运行会没有 config.yaml" >&2
fi

if [[ -f "$ASSETS/icon.icns" ]]; then
    cp "$ASSETS/icon.icns" "$APP/Contents/Resources/AppIcon.icns"
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" "$APP/Contents/Info.plist" 2>/dev/null || true
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

# 先内层后外层。helper 单独用它自己的 entitlements。
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
  • ad-hoc 签名（-）**装不了免密 helper** —— SMAppService 要求 helper 与主程序同属一个
    Team ID，且客户端要满足 HelperConstants.clientCodeRequirement 里那条
    "certificate leaf[subject.OU] = 6AXTRT5TV4"。本地只能验到「注册被拒」这一步。
    真正可用的 helper 必须用正式开发者证书签，走外部仓库 integemjack/schat.build。
  • .app 需放到 /Applications 或用户目录下再启动；从 .build 里直接跑，
    LaunchServices 可能不认它。
EOF
