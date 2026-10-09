<#
.SYNOPSIS
    重建「只解不编」的 FFmpeg 静态库：去掉全部编码器，解码器 / 解复用器一个不少。

.DESCRIPTION
    工程里那份 FFmpeg 是 vcpkg 那套构建的一个变体——配置行完全相同，只多一条
    --disable-encoders。看图软件只解码、从不编码，去掉编码器后可执行文件小约 10 MiB，
    解码能力零损失（当前配置：解码器 531、解复用器 363、复用器 184）。

    为什么 opus 与 adpcm_g722 这两个编码器必须留着：
      FFmpeg 把它们的 x86 asm 对象挂在 CONFIG_*_ENCODER 下，而**解码路径用的是同一份**——
        opus/pvq.c:918    ff_celt_pvq_init_x86()   ← x86/celt_pvq_init.o
        g722dsp.c:77      ff_g722dsp_init_x86()    ← x86/g722dsp*.o
      全关掉会让 Opus / G.722 解码器各差几十个符号链不上（实测上百条 LNK2001）。

    源码用 vcpkg 已经打好补丁的那份：它那批补丁里有好几个是 MSVC 构建的必需品
    （0046 修 MSVC 检测、0007 修静态库命名、0050 修链接测试里的绝对路径……），
    拿官方 tarball 直接编是编不出来的。所以要先在 vcpkg 里装过一次 ffmpeg，
    buildtrees 下会留下打过补丁的源码树。

    构建在独立目录里做 out-of-tree 配置，**不动 vcpkg 的构建树**。用 -Install 时才把
    那 7 个静态库复制进 JarkViewer/libffmpeg/。

    路径与环境的五个坑（都实测踩过，脚本里已经规避，改脚本时别踩回去）：
      1. MSVC 的 bin 必须排在 /usr/bin 前面——msys2 的 coreutils 里也有个 link.exe，
         会被先找到，configure 的链接测试全挂。
      2. 不能设 MSYS2_ARG_CONV_EXCL：它关掉 msys2 的路径转换，cl.exe 拿到
         "/d/vcpkg/.../x.c" 会把开头当成 /D 选项，报 D8043 未知选项。
      3. 传给 configure 的路径要写 D:/... 而不是 /d/...——configure 从 $0 推 SRC_PATH，
         写成 POSIX 形式会把 /d/... 塞进 config.mak。
      4. 改过 configure 之后必须 make clean 再 make，残留的旧 .o 里还引用着
         ff_*_encoder，直接链会得到上百个 LNK2001。
      5. ar-lib 要先放进 PATH（vcpkg 的 share/vcpkg-make/wrappers），否则报
         "ar-lib: command not found"。

.EXAMPLE
    pwsh tools/build-ffmpeg-noenc.ps1                    # 只构建，产物留在工作目录
    pwsh tools/build-ffmpeg-noenc.ps1 -Install           # 构建并替换 JarkViewer/libffmpeg
#>
param(
    [string]$Version = "9.0.2",
    [string]$VcpkgRoot = "D:\vcpkg",
    [string]$Triplet = "x64-windows-static",
    [string]$Msys2Root = "",
    [string]$WorkDir = (Join-Path $env:TEMP "jarkviewer-ffmpeg-noenc"),
    [switch]$Install
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$appRoot = Join-Path $repoRoot "JarkViewer"
$installed = Join-Path $VcpkgRoot "installed\$Triplet"

function To-UnixPath([string]$p) {          # D:\a\b  ->  /d/a/b
    $p = $p -replace '\\', '/'
    if ($p -match '^([A-Za-z]):') { $p = '/' + $Matches[1].ToLower() + $p.Substring(2) }
    return $p
}
function To-WinSlashPath([string]$p) {      # D:\a\b  ->  D:/a/b
    return ($p -replace '\\', '/')
}

# ---- 1. 找工具链 -------------------------------------------------------------
# msys2 得同时有 bash 和 make：vcpkg 的 downloads 里还躺着一堆只含 pkg-config 的小目录。
$msys2 = $null
$candidates = @($Msys2Root, $env:MSYS2_ROOT, "C:\msys64") + @(
    Get-ChildItem (Join-Path $VcpkgRoot "downloads\tools\msys2") -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name | ForEach-Object { $_.FullName })
foreach ($c in $candidates) {
    if ($c -and (Test-Path -LiteralPath "$c\usr\bin\bash.exe") -and (Test-Path -LiteralPath "$c\usr\bin\make.exe")) {
        $msys2 = $c; break
    }
}
if (-not $msys2) { throw "找不到可用的 MSYS2（需要 usr\bin 下同时有 bash.exe 与 make.exe）。装一个 MSYS2 后用 -Msys2Root 指定，或先跑一次 vcpkg。" }

$nasmDir = $null
foreach ($d in @((Get-ChildItem (Join-Path $VcpkgRoot "downloads\tools\nasm") -Directory -ErrorAction SilentlyContinue |
                    Sort-Object Name -Descending | ForEach-Object { $_.FullName }),
                 "C:\Program Files\nasm")) {
    if ($d -and (Test-Path -LiteralPath "$d\nasm.exe")) { $nasmDir = $d; break }
}
if (-not $nasmDir) { throw "找不到 nasm.exe（x86 汇编要它）。装 nasm 或让 vcpkg 拉一个。" }

$arLibDir = $null
foreach ($d in @((Join-Path $installed "..\x64-windows\share\vcpkg-make\wrappers"),
                 (Join-Path $installed "share\vcpkg-make\wrappers"))) {
    if (Test-Path -LiteralPath (Join-Path $d "ar-lib")) { $arLibDir = (Resolve-Path -LiteralPath $d).Path; break }
}
if (-not $arLibDir) { throw "找不到 vcpkg 的 ar-lib 包装脚本（$VcpkgRoot\installed\x64-windows\share\vcpkg-make\wrappers）。它的 FFmpeg 构建拿 ar-lib 包 lib.exe。" }

$pkgConfig = Get-ChildItem (Join-Path $VcpkgRoot "downloads\tools\msys2") -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName "mingw64\bin\pkg-config.exe" } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $pkgConfig) {
    $pkgConfig = (Get-Command pkg-config -ErrorAction SilentlyContinue).Source
}
if (-not $pkgConfig) { throw "找不到 pkg-config.exe：外部库（dav1d / aom / vpx / srt …）靠它定位。" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "找不到 vswhere.exe，请先安装 Visual Studio 或 Build Tools" }
$vsPath = & $vswhere -latest -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

# ---- 2. 找打好补丁的 FFmpeg 源码 ---------------------------------------------
$srcRoot = Join-Path $VcpkgRoot "buildtrees\ffmpeg\src"
$srcDir = Get-ChildItem $srcRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "configure") } |
    Sort-Object Name -Descending |
    Where-Object { $_.Name -like "*$Version*" } | Select-Object -First 1
if (-not $srcDir) {
    $srcDir = Get-ChildItem $srcRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "configure") } |
        Sort-Object Name -Descending | Select-Object -First 1
}
if (-not $srcDir) {
    throw "在 $srcRoot 下找不到 FFmpeg 源码。先在 vcpkg 里装一次：`n" +
          "  vcpkg install ffmpeg[all,amf,...]:$Triplet`n" +
          "（补丁版源码是 MSVC 构建的前提，官方 tarball 编不出来）"
}
$srcDir = $srcDir.FullName
# vcpkg 补丁标记：0040 那支会给 libavformat/avformat.h 加 av_stream_get_first_dts
if (-not (Select-String -LiteralPath (Join-Path $srcDir "libavformat\avformat.h") -Pattern "av_stream_get_first_dts" -Quiet)) {
    throw "$srcDir 看起来不是 vcpkg 打过补丁的源码（缺 av_stream_get_first_dts 标记），拿它编必然失败。"
}

$buildDir = Join-Path $WorkDir "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

Write-Host "MSYS2      $msys2"
Write-Host "nasm       $nasmDir"
Write-Host "ar-lib     $arLibDir"
Write-Host "pkg-config $pkgConfig"
Write-Host "FFmpeg 源码 $srcDir"
Write-Host "构建目录    $buildDir"
Write-Host ""

# ---- 3. 生成并运行构建脚本 ---------------------------------------------------
# 配置行与 vcpkg 的那份逐项对齐，只删掉 --enable-encoder=h264_qsv、末尾补 --disable-encoders
# 与两个必须留的编码器。shell 里只写 ASCII，中文说明留在本文件的注释里。
$bash = Join-Path $msys2 "usr\bin\bash.exe"
$sh = @"
set -e

MSVC_BIN="`$(echo "`$VCToolsInstallDir" | /usr/bin/sed -e 's|\\\\|/|g' -e 's|^\([A-Za-z]\):|/\L\1|')bin/HostX64/x64"
export PATH="`$MSVC_BIN:/usr/bin:$(To-UnixPath $nasmDir):$(To-UnixPath $arLibDir):`$PATH"
export PKG_CONFIG_PATH="$(To-WinSlashPath (Join-Path $installed "lib\pkgconfig"))"

cd "$(To-WinSlashPath $buildDir)"

echo "=== configure ==="
sh "$(To-WinSlashPath $srcDir)/configure" \
    --prefix="$(To-WinSlashPath (Join-Path $WorkDir "install"))" \
    --toolchain=msvc --enable-pic --disable-doc --enable-runtime-cpudetect --disable-autodetect \
    --target-os=win32 --enable-w32threads --enable-d3d11va --enable-d3d12va --enable-dxva2 --enable-mediafoundation \
    --cc=cl.exe --host-cc=cl.exe --cxx=cl.exe --windres=rc.exe --ld=link.exe "--ar=ar-lib lib.exe" --ranlib=: \
    --disable-ffmpeg --disable-ffplay --disable-ffprobe \
    --enable-avcodec --enable-avdevice --enable-avformat --enable-avfilter --enable-swresample --enable-swscale \
    --disable-alsa --enable-amf --enable-libaom --enable-libass --disable-avisynth --enable-bzlib \
    --enable-libdav1d --enable-libsvtav1 --disable-libfdk-aac --enable-libfontconfig --disable-libharfbuzz \
    --enable-libfreetype --enable-libfribidi --enable-iconv --enable-libilbc --enable-lzma --enable-libmp3lame \
    --enable-libmodplug --enable-cuda --enable-nvenc --enable-nvdec --enable-cuvid --enable-ffnvcodec \
    --enable-opencl --enable-opengl --enable-libopenh264 --enable-libopenjpeg --enable-libopenmpt \
    --disable-openssl --enable-schannel --enable-libopus --enable-sdl2 --enable-libsnappy --enable-libsoxr \
    --enable-libspeex --enable-libssh --disable-libtensorflow --disable-libtesseract --enable-libtheora \
    --enable-libtwolame --enable-libvorbis --enable-libvpx --enable-vulkan --enable-libwebp --disable-libx264 \
    --disable-libx265 --enable-libxml2 --enable-zlib --enable-libsrt --enable-libvpl \
    --enable-decoder=h264_qsv \
    --disable-vaapi --disable-libzmq --disable-librubberband \
    --enable-cross-compile --extra-cflags=-DHAVE_UNISTD_H=0 \
    --pkg-config="$(To-WinSlashPath $pkgConfig)" \
    --pkg-config-flags=--static --enable-optimizations \
    --extra-cflags=-MT --extra-cxxflags=-MT \
    --extra-ldflags=-libpath:"$(To-WinSlashPath (Join-Path $installed "lib"))" \
    --arch=x86_64 --enable-asm --enable-x86asm \
    --disable-encoders \
    --enable-encoder=opus --enable-encoder=adpcm_g722

echo "=== make clean ==="
make clean >/dev/null 2>&1 || true

echo "=== make ==="
make -j"`$(nproc)"

echo "=== done ==="
"@

$scriptFile = Join-Path $WorkDir "build.sh"
[IO.File]::WriteAllText($scriptFile, $sh.Replace("`r`n", "`n"), (New-Object Text.UTF8Encoding $false))

Write-Host "=== 构建 FFmpeg（十几分钟）===" -ForegroundColor Cyan
cmd /c "call `"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
}
& $bash (To-UnixPath $scriptFile)
if ($LASTEXITCODE -ne 0) { throw "FFmpeg 构建失败" }

# ---- 4. 核对产物 -------------------------------------------------------------
$libNames = @("avcodec", "avdevice", "avfilter", "avformat", "avutil", "swresample", "swscale")
$produced = @{}
foreach ($name in $libNames) {
    $p = Join-Path $buildDir "lib$name\$name.lib"
    if (-not (Test-Path -LiteralPath $p)) { throw "没有生成 $p" }
    $produced[$name] = $p
}

# 配置门禁：编码器只该剩 opus 与 adpcm_g722，解码器不能少
$components = Join-Path $buildDir "config_components.h"
$enabledEnc = Select-String -LiteralPath $components -Pattern '^#define CONFIG_([A-Z0-9_]+)_ENCODER 1' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
$decCount = (Select-String -LiteralPath $components -Pattern '^#define CONFIG_[A-Z0-9_]+_DECODER 1').Count
$demuxCount = (Select-String -LiteralPath $components -Pattern '^#define CONFIG_[A-Z0-9_]+_DEMUXER 1').Count
Write-Host ""
Write-Host ("编码器 {0} 个：{1}" -f $enabledEnc.Count, ($enabledEnc -join " "))
Write-Host ("解码器 {0} 个，解复用器 {1} 个" -f $decCount, $demuxCount)
if ($enabledEnc.Count -ne 2 -or $enabledEnc -notcontains "OPUS" -or $enabledEnc -notcontains "ADPCM_G722") {
    Write-Host "!! 编码器集合不是 {OPUS, ADPCM_G722}：多留了白占体积，少留了会让 Opus/G.722 解码器链不上" -ForegroundColor Yellow
}
if ($decCount -lt 500) { Write-Host "!! 解码器只剩 $decCount 个，比预期少——检查 configure 行是不是被动过" -ForegroundColor Yellow }

foreach ($name in $libNames) {
    Write-Host ("  {0,-14} {1,8:N1} MB" -f "$name.lib", ((Get-Item -LiteralPath $produced[$name]).Length / 1MB))
}

if (-not $Install) {
    Write-Host ""
    Write-Host "未安装到工程。加 -Install 可替换 JarkViewer/libffmpeg/。" -ForegroundColor Yellow
    return
}

# ---- 5. 装进工程 -------------------------------------------------------------
$targetLibDir = Join-Path $appRoot "libffmpeg"
foreach ($name in $libNames) {
    Copy-Item -LiteralPath $produced[$name] -Destination (Join-Path $targetLibDir "$name.lib") -Force
}
Write-Host ""
Write-Host "已替换 $targetLibDir 下的 7 个库。" -ForegroundColor Green
Write-Host "接着做：pwsh ./buildRelease.ps1，再跑一遍" -ForegroundColor Green
Write-Host "  ./x64/Release/JarkViewer.exe --probe --playback-test `"D:\Downloads\test\livp`"   # 实况/视频解码（126 项）"
