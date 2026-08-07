#!/usr/bin/env bash
# 下载 clashauto 最新 release 的 Coast 全平台产物并验证：
#   1) 拉取 release 资产 + 校验 sha256
#   2) 结构检查：Windows 扁平/瘦身/自包含、macOS Coast.app+bundle id、Linux 扁平 .deb
#   3) Linux 无头真跑：装 .deb → Xvfb 跑 Coast → 验「能启动不崩 + 内嵌配置落地」
# 不用 set -e：要把所有检查跑完再汇总。
set -uo pipefail

REPO="${VALIDATE_REPO:-ClashrAuto/clashauto}"
WORK=/work; mkdir -p "$WORK"; cd "$WORK"
PASS=0; FAIL=0; SKIP=0
ok(){   echo "  ✅ $*"; PASS=$((PASS+1)); }
bad(){  echo "  ❌ $*"; FAIL=$((FAIL+1)); }
skip(){ echo "  ⏭️  $*"; SKIP=$((SKIP+1)); }
sec(){  echo; echo "════ $* ════"; }
shopt -s nullglob
# nullglob-safe「取匹配的第一个文件」：无匹配→空串。不用 `ls glob|head`——无匹配时 nullglob
# 会让 `ls`(无参)列出整个目录、head 取到错的文件（就踩过这个坑）。
pick(){ local a=( $1 ); printf '%s' "${a[0]:-}"; }

sec "拉取最新 release ($REPO)"
J=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest") \
  || { echo "release API 拉取失败（代理/网络？）"; exit 2; }
TAG=$(echo "$J" | jq -r '.tag_name // empty')
NAME=$(echo "$J" | jq -r '.name // empty')
echo "release: $NAME  tag: $TAG"
[ -n "$TAG" ] || { echo "没解析到 tag_name"; exit 2; }
VER="${TAG#v}"
# 缓存卷可能残留上一次 release 的产物；清掉非本版本的，避免 glob 抓到旧文件。
for f in Coast-*; do case "$f" in *"$VER"*) ;; *) rm -f "$f";; esac; done

echo "$J" | jq -r '.assets[] | [.name, .browser_download_url] | @tsv' > assets.tsv
echo "资产 $(wc -l < assets.tsv) 个，下载中…"
while IFS=$'\t' read -r n u; do
  [ -f "$n" ] || curl -fsSL --retry 3 -o "$n" "$u" || echo "  ⚠️ 下载失败: $n"
done < assets.tsv
ls -la *.zip *.exe *.dmg *.deb *.tar.gz 2>/dev/null || true

sec "sha256 校验（对每个带 .sha256 边车的产物）"
shopt -s nullglob
for s in *.sha256; do
  f="${s%.sha256}"; [ -f "$f" ] || continue
  want=$(awk '{print $1}' "$s")
  got=$(sha256sum "$f" | awk '{print $1}')
  [ "$want" = "$got" ] && ok "sha256 OK: $f" || bad "sha256 不匹配: $f (want ${want:0:12}… got ${got:0:12}…)"
done

sec "Windows 便携包 — 扁平 / 全静态（包里一个 DLL 都不该有） / 自包含"
WZ=$(pick 'Coast-*-windows-x64-portable.zip')
if [ -n "$WZ" ]; then
  echo "  包大小: $(du -h "$WZ" | cut -f1)"
  rm -rf win; mkdir win; unzip -q "$WZ" -d win
  [ -f win/Coast.exe ] && ok "Coast.exe 在包根（扁平，无 clashauto-c++ 子目录）" || bad "Coast.exe 不在根"
  { [ ! -e win/clashauto-c++ ] && [ ! -e win/Clashr-Auto ]; } && ok "无 clashauto-c++ / Clashr-Auto 目录（自包含）" || bad "仍有旧子目录"
  # ★ 2026-08 起 Qt 是**静态链接**的：包里应当只剩 Coast.exe + core.exe。
  #   这条断言取代了原来的「Qt6Quick.dll 已部署」——那时缺 DLL 是故障，现在有 DLL 才是故障。
  #   任何 DLL 出现在这里都说明某一环退回了共享构建，而那种包在装了 VS/Qt 的机器上照跑，
  #   只有干净的用户机器会 0xC0000135。
  STRAY_DLL=$(find win -iname '*.dll' | sed 's|^win/||' | sort)
  [ -z "$STRAY_DLL" ] && ok "包里没有任何 DLL（Qt/MSVC 运行库全部静态进 exe）" \
                      || { bad "包里仍有 DLL —— 静态化没生效："; echo "$STRAY_DLL" | head -10 | sed 's/^/       /'; }
  # 目录也该是平的：静态构建没有 platforms/ plugins/ qml/ 这些运行期加载目录。
  STRAY_DIR=$(find win -mindepth 1 -maxdepth 1 -type d | sed 's|^win/||' | sort)
  [ -z "$STRAY_DIR" ] && ok "包根无任何子目录（platforms/ plugins/ qml/ 都不再需要）" \
                      || { bad "包根仍有子目录："; echo "$STRAY_DIR" | sed 's/^/       /'; }
  [ -f win/core.exe ]              && ok "core.exe 已集成（打包默认带正式版内核）" || bad "缺 core.exe（打包应默认集成内核）"
else bad "没找到 windows x64 便携 zip"; fi

sec "macOS DMG — Coast.app + com.yuehongsun.coast（best-effort，7z 解 DMG）"
# release 上有**两个** mac 包，逐个验（结构要求相同）：
#   Coast-<ver>-macos-universal.dmg      —— Swift 版（macOS 26+，外部仓库签名公证后 clobber 上来）
#   Coast-<ver>-macos-universal-qt.dmg   —— Qt 版（macOS 13+，Intel + Apple Silicon，本 CI 打）
# 少一个不判失败：Swift 那个由外部仓库异步上传，本仓库的 run 结束时可能还没到。
MACDMGS=( Coast-*-macos-*.dmg )
if [ ${#MACDMGS[@]} -eq 0 ]; then
  bad "没找到任何 macos dmg"
else
  case " ${MACDMGS[*]} " in *-qt.dmg*) ;; *) skip "没见到 Qt 版 mac 包（…-macos-universal-qt.dmg）";; esac
fi
for i in "${!MACDMGS[@]}"; do
  DMG="${MACDMGS[$i]}"
  echo "  ── $DMG（$(du -h "$DMG" | cut -f1)）"
  rm -rf "macx$i" "macx2$i"; mkdir "macx$i"
  7z x -y -o"macx$i" "$DMG" >/dev/null 2>&1
  HFS=$(find "macx$i" -iname '*.hfs*' 2>/dev/null | head -1)
  [ -n "$HFS" ] && 7z x -y -o"macx2$i" "$HFS" >/dev/null 2>&1 || true
  APP=$(find "macx$i" "macx2$i" -maxdepth 5 -name 'Coast.app' -type d 2>/dev/null | head -1)
  if [ -z "$APP" ]; then
    skip "7z 解不出 $DMG 内的 Coast.app（p7zip 对 DMG 支持有限，非产物问题）——请在 Mac 上挂载手动核对"
    continue
  fi
  ok "Coast.app 存在（$DMG）"
  PL="$APP/Contents/Info.plist"
  grep -aq "com.yuehongsun.coast" "$PL" && ok "Info.plist 含 com.yuehongsun.coast（bundle id 已改）" || bad "Info.plist 没有新 bundle id"
  grep -aq "com.yuehongsun.auto" "$PL" && bad "Info.plist 仍含旧 com.yuehongsun.auto" || ok "无旧 bundle id 残留"
  [ -e "$APP/Contents/MacOS/Coast" ] && ok "Contents/MacOS/Coast" || bad "缺 MacOS/Coast"
  [ -e "$APP/Contents/MacOS/com.yuehongsun.coast.helper" ] && ok "helper = com.yuehongsun.coast.helper" || skip "没见到 helper 可执行（7z 可能没解全）"
  [ ! -e "$APP/Contents/Clashr-Auto" ] && ok "无 Contents/Clashr-Auto（自包含）" || bad "仍塞了 Contents/Clashr-Auto"
  # 两条线都默认集成内核（Swift 走 make_app.sh，Qt 走 CI 的 Bundle 步骤）。
  # 7z 对 DMG 的解包不完整，所以缺失只 skip 提示人工核对，不判失败。
  [ -e "$APP/Contents/Resources/core" ] && ok "Resources/core 内核已集成" || skip "未见 Resources/core（7z 可能没解全——请人工核对）"
  # 两条线的最低系统版本不同（Swift 26.0 / Qt 13.0），装错的人双击没反应且毫无提示，
  # 所以把 plist 里的值原样打出来供人眼核对（值的位置随 plist 格式而异，不做断言）。
  echo "     LSMinimumSystemVersion: $(grep -a -A1 'LSMinimumSystemVersion' "$PL" | tr -d '\0' | tail -1 | tr -d ' \t' || true)"
done

sec "Linux .deb — 扁平安装布局"
DEB=$(pick 'Coast-*-linux-x64.deb')
if [ -n "$DEB" ]; then
  dpkg-deb -c "$DEB" > deb-contents.txt 2>/dev/null
  grep -q '/opt/coast/coast'                    deb-contents.txt && ok "装到 /opt/coast/coast（扁平）"        || bad "deb 布局不是 /opt/coast/coast"
  grep -q '/usr/bin/coast'                       deb-contents.txt && ok "/usr/bin/coast 符号链接"              || bad "缺 /usr/bin/coast"
  grep -q '/usr/share/applications/coast.desktop' deb-contents.txt && ok "coast.desktop 桌面项"               || bad "缺 coast.desktop"
  grep -q 'clashauto-c++' deb-contents.txt && bad "deb 仍含 clashauto-c++ 子目录" || ok "无 clashauto-c++ 子目录"
  grep -q 'Clashr-Auto'   deb-contents.txt && bad "deb 仍含 Clashr-Auto"        || ok "无 Clashr-Auto"
  grep -q '/opt/coast/core$' deb-contents.txt && ok "/opt/coast/core 内核已集成（打包默认带正式版内核）" || bad "缺 /opt/coast/core（打包应默认集成内核）"
  # ★ 静态 Qt：/opt/coast 下不该再有自带的 Qt 运行时（原来是 lib/ plugins/ qml/ + qt.conf）。
  QTLEFT=$(grep -E '/opt/coast/(lib|plugins|qml)/|/opt/coast/qt\.conf' deb-contents.txt | head -5)
  [ -z "$QTLEFT" ] && ok "deb 内无自带 Qt 运行时（lib/ plugins/ qml/ qt.conf 都没了）" \
                   || { bad "deb 里仍有自带 Qt 运行时 —— 静态化没生效："; echo "$QTLEFT" | sed 's/^/       /'; }
else bad "没找到 linux x86_64 .deb"; fi

sec "Linux 无头运行冒烟（装 .deb + Xvfb 真跑 Coast）"
if [ -n "$DEB" ]; then
  # ★ 先确认这些库**装 deb 之前确实不在**——否则下面那句「依赖解析 OK」就是空话。
  #   容器镜像一旦预装了 Qt 运行时库，Depends 漏写一项也照样绿灯，只有用户的干净机器会炸。
  #   挑的这几个都不是 xvfb/xauth/dbus-x11 的依赖，正常情况下这里必须是「未预装」。
  preinstalled=""
  for l in libxcb-cursor0 libgl1 libopengl0 libx11-xcb1 libzstd1 libsm6; do
    dpkg -s "$l" >/dev/null 2>&1 && preinstalled="$preinstalled $l"
  done
  if [ -n "$preinstalled" ]; then
    bad "容器已预装 Qt 运行时库（$preinstalled）——依赖解析这一项形同虚设，请从 Dockerfile 移除"
  else
    ok "装 deb 前容器不含 Qt 运行时库（依赖解析这一项是真测的）"
  fi
  if apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq "./$DEB" >/dev/null 2>&1; then
    ok "dpkg 安装成功（.deb 声明的运行时依赖解析 OK）"
  else
    bad "deb 安装失败（依赖问题？）"
  fi
  # 安装后复核：Depends 是否真的把这些拉了进来。漏写的话上面 apt 不会报错（它只装声明了的），
  # 要到 25 秒冒烟那步才以「platform plugin 加载失败」的形式暴露——那时错因已经隔了好几层。
  missing=""
  for l in libxcb-cursor0 libgl1 libopengl0 libx11-xcb1 libzstd1 libsm6 libice6; do
    dpkg -s "$l" >/dev/null 2>&1 || missing="$missing $l"
  done
  [ -n "$missing" ] && bad "装完 deb 后仍缺:$missing —— Depends 漏写，干净系统上会起不来" \
                    || ok "Depends 已把关键运行时库全部拉入"
  if [ -x /opt/coast/coast ]; then
    # ★ 在**干净容器**里核实 Qt 真的不在动态依赖里。构建机上有 Qt，这个结论只有在这里才作数。
    echo "  ── ldd /opt/coast/coast ──"; ldd /opt/coast/coast | sed 's/^/       /'
    ldd /opt/coast/coast | grep -q 'libQt6' \
      && bad "coast 仍动态依赖 Qt —— 静态化没生效" \
      || ok "coast 不依赖任何 libQt6（Qt 已静态进二进制）"
    # ★ 插件自证。这是**最有价值的一条**：TLS 后端 / QSQLITE / 图片格式 / QML 模块在静态构建下
    #   靠链接期导入，漏了不报编译或链接错误，只是运行时少一块功能（订阅拉不到、历史不记、
    #   空窗口）。开发机上这些插件多半从别处也能加载到，只有干净系统才暴露。
    if QT_QPA_PLATFORM=offscreen HOME=/root COAST_STATICDEPS_SELFTEST=1 \
         /opt/coast/coast > staticdeps.log 2>&1; then
      ok "插件自证通过（TLS / QSQLITE / 图片格式 / QML 模块都在二进制里）"
    else
      bad "插件自证失败："; sed 's/^/       /' staticdeps.log | head -12
    fi
    export COAST_NO_AUTOSTART=1   # 跳过「自动下载并启动内核」，只验 UI 起 + 配置种子落地
    export HOME=/root
    rm -rf /root/.local/share/Coast
    # 跑两遍渲染后端：
    #  default  = 现在的默认路径（app 不再强制软件后端 → Linux 走 OpenGL RHI/llvmpipe）。
    #             无 GPU 的 Xvfb 里必须能起来，否则就是这次改动带来的回归。
    #  software = 应急退回路径（QT_QUICK_BACKEND=software 仍然有效，不再被代码覆盖）。
    for backend in default software; do
      if [ "$backend" = software ]; then export QT_QUICK_BACKEND=software; else unset QT_QUICK_BACKEND; fi
      echo "  跑 25s（COAST_NO_AUTOSTART=1，渲染后端=$backend，Xvfb）…"
      timeout 25 xvfb-run -a -s "-screen 0 1280x800x24" /opt/coast/coast >"app-$backend.log" 2>&1
      rc=$?
      echo "  退出码=$rc（124=超时仍在运行=没崩=好）"
      [ "$rc" = "124" ] && ok "[$backend] 启动后存活 25s 未退出（渲染成功、未崩溃）" \
                        || bad "[$backend] 提前退出（rc=$rc）——疑似启动即崩溃，见下方日志"
      if grep -Eiq 'Segmentation fault|core dumped|is not installed|Failed to create|Could not load|Could not find the Qt platform|cannot open shared object|symbol lookup error' "app-$backend.log"; then
        bad "[$backend] 日志出现致命错误："; grep -Ei 'Segmentation|is not installed|Failed to create|Could not (load|find)|shared object|symbol lookup' "app-$backend.log" | head -6 | sed 's/^/       /'
      else
        ok "[$backend] 日志无致命 Qt/加载错误"
      fi
    done
    unset QT_QUICK_BACKEND
    cp -f app-default.log app.log 2>/dev/null || true
    if [ -f /root/.local/share/Coast/config/config.yaml ]; then
      ok "内嵌配置已落地：~/.local/share/Coast/config/config.yaml（自包含 qrc 种子 + 可写权限 OK）"
      [ -f /root/.local/share/Coast/config/full.yaml ] && ok "full.yaml 也已生成（ConfigBuilder 跑通）" || skip "full.yaml 未生成（可能因未起内核，正常）"
    else
      bad "配置种子没落地——qrc 只读坑 / configDir 路径问题？"
    fi
    echo "  ── app.log 末尾 15 行 ──"; tail -15 app.log 2>/dev/null | sed 's/^/       /'
  else
    bad "/opt/coast/coast 不存在或不可执行"
  fi
fi

sec "Linux 便携 tar.gz — 扁平"
TG=$(pick 'Coast-*-linux-x64-portable.tar.gz')
if [ -n "$TG" ]; then
  tar tzf "$TG" > tar-list.txt 2>/dev/null
  grep -q '^Coast/coast$' tar-list.txt && ok "tar 内 Coast/coast（扁平，二进制在包根）" || bad "tar 布局不对（期望 Coast/coast）"
  grep -q 'clashauto-c++' tar-list.txt && bad "tar 仍含 clashauto-c++" || ok "tar 无 clashauto-c++ 子目录"
  grep -q '^Coast/core$' tar-list.txt && ok "tar 内已集成内核 Coast/core" || bad "tar 缺 core（打包应默认集成内核）"
else skip "没找到 linux x86_64 便携 tar.gz"; fi

sec "汇总"
echo "PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP   （release $TAG）"
if [ "$FAIL" = 0 ]; then echo "🎉 全部通过"; exit 0; else echo "⚠️ 有 $FAIL 项未通过——见上方 ❌"; exit 1; fi
