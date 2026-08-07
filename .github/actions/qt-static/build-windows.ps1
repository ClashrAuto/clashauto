# 从源码编一套**静态** Qt（Windows MSVC，x64 或 arm64 交叉）。产物 = $env:QT_PREFIX。
#
# Windows 这条腿是「全静态」收益最大的：共享构建的包里除了 Qt 的十几个 DLL，还必须额外
# 拷一份 MSVC 运行库（vcruntime140/msvcp140/msvcp140_1/_2），少一个就是 0xC0000135
# 「双击没反应」——本仓库为此栽过、真机验证过、在 release.yml 里留了整段注释。
# -static-runtime 之后这些全部进 exe，包里一个 DLL 都不剩。
#
# 需要的环境变量：QT_VERSION / QT_PREFIX / QT_SRC / QT_ARCH(x64|arm64) / QT_HOST_PATH(仅 arm64)
$ErrorActionPreference = 'Stop'

$qtmm = $env:QT_VERSION -replace '\.\d+$', ''    # 6.8.3 → 6.8
# 模块集与 build-posix.sh 一字不差（没有 qtsvg / qttools，理由见那边的注释）。
$modules = @('qtbase', 'qtshadertools', 'qtdeclarative')

Write-Host "::group::[qt-static] MSVC 环境 ($env:QT_ARCH)"
# 不用第三方 action，直接问 vswhere 要安装路径再吃 vcvarsall 的环境。
# arm64 是**交叉编译**：宿主 x64 编译器产出 arm64 代码，用 x64_arm64 这个组合。
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "vswhere 找不到 Visual Studio" }
$vcvarsArch = if ($env:QT_ARCH -eq 'arm64') { 'x64_arm64' } else { 'x64' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
cmd /c "`"$vcvars`" $vcvarsArch && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:\$($matches[1])" -Value $matches[2] }
}
Write-Host "cl.exe: $((Get-Command cl.exe).Source)"
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    choco install ninja -y --no-progress | Out-Null
}
Write-Host "::endgroup::"

Write-Host "::group::[qt-static] 取源码"
New-Item -ItemType Directory -Force $env:QT_SRC | Out-Null
Set-Location $env:QT_SRC
$mirrors = @(
    "https://download.qt.io/archive/qt/$qtmm/$env:QT_VERSION/submodules",
    "https://mirrors.ustc.edu.cn/qtproject/archive/qt/$qtmm/$env:QT_VERSION/submodules",
    "https://qt-mirror.dannhauer.de/archive/qt/$qtmm/$env:QT_VERSION/submodules"
)
foreach ($m in $modules) {
    $tb = "$m-everywhere-src-$env:QT_VERSION.tar.xz"
    if (-not (Test-Path $tb)) {
        $ok = $false
        foreach ($base in $mirrors) {
            Write-Host "  $tb  <-  $base"
            try {
                Invoke-WebRequest -Uri "$base/$tb" -OutFile "$tb.part" -TimeoutSec 600
                Move-Item "$tb.part" $tb -Force; $ok = $true; break
            } catch { Write-Host "    失败: $($_.Exception.Message)" }
        }
        if (-not $ok) { Write-Host "::error::$tb 所有镜像均下载失败"; exit 1 }
    }
    if (-not (Test-Path $m)) {
        New-Item -ItemType Directory -Force $m | Out-Null
        # Windows 10+ 自带 bsdtar，认 .tar.xz
        tar -xf $tb -C $m --strip-components=1
        if ($LASTEXITCODE -ne 0) { throw "解压 $tb 失败" }
    }
}
Write-Host "::endgroup::"

Write-Host "::group::[qt-static] configure + build qtbase"
# 关键 flag：
#   -static -static-runtime  Qt 出 .lib，且链**静态 CRT(/MT)** —— 后者决定包里还要不要
#                            躺 vcruntime140*.dll。CMakeLists 那侧会同步把 app 与 Rust
#                            静态库都切到 /MT（见 CMAKE_MSVC_RUNTIME_LIBRARY 那段）。
#   -schannel -no-openssl    TLS 用 Windows 原生 Schannel，整个包不需要任何 OpenSSL DLL。
#                            本机实测共享 Qt 也是走 schannel（backends=cert-only,schannel），
#                            所以这不是"换后端"，只是把已经在用的那个显式钉死。
$common = @(
    '-static', '-static-runtime', '-release', "-prefix", $env:QT_PREFIX,
    '-opensource', '-confirm-license',
    '-nomake', 'examples', '-nomake', 'tests',
    '-no-icu', '-schannel', '-no-openssl',
    '-qt-pcre', '-qt-harfbuzz', '-qt-libpng', '-qt-libjpeg', '-qt-zlib', '-qt-doubleconversion',
    '-sql-sqlite', '-no-sql-mysql', '-no-sql-psql', '-no-sql-odbc'
)
if ($env:QT_ARCH -eq 'arm64') {
    # ★ 五套配置里**最脆**的一套：交叉编 Qt 自己。宿主 Qt 提供 moc/rcc/qmlcachegen 等工具
    #   （必须同版本），目标 mkspec 是 win32-arm64-msvc。这里失败的话先读 job 日志里
    #   configure 的 "Checking for ..." 段，别去猜 —— 本仓库为"看得到红灯看不到报错"
    #   已经吃过 14 次亏，release.yml 里那段 ::error:: 注解就是为此加的。
    if (-not $env:QT_HOST_PATH) { throw "arm64 交叉编译必须提供 QT_HOST_PATH（同版本宿主 Qt）" }
    $common += @('-platform', 'win32-arm64-msvc', '-qt-host-path', $env:QT_HOST_PATH)
}

$b = Join-Path $env:QT_SRC 'b-qtbase'
Remove-Item -Recurse -Force $b -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $b | Out-Null
Set-Location $b
& "$env:QT_SRC\qtbase\configure.bat" @common -- -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "qtbase configure 失败 (exit=$LASTEXITCODE)" }
cmake --build . --parallel
if ($LASTEXITCODE -ne 0) { throw "qtbase 构建失败" }
cmake --install .
if ($LASTEXITCODE -ne 0) { throw "qtbase 安装失败" }
Write-Host "::endgroup::"

foreach ($m in @('qtshadertools', 'qtdeclarative')) {
    Write-Host "::group::[qt-static] $m"
    $bm = Join-Path $env:QT_SRC "b-$m"
    Remove-Item -Recurse -Force $bm -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $bm | Out-Null
    Set-Location $bm
    & "$env:QT_PREFIX\bin\qt-configure-module.bat" "$env:QT_SRC\$m"
    if ($LASTEXITCODE -ne 0) { throw "$m configure 失败" }
    cmake --build . --parallel
    if ($LASTEXITCODE -ne 0) { throw "$m 构建失败" }
    cmake --install .
    if ($LASTEXITCODE -ne 0) { throw "$m 安装失败" }
    Write-Host "::endgroup::"
}

if ($env:QT_ARCH -ne 'arm64') {
    Write-Host "::group::[qt-static] OpenSSL (静态 /MT)"
    # ★ 为什么静态 Qt 会牵连出这一步：切到 /MT 之后，runner 自带的那份 OpenSSL
    #   （C:\Program Files\OpenSSL，用 /MD 编的）会在 CMakeLists 的"真链测试"里链不上，
    #   于是 COAST_HAVE_OPENSSL=OFF —— ss/vmess/reality 三个进程内出站**静默**不编入、
    #   回退 mihomo。构建仍然全绿，没人会发现。所以这里用 vcpkg 的 x64-windows-static
    #   三元组（默认就是 /MT）另编一份。
    #   放在 Qt 前缀底下的 _openssl/ 里，是为了跟静态 Qt 共用**同一个缓存条目** ——
    #   多一个 cache path 就多一处「其中一半命中」的状态要考虑，不值得。Qt 不在乎多个子目录。
    #   arm64 不做：环境里没有 arm64 的 OpenSSL，现状本来就是降级，不在本次改动范围内。
    $vcpkg = Join-Path $env:VCPKG_INSTALLATION_ROOT 'vcpkg.exe'
    if (-not (Test-Path $vcpkg)) { throw "runner 上找不到 vcpkg（VCPKG_INSTALLATION_ROOT=$env:VCPKG_INSTALLATION_ROOT）" }
    $sslRoot = Join-Path $env:QT_PREFIX '_openssl'
    & $vcpkg install openssl:x64-windows-static --x-install-root="$sslRoot"
    if ($LASTEXITCODE -ne 0) { throw "vcpkg 装 openssl:x64-windows-static 失败" }
    $lib = Join-Path $sslRoot 'x64-windows-static\lib\libcrypto.lib'
    if (-not (Test-Path $lib)) { throw "没找到 $lib —— vcpkg 布局变了？" }
    Write-Host "静态 libcrypto: $lib"
    Write-Host "::endgroup::"
}

Write-Host "[qt-static] 产物概览"
$libs = Get-ChildItem "$env:QT_PREFIX\lib" -Filter *.lib -ErrorAction SilentlyContinue
Write-Host "静态库: $($libs.Count) 个"
# ★ 门禁：静态 Qt 的 bin/ 里不该有任何 Qt6*.dll。有的话说明 -static 没生效，
#   而后面 app 照样能编能链、能打包，直到用户装上才发现缺 DLL。必须当场炸。
$dlls = Get-ChildItem "$env:QT_PREFIX\bin" -Filter 'Qt6*.dll' -ErrorAction SilentlyContinue
if ($dlls) {
    Write-Host "::error::$env:QT_PREFIX\bin 里有 $($dlls.Count) 个 Qt6 DLL —— 这不是静态 Qt"
    $dlls | Select-Object -First 10 -ExpandProperty Name
    exit 1
}
foreach ($p in @('platforms\qwindows.lib', 'platforms\qoffscreen.lib', 'sqldrivers\qsqlite.lib')) {
    if (-not (Test-Path "$env:QT_PREFIX\plugins\$p")) { Write-Host "::warning::缺插件 $p" }
}
Write-Host "QT_STATIC_BUILD_OK"
