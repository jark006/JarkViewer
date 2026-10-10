# 只支持 PowerShell 7（pwsh）：下面的守卫会拒绝 Windows PowerShell 5.1，并提示怎么装 pwsh。
# 本文件必须保留 UTF-8 BOM——5.1 会把无 BOM 的 .ps1 按 ANSI 解码，脚本在跑到守卫之前就已经
# 乱码/语法报错，用户看到的是一句莫名其妙的报错，而不是这条提示。
<#
.SYNOPSIS
    用 zlib-ng 重建 zlib.lib，PNG 解压明显提速（大图打开更快的最后一段）。

.DESCRIPTION
    libpng 解压 PNG 的 IDAT 走的是 zlib 的 inflate，一张几百 MB 的 16 位 PNG
    里 inflate 能占掉解码时间的大头。zlib-ng 的 compat 构建（ZLIB_COMPAT=ON）
    导出与 zlib 完全相同的符号名和结构体布局，在 SSE2 / SSSE3 / AVX2 上按 CPU
    运行时派发优化实现，而基线要求不抬高。

    仓库里只有一份 zlib（JarkViewer/libopencv/zlib.lib），opencv_world 是最终
    链接时才去解析 inflate 这些符号的，所以把这个 .lib 换成 zlib-ng 的就够了，
    不必重建 OpenCV。include 下的 zlib.h / zconf.h 也要一起换（zconf.h 会引用
    zlib_name_mangling.h；原版头文件在 git 历史里，回退用 git checkout 即可）。

    需要装有「适用于 Windows 的 C++ CMake 工具」组件的 Visual Studio / Build Tools。

.EXAMPLE
    pwsh tools/build-zlib-ng.ps1              # 只构建，产物留在工作目录
    pwsh tools/build-zlib-ng.ps1 -Install     # 构建并替换仓库里的库与头文件（库原件自动备份）
#>
param(
    [string]$WorkDir = (Join-Path $env:TEMP "jarkviewer-zlib-ng"),
    [switch]$Install
)

# --- PowerShell 7 (pwsh) only -------------------------------------------------------------
# Windows PowerShell 5.1 is refused below: it reads BOM-less .ps1 files as ANSI (mojibake, and
# sometimes a syntax error that hides this guard) and quotes Start-Process arguments
# differently, which the UI test scripts depend on. Run everything with pwsh.
if ($PSVersionTable.PSEdition -ne 'Core') {
    Write-Host "This script requires PowerShell 7 (pwsh); Windows PowerShell $($PSVersionTable.PSVersion) is not supported." -ForegroundColor Red
    Write-Host "Re-run with:  pwsh -File `"$PSCommandPath`"" -ForegroundColor Yellow
    Write-Host "Install:      winget install --id Microsoft.PowerShell" -ForegroundColor Yellow
    exit 1
}

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$appRoot = Join-Path $repoRoot "JarkViewer"
$version = "2.3.3"
$archiveUrl = "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/$version.tar.gz"
$archiveSha256 = "F9C65AA9C852EB8255B636FD9F07CE1C406F061EC19A2E7D508B318CA0C907D1"

$srcDir = Join-Path $WorkDir "src"
$buildDir = Join-Path $WorkDir "build"
$installDir = Join-Path $WorkDir "install"
New-Item -ItemType Directory -Force -Path $srcDir, $buildDir | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "找不到 vswhere.exe，请先安装 Visual Studio 或 Build Tools" }
$vsPath = & $vswhere -latest -property installationPath
$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
foreach ($tool in @($cmake, $ninja)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "缺少 $tool，请在 Visual Studio 安装程序里勾选「适用于 Windows 的 C++ CMake 工具」"
    }
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "call `"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
}

$source = Join-Path $srcDir "zlib-ng-$version"
if (-not (Test-Path -LiteralPath $source)) {
    # 优先 git 克隆（走 SSH 时最稳，内容由 git 自身校验 commit），
    # 不行再回退到 https 下载 + SHA-256 校验（部分网络环境会掐断 github 的 https）
    $cloned = $false
    if (Get-Command git -ErrorAction SilentlyContinue) {
        Write-Host "经 git 克隆 zlib-ng $version ..."
        & git clone --quiet --depth 1 --branch $version "git@github.com:zlib-ng/zlib-ng.git" $source
        $cloned = ($LASTEXITCODE -eq 0) -and (Test-Path -LiteralPath (Join-Path $source "CMakeLists.txt"))
        if (-not $cloned -and (Test-Path -LiteralPath $source)) {
            Remove-Item -Recurse -Force -LiteralPath $source
        }
        if (-not $cloned) { Write-Host "git 克隆失败，回退到 https 下载 ..." -ForegroundColor Yellow }
    }

    if (-not $cloned) {
        $archive = Join-Path $srcDir "zlib-ng-$version.tar.gz"
        if (-not (Test-Path -LiteralPath $archive)) {
            Write-Host "下载 zlib-ng $version ..."
            Invoke-WebRequest -Uri $archiveUrl -OutFile $archive -UseBasicParsing
        }
        $actualSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        if ($actualSha256 -ne $archiveSha256) {
            throw "zlib-ng 源码包校验失败：期望 $archiveSha256，实际 $actualSha256"
        }
        Write-Host "解压 ..."
        tar -xzf $archive -C $srcDir
        if (-not (Test-Path -LiteralPath $source)) { throw "解压后没有找到 $source" }
    }
}

$options = @(
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",   # 与主程序的 /MT 一致
    "-DCMAKE_INSTALL_PREFIX=$installDir",
    "-DBUILD_SHARED_LIBS=OFF",

    # 关键：导出与 zlib 完全相同的符号名和结构体布局，对已编译好的库是原地替换
    "-DZLIB_COMPAT=ON",

    # 运行时按 CPU 派发，不抬高基线要求
    "-DWITH_OPTIM=ON", "-DWITH_NATIVE_INSTRUCTIONS=OFF",

    "-DZLIB_ENABLE_TESTS=OFF", "-DZLIBNG_ENABLE_TESTS=OFF", "-DWITH_GTEST=OFF",
    "-DWITH_BENCHMARKS=OFF",

    "-S", $source, "-B", $buildDir
)

Write-Host "=== 配置 zlib-ng ===" -ForegroundColor Cyan
$log = & $cmake @options 2>&1
if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host $_ }; throw "zlib-ng 配置失败" }
$log | Select-String -Pattern "Compat|compat|Optim|SSE|AVX|Build type" | ForEach-Object { Write-Host $_.Line }

Write-Host "=== 编译 zlib-ng ===" -ForegroundColor Cyan
& $cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "zlib-ng 编译失败" }
& $cmake --install $buildDir | Out-Null
if ($LASTEXITCODE -ne 0) { throw "zlib-ng 安装失败" }

$producedLib = Get-ChildItem (Join-Path $installDir "lib") -Filter "*.lib" |
    Sort-Object Length -Descending | Select-Object -First 1
if (-not $producedLib) { throw "没有生成静态库" }
# zconf.h 会 #include "zlib_name_mangling.h"，少拷这一个编译就直接报找不到头文件
$producedHeaders = @("zlib.h", "zconf.h", "zlib_name_mangling.h") | ForEach-Object {
    $header = Join-Path $installDir "include\$_"
    if (-not (Test-Path -LiteralPath $header)) { throw "缺少头文件 $_" }
    $header
}

$targetLib = Join-Path $appRoot "libopencv\zlib.lib"
$oldSize = if (Test-Path -LiteralPath $targetLib) { (Get-Item -LiteralPath $targetLib).Length / 1KB } else { 0 }
Write-Host ""
"产物 {0}  {1:N0} KB   （仓库现有 {2:N0} KB）" -f $producedLib.Name, ($producedLib.Length / 1KB), $oldSize

if (-not $Install) {
    Write-Host ""
    Write-Host "未安装。加 -Install 参数可替换仓库里的库与头文件。" -ForegroundColor Yellow
    return
}

# 库原件备份到同目录（libopencv/.gitignore 忽略 *.lib，不会进 git）。
# 备份只在第一次建立：反复 -Install 时不能让备份被新文件覆盖，否则想回退原版 zlib 就没有了。
# 头文件不物理备份：原版就在 git 历史里，git checkout -- JarkViewer/include/zlib.h 即可。
$backupLib = Join-Path $appRoot "libopencv\zlib-1.3.1.lib"
if ((Test-Path -LiteralPath $targetLib) -and -not (Test-Path -LiteralPath $backupLib)) {
    Copy-Item -LiteralPath $targetLib -Destination $backupLib
    Write-Host "已备份 zlib.lib → libopencv/zlib-1.3.1.lib"
}
Copy-Item -LiteralPath $producedLib.FullName -Destination $targetLib -Force
Write-Host "已替换 $targetLib"

foreach ($header in $producedHeaders) {
    $target = Join-Path $appRoot ("include\" + [IO.Path]::GetFileName($header))
    Copy-Item -LiteralPath $header -Destination $target -Force
    Write-Host "已替换 $target"
}

Write-Host ""
Write-Host "装好了。重新构建后验证：" -ForegroundColor Green
Write-Host "  pwsh ./buildRelease.ps1"
Write-Host "  ./x64/Release/JarkViewer.exe --probe <大 PNG>    # 对比解码耗时"
