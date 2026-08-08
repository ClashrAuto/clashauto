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

# ★★ Ninja 必须**确实在 PATH 上**，这是这条腿最贵的一课。
#
#   Qt 的 configure 在找不到 ninja 时不会报错，而是静默退回（QtProcessConfigureArgs.cmake）：
#       find_program(ninja ninja); if(ninja) → Ninja
#       else() → Windows + cl.exe → "NMake Makefiles"（只有装了 jom 才升级成 JOM）
#   而**纯 NMake 是严格单线程的**，`cmake --build --parallel` 对它毫无作用。
#
#   第一版这里写的是「没有就 choco install」——但 choco 装完只改机器级 PATH，
#   **当前进程和它的子进程都看不到**，于是 configure.bat 照样找不到 ninja、
#   照样退回 NMake。后果不是报错，是慢到天上去：Linux 31–41 分钟、macOS 通用二进制
#   48 分钟能编完的同一套东西，Windows 两个 job 双双跑满 GitHub 的 6 小时上限被掐死，
#   而日志要仓库 admin 才能拉，红灯里只有一句 "exceeded the maximum execution time"。
#
#   所以现在：按顺序找 → 找不到就装并**显式把目录加进本进程 PATH** → 还没有就**当场硬失败**。
#   下面还会把最终结果作为 ::warning:: 打出来 —— annotation 是公开可读的，
#   万一下次又超时，至少能一眼看出用的是不是 Ninja。
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $ninja) {
    # VS 自带一份，vcvarsall 不一定会把它挂上 PATH
    $bundled = Get-ChildItem (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe') -ErrorAction SilentlyContinue |
               Select-Object -First 1 -ExpandProperty FullName
    if ($bundled) {
        $env:Path = (Split-Path $bundled) + ';' + $env:Path
        $ninja = $bundled
    }
}
if (-not $ninja) {
    choco install ninja -y --no-progress | Out-Null
    # ★ 关键：choco 只改机器级 PATH，本进程不会自动生效，必须自己接上。
    foreach ($p in @("$env:ProgramData\chocolatey\bin", "$env:ChocolateyInstall\bin")) {
        if ($p -and (Test-Path (Join-Path $p 'ninja.exe'))) { $env:Path = "$p;$env:Path" }
    }
    $ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
}
if (-not $ninja) {
    Write-Host "::error::PATH 上没有 ninja —— Qt 的 configure 会**静默**退回单线程 NMake，"
    Write-Host "::error::结果是这个 job 跑满 6 小时被掐掉。宁可现在就失败。"
    exit 1
}
$cores = [int]$env:NUMBER_OF_PROCESSORS
Write-Host "::warning::静态 Qt 构建环境: ninja=$ninja  cores=$cores  arch=$env:QT_ARCH"
Write-Host "ninja 版本: $(& $ninja --version)"
Write-Host "::endgroup::"

# 每个模块结束时把耗时作为 annotation 打出来。日志要 admin 才能读，annotation 不用 ——
# 下次再撞上超时，至少知道是卡在哪个模块、之前那些用了多久。
$swAll = [Diagnostics.Stopwatch]::StartNew()
function Report-Stage([string]$stage, [Diagnostics.Stopwatch]$sw) {
    Write-Host ("::warning::[qt-static] {0} 用时 {1:N1} 分钟（累计 {2:N1} 分钟）" -f `
        $stage, $sw.Elapsed.TotalMinutes, $swAll.Elapsed.TotalMinutes)
}

# Windows Defender 实时扫描会挨个扫过每一个 .obj，在 GH runner 上是数倍的开销。
# 装 Qt 这种几万个目标文件的构建尤其明显。失败不阻断（权限不够就算了）。
#
# ★★ 必须带**硬超时**，不能只 try/catch。Add-MpPreference 在 Defender 正在更新特征库时
#    会挂住不返回 —— 那不是异常，catch 接不到，脚本就停在这里，一直停到 GitHub 的 6 小时
#    上限把整个 job 掐掉。2026-08-08 实测：`静态 Qt 构建环境: ninja=...` 这条注解打出来了，
#    而它**之后**的每一条（包括第三道闸那条生成器断言）一条都没有 —— 六小时里连 qtbase 的
#    configure 都没跑到。这一句正是那两条注解之间的第一件事。
#    真挂住了就放弃排除项继续走（只是慢一点），绝不拿整个 job 陪葬。
$swStage = [Diagnostics.Stopwatch]::StartNew()
$mp = Start-Job -ScriptBlock {
    param($src, $prefix)
    Add-MpPreference -ExclusionPath $src, $prefix -ErrorAction Stop
} -ArgumentList $env:QT_SRC, $env:QT_PREFIX
if (Wait-Job $mp -Timeout 60) {
    if ($mp.State -eq 'Completed') {
        Write-Host "已给 $env:QT_SRC / $env:QT_PREFIX 加 Defender 排除项"
    } else {
        Write-Host "::warning::加不上 Defender 排除项（$($mp.ChildJobs[0].JobStateInfo.Reason)）——构建会慢一些"
    }
} else {
    Stop-Job $mp -ErrorAction SilentlyContinue
    Write-Host "::warning::Add-MpPreference 60 秒没返回，已放弃排除项继续构建（构建会慢一些）"
}
Remove-Job $mp -Force -ErrorAction SilentlyContinue
Report-Stage 'Defender 排除项' $swStage

Write-Host "::group::[qt-static] 取源码"
New-Item -ItemType Directory -Force $env:QT_SRC | Out-Null
Set-Location $env:QT_SRC
$mirrors = @(
    "https://download.qt.io/archive/qt/$qtmm/$env:QT_VERSION/submodules",
    "https://mirrors.ustc.edu.cn/qtproject/archive/qt/$qtmm/$env:QT_VERSION/submodules",
    "https://qt-mirror.dannhauer.de/archive/qt/$qtmm/$env:QT_VERSION/submodules"
)
foreach ($m in $modules) {
    $swStage = [Diagnostics.Stopwatch]::StartNew()
    $tb = "$m-everywhere-src-$env:QT_VERSION.tar.xz"
    if (-not (Test-Path $tb)) {
        $ok = $false
        foreach ($base in $mirrors) {
            Write-Host "  $tb  <-  $base"
            # ★ 用 curl.exe（Win10+ 自带）而不是 Invoke-WebRequest：这里要的是「卡住就换下一家」，
            #   而 IWR 的 -TimeoutSec 只管**每次读操作**，一条一直涓流的连接可以拖到天亮，脚本
            #   就停在这一句上 —— 与上面 Add-MpPreference 同一类失败：不是异常，catch 接不到。
            #   --speed-limit/--speed-time 是关键那对：连续 60 秒低于 10 KB/s 就判死并返回非零。
            #   --max-time 20 分钟兜底（qtbase 源码包 ~50MB，正常一两分钟）。
            & curl.exe -fSL --connect-timeout 30 --max-time 1200 `
                --speed-limit 10240 --speed-time 60 --retry 2 --retry-delay 5 `
                -o "$tb.part" "$base/$tb"
            if ($LASTEXITCODE -eq 0 -and (Test-Path "$tb.part")) {
                Move-Item "$tb.part" $tb -Force; $ok = $true; break
            }
            Write-Host "    失败 (curl exit=$LASTEXITCODE)"
            Remove-Item "$tb.part" -Force -ErrorAction SilentlyContinue
        }
        if (-not $ok) { Write-Host "::error::$tb 所有镜像均下载失败"; exit 1 }
    }
    Report-Stage "下载 $m" $swStage
    if (-not (Test-Path $m)) {
        $swStage = [Diagnostics.Stopwatch]::StartNew()
        New-Item -ItemType Directory -Force $m | Out-Null
        # Windows 10+ 自带 bsdtar，认 .tar.xz
        tar -xf $tb -C $m --strip-components=1
        if ($LASTEXITCODE -ne 0) { throw "解压 $tb 失败" }
        Report-Stage "解压 $m" $swStage
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
    # ★ 显式钉死 Ninja。不写这条时 configure 会自己 find_program(ninja)，找不到就**静默**
    #   退回 "NMake Makefiles"（单线程），而那正是上一版两个 Windows job 双双跑满 6 小时的根因。
    #   上面已经保证 ninja 在 PATH 上了，这里是第二道闸：真出问题时 configure 会明确报错，
    #   而不是安安静静地慢十倍。
    '-cmake-generator', 'Ninja',
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
$sw = [Diagnostics.Stopwatch]::StartNew()
& "$env:QT_SRC\qtbase\configure.bat" @common -- -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "qtbase configure 失败 (exit=$LASTEXITCODE)" }
# configure 单独计一段。它和上面每一段都有自己的注解 —— 注解是公开可读的，日志要 admin，
# 所以下次再撞 6 小时上限时，「最后一条注解是哪一段」就直接指出卡在哪儿。
Report-Stage 'qtbase configure' $sw

# ★★ 第三道闸，也是最重要的一道：**从 CMakeCache.txt 核对真正选中的生成器**。
#   前两道（PATH 上有 ninja、-cmake-generator Ninja）都是"输入侧"的保证，这一道查的是
#   **结果**。差别很实在：上一版没有它，生成器选成 NMake 的后果是 6 小时后一句
#   "exceeded the maximum execution time"，而作业日志要仓库 admin 才能拉 —— 等于
#   烧掉六小时机时换来零信息。有了它，同样的问题 5 分钟内就是一条写明缘由的红灯。
$cache = Join-Path $b 'CMakeCache.txt'
$gen = ''
if (Test-Path $cache) {
    $m = Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
    if ($m) { $gen = $m.Matches[0].Groups[1].Value }
}
Write-Host "::warning::qtbase 实际使用的生成器 = '$gen'（cores=$cores）"
if ($gen -ne 'Ninja') {
    Write-Host "::error::生成器是 '$gen' 而不是 Ninja —— NMake/VS 这类要么单线程要么慢得多，"
    Write-Host "::error::这个 job 会跑满 6 小时被掐掉。与其烧六小时，不如现在就失败。"
    exit 1
}

# ★ 显式 --parallel $cores。不带数字时 CMake 用「构建工具的默认值」，等于把并行度交给
#   生成器决定 —— 而生成器选错正是上一版超时的根因，这里不留任何余地。
cmake --build . --parallel $cores
if ($LASTEXITCODE -ne 0) { throw "qtbase 构建失败" }
cmake --install .
if ($LASTEXITCODE -ne 0) { throw "qtbase 安装失败" }
Report-Stage 'qtbase' $sw
Write-Host "::endgroup::"

foreach ($m in @('qtshadertools', 'qtdeclarative')) {
    Write-Host "::group::[qt-static] $m"
    $bm = Join-Path $env:QT_SRC "b-$m"
    Remove-Item -Recurse -Force $bm -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $bm | Out-Null
    Set-Location $bm
    $swm = [Diagnostics.Stopwatch]::StartNew()
    & "$env:QT_PREFIX\bin\qt-configure-module.bat" "$env:QT_SRC\$m"
    if ($LASTEXITCODE -ne 0) { throw "$m configure 失败" }
    cmake --build . --parallel $cores
    if ($LASTEXITCODE -ne 0) { throw "$m 构建失败" }
    cmake --install .
    if ($LASTEXITCODE -ne 0) { throw "$m 安装失败" }
    Report-Stage $m $swm
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
